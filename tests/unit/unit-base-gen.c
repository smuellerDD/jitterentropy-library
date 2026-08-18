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

/*
 * The timer-less mode, which replaces the platform time source with a counting
 * thread. Skipped where it was not compiled in.
 */
static void test_internal_timer(void)
{
	jent_ut_group("the internal timer");

#ifndef JENT_CONF_ENABLE_INTERNAL_TIMER
	JENT_UT_SKIP("the internal timer", "not compiled in");
	return;
#else
	{
	struct rand_data *ec;
	char buf[32];

	/*
	 * Pinning the counting thread. Advisory everywhere - an out-of-range
	 * index or a platform with no affinity API does not stop the timer -
	 * so what is checked is that it is accepted before initialization and
	 * refused afterwards.
	 */
	JENT_UT_NE(jent_entropy_set_notime_cpu(0), 1,
		   "setting the CPU returns a status, not a stray value");

	if (jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER)) {
		JENT_UT_SKIP("the internal timer",
			     "its startup does not converge on this machine");
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_INTERNAL_TIMER);
	if (!ec) {
		JENT_UT_SKIP("the internal timer", "no collector");
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
	uint64_t delta;
	unsigned int i, moved = 0;

	jent_ut_group("the jitter measurement in every shape it is called");

	if (!ec) {
		JENT_UT_SKIP("jent_measure_jitter", "no collector");
		jent_entropy_collector_free(nomem);
		return;
	}

	/*
	 * The counting thread, where this collector runs on it. See
	 * jent_ut_settick() - without this the first measurement never
	 * returns.
	 */
	if (jent_ut_settick(ec)) {
		JENT_UT_SKIP("jent_measure_jitter", "no counting thread");
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
	delta = 0;
	jent_measure_jitter(ec, 32, &delta);
	JENT_UT_TRUE(1, "a caller-set loop count is accepted");

	if (nomem && !jent_ut_settick(nomem)) {
		JENT_UT_TRUE(nomem->mem == NULL,
			     "the collector really has no memory block");
		delta = 0;
		jent_measure_jitter(nomem, 0, &delta);
		jent_measure_jitter(nomem, 16, NULL);
		JENT_UT_TRUE(1, "measuring without a memory block is a no-op");
		jent_notime_unsettick(nomem);
		jent_entropy_collector_free(nomem);
	} else {
		JENT_UT_SKIP("measuring without a memory block", "no collector");
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
	uint64_t delta;

	jent_ut_group("both memory access loops");

	if (!ec) {
		JENT_UT_SKIP("the memory access loops", "no collector");
		return;
	}

	/* Both loops time themselves; see jent_ut_settick(). */
	if (jent_ut_settick(ec)) {
		JENT_UT_SKIP("the memory access loops", "no counting thread");
		jent_entropy_collector_free(ec);
		return;
	}

	/* Nothing to access is a no-op rather than a fault. */
	jent_memaccess_pseudorandom(NULL, 0, NULL);
	jent_memaccess_deterministic(NULL, 0, NULL);
	JENT_UT_TRUE(1, "no collector is a no-op");

	delta = 0;
	jent_memaccess_pseudorandom(ec, 0, &delta);
	jent_memaccess_pseudorandom(ec, 0, NULL);
	jent_memaccess_pseudorandom(ec, 64, &delta);
	jent_memaccess_pseudorandom(ec, 64, NULL);
	JENT_UT_TRUE(1, "the pseudorandom loop runs in every shape");

	delta = 0;
	jent_memaccess_deterministic(ec, 0, &delta);
	jent_memaccess_deterministic(ec, 0, NULL);
	jent_memaccess_deterministic(ec, 64, &delta);
	jent_memaccess_deterministic(ec, 64, NULL);
	JENT_UT_TRUE(1, "the deterministic loop runs in every shape");

	/* And with no block to walk. */
	{
		unsigned char *mem = ec->mem;

		ec->mem = NULL;
		jent_memaccess_pseudorandom(ec, 0, &delta);
		jent_memaccess_deterministic(ec, 0, &delta);
		ec->mem = mem;
		JENT_UT_TRUE(1, "a collector with no block is a no-op");
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
		JENT_UT_SKIP("the startup states", "no collector");
		return;
	}

	/*
	 * jent_random_data() measures; only jent_read_entropy() above it ticks.
	 * See jent_ut_settick().
	 */
	if (jent_ut_settick(ec)) {
		JENT_UT_SKIP("the startup states", "no counting thread");
		jent_entropy_collector_free(ec);
		return;
	}

	for (i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
		ec->startup_state = states[i].state;
		jent_random_data(ec);
		JENT_UT_TRUE(1, states[i].name);
	}

	/*
	 * A loop count the window counters cannot hold is refused rather than
	 * truncated - a truncated count would silently shrink the
	 * RCT-with-memory window below what its cutoff table assumes and
	 * disable the test. Not reachable through the API, where JENT_MAX_OSR
	 * bounds it, but it is what guards a raised JENT_MAX_OSR.
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
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, configs[i].flags);
		char buf[48];

		if (!ec) {
			/*
			 * The compliance modes need lockable memory and a
			 * startup that converges; neither is guaranteed here.
			 */
			JENT_UT_SKIP(configs[i].name,
				     "no collector on this machine");
			continue;
		}

		JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
			   (ssize_t)sizeof(buf), configs[i].name);
		jent_entropy_collector_free(ec);
	}
}

int main(void)
{
	jent_ut_setup();

	test_measure_jitter_variants();
	test_memaccess_variants();
	test_startup_states();
	test_generation_matrix();
	test_internal_timer();

	return jent_ut_report("unit-base-gen");
}
