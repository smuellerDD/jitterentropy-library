/*
 * Jitter RNG: unit tests for the health tests on a running collector
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/*
 * What tests/health cannot reach: it stubs out the recovery loop of the RCT
 * with memory, so the blocks that loop generates never run there. Here they
 * run for real, on a mocked clock that keeps them deterministic - which is why
 * this program defines JENT_CONF_ENABLE_MOCK_TIMER for its own copy of the
 * sources, as unit-mock does.
 *
 * And jent_apt_duplicate(), which carries the APT state over to a replacement
 * collector, judged by what the replacement then reports rather than by the
 * counters it holds. The library's own replacements never reach it - each
 * begins in another startup stage than the collector it replaces (see
 * jent_health_duplicate()) - so this covers the helper for a replacement that
 * does not.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <stdlib.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-sha3.c"
#include "jitterentropy-gcd.c"
#include "jitterentropy-health.c"
#include "jitterentropy-noise.c"
#include "jitterentropy-timer.c"
#include "jitterentropy-base.c"
#include "jitterentropy-uuid.c"
#include "jitterentropy-status.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * A clock that moves by a varying step, so no measurement is stuck and no
 * health test fires. On request it holds its last reading for a number of
 * reads - a measurement reads the clock more than once.
 */
struct uh_clock {
	uint64_t now;
	unsigned int step;
	unsigned int repeat;
	size_t served;
	/*
	 * Move a collector's startup state on at a chosen reading, which is
	 * what a reentrant path does to the state machine from inside a
	 * measurement. See test_startup_state_reentrancy().
	 */
	struct rand_data *poke;
	size_t poke_at;
	/* The state it is moved to. */
	enum jent_startup_state poke_state;
};

static void uh_clock_cb(void *arg, uint64_t *out)
{
	struct uh_clock *c = arg;

	c->served++;

	if (c->poke && c->served == c->poke_at) {
		c->poke->startup_state = c->poke_state;
		c->poke = NULL;
	}

	if (c->repeat) {
		c->repeat--;
		*out = c->now;
		return;
	}

	c->step = (c->step * 1103515245u + 12345u);
	c->now += 500 + (c->step >> 22);
	*out = c->now;
}

/*
 * The recovery loop of the RCT with memory after the startup has completed.
 * Each of its blocks is a window of its own; before the first sample - the
 * priming measurement a completed collector takes ahead of every block - the
 * outer window it interrupts must be closed. Otherwise that sample is judged
 * at the outer window's position with the count still at the cutoff: a
 * non-stuck one raises the intermittent failure the recovery exists to rule
 * out, and a stuck one pushes the count past the cutoff, which from osr 8 on
 * is the NTG.1 permanent cutoff. The priming measurement is now kept out of
 * the health tests altogether (jent_measure_jitter_one() with health 0); the
 * cases stay, as whichever way the priming goes, the recovery has to come
 * out clean.
 */
