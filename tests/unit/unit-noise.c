/*
 * Jitter RNG: unit tests for the noise source on a mocked clock
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
 * What the noise source does with its measurements, counted on a clock this
 * program supplies: how many it takes per block, that a stuck one is taken
 * again rather than counted, that both NTG.1 startup stages run and feed the
 * pool, and that a read failing part-way hands back nothing it generated.
 *
 * Every one of these is invisible in the output - a block is 32 random-looking
 * bytes however few measurements went into it - which is why they are counted
 * at the clock. jent_set_mock_timer() is internal and only compiled into a
 * build that asked for it, so this program defines JENT_CONF_ENABLE_MOCK_TIMER
 * for its own copy of the sources, as unit-mock does.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
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
 * The clock: monotonic with varying steps, which passes the startup and the
 * health tests, and counts every reading. Two optional disturbances:
 *
 * - stuck_every: every so many readings repeat the previous step, so the
 *   delta equals the last one and the measurement is stuck.
 * - trip_ec: at reading trip_at, set trip_bit in that collector's health
 *   failures, as a health test would in the middle of a block.
 */
struct nz_clock {
	uint64_t t;
	uint64_t last_step;
	unsigned int lcg;
	unsigned long served;
	unsigned int stuck_every;
	struct rand_data *trip_ec;
	unsigned long trip_at;
	unsigned int trip_bit;
};

static struct nz_clock nz;

static void nz_clock_cb(void *arg, uint64_t *out)
{
	struct nz_clock *c = arg;

	c->served++;

	if (c->trip_ec && c->served == c->trip_at)
		c->trip_ec->health_failure |= c->trip_bit;

	if (!c->stuck_every || c->served % c->stuck_every) {
		c->lcg = c->lcg * 1103515245u + 12345u;
		c->last_step = 500 + (c->lcg >> 22);
	}
	c->t += c->last_step;
	*out = c->t;
}

static void nz_clock_plain(void)
{
	nz.stuck_every = 0;
	nz.trip_ec = NULL;
}

/* The flags of every collector here: the mocked clock, not a counting thread. */
#define NZ_FLAGS JENT_DISABLE_INTERNAL_TIMER

static struct rand_data *nz_alloc(unsigned int osr, unsigned int flags,
				  const char *what)
{
	struct rand_data *ec;

	nz_clock_plain();
	ec = jent_entropy_collector_alloc(osr, flags | NZ_FLAGS);
	if (!ec)
		JENT_UT_NO_COLLECTOR(what, flags | NZ_FLAGS);

	return ec;
}

/*
 * The measurements of one block: 256 bits at the oversampling rate - with the
 * safety factor of 64 + 1 bits on top in a compliance mode - rounded up to a
 * multiple of three, the window of the RCT with memory. Written out here
 * rather than taken from the macros that compute it, so that this is a check
 * of them.
 */
static void test_loop_counts(void)
{
	static const struct {
		unsigned int osr;
		unsigned int flags;
		unsigned int want;
		const char *what;
	} cases[] = {
		/* 256 * 4 = 1024, rounded up to 1026 */
		{ 4, 0, 1026, "256 bits per OSR, rounded up to a multiple of three" },
		/* (256 + 65) * 3 = 963 */
		{ 3, JENT_FORCE_FIPS, 963,
		  "and the safety factor on top in FIPS mode" },
	};
	size_t i;

	jent_ut_group("the measurements of one block");

	for (i = 0; i < JENT_ARRAY_SIZE(cases); i++) {
		struct rand_data *ec = nz_alloc(cases[i].osr, cases[i].flags,
						cases[i].what);

		if (!ec)
			continue;

		jent_random_data(ec);
		JENT_UT_EQ(ec->rct_mem_nosr, cases[i].want, cases[i].what);
		jent_entropy_collector_free(ec);
	}
}

/*
 * A stuck measurement carries no entropy and is taken again: the block still
 * holds as many measurements that are not stuck as its loop count says.
 * Without memory access, one clock reading per measurement on the common
 * path, plus the priming reading ahead of the block - the pseudorandom memory
 * access reads the clock another 16 times per measurement to seed its address
 * generator, which would bury a stuck step in a sum of 17.
 */
static void test_stuck_measurements_retried(void)
{
	struct rand_data *ec = nz_alloc(0, JENT_DISABLE_MEMORY_ACCESS,
					"stuck measurements are retried");
	unsigned long before, readings;

	jent_ut_group("stuck measurements are taken again");

	if (!ec)
		return;

	nz.stuck_every = 4;
	before = nz.served;
	jent_random_data(ec);
	readings = nz.served - before;
	nz_clock_plain();

	printf("  note: %lu readings for a loop count of %u\n", readings,
	       ec->rct_mem_nosr);

	/* Every fourth measurement is stuck: a quarter more of them. */
	JENT_UT_TRUE(readings > 1 + ec->rct_mem_nosr + ec->rct_mem_nosr / 5,
		     "a block on a clock with stuck readings takes more of them");

	jent_entropy_collector_free(ec);
}

/*
 * NTG.1 starts on two noise sources, each sampled on its own for a full loop
 * count: the memory access first, then the hash loop. Both stages measure with
 * two readings each, so a startup that skipped one would take half of them.
 */
