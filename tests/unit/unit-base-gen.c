/*
 * Jitter RNG: unit tests for src/jitterentropy-base.c and
 * src/jitterentropy-status.c
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
 * The whole library is absorbed here rather than linked, as in the AMALGAMATED
 * programs under tests/raw-entropy: the flag decoding
 * (jent_memsize(), jent_hashloop_cnt()) is internal to the library and a
 * shared build does not export it.
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
 * This file covers the noise collection and startup paths.
 */

/*
 * Start the counting thread of a collector that uses the internal timer, as
 * jent_read_entropy() does around every generation.
 *
 * The tests below drive the noise source functions directly, and on a machine
 * whose platform timer is too coarse the startup self test puts every
 * collector on the internal timer. jent_get_nstime_internal() then spins in
 * jent_yield() until the counting thread ticks, so a direct call with no
 * thread running never returns - as on Windows, whose
 * QueryPerformanceCounter() is far too coarse for a jitter measurement.
 *
 * Returns 0 when the collector may be measured with, as jent_notime_settick()
 * does. Every caller pairs this with jent_notime_unsettick(), whose stop is
 * what lets the collector be freed.
 */
static int jent_ut_settick(struct rand_data *ec)
{
	return ec ? jent_notime_settick(ec) : 0;
}

/* What no delta computed from two time stamps can be. */
#define JENT_UT_DELTA_UNSET	UINT64_MAX

/*
 * The byte sum of the memory block, modulo 256. Every memory access adds one
 * to one byte and wraps at 255, so it grows by exactly the accesses made.
 */
static unsigned int jent_ut_memsum(const struct rand_data *ec)
{
	uint64_t i;
	unsigned int sum = 0;

	for (i = 0; i <= ec->memmask; i++)
		sum += ec->mem[i];

	return sum & 0xff;
}

/* The accesses made since @before was taken, modulo 256. */
static unsigned int jent_ut_memaccesses(const struct rand_data *ec,
					unsigned int before)
{
	return (jent_ut_memsum(ec) - before) & 0xff;
}

/*
 * The configuration of the counting thread, while the window for it is still
 * open. Run before every other test: any collector allocation runs the startup,
 * which closes it, and a check made afterwards can only see the refusal.
 */
static void test_notime_config_window(void)
{
	jent_ut_group("the internal timer configuration window");

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	JENT_UT_EQ(jent_atomic_load_int(&jent_notime_switch_blocked), 0,
		   "nothing has closed the window yet");
	/*
	 * A CPU no machine has: accepted as any other, and pinning to it
	 * cannot succeed - the pin is advisory - so it takes no CPU away from
	 * the later tests' counting threads the way pinning them all to CPU 0
	 * would.
	 */
	JENT_UT_EQ(jent_entropy_set_notime_cpu(~0UL), 0,
		   "pinning the counting thread is accepted before "
		   "initialization");
#else
	JENT_UT_SKIP("the configuration window", "no internal timer");
#endif
}

/*
 * The timer-less mode, which replaces the platform time source with a counting
 * thread. Skipped where it was not compiled in.
 */
static void test_internal_timer(void)
{
	jent_ut_group("the internal timer");

#ifndef JENT_CONF_ENABLE_INTERNAL_TIMER
	/* A negative errno, as documented, not a bare -1. */
	JENT_UT_EQ(jent_entropy_set_notime_cpu(0), -EOPNOTSUPP,
		   "pinning a thread that is not compiled in is refused");
	JENT_UT_EQ(jent_entropy_switch_notime_impl(NULL), -EOPNOTSUPP,
		   "and so is replacing it");
	JENT_UT_SKIP("the internal timer", "not compiled in");
	return;
#else
	{
	struct rand_data *ec;
	char buf[32];
	int ret;

	/*
	 * Pinning the counting thread is advisory everywhere - an out-of-range
	 * index or a platform with no affinity API does not stop the timer -
	 * so what is checked is that it is accepted before initialization
	 * (test_notime_config_window(), which has to run first) and refused
	 * afterwards (below).
	 */
	ret = jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER);
	if (ret) {
		JENT_UT_NO_STARTUP("the internal timer", ret);
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_INTERNAL_TIMER);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the internal timer",
				     JENT_FORCE_INTERNAL_TIMER);
		return;
	}

	JENT_UT_EQ(ec->enable_notime, 1, "the collector uses it");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   (ssize_t)sizeof(buf), "and produces entropy with it");

	jent_entropy_collector_free(ec);

	/* Switching the implementation is denied once initialized. */
	JENT_UT_EQ(jent_entropy_switch_notime_impl(NULL), -EAGAIN,
		   "switching the timer implementation is denied afterwards");
	JENT_UT_EQ(jent_entropy_set_notime_cpu(0), -EAGAIN,
		   "and so is moving its thread");
	}
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */
}