static void test_recovery_after_startup(unsigned int osr, unsigned int stuck)
{
	struct uh_clock c = { 1, 7, 0, 0, NULL, 0, jent_startup_completed };
	struct rand_data *ec;
	unsigned int failure, per_measurement;
	size_t served, measurements, expected;

	printf("  osr %u, %s priming measurement\n", osr,
	       stuck ? "a stuck" : "a good");

	jent_set_mock_timer(uh_clock_cb, &c);

	ec = jent_entropy_collector_alloc(osr, JENT_NTG1);
	if (!ec) {
		JENT_UT_SKIP("the recovery loop",
			     "no NTG.1 collector on the mocked clock");
		jent_set_mock_timer(NULL, NULL);
		return;
	}

	JENT_UT_EQ(ec->startup_state, jent_startup_completed,
		   "the collector has completed its startup");
	JENT_UT_EQ(jent_health_failure(ec), 0,
		   "and passed its health tests on the mocked clock");
	JENT_UT_TRUE(ec->rct_mem_cutoff < ec->rct_mem_cutoff_permanent,
		     "the recovery loop is reachable at this rate");

	/*
	 * How many times one measurement reads the clock, measured rather than
	 * assumed: the bound below is a number of measurements, and the clock
	 * only counts reads. Comparing the two directly - which this did -
	 * leaves the bound loose by exactly this factor (17 here), enough for
	 * a recovery that ran a single block instead of all ten to pass it.
	 */
	served = c.served;
	jent_measure_jitter(ec, 0, NULL);
	per_measurement = (unsigned int)(c.served - served);
	JENT_UT_TRUE(per_measurement > 0, "a measurement reads the clock");
	if (!per_measurement)
		per_measurement = 1;

	/*
	 * For a stuck priming measurement, hold the clock for as many reads
	 * as one measurement takes, at the stamp it is a delta against.
	 */
	if (stuck) {
		c.repeat = per_measurement;
		ec->prev_time = c.now;
	}

	/*
	 * Part-way through a window, one stuck measurement short of the
	 * cutoff, on a counted position - where the next stuck measurement
	 * enters the recovery loop.
	 */
	ec->health_failure = 0;
	ec->rct_mem_ctr = 3;
	ec->rct_mem_count = (unsigned short)(ec->rct_mem_cutoff - 1);
	served = c.served;

	jent_rct_mem_insert(ec, 1);

	failure = jent_health_failure(ec);
	JENT_UT_EQ(failure & (JENT_RCT_MEM_FAILURE |
			      JENT_RCT_MEM_FAILURE_PERMANENT), 0,
		   "blocks on a good clock recover from the cutoff");
	JENT_UT_EQ(failure, 0, "and no other health test fired");

	/*
	 * Every block of the loop is one jent_random_data_one() - which takes
	 * ->rct_mem_nosr non-stuck measurements - preceded by the priming one
	 * of a completed collector. Ten blocks, so ten times that; nine fall
	 * short of it. On this clock no measurement is stuck, so the count is
	 * exact up to the few the startup leaves in flight - fewer than one
	 * more block, so eleven exceed it.
	 *
	 * Ten as a literal, not JENT_RCT_MEM_RECOVERY_LOOP_CNT: the number of
	 * blocks is what is under test, and a bound taken from the constant
	 * would follow any change of it.
	 */
	measurements = (c.served - served) / per_measurement;
	expected = (size_t)10 * ((size_t)ec->rct_mem_nosr + 1);
	if (measurements < expected ||
	    measurements >= expected + ec->rct_mem_nosr + 1)
		printf("    %zu measurements, %zu expected (%u reads each)\n",
		       measurements, expected, per_measurement);
	JENT_UT_TRUE(measurements >= expected,
		     "the recovery generated all ten of its blocks");
	JENT_UT_TRUE(measurements < expected + ec->rct_mem_nosr + 1,
		     "and no eleventh");
	JENT_UT_EQ(ec->rct_mem_ctr, 4,
		   "the outer window continues where it was interrupted");
	JENT_UT_EQ(ec->rct_mem_count, 0, "with a fresh count");
	JENT_UT_EQ(ec->in_recovery, 0, "and the recovery has ended");

	jent_entropy_collector_free(ec);
	jent_set_mock_timer(NULL, NULL);
}

static void test_recovery(void)
{
	jent_ut_group("the RCT-with-memory recovery loop after startup");

	test_recovery_after_startup(0, 0);
	test_recovery_after_startup(0, 1);
	/* Where the NTG.1 permanent cutoff is one above the intermittent one. */
	test_recovery_after_startup(8, 0);
	test_recovery_after_startup(8, 1);
}

/*
 * A failure ends the recovery. The blocks after it prove nothing - the loop
 * exists to tell an expected false positive from a real one, and a real one
 * has been established - and generating them means driving a noise source that
 * has already been judged bad.
 *
 * Nothing else in the tree covers the break: tests/health and tests/fuzz both
 * replace jent_random_data_recovery() with a stub, and the stub never raises a
 * failure. What makes it observable here is that every block begins with a
 * priming measurement of its own, whether or not the block then generates
 * anything - so ten blocks read the clock ten times over even when each one
 * gives up at once, and one block reads it once.
 */
