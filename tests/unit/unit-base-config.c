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
 * This file covers the configuration a collector derives from its flags.
 */

/*
 * Every JENT_MAX_MEMSIZE_* flag decodes to the size its name states. The flags
 * are a bit field the caller composes, and a shift that is off by one would
 * silently hand out a block of the wrong size rather than fail.
 */
static void test_memsize(void)
{
	static const struct {
		unsigned int flag;
		uint32_t size;
		const char *name;
	} sizes[] = {
		{ JENT_MAX_MEMSIZE_1kB,		1024,		"1kB" },
		{ JENT_MAX_MEMSIZE_2kB,		2048,		"2kB" },
		{ JENT_MAX_MEMSIZE_4kB,		4096,		"4kB" },
		{ JENT_MAX_MEMSIZE_8kB,		8192,		"8kB" },
		{ JENT_MAX_MEMSIZE_16kB,	16384,		"16kB" },
		{ JENT_MAX_MEMSIZE_32kB,	32768,		"32kB" },
		{ JENT_MAX_MEMSIZE_64kB,	65536,		"64kB" },
		{ JENT_MAX_MEMSIZE_128kB,	131072,		"128kB" },
		{ JENT_MAX_MEMSIZE_256kB,	262144,		"256kB" },
		{ JENT_MAX_MEMSIZE_512kB,	524288,		"512kB" },
		{ JENT_MAX_MEMSIZE_1MB,		1048576,	"1MB" },
		{ JENT_MAX_MEMSIZE_2MB,		2097152,	"2MB" },
		{ JENT_MAX_MEMSIZE_4MB,		4194304,	"4MB" },
		{ JENT_MAX_MEMSIZE_8MB,		8388608,	"8MB" },
		{ JENT_MAX_MEMSIZE_16MB,	16777216,	"16MB" },
		{ JENT_MAX_MEMSIZE_32MB,	33554432,	"32MB" },
		{ JENT_MAX_MEMSIZE_64MB,	67108864,	"64MB" },
		{ JENT_MAX_MEMSIZE_128MB,	134217728,	"128MB" },
		{ JENT_MAX_MEMSIZE_256MB,	268435456,	"256MB" },
		{ JENT_MAX_MEMSIZE_512MB,	536870912,	"512MB" },
	};
	size_t i;

	jent_ut_group("jent_memsize decodes every JENT_MAX_MEMSIZE_* flag");

	for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		JENT_UT_EQ(jent_memsize(sizes[i].flag), sizes[i].size,
			   sizes[i].name);

		/*
		 * The size field must be read out of the flags word without
		 * being disturbed by the option bits sharing it.
		 */
		JENT_UT_EQ(jent_memsize(sizes[i].flag | JENT_FORCE_FIPS |
					JENT_DISABLE_MEMORY_ACCESS),
			   sizes[i].size,
			   "the size is independent of the other flags");
	}

	/*
	 * A size field larger than the table can reach: clamped rather than
	 * shifted out of the uint32_t it is computed in. This is reachable
	 * from jent_status() on a collector allocated with
	 * JENT_DISABLE_MEMORY_ACCESS, whose flags are never normalized.
	 */
	JENT_UT_EQ(jent_memsize(JENT_MAX_MEMSIZE_MASK),
		   jent_memsize(JENT_MAX_MEMSIZE_MAX),
		   "an out-of-range size field is clamped to the maximum");
}

/* Likewise for the hash loop count field. */

static void test_hashloop(void)
{
	static const struct {
		unsigned int flag;
		unsigned int cnt;
	} loops[] = {
		{ JENT_HASHLOOP_1,	1 },
		{ JENT_HASHLOOP_2,	2 },
		{ JENT_HASHLOOP_4,	4 },
		{ JENT_HASHLOOP_8,	8 },
		{ JENT_HASHLOOP_16,	16 },
		{ JENT_HASHLOOP_32,	32 },
		{ JENT_HASHLOOP_64,	64 },
		{ JENT_HASHLOOP_128,	128 },
	};
	size_t i;
	char what[64];

	jent_ut_group("jent_hashloop_cnt decodes every JENT_HASHLOOP_* flag");

	for (i = 0; i < sizeof(loops) / sizeof(loops[0]); i++) {
		snprintf(what, sizeof(what), "JENT_HASHLOOP_%u", loops[i].cnt);
		JENT_UT_EQ(jent_hashloop_cnt(loops[i].flag), loops[i].cnt, what);
		JENT_UT_EQ(jent_hashloop_cnt(loops[i].flag | JENT_NTG1 |
					     JENT_MAX_MEMSIZE_1MB),
			   loops[i].cnt,
			   "the count is independent of the other flags");
	}
}

/* The oversampling rate is clamped into [JENT_MIN_OSR, JENT_MAX_OSR]. */

static void test_osr(void)
{
	jent_ut_group("the oversampling rate is clamped");

	JENT_UT_EQ(ensure_osr_is_at_least_minimal(0), JENT_MIN_OSR, "0 becomes the minimum");
	JENT_UT_EQ(ensure_osr_is_at_least_minimal(1), JENT_MIN_OSR, "1 becomes the minimum");
	JENT_UT_EQ(ensure_osr_is_at_least_minimal(JENT_MIN_OSR), JENT_MIN_OSR,
		   "the minimum is kept");
	JENT_UT_EQ(ensure_osr_is_at_least_minimal(JENT_MIN_OSR + 1), JENT_MIN_OSR + 1,
		   "a value above the minimum is kept");
}

static void test_version(void)
{
	jent_ut_group("jent_version");

	JENT_UT_EQ(jent_version(), JENT_VERSION,
		   "the reported version is the one jitterentropy.h defines");
}

int main(void)
{
	test_memsize();
	test_hashloop();
	test_osr();
	test_version();

	return jent_ut_report("unit-base-config");
}