/* The backend capability query behind JENT_FORCE_SECURE_MEM. */

/*
 * The measurement the whole noise source is built on, driven directly across
 * the shapes its callers use: with and without a delta returned to the caller,
 * with the loop count left to the collector and forced by the caller, and on a
 * collector with no memory block at all. Ordinary generation only ever uses
 * one of these.
 */
static void test_measure_jitter_variants(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	struct rand_data *nomem =
		jent_entropy_collector_alloc(0, JENT_DISABLE_MEMORY_ACCESS);
	struct jent_sha_ctx pool;
	uint64_t delta;
	unsigned int i, moved = 0, sum;

	jent_ut_group("the jitter measurement in every shape it is called");

	if (!ec) {
		JENT_UT_NO_COLLECTOR("jent_measure_jitter", 0);
		jent_entropy_collector_free(nomem);
		return;
	}

	/*
	 * The counting thread, where this collector runs on it. See
	 * jent_ut_settick() - without this the first measurement never
	 * returns.
	 */
	if (jent_ut_settick(ec)) {
		JENT_UT_FAIL("%s: the collector's counting thread does not start",
			     "jent_measure_jitter");
		jent_entropy_collector_free(nomem);
		jent_entropy_collector_free(ec);
		return;
	}

	/* Prime ->prev_time, as every caller does before measuring. */
	jent_measure_jitter(ec, 0, NULL);

	/* No delta wanted: the measurement still has to happen. */
	for (i = 0; i < 16; i++)
		jent_measure_jitter(ec, 0, NULL);
	JENT_UT_TRUE(ec->prev_time != 0, "a measurement without a delta runs");

	/* Delta wanted. */
	for (i = 0; i < 64; i++) {
		delta = 0;
		jent_measure_jitter(ec, 0, &delta);
		if (delta)
			moved++;
	}
	JENT_UT_NE(moved, 0, "a measurement returns a delta that varies");

	/* A caller-supplied loop count, as the recording tools use. */
	sum = jent_ut_memsum(ec);
	pool = ec->hash_state;
	delta = JENT_UT_DELTA_UNSET;
	jent_measure_jitter(ec, 32, &delta);
	JENT_UT_EQ(jent_ut_memaccesses(ec, sum), 32,
		   "a caller-set loop count sets the memory accesses");
	JENT_UT_TRUE(memcmp(&pool, &ec->hash_state, sizeof(pool)) &&
		     delta != JENT_UT_DELTA_UNSET,
		     "and the measurement with it reaches the pool");

	if (nomem && !jent_ut_settick(nomem)) {
		JENT_UT_TRUE(nomem->mem == NULL,
			     "the collector really has no memory block");
		pool = nomem->hash_state;
		delta = JENT_UT_DELTA_UNSET;
		jent_measure_jitter(nomem, 0, &delta);
		JENT_UT_TRUE(memcmp(&pool, &nomem->hash_state,
				    sizeof(pool)) &&
			     delta != JENT_UT_DELTA_UNSET,
			     "the measurement runs without a memory block");
		pool = nomem->hash_state;
		jent_measure_jitter(nomem, 16, NULL);
		JENT_UT_TRUE(memcmp(&pool, &nomem->hash_state, sizeof(pool)),
			     "with a caller-set loop count as well");
		jent_notime_unsettick(nomem);
		jent_entropy_collector_free(nomem);
	} else if (!nomem) {
		JENT_UT_NO_COLLECTOR("measuring without a memory block",
				     JENT_DISABLE_MEMORY_ACCESS);
	} else {
		JENT_UT_SKIP("measuring without a memory block",
			     "no counting thread");
		jent_entropy_collector_free(nomem);
	}

	jent_notime_unsettick(ec);
	jent_entropy_collector_free(ec);
}

/*
 * The two memory access loops, driven directly. Which of them a measurement
 * uses is a compile-time choice, and each is called with and without a delta
 * returned and with the loop count left to the collector or forced - the
 * combinations its two callers do not both make.
 */