static void test_recovery_stops_at_failure(void)
{
	struct uh_clock c = { 1, 7, 0, 0, NULL, 0, jent_startup_completed };
	struct rand_data *ec;
	unsigned int per_measurement;
	size_t served, measurements;

	jent_ut_group("the recovery loop stops at the first failure");

	jent_set_mock_timer(uh_clock_cb, &c);

	ec = jent_entropy_collector_alloc(0, JENT_NTG1);
	if (!ec) {
		JENT_UT_SKIP("the recovery loop",
			     "no NTG.1 collector on the mocked clock");
		jent_set_mock_timer(NULL, NULL);
		return;
	}

	served = c.served;
	jent_measure_jitter(ec, 0, NULL);
	per_measurement = (unsigned int)(c.served - served);
	JENT_UT_TRUE(per_measurement > 0, "a measurement reads the clock");
	if (!per_measurement)
		per_measurement = 1;

	/*
	 * A clock that has stopped, against an RCT one stuck measurement short
	 * of its permanent cutoff: the first measurement of the very first
	 * block - the one after its ->prev_time priming, which the health
	 * tests do not judge - is stuck and raises the failure, and the block
	 * then generates nothing further.
	 */
	c.repeat = ~0u;
	ec->prev_time = c.now;
	ec->health_failure = 0;
	ec->rct_count = ec->rct_cutoff_permanent - 1;
	ec->stuck_prime = 0;

	served = c.served;
	jent_random_data_recovery(ec, JENT_RCT_MEM_RECOVERY_LOOP_CNT);
	measurements = (c.served - served) / per_measurement;

	JENT_UT_TRUE(jent_health_failure(ec) & JENT_RCT_FAILURE_PERMANENT,
		     "the first block of the recovery fails");
	JENT_UT_EQ(measurements, 2,
		   "and not one of the nine blocks behind it is generated");

	jent_entropy_collector_free(ec);
	jent_set_mock_timer(NULL, NULL);
}

/*
 * The NTG.1 startup state machine names its successor state rather than
 * decrementing the one it found. The two agree as long as nothing else touches
 * ->startup_state, which is why a decrement survived so long; they part company
 * when a reentrant path - the RCT-with-memory recovery loop is one - has
 * already moved the state on by the time the stage finishes. A decrement then
 * subtracts from the new value and lands outside the enum, and the switch falls
 * through and subtracts again.
 *
 * The mocked clock stands in for that path: it moves the state while the memory
 * stage is taking a measurement.
 */
static void test_startup_state_reentrancy(void)
{
	struct uh_clock c = { 1, 7, 0, 0, NULL, 0, jent_startup_completed };
	struct rand_data *ec;

	jent_ut_group("the startup state machine against a reentrant path");

	jent_set_mock_timer(uh_clock_cb, &c);

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the startup states", 0);
		jent_set_mock_timer(NULL, NULL);
		return;
	}

	ec->startup_state = jent_startup_memory;
	ec->health_failure = 0;

	/* Part way into the first measurement of the memory stage. */
	c.poke = ec;
	c.poke_at = c.served + 3;

	jent_random_data(ec);

	JENT_UT_TRUE(c.poke == NULL, "the state was moved from under the stage");
	JENT_UT_EQ(ec->startup_state, jent_startup_completed,
		   "and the stage still names the successor it means");
	JENT_UT_TRUE(ec->startup_state >= jent_startup_completed &&
		     ec->startup_state <= jent_startup_memory,
		     "never a value outside the enum");
	JENT_UT_EQ(ec->stuck_prime, JENT_STUCK_PRIME,
		   "and the stuck test primes its reference from the next source");

	jent_entropy_collector_free(ec);
	jent_set_mock_timer(NULL, NULL);
}

static void uh_health_init(struct rand_data *ec, unsigned int osr)
{
	memset(ec, 0, sizeof(*ec));
	ec->osr = osr;
	ec->is_fips_enabled = 1;
	jent_health_init(ec, jent_health_init_type_common);
}

/*
 * The APT state jent_apt_duplicate() hands a replacement collector in the same
 * stage and on the same clock. From osr 14 on the common
 * permanent cutoff lies at most one above the intermittent one, so a count
 * primed at the intermittent cutoff would make the first repetition after the
 * reallocation a permanent failure, whatever the window held.
 */