static void test_ntg1_startup_stages(void)
{
	struct rand_data *ec = nz_alloc(0, JENT_NTG1, "the NTG.1 startup stages");
	struct rand_data *fresh;
	unsigned long before, readings;

	jent_ut_group("the NTG.1 startup stages");

	if (!ec)
		return;

	/* Where a new NTG.1 collector starts, before its startup runs. */
	fresh = jent_entropy_collector_alloc_internal(0, JENT_NTG1 | NZ_FLAGS);
	if (fresh) {
		JENT_UT_EQ(fresh->startup_state, jent_startup_memory,
			   "an NTG.1 collector starts with the memory stage");
		jent_entropy_collector_free(fresh);
	} else {
		JENT_UT_FAIL("%s", "no NTG.1 collector without its startup");
	}

	ec->startup_state = jent_startup_memory;
	before = nz.served;
	jent_random_data(ec);
	readings = nz.served - before;

	JENT_UT_EQ(ec->startup_state, jent_startup_completed,
		   "the stages end in the completed state");
	JENT_UT_EQ(ec->health_failure, 0, "without a health test failure");
	printf("  note: %lu readings for a loop count of %u\n", readings,
	       ec->rct_mem_nosr);
	JENT_UT_TRUE(readings >= 4UL * ec->rct_mem_nosr,
		     "both stages took a full loop count of measurements");

	jent_entropy_collector_free(ec);
}

/*
 * Each stage's measurements go into the pool: sampling a source without
 * conditioning it would leave the startup with the entropy of one.
 */
static void test_ntg1_stages_feed_the_pool(void)
{
	static const struct {
		unsigned int (*measure)(struct rand_data *, uint64_t,
					uint64_t *, int);
		const char *what;
	} stages[] = {
		{ jent_measure_jitter_ntg1_memaccess,
		  "the memory access stage feeds the pool" },
		{ jent_measure_jitter_ntg1_sha3,
		  "the hash loop stage feeds the pool" },
	};
	struct rand_data *ec = nz_alloc(0, JENT_NTG1, "the NTG.1 stages' pool");
	size_t i;

	jent_ut_group("the NTG.1 stages feed the pool");

	if (!ec)
		return;

	for (i = 0; i < JENT_ARRAY_SIZE(stages); i++) {
		struct jent_sha_ctx before;

		memcpy(&before, &ec->hash_state, sizeof(before));
		jent_random_data_one(ec, stages[i].measure);
		JENT_UT_TRUE(memcmp(&before, &ec->hash_state,
				    sizeof(before)) != 0, stages[i].what);
		jent_memset_secure(&before, sizeof(before));
	}

	jent_entropy_collector_free(ec);
}

/*
 * A read that fails in its second block hands back nothing: the first block
 * was already copied into the caller's buffer and is wiped again. The failure
 * is set from the clock half way through the second block, as a health test
 * would set it.
 */
static void test_failed_read_outputs_nothing(void)
{
	struct rand_data *ec = nz_alloc(3, JENT_FORCE_FIPS,
					"a failed read outputs nothing");
	unsigned char buf[64];
	unsigned int nonzero = 0;
	size_t i;
	ssize_t ret;

	jent_ut_group("a read that fails part-way outputs nothing");

	if (!ec)
		return;

	/*
	 * Half a block into the second one, measured on a block of its own
	 * rather than computed: stuck readings make a block take more.
	 */
	{
		unsigned long before = nz.served;

		if (jent_read_entropy(ec, (char *)buf, 32) != 32) {
			JENT_UT_FAIL("%s", "no block to measure");
			jent_entropy_collector_free(ec);
			return;
		}
		nz.trip_at = nz.served + (nz.served - before) * 3 / 2;
	}
	nz.trip_ec = ec;
	nz.trip_bit = JENT_APT_FAILURE_PERMANENT;

	memset(buf, 0xa5, sizeof(buf));
	ret = jent_read_entropy(ec, (char *)buf, sizeof(buf));
	nz_clock_plain();

	JENT_UT_EQ(ret, JENT_ERR_APT_PERMANENT,
		   "the failure in the second block is returned");
	for (i = 0; i < 32; i++) {
		if (buf[i])
			nonzero++;
	}
	JENT_UT_EQ(nonzero, 0, "the first block was wiped from the buffer");
	for (i = 32; i < sizeof(buf); i++) {
		if (buf[i] != 0xa5)
			nonzero++;
	}
	JENT_UT_EQ(nonzero, 0, "and nothing was written past it");

	jent_entropy_collector_free(ec);
}

int main(void)
{
	jent_ut_setup();

	nz.t = 1;
	nz.lcg = 7;
	if (jent_set_mock_timer(nz_clock_cb, &nz)) {
		JENT_UT_SKIP("the noise source", "the clock cannot be mocked");
		return jent_ut_report("unit-noise");
	}

	test_loop_counts();
	test_stuck_measurements_retried();
	test_ntg1_startup_stages();
	test_ntg1_stages_feed_the_pool();
	test_failed_read_outputs_nothing();

	jent_set_mock_timer(NULL, NULL);

	return jent_ut_report("unit-noise");
}