static void test_memaccess_variants(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	uint64_t delta, n;
	unsigned int sum, loc;

	jent_ut_group("both memory access loops");

	if (!ec) {
		JENT_UT_NO_COLLECTOR("the memory access loops", 0);
		return;
	}

	/* Both loops time themselves; see jent_ut_settick(). */
	if (jent_ut_settick(ec)) {
		JENT_UT_FAIL("%s: the collector's counting thread does not start",
			     "the memory access loops");
		jent_entropy_collector_free(ec);
		return;
	}

	/* Nothing to access is a no-op rather than a fault. */
	delta = JENT_UT_DELTA_UNSET;
	jent_memaccess_pseudorandom(NULL, 0, &delta);
	jent_memaccess_deterministic(NULL, 0, &delta);
	JENT_UT_TRUE(delta == JENT_UT_DELTA_UNSET, "no collector is a no-op");

	/* Every shape makes the accesses asked for; a delta only if asked. */
	sum = jent_ut_memsum(ec);
	delta = JENT_UT_DELTA_UNSET;
	jent_memaccess_pseudorandom(ec, 0, &delta);
	JENT_UT_TRUE(delta != JENT_UT_DELTA_UNSET,
		     "the pseudorandom loop returns a delta when asked");
	jent_memaccess_pseudorandom(ec, 0, NULL);
	JENT_UT_EQ(jent_ut_memaccesses(ec, sum),
		   (2 * ec->memaccessloops) & 0xff,
		   "the pseudorandom loop makes the collector's accesses");
	sum = jent_ut_memsum(ec);
	jent_memaccess_pseudorandom(ec, 64, &delta);
	jent_memaccess_pseudorandom(ec, 64, NULL);
	JENT_UT_EQ(jent_ut_memaccesses(ec, sum), 128,
		   "the pseudorandom loop makes the caller's accesses");

	/* The deterministic walk also advances by a known stride. */
	sum = jent_ut_memsum(ec);
	loc = ec->memlocation;
	n = 2 * (uint64_t)ec->memaccessloops + 128;
	delta = JENT_UT_DELTA_UNSET;
	jent_memaccess_deterministic(ec, 0, &delta);
	JENT_UT_TRUE(delta != JENT_UT_DELTA_UNSET,
		     "the deterministic loop returns a delta when asked");
	jent_memaccess_deterministic(ec, 0, NULL);
	jent_memaccess_deterministic(ec, 64, &delta);
	jent_memaccess_deterministic(ec, 64, NULL);
	JENT_UT_EQ(jent_ut_memaccesses(ec, sum), n & 0xff,
		   "the deterministic loop makes the accesses asked for");
	JENT_UT_EQ(ec->memlocation,
		   (loc + n * (JENT_MEMORY_BLOCKSIZE - 1)) %
		   ((uint64_t)ec->memmask + 1),
		   "the deterministic loop walks one stride per access");

	/* And with no block to walk: returns before touching anything. */
	{
		unsigned char *mem = ec->mem;

		loc = ec->memlocation;
		delta = JENT_UT_DELTA_UNSET;
		ec->mem = NULL;
		jent_memaccess_pseudorandom(ec, 0, &delta);
		jent_memaccess_deterministic(ec, 0, &delta);
		ec->mem = mem;
		JENT_UT_TRUE(delta == JENT_UT_DELTA_UNSET &&
			     ec->memlocation == loc,
			     "a collector with no block is a no-op");
	}

	jent_notime_unsettick(ec);
	jent_entropy_collector_free(ec);
}

/*
 * The NTG.1 startup state machine. It samples the memory access and the hash
 * as two separate noise sources before releasing anything, so its three states
 * are only walked once per collector - and only in NTG.1 mode.
 */
static void test_startup_states(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	static const struct {
		enum jent_startup_state state;
		const char *name;
	} states[] = {
		{ jent_startup_memory,		"the memory sampling stage" },
		{ jent_startup_sha3,		"the hash sampling stage" },
		{ jent_startup_completed,	"the completed state" },
	};
	size_t i;

	jent_ut_group("the NTG.1 startup states");

	if (!ec) {
		JENT_UT_NO_COLLECTOR("the startup states", 0);
		return;
	}

	/*
	 * jent_random_data() measures; only jent_read_entropy() above it ticks.
	 * See jent_ut_settick().
	 */
	if (jent_ut_settick(ec)) {
		JENT_UT_FAIL("%s: the collector's counting thread does not start",
			     "the startup states");
		jent_entropy_collector_free(ec);
		return;
	}

	/*
	 * Each stage names its successor, and the memory stage falls through
	 * the hash one, so a single call from any state ends in the completed
	 * one. Asserted rather than merely reached: this used to be
	 * JENT_UT_TRUE(1, ...), which held whatever the state machine did with
	 * the state - including leaving it outside the enum.
	 */
	for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		ec->startup_state = states[i].state;
		jent_random_data(ec);
		JENT_UT_EQ(ec->startup_state, jent_startup_completed,
			   states[i].name);
	}

	/*
	 * A loop count the window counters cannot hold is refused rather than
	 * truncated - a truncated count would silently shrink the
	 * RCT-with-memory window below what its cutoff table assumes and
	 * disable the test. Not reachable through the API, where JENT_MAX_OSR
	 * bounds it; this is the arithmetic behind the build assertion that
	 * caps JENT_MAX_OSR.
	 */
	ec->startup_state = jent_startup_completed;
	ec->osr = 60000;
	ec->health_failure = 0;
	jent_random_data(ec);
	JENT_UT_TRUE((ec->health_failure & JENT_RCT_MEM_FAILURE_PERMANENT) != 0,
		     "an oversampling rate that overflows the window is refused");

	/*
	 * And one too small to cover a single output block, which would leave
	 * the window shorter than the data it is supposed to describe.
	 */
	ec->startup_state = jent_startup_completed;
	ec->osr = 0;
	ec->health_failure = 0;
	jent_random_data(ec);
	JENT_UT_TRUE((ec->health_failure & JENT_RCT_MEM_FAILURE_PERMANENT) != 0,
		     "and one too small to cover an output block");

	ec->osr = JENT_MIN_OSR;
	ec->health_failure = 0;
	jent_notime_unsettick(ec);
	jent_entropy_collector_free(ec);
}