static void test_apt_duplicate(void)
{
	struct rand_data old_ec, new_ec;
	unsigned int i;

	jent_ut_group("the APT state carried over to a replacement");

	/* A window barely begun, with nothing wrong in it. */
	uh_health_init(&old_ec, 13);
	uh_health_init(&new_ec, 14);
	JENT_UT_TRUE(new_ec.apt_cutoff_permanent - new_ec.apt_cutoff <= 1,
		     "the replacement's cutoffs are one apart");

	old_ec.apt_base = 0xc0ffee;
	old_ec.apt_base_set = 1;
	old_ec.apt_count = 2;
	old_ec.apt_observations = 42;
	jent_apt_duplicate(&new_ec, &old_ec);
	JENT_UT_EQ(new_ec.apt_count, 2, "the count the window holds is kept");
	JENT_UT_EQ(new_ec.apt_observations, 42, "at its window position");

	jent_apt_insert(&new_ec, 0xc0ffee);
	JENT_UT_EQ(jent_health_failure(&new_ec), 0,
		   "one repetition after the reallocation is no failure");

	/* A window failing throughout still escalates. */
	uh_health_init(&old_ec, 13);
	uh_health_init(&new_ec, 14);
	old_ec.apt_base = 0xc0ffee;
	old_ec.apt_base_set = 1;
	old_ec.apt_count = old_ec.apt_cutoff;
	old_ec.apt_observations = old_ec.apt_cutoff;
	jent_apt_duplicate(&new_ec, &old_ec);

	for (i = old_ec.apt_cutoff; i < JENT_APT_WINDOW_SIZE; i++)
		jent_apt_insert(&new_ec, 0xc0ffee);
	JENT_UT_TRUE(jent_health_failure(&new_ec) & JENT_APT_FAILURE_PERMANENT,
		     "a window repeating one symbol throughout is permanent");

	/* A completed window leaves nothing to continue. */
	uh_health_init(&old_ec, 13);
	uh_health_init(&new_ec, 14);
	old_ec.apt_base = 0xc0ffee;
	old_ec.apt_base_set = 0;
	old_ec.apt_count = old_ec.apt_cutoff;
	old_ec.apt_observations = JENT_APT_WINDOW_SIZE;
	jent_apt_duplicate(&new_ec, &old_ec);
	JENT_UT_EQ(new_ec.apt_base_set, 0,
		   "after a completed window the next symbol is a new base");

	jent_apt_insert(&new_ec, 0xc0ffee);
	JENT_UT_EQ(jent_health_failure(&new_ec), 0,
		   "and the completed window's count is not continued");
}

#ifdef JENT_HEALTH_LAG_PREDICTOR
/*
 * The stuck test's reference across a lag reset in the middle of a window -
 * what every FIPS / NTG.1 startup stage boundary does. The ring is indexed by
 * the observation count the reset clears, so the last deltas have to be where
 * the index then looks: for the next measurement of the collector, and for
 * the replacement that takes them over when the collector fails.
 */
static void test_lag_reset_keeps_reference(void)
{
	struct rand_data ec, new_ec;
	unsigned int n, i;

	jent_ut_group("a lag reset mid-window keeps the stuck test's reference");

	for (n = 1; n <= 2 * JENT_LAG_HISTORY_SIZE + 3; n++) {
		uh_health_init(&ec, 3);
		for (i = 1; i <= n; i++)
			jent_lag_insert(&ec, 1000 + 7 * i);

		jent_lag_reset(&ec);
		if (JENT_LAG_HISTORY(&ec, 0) != 1000 + 7 * n ||
		    (n > 1 && JENT_LAG_HISTORY(&ec, 1) != 1000 + 7 * (n - 1))) {
			JENT_UT_FAIL("after %u deltas the reference is "
				     "%llu, %llu", n,
				     (unsigned long long)JENT_LAG_HISTORY(&ec, 0),
				     (unsigned long long)JENT_LAG_HISTORY(&ec, 1));
			return;
		}

		uh_health_init(&new_ec, 4);
		jent_lag_duplicate(&new_ec, &ec);
		if (JENT_LAG_HISTORY(&new_ec, 0) != 1000 + 7 * n) {
			JENT_UT_FAIL("a replacement after %u deltas takes %llu",
				     n, (unsigned long long)
					JENT_LAG_HISTORY(&new_ec, 0));
			return;
		}
	}
	jent_ut_checks++;

	/* So a ramp continuing across the reset is stuck at once. */
	uh_health_init(&ec, 3);
	for (i = 1; i <= 13; i++)
		jent_lag_insert(&ec, 1000 + 7 * i);
	jent_lag_reset(&ec);
	JENT_UT_TRUE(jent_stuck(&ec, 1000 + 7 * 14),
		     "a constant step continuing across the reset is stuck");
}
#endif /* JENT_HEALTH_LAG_PREDICTOR */

int main(void)
{
	jent_ut_setup();

	test_recovery();
	test_recovery_stops_at_failure();
	test_startup_state_reentrancy();
	test_apt_duplicate();
#ifdef JENT_HEALTH_LAG_PREDICTOR
	test_lag_reset_keeps_reference();
#endif

	return jent_ut_report("unit-health");
}