/*
 * The error codes a read returns for what the health tests saw. They report
 * in the compliance modes only, and what they report on is the noise source
 * of the machine the test runs on. JENT_ERR_EINVAL, JENT_ERR_NOTIME and
 * JENT_ERR_SELFTEST are deliberately not among them: those are the library
 * failing to do its job, on any machine.
 */
static int jent_ut_health_error(ssize_t ret)
{
	switch (ret) {
	case JENT_ERR_RCT:
	case JENT_ERR_APT:
	case JENT_ERR_LAG:
	case JENT_ERR_RCT_MEM:
	case JENT_ERR_RCT_PERMANENT:
	case JENT_ERR_APT_PERMANENT:
	case JENT_ERR_LAG_PERMANENT:
	case JENT_ERR_RCT_MEM_PERMANENT:
		return 1;
	default:
		return 0;
	}
}

/*
 * Generation across the configurations that change how a block is produced:
 * the hash loop count, whether all caches size the memory block, and the NTG.1
 * startup sequence, which samples the memory access and the hash as two
 * separate noise sources before releasing anything.
 */
static void test_generation_matrix(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
	} configs[] = {
		{ JENT_HASHLOOP_1,			"one hash loop" },
		{ JENT_HASHLOOP_8,			"eight hash loops" },
		{ JENT_HASHLOOP_128,			"the maximum hash loops" },
		{ JENT_CACHE_ALL,			"sized by all caches" },
		{ JENT_DISABLE_MEMORY_ACCESS,		"no memory access" },
		{ JENT_MAX_MEMSIZE_1kB,			"the smallest memory block" },
		{ JENT_FORCE_FIPS | JENT_HASHLOOP_4,	"FIPS with four hash loops" },
		{ JENT_NTG1,				"the NTG.1 startup" },
	};
	size_t i;

	jent_ut_group("generation across the configurations");

	for (i = 0; i < sizeof(configs) / sizeof(configs[0]); i++) {
		unsigned int compliance =
			(configs[i].flags & (JENT_FORCE_FIPS | JENT_NTG1)) ||
			jent_fips_enabled();
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, configs[i].flags);
		char buf[48];
		ssize_t ret;

		if (!ec) {
			/*
			 * The compliance modes need lockable memory and a
			 * startup that converges; neither is guaranteed here.
			 * The other configurations need neither.
			 */
			JENT_UT_NO_COLLECTOR(configs[i].name,
					     configs[i].flags);
			continue;
		}

		ret = jent_read_entropy(ec, buf, sizeof(buf));

		/*
		 * The same reasoning that gives the generation runs of
		 * jitterentropy-rng the "unreliable" label in the top-level
		 * CMakeLists.txt, and that skips the allocation above: a
		 * compliance mode runs the health tests over the noise source
		 * this machine has, and a loaded or shared one repeats a
		 * delta often enough to reach a cutoff within a window. That
		 * is a property of the machine, not a defect here. The other
		 * configurations do not report a health test at all, so a
		 * failure in one of those is the defect this looks for and is
		 * never skipped.
		 */
		if (compliance && jent_ut_health_error(ret)) {
			char why[80];

			snprintf(why, sizeof(why),
				 "the health tests returned %zd for this machine's noise source",
				 ret);
			JENT_UT_SKIP(configs[i].name, why);
			jent_entropy_collector_free(ec);
			continue;
		}

		JENT_UT_EQ(ret, (ssize_t)sizeof(buf), configs[i].name);
		jent_entropy_collector_free(ec);
	}
}

int main(void)
{
	jent_ut_setup();

	/* First: see the comment above it. */
	test_notime_config_window();
	test_measure_jitter_variants();
	test_memaccess_variants();
	test_startup_states();
	test_generation_matrix();
	test_internal_timer();

	return jent_ut_report("unit-base-gen");
}
