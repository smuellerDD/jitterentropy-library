/*
 * Jitter RNG: unit tests for the platform backends in arch/
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
 * Jitter RNG: unit tests for the cache size discovery backend.
 *
 * Every assertion has to hold on every platform: what is checked is the
 * contract the backend header in arch/ states, not the behaviour of one
 * implementation.
 */

/*
 * As in the AMALGAMATED programs under tests/raw-entropy: several arch sources
 * are absorbed here, and the ones needing _GNU_SOURCE define it themselves
 * before their own includes - which is too late once an earlier source in this
 * translation unit has already pulled the headers in. Stated once up front.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef _MSC_VER
# include <dirent.h>
# include <unistd.h>
#endif

/*
 * The root of the sysfs cache walk, a variable here so that
 * test_cache_sysconf_fallback() can point it at nothing and reach the
 * fallback behind it. The production build keeps the literal in the source.
 *
 * Guarded, or every non-Linux target would carry an unused static: the backend
 * that reads the macro is selected on __linux__ too.
 */
#ifdef __linux__
static const char *jent_test_sysfs_root = "/sys/devices/system/cpu";
# define JENT_SYSFS_CPU_DIR jent_test_sysfs_root
#endif

/* And /proc/cpuinfo, the last resort behind sysconf on Arm, for the same. */
#if defined(__linux__) && (defined(__aarch64__) || defined(__arm__))
static const char *jent_test_cpuinfo = "/proc/cpuinfo";
# define JENT_PROC_CPUINFO jent_test_cpuinfo
/* The backend undefines the macro after use; this one is the tests' own. */
# define JENT_UT_CPUINFO
#endif

/*
 * And CPUID, the last resort behind sysconf on x86, which is replaced rather
 * than pointed elsewhere: a null pointer asks the real instruction.
 */
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__)) && \
    (defined(__GNUC__) || defined(__clang__))
static int (*jent_test_cpuid)(unsigned int leaf, unsigned int subleaf,
			      unsigned int *eax, unsigned int *ebx,
			      unsigned int *ecx, unsigned int *edx);
# define JENT_CACHE_CPUID_COUNT \
	(jent_test_cpuid ? jent_test_cpuid : jent_cpuid_count_user)
# define JENT_UT_CPUID
#endif

/* The AArch64 cache ID register decoder of the kernel backend, see there. */
#define JENT_UT_CACHE_ARM64

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * The cache size the memory block is derived from. jent_cache_size_roundup()
 * returns 0 when nothing can be discovered, in which case the caller falls
 * back to its own default.
 */
/*
 * Whether the backend has to have an answer. macOS always has one: every Mac
 * reports hw.l1dcachesize. Windows has one wherever the operating system
 * describes a cache at all - which it need not: a Windows on Arm guest on a
 * QEMU virt machine whose firmware tables list no caches gets no RelationCache
 * record. So the test asks first, with the size query alone: a non-zero size
 * means records exist, and parsing them stays the backend's job - the one the
 * check below holds it to. CPUID on x86 - the BSD backend, and the
 * last resort of the Linux one - does where the CPU describes its caches in
 * leaf 4 or 0x8000001D at all; a hypervisor's CPU model may leave both empty
 * (QEMU's default AMD-flavoured model does, which is what the DragonFly BSD CI
 * guest runs on), and the backend then rightly reports nothing. Everywhere
 * else, nothing may legitimately be the answer: an Arm core the table does not
 * list in a build sandbox without /sys, or a libc without the _SC_LEVEL* names
 * on a target with nothing behind them.
 */
static int ut_cache_must_answer(void)
{
#if defined(JENT_ARCH_CACHE_WINDOWS)
	DWORD len = 0;

	if (!GetLogicalProcessorInformationEx(RelationCache, NULL, &len) &&
	    GetLastError() == ERROR_INSUFFICIENT_BUFFER && len > 0)
		return 1;
	printf("  note: Windows reports no cache relationship record\n");
	return 0;
#elif defined(JENT_ARCH_CACHE_APPLE)
	return 1;
#elif defined(JENT_ARCH_CACHE_CPUID) || defined(JENT_ARCH_CACHE_LINUX_CPUID)
	static const unsigned int leaves[] = {
		JENT_CPUID_LEAF_CACHE, JENT_CPUID_LEAF_CACHE_EXT
	};
	unsigned int eax, ebx, ecx, edx;
	size_t i;

	/* Subleaf 0 names the first cache, or type 0 for none. */
	for (i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
		if (jent_cpuid_count_user(leaves[i], 0, &eax, &ebx, &ecx,
					  &edx) && (eax & 0x1F))
			return 1;
	}
	printf("  note: the CPU describes no cache in CPUID leaf 4 or "
	       "0x8000001D\n");
	return 0;
#else
	return 0;
#endif
}

/*
 * What a data cache of real hardware can be: the Arm backend's floor below
 * which a size is a hypervisor's placeholder, and 4 GiB, far beyond the
 * largest single cache shipping (the L3 of a stacked-cache CCD, 96 MiB).
 */
#define UT_CACHE_MIN	4096
#define UT_CACHE_MAX	(UINT64_C(1) << 32)

static int cache_plausible(uint64_t size)
{
	return size == 0 || (size >= UT_CACHE_MIN && size <= UT_CACHE_MAX);
}

static void test_cache(void)
{
	uint64_t one = jent_cache_size_roundup(0);
	uint64_t all = jent_cache_size_roundup(1);
	uint64_t l1 = UINT64_MAX, l2 = UINT64_MAX, l3 = UINT64_MAX;

	jent_ut_group("jent_cache_size_roundup");

	/*
	 * The backend itself first. The round-up below makes a power of two of
	 * whatever it is fed, so it cannot show that the sizes are sizes: a
	 * backend reporting 64 bytes, or garbage, passes it all the same.
	 */
	jent_get_cachesize_uncached(&l1, &l2, &l3);
	printf("  note: backend reports L1 %llu, L2 %llu, L3 %llu bytes\n",
	       (unsigned long long)l1, (unsigned long long)l2,
	       (unsigned long long)l3);
	JENT_UT_TRUE(cache_plausible(l1), "the L1 size is a plausible cache");
	JENT_UT_TRUE(cache_plausible(l2), "the L2 size is a plausible cache");
	JENT_UT_TRUE(cache_plausible(l3), "the L3 size is a plausible cache");
	JENT_UT_TRUE(l2 == 0 || l2 >= l1, "an L2 is no smaller than the L1");

	printf("  note: single cache %llu bytes, all caches %llu bytes\n",
	       (unsigned long long)one, (unsigned long long)all);

	if (ut_cache_must_answer()) {
		JENT_UT_NE(l1, 0, "this backend knows the L1 data cache here");
		JENT_UT_NE(one, 0, "so there is a single cache size");
		JENT_UT_NE(all, 0, "and a total one");
	} else if (!one && !all) {
		JENT_UT_SKIP("jent_cache_size_roundup",
			     "no cache size is discoverable here");
		return;
	}

	/*
	 * What the memoised answer makes of the backend's: exactly the
	 * combiner's result for the same sizes, which test_cache_roundup()
	 * checks against known values.
	 */
	JENT_UT_EQ(one, jent_cache_roundup_from_sizes(l1, l2, l3, 0),
		   "the single cache size is the L1, rounded up");
	JENT_UT_EQ(all, jent_cache_roundup_from_sizes(l1, l2, l3, 1),
		   "the total is all levels, rounded up");
	if (l1)
		JENT_UT_TRUE(one > l1, "and exceeds the L1 it is derived from");
	if (one && all)
		JENT_UT_TRUE(all >= one,
			     "all caches together are at least one cache");
}

/*
 * The AArch64 cache ID register decoder of the Linux kernel backend, fed
 * register values. Compiled here through JENT_UT_CACHE_ARM64 on every
 * platform, since it is pure.
 *
 * CCSIDR_EL1 without FEAT_CCIDX: LineSize bits [2:0] (log2(bytes) - 4),
 * Associativity - 1 bits [12:3], NumSets - 1 bits [27:13]. With it:
 * Associativity - 1 bits [23:3], NumSets - 1 bits [55:32].
 */
#define CCSIDR(line, ways, sets) \
	((uint64_t)(line) | ((uint64_t)((ways) - 1) << 3) | \
	 ((uint64_t)((sets) - 1) << 13))
#define CCSIDR_CCIDX(line, ways, sets) \
	((uint64_t)(line) | ((uint64_t)((ways) - 1) << 3) | \
	 ((uint64_t)((sets) - 1) << 32))

/* CLIDR_EL1: L1 separate I and D (3), L2 unified (4), L3 unified (4). */
#define CLIDR_L1_L2_L3	((uint64_t)3 | ((uint64_t)4 << 3) | ((uint64_t)4 << 6))

/* 32 KiB L1 D, 1 MiB L2, 8 MiB L3, all with 64-byte lines. */
static uint64_t ccsidr_real(unsigned int level)
{
	switch (level) {
	case 1: return CCSIDR(2, 4, 128);
	case 2: return CCSIDR(2, 16, 1024);
	default: return CCSIDR(2, 16, 8192);
	}
}

static uint64_t ccsidr_real_ccidx(unsigned int level)
{
	switch (level) {
	case 1: return CCSIDR_CCIDX(2, 4, 128);
	case 2: return CCSIDR_CCIDX(2, 16, 1024);
	default: return CCSIDR_CCIDX(2, 16, 8192);
	}
}

/* What a KVM guest reads from Linux 6.3 on: one set, one way, per level. */
static uint64_t ccsidr_kvm(unsigned int level)
{
	(void)level;
	return CCSIDR(2, 1, 1);
}

/* A real L1 behind placeholders for the outer levels. */
static uint64_t ccsidr_l1_only(unsigned int level)
{
	return level == 1 ? CCSIDR(2, 4, 256) : CCSIDR(2, 1, 1);
}

static void test_cache_arm64(void)
{
	uint64_t l1, l2, l3;

	jent_ut_group("the AArch64 cache ID register decoder");

	jent_cache_sizes_arm64(CLIDR_L1_L2_L3, 0, ccsidr_real, &l1, &l2, &l3);
	JENT_UT_EQ(l1, 32768, "a 32 KiB L1 data cache is decoded");
	JENT_UT_EQ(l2, 1048576, "a 1 MiB L2");
	JENT_UT_EQ(l3, 8388608, "an 8 MiB L3");

	jent_cache_sizes_arm64(CLIDR_L1_L2_L3, 1, ccsidr_real_ccidx,
			       &l1, &l2, &l3);
	JENT_UT_EQ(l1, 32768, "and the same in the FEAT_CCIDX layout");
	JENT_UT_EQ(l3, 8388608, "the L3 included");

	jent_cache_sizes_arm64(CLIDR_L1_L2_L3, 0, ccsidr_kvm, &l1, &l2, &l3);
	JENT_UT_TRUE(l1 == 0 && l2 == 0 && l3 == 0,
		     "the 64 bytes per level a KVM guest reads count as unknown");

	jent_cache_sizes_arm64(CLIDR_L1_L2_L3, 0, ccsidr_l1_only,
			       &l1, &l2, &l3);
	JENT_UT_EQ(l1, 65536, "a real level is kept");
	JENT_UT_TRUE(l2 == 0 && l3 == 0,
		     "while the placeholders beside it are dropped");

	jent_cache_sizes_arm64(0, 0, ccsidr_real, &l1, &l2, &l3);
	JENT_UT_TRUE(l1 == 0 && l2 == 0 && l3 == 0,
		     "a CLIDR naming no cache reports none");
}

/* The FIPS mode query. Whatever it answers, it must answer a boolean. */

/*
 * The combiner every cache backend feeds. It is pure, and its job - sum the
 * levels the caller asked for, then round up to the next power of two - has to
 * hold for the level combinations a given machine does not present. Common to
 * every backend, so it is tested on every platform.
 */
static void test_cache_roundup(void)
{
	jent_ut_group("the cache size combiner");

	JENT_UT_EQ(jent_cache_roundup_from_sizes(0, 0, 0, 0), 0,
		   "nothing discovered gives no size");
	JENT_UT_EQ(jent_cache_roundup_from_sizes(0, 0, 0, 1), 0,
		   "and the same across all caches");

	/* Only L1 counts unless all caches were asked for. */
	JENT_UT_EQ(jent_cache_roundup_from_sizes(32768, 262144, 8388608, 0),
		   65536, "L1 alone rounds up to the next power of two");
	JENT_UT_EQ(jent_cache_roundup_from_sizes(32768, 262144, 8388608, 1),
		   16777216, "all caches sum before rounding up");

	/* A level that was not discovered contributes nothing. */
	JENT_UT_EQ(jent_cache_roundup_from_sizes(32768, 0, 0, 1), 65536,
		   "an undiscovered L2 and L3 contribute nothing");
	JENT_UT_EQ(jent_cache_roundup_from_sizes(0, 262144, 0, 1), 524288,
		   "an undiscovered L1 leaves the others");
	JENT_UT_EQ(jent_cache_roundup_from_sizes(0, 0, 8388608, 1), 16777216,
		   "an L3 alone still rounds up");

	/* Sizes from 4 GiB on, which a 32-bit sum once wrapped to nothing. */
	JENT_UT_EQ(jent_cache_roundup_from_sizes(UINT64_C(1) << 32, 0, 0, 0),
		   UINT64_C(1) << 33, "a 4 GiB level rounds up to 8 GiB");
	JENT_UT_EQ(jent_cache_roundup_from_sizes(UINT64_MAX, UINT64_MAX,
						 UINT64_MAX, 1),
		   UINT64_C(1) << 63, "a garbage sum saturates at 2^63");
	JENT_UT_EQ(jent_cache_size_bits(UINT64_C(1) << 63), 63,
		   "whose exponent the memoised answer keeps");

	/* An exact power of two rounds to the next one, never to itself. */
	JENT_UT_EQ(jent_cache_roundup_from_sizes(65536, 0, 0, 0), 131072,
		   "an exact power of two rounds up to the next");
}

/*
 * The helpers behind the backends above. They are static to their translation
 * unit and only reached on the fallback paths - a machine where sysfs
 * describes its caches never runs the sysconf one, and a getrandom() that
 * works never runs the /dev/urandom one - so they are called here directly.
 * Otherwise they would be exercised on no machine that could report a problem.
 */

#if defined(JENT_ARCH_CACHE_LINUX)
/*
 * The sysfs attribute parsers. Split out of the walk that reads them (see
 * arch/jitterentropy-arch-cache.c) precisely so that the shapes the kernel
 * produces and the malformed ones it must not be fooled by can both be fed in
 * here - a machine only ever presents one of them.
 */
static void test_cache_parsers(void)
{
	static const struct {
		const char *attr;
		int ok;
		uint64_t want;
		const char *what;
	} sizes[] = {
		{ "32K\n",	1, 32768,	"a kilobyte size" },
		{ "8M\n",	1, 8388608,	"a megabyte size" },
		{ "512\n",	1, 512,		"a bare byte count" },
		{ "0K\n",	0, 0,		"a zero size" },
		{ "K\n",	0, 0,		"a suffix with no number" },
		{ "\n",	0, 0,		"an empty attribute" },
		{ "abc\n",	0, 0,		"a non-numeric attribute" },
		{ "-4K\n",	0, 0,		"a negative size" },
		{ "99999999999999999999M\n", 0, 0,
		  "a size that saturates strtol" },
		{ "4194304K\n",	1, UINT64_C(1) << 32, "a size of 4 GiB" },
		{ "99999999999999M\n", 0, 0,
		  "a size that would overflow the shift" },
	};
	static const struct {
		const char *attr;
		int ok;
		long want;
		const char *what;
	} levels[] = {
		{ "1\n",	1, 1,	"level 1" },
		{ "2\n",	1, 2,	"level 2" },
		{ "3\n",	1, 3,	"level 3" },
		{ "0\n",	0, 0,	"level 0" },
		{ "-1\n",	0, 0,	"a negative level" },
		{ "\n",	0, 0,	"an empty attribute" },
		{ "x\n",	0, 0,	"a non-numeric attribute" },
	};
	size_t i;

	jent_ut_group("the sysfs cache attribute parsers");

	for (i = 0; i < JENT_ARRAY_SIZE(sizes); i++) {
		char buf[32];
		uint64_t val = 0;
		int ret;

		snprintf(buf, sizeof(buf), "%s", sizes[i].attr);
		ret = jent_parse_cache_size(buf, strlen(buf), sizeof(buf), &val);

		JENT_UT_EQ(!ret, sizes[i].ok, sizes[i].what);
		if (sizes[i].ok && !ret)
			JENT_UT_EQ(val, sizes[i].want, "with the right value");
	}

	/*
	 * A read that filled the buffer may have lost its K or M, which would
	 * undercount the cache 1024-fold, so it is rejected rather than
	 * parsed.
	 */
	{
		char buf[8] = "1234567";
		uint64_t val = 0;

		JENT_UT_NE(jent_parse_cache_size(buf, sizeof(buf), sizeof(buf),
						 &val), 0,
			   "an attribute that filled the buffer is rejected");
	}

	for (i = 0; i < JENT_ARRAY_SIZE(levels); i++) {
		long val = -1;
		int ret = jent_parse_cache_level(levels[i].attr, &val);

		JENT_UT_EQ(!ret, levels[i].ok, levels[i].what);
		if (levels[i].ok && !ret)
			JENT_UT_EQ(val, levels[i].want, "with the right value");
	}

	/* Only data and unified caches count towards the working set. */
	JENT_UT_TRUE(jent_cache_type_is_data("Data\n"), "a data cache counts");
	JENT_UT_TRUE(jent_cache_type_is_data("Unified\n"),
		     "a unified cache counts");
	JENT_UT_TRUE(!jent_cache_type_is_data("Instruction\n"),
		     "an instruction cache does not");
	JENT_UT_TRUE(!jent_cache_type_is_data("\n"),
		     "and neither does an empty attribute");
}

static void test_cache_helpers(void)
{
	uint64_t l1 = UINT64_MAX, l2 = UINT64_MAX, l3 = UINT64_MAX;
	char buf[64];

	jent_ut_group("the cache discovery helpers");

	/*
	 * The sysconf path. Every value is either a real size or zero for
	 * "this libc does not know"; a negative sysconf() reply must be
	 * clamped to zero rather than passed on as a size.
	 */
	jent_get_cachesize_sysconf(&l1, &l2, &l3);
	JENT_UT_TRUE(l1 != UINT64_MAX, "the sysconf L1 size is set");
	JENT_UT_TRUE(l2 != UINT64_MAX, "the sysconf L2 size is set");
	JENT_UT_TRUE(l3 != UINT64_MAX, "the sysconf L3 size is set");
	printf("  note: sysconf reports L1 %llu, L2 %llu, L3 %llu\n",
	       (unsigned long long)l1, (unsigned long long)l2,
	       (unsigned long long)l3);

	/* The sysfs reader, on a path that does not exist. */
	JENT_UT_TRUE(jent_read_sysfs_attr("/nonexistent/jent/cache/attr", buf,
					  sizeof(buf)) < 0,
		     "an unreadable sysfs attribute is reported as an error");
}

#ifdef JENT_UT_CPUID
/* A CPU without the cache leaves, as a hypervisor hiding them presents. */
static int cpuid_none(unsigned int leaf, unsigned int subleaf,
		      unsigned int *eax, unsigned int *ebx,
		      unsigned int *ecx, unsigned int *edx)
{
	(void)leaf;
	(void)subleaf;
	*eax = *ebx = *ecx = *edx = 0;
	return 0;
}
#endif

/*
 * Where sysfs answers, the walk finds an L1 and the function returns before
 * the fallback; taking sysfs away is the only way to reach it.
 */
static void test_cache_sysconf_fallback(void)
{
	uint64_t l1 = UINT64_MAX, l2 = UINT64_MAX, l3 = UINT64_MAX;
	uint64_t s1 = UINT64_MAX, s2 = UINT64_MAX, s3 = UINT64_MAX;
	const char *saved = jent_test_sysfs_root;

	jent_ut_group("the cache sysconf fallback");

	jent_get_cachesize_sysconf(&s1, &s2, &s3);

	/* On Arm the core types would answer next; they are not asked here. */
	jent_test_sysfs_root = "/nonexistent/jent/sys/devices/system/cpu";
#ifdef JENT_UT_CPUINFO
	jent_test_cpuinfo = "/nonexistent/jent/proc/cpuinfo";
#endif
#ifdef JENT_UT_CPUID
	jent_test_cpuid = cpuid_none;
#endif
	jent_get_cachesize_uncached(&l1, &l2, &l3);
	jent_test_sysfs_root = saved;
#ifdef JENT_UT_CPUINFO
	jent_test_cpuinfo = "/proc/cpuinfo";
#endif
#ifdef JENT_UT_CPUID
	jent_test_cpuid = NULL;
#endif

	/* Zeros included: musl has no _SC_LEVEL* and none may be invented. */
	JENT_UT_EQ(l1, s1, "the L1 size falls back to sysconf");
	JENT_UT_EQ(l2, s2, "the L2 size falls back to sysconf");
	JENT_UT_EQ(l3, s3, "the L3 size falls back to sysconf");

	/* And the walk itself still answers when the root is real. */
	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_uncached(&l1, &l2, &l3);
	JENT_UT_TRUE(l1 != UINT64_MAX && l2 != UINT64_MAX && l3 != UINT64_MAX,
		     "and the real root is used again afterwards");
	printf("  note: fallback gave L1 %llu, the real root gives L1 %llu\n",
	       (unsigned long long)s1, (unsigned long long)l1);
}
#else
static void test_cache_helpers(void)
{
	JENT_UT_SKIP("the cache discovery helpers",
		     "not the sysfs/sysconf cache backend");
}
static void test_cache_sysconf_fallback(void)
{
	JENT_UT_SKIP("the cache sysconf fallback",
		     "not the sysfs/sysconf cache backend");
}
static void test_cache_parsers(void)
{
	JENT_UT_SKIP("the sysfs cache attribute parsers",
		     "not the sysfs cache backend");
}
#endif

/*
 * A sysfs cache tree built for the walk to read. The shapes below are the ones
 * a real machine either does not have or has only one of: an instruction cache
 * that must be skipped, attributes that do not parse, a hole in the index
 * numbering that ends the scan, and two levels whose largest entry must win.
 */
#if defined(JENT_ARCH_CACHE_LINUX)
static char sysfs_root[] = "/tmp/jent-sysfs-XXXXXX";

static int sysfs_write(const char *dir, unsigned int idx, const char *attr,
		       const char *value)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/index%u", dir, idx);
	if (mkdir(path, 0700) && errno != EEXIST)
		return -1;

	snprintf(path, sizeof(path), "%s/index%u/%s", dir, idx, attr);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(value, f);
	fclose(f);
	return 0;
}

static int sysfs_index(const char *dir, unsigned int idx, const char *type,
		       const char *level, const char *size)
{
	if (type && sysfs_write(dir, idx, "type", type))
		return -1;
	if (level && sysfs_write(dir, idx, "level", level))
		return -1;
	if (size && sysfs_write(dir, idx, "size", size))
		return -1;
	return 0;
}

/*
 * The trees above live under /tmp and were left there: one directory per run,
 * a few hundred files each, on every machine and every CI job that runs the
 * suite. Removed depth first; the trees hold nothing but directories and
 * regular files, and nothing follows a symlink because none is created.
 */
static void sysfs_rmtree(const char *path)
{
	DIR *d = opendir(path);
	struct dirent *e;

	if (!d) {
		unlink(path);
		return;
	}

	while ((e = readdir(d))) {
		char sub[512];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;

		snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
		if (lstat(sub, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			sysfs_rmtree(sub);
		else
			unlink(sub);
	}

	closedir(d);
	rmdir(path);
}

static void test_sysfs_cache_walk(void)
{
	char dir[128];
	uint64_t l1 = UINT64_MAX, l2 = UINT64_MAX, l3 = UINT64_MAX;

	jent_ut_group("the sysfs cache walk against a synthetic tree");

	if (!mkdtemp(sysfs_root)) {
		JENT_UT_SKIP("the sysfs cache walk", "no temporary directory");
		return;
	}

	snprintf(dir, sizeof(dir), "%s/cpu0", sysfs_root);
	if (mkdir(dir, 0700)) {
		JENT_UT_SKIP("the sysfs cache walk", "cpu0 is not creatable");
		return;
	}
	snprintf(dir, sizeof(dir), "%s/cpu0/cache", sysfs_root);
	if (mkdir(dir, 0700)) {
		JENT_UT_SKIP("the sysfs cache walk", "the cache dir is not creatable");
		return;
	}

	/*
	 * index0 an instruction cache (skipped), index1 the L1 data cache,
	 * index2 a unified L2, index3 an L3 whose size does not parse, index4
	 * an L1 larger than index1 so the larger must win, index5 a level that
	 * does not parse.
	 *
	 * index6 has a type but no level attribute and index7 no size, both of
	 * which the walk has to skip rather than read past.
	 *
	 * index8 is the L3 that does parse, and index9 a level 4 cache - the
	 * eDRAM victim cache of a Crystal Well or Broadwell part, which sysfs
	 * lists at level 4 beside the L3. It is far larger than the L3, so a
	 * walk that dropped it into the L3 slot would be plain to see here:
	 * doing that is what once sized JENT_CACHE_ALL at 256 MiB.
	 *
	 * index10 has an empty type attribute, which reads as no attribute at
	 * all and ends the scan.
	 */
	if (sysfs_index(dir, 0, "Instruction\n", "1\n", "32K\n") ||
	    sysfs_index(dir, 1, "Data\n", "1\n", "16K\n") ||
	    sysfs_index(dir, 2, "Unified\n", "2\n", "1M\n") ||
	    sysfs_index(dir, 3, "Unified\n", "3\n", "not-a-size\n") ||
	    sysfs_index(dir, 4, "Data\n", "1\n", "48K\n") ||
	    sysfs_index(dir, 5, "Data\n", "no-level\n", "8K\n") ||
	    sysfs_index(dir, 6, "Data\n", NULL, "8K\n") ||
	    sysfs_index(dir, 7, "Data\n", "1\n", NULL) ||
	    sysfs_index(dir, 8, "Unified\n", "3\n", "8M\n") ||
	    sysfs_index(dir, 9, "Unified\n", "4\n", "128M\n") ||
	    sysfs_index(dir, 10, "", "1\n", "8K\n")) {
		JENT_UT_SKIP("the sysfs cache walk", "the tree is not writable");
		return;
	}

	jent_get_cachesize_sysfs_dir(sysfs_root, &l1, &l2, &l3);

	JENT_UT_EQ(l1, 49152, "the largest L1 data cache is taken");
	JENT_UT_EQ(l2, 1048576, "the unified L2 is taken");
	JENT_UT_EQ(l3, 8388608,
		   "the unified L3 is taken, and neither the L3 whose size "
		   "does not parse nor the 128M level 4 beside it");

	/* A tree that does not exist leaves every level at zero. */
	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_sysfs_dir("/nonexistent/jent/cpu", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "an absent tree reports no L1");
	JENT_UT_EQ(l2, 0, "an absent tree reports no L2");
	JENT_UT_EQ(l3, 0, "an absent tree reports no L3");

	/*
	 * A tree with more cache indices than the walk looks at: it stops
	 * after sixteen rather than following the numbering wherever it goes.
	 */
	{
		char full[128];
		unsigned int idx;
		int ok = 1;

		snprintf(full, sizeof(full), "%s/many", sysfs_root);
		mkdir(full, 0700);
		snprintf(full, sizeof(full), "%s/many/cpu0", sysfs_root);
		mkdir(full, 0700);
		snprintf(full, sizeof(full), "%s/many/cpu0/cache", sysfs_root);
		mkdir(full, 0700);

		/*
		 * The indices past the sixteenth carry a different size, so
		 * the answer says where the walk stopped. They all used to
		 * read 4K, which is what a walk reading all twenty would have
		 * reported just the same.
		 */
		for (idx = 0; idx < 20; idx++) {
			if (sysfs_index(full, idx, "Data\n", "1\n",
					idx < 16 ? "4K\n" : "64K\n")) {
				ok = 0;
				break;
			}
		}

		if (ok) {
			char root[128];

			snprintf(root, sizeof(root), "%s/many", sysfs_root);
			l1 = UINT64_MAX;
			jent_get_cachesize_sysfs_dir(root, &l1, &l2, &l3);
			JENT_UT_EQ(l1, 4096,
				   "the walk stops after sixteen cache indices");
		}
	}

	/*
	 * More than one CPU. Every tree above has a cpu0 and nothing else, so
	 * neither the enumeration of the other CPUs nor the largest-wins rule
	 * across them was reached: on a hybrid part (P-cores and E-cores,
	 * big.LITTLE) the per-core data caches differ, and the collector runs
	 * on whichever core it is scheduled to.
	 */
	{
		char many[160];
		unsigned int cpu;
		int ok = 1;

		snprintf(many, sizeof(many), "%s/cpus", sysfs_root);
		mkdir(many, 0700);

		for (cpu = 0; cpu < 4 && ok; cpu++) {
			char cache[192];

			snprintf(cache, sizeof(cache), "%s/cpus/cpu%u",
				 sysfs_root, cpu);
			mkdir(cache, 0700);
			snprintf(cache, sizeof(cache), "%s/cpus/cpu%u/cache",
				 sysfs_root, cpu);
			if (mkdir(cache, 0700)) {
				ok = 0;
				break;
			}

			/* cpu2 is the big core: the largest L1 and the L3. */
			if (sysfs_index(cache, 0, "Data\n", "1\n",
					cpu == 2 ? "64K\n" : "32K\n") ||
			    sysfs_index(cache, 1, "Unified\n", "2\n",
					cpu == 2 ? "2M\n" : "512K\n") ||
			    (cpu == 2 &&
			     sysfs_index(cache, 2, "Unified\n", "3\n", "16M\n")))
				ok = 0;
		}

		if (ok) {
			l1 = l2 = l3 = UINT64_MAX;
			jent_get_cachesize_sysfs_dir(many, &l1, &l2, &l3);
			JENT_UT_EQ(l1, 65536,
				   "the largest L1 across the CPUs is taken");
			JENT_UT_EQ(l2, 2097152, "and the largest L2");
			JENT_UT_EQ(l3, 16777216,
				   "and an L3 only one of them reports");
		} else {
			JENT_UT_SKIP("the multi-CPU cache walk",
				     "the tree is not writable");
		}
	}

	/* index0 missing at all ends the scan immediately. */
	{
		char empty[128];

		snprintf(empty, sizeof(empty), "%s/empty", sysfs_root);
		mkdir(empty, 0700);
		l1 = UINT64_MAX;
		jent_get_cachesize_sysfs_dir(empty, &l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "a tree with no cache directory reports no L1");
	}

	sysfs_rmtree(sysfs_root);
}
#else
static void test_sysfs_cache_walk(void)
{
	JENT_UT_SKIP("the sysfs cache walk", "not the sysfs cache backend");
}
#endif

/*
 * CPUID, the last resort of the Linux backend on x86 - the only one that
 * answers on musl in a container without /sys. A made-up geometry is served in
 * place of the instruction, under the Intel leaf or the AMD one.
 */
#if defined(JENT_ARCH_CACHE_LINUX_CPUID) && defined(JENT_UT_CPUID)

/* 32 KB L1 data: 8 ways * 1 partition * 64 byte lines * 64 sets. */
#define CPUID_L1	32768
/* 1 MB unified L2: 16 * 1 * 64 * 1024. */
#define CPUID_L2	1048576
/* 8 MB unified L3: 16 * 1 * 64 * 8192. */
#define CPUID_L3	8388608

static unsigned int cpuid_fake_leaf;

static int cpuid_fake(unsigned int leaf, unsigned int subleaf,
		      unsigned int *eax, unsigned int *ebx,
		      unsigned int *ecx, unsigned int *edx)
{
	*eax = *ebx = *ecx = *edx = 0;
	if (leaf != cpuid_fake_leaf)
		return 1;	/* the leaf exists but reports no cache */

	switch (subleaf) {
	case 0:		/* L1 data */
		*eax = 1 | (1 << 5);
		*ebx = (7U << 22) | 63;
		*ecx = 63;
		break;
	case 1:		/* L1 instruction, to be skipped */
		*eax = 2 | (1 << 5);
		*ebx = (7U << 22) | 63;
		*ecx = 1023;
		break;
	case 2:		/* L2 unified */
		*eax = 3 | (2 << 5);
		*ebx = (15U << 22) | 63;
		*ecx = 1023;
		break;
	case 3:		/* L3 unified */
		*eax = 3 | (3 << 5);
		*ebx = (15U << 22) | 63;
		*ecx = 8191;
		break;
	default:	/* type 0: no further caches */
		break;
	}
	return 1;
}

static void test_cache_cpuid_fallback(void)
{
	uint64_t l1 = UINT64_MAX, l2 = UINT64_MAX, l3 = UINT64_MAX;
	uint64_t s1 = UINT64_MAX, s2 = UINT64_MAX, s3 = UINT64_MAX;
	const char *saved = jent_test_sysfs_root;

	jent_ut_group("the cache CPUID fallback");

	jent_test_cpuid = cpuid_fake;

	/* Both leaves: Intel's 4, and AMD's 0x8000001D behind an empty 4. */
	cpuid_fake_leaf = 4;
	jent_get_cachesize_cpuid(&l1, &l2, &l3);
	JENT_UT_EQ(l1, CPUID_L1, "leaf 4 gives the L1 data cache");
	JENT_UT_EQ(l2, CPUID_L2, "leaf 4 gives the L2");
	JENT_UT_EQ(l3, CPUID_L3, "leaf 4 gives the L3");

	cpuid_fake_leaf = 0x8000001DU;
	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_cpuid(&l1, &l2, &l3);
	JENT_UT_EQ(l1, CPUID_L1, "leaf 0x8000001D gives the L1 data cache");
	JENT_UT_EQ(l3, CPUID_L3, "leaf 0x8000001D gives the L3");

	/*
	 * Without sysfs, whatever sysconf reports stands - glibc does - and
	 * only the levels it leaves at zero - all of them on musl - are
	 * CPUID's.
	 */
	cpuid_fake_leaf = 4;
	jent_get_cachesize_sysconf(&s1, &s2, &s3);
	jent_test_sysfs_root = "/nonexistent/jent/sys/devices/system/cpu";
	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_uncached(&l1, &l2, &l3);
	jent_test_sysfs_root = saved;
	JENT_UT_EQ(l1, s1 > 0 ? s1 : CPUID_L1,
		   "without sysfs an L1 sysconf lacks comes from CPUID");
	JENT_UT_EQ(l2, s2 > 0 ? s2 : CPUID_L2,
		   "without sysfs an L2 sysconf lacks comes from CPUID");
	JENT_UT_EQ(l3, s3 > 0 ? s3 : CPUID_L3,
		   "without sysfs an L3 sysconf lacks comes from CPUID");

	/*
	 * A sysfs that states the L1 only: a level it leaves out is completed,
	 * and never by CPUID where sysconf - glibc - already answers. The L1
	 * is only ever raised, as a hybrid part demands.
	 */
	{
		static char root[] = "/tmp/jent-sysfs-cpuid-XXXXXX";
		char dir[160];

		if (mkdtemp(root)) {
			snprintf(dir, sizeof(dir), "%s/cpu0", root);
			mkdir(dir, 0700);
			snprintf(dir, sizeof(dir), "%s/cpu0/cache", root);
			if (!mkdir(dir, 0700) &&
			    !sysfs_index(dir, 0, "Data\n", "1\n", "16K\n")) {
				jent_test_sysfs_root = root;
				l1 = l2 = l3 = UINT64_MAX;
				jent_get_cachesize_uncached(&l1, &l2, &l3);
				jent_test_sysfs_root = saved;

				JENT_UT_EQ(l1, s1 > 16384 ? s1 : 16384,
					   "the L1 sysfs reports is never lowered");
				JENT_UT_EQ(l2, s2 > 0 ? s2 : CPUID_L2,
					   "the L2 it lacks comes from sysconf or CPUID");
				JENT_UT_EQ(l3, s3 > 0 ? s3 : CPUID_L3,
					   "the L3 it lacks comes from sysconf or CPUID");
			}
			sysfs_rmtree(root);
		}
	}

	/* No cache leaf at all leaves the unknown levels unknown. */
	jent_test_cpuid = cpuid_none;
	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_cpuid(&l1, &l2, &l3);
	JENT_UT_TRUE(l1 == 0 && l2 == 0 && l3 == 0,
		     "a CPU without the cache leaves reports nothing");

	/* And the real instruction. */
	jent_test_cpuid = NULL;
	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_cpuid(&l1, &l2, &l3);
	JENT_UT_TRUE(l1 != UINT64_MAX && l2 != UINT64_MAX && l3 != UINT64_MAX,
		     "the real CPUID answers with sizes, or none");
	printf("  note: CPUID gives L1 %llu, L2 %llu, L3 %llu\n",
	       (unsigned long long)l1, (unsigned long long)l2,
	       (unsigned long long)l3);
}
#else
static void test_cache_cpuid_fallback(void)
{
	JENT_UT_SKIP("the cache CPUID fallback",
		     "not the Linux backend on x86");
}
#endif

/*
 * The core types of /proc/cpuinfo, the last resort on Arm. The file is fed in
 * rather than read: a machine presents one core pairing, and one whose sysfs
 * answers never reaches this at all.
 */
#if defined(JENT_ARCH_CACHE_LINUX) && defined(JENT_UT_CPUINFO)

/*
 * The largest caches the TRMs allow: a Cortex-A57 has a fixed 32 KB L1 data
 * cache, a Cortex-A53 and A55 one of up to 64 KB, and the A57 and A53 up to
 * 2 MB of L2 - the A55 only 256 KB, but a DSU with up to 4 MB of L3 behind
 * it, where the other two have no L3. A Cortex-X925 sits in a DSU-120.
 */
#define A57_L1	32768
#define A57_L2	2097152
#define A53_L1	65536
#define A53_L2	2097152
#define A55_L1	65536
#define A55_L2	262144
#define A55_L3	4194304
#define X925_L3	33554432

/* Feeds @text line by line, as jent_get_cachesize_cpuinfo_file() does. */
static void cpuinfo_lines(const char *text, uint64_t *l1, uint64_t *l2,
			  uint64_t *l3)
{
	char line[256];
	long implementer = -1;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;
	while (*text) {
		size_t n = strcspn(text, "\n");

		snprintf(line, sizeof(line), "%.*s", (int)n, text);
		jent_cpuinfo_arm_line(line, &implementer, l1, l2, l3);
		text += n;
		if (*text)
			text++;
	}
}

/* A Nexus 5X (MSM8992): four Cortex-A53, two Cortex-A57. */
static const char cpuinfo_msm8992[] =
	"processor\t: 0\n"
	"BogoMIPS\t: 38.40\n"
	"Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid\n"
	"CPU implementer\t: 0x41\n"
	"CPU architecture: 8\n"
	"CPU variant\t: 0x0\n"
	"CPU part\t: 0xd03\n"
	"CPU revision\t: 4\n"
	"\n"
	"processor\t: 4\n"
	"BogoMIPS\t: 38.40\n"
	"Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid\n"
	"CPU implementer\t: 0x41\n"
	"CPU architecture: 8\n"
	"CPU variant\t: 0x1\n"
	"CPU part\t: 0xd07\n"
	"CPU revision\t: 3\n"
	"\n"
	"Hardware\t: Qualcomm Technologies, Inc MSM8992\n";

static void test_cache_cpuinfo(void)
{
	static char path[] = "/tmp/jent-cpuinfo-XXXXXX";
	uint64_t l1, l2, l3;
	int fd;

	jent_ut_group("the Arm core types of /proc/cpuinfo");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xd07\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, A57_L1, "a Cortex-A57 has the L1 its TRM fixes");
	JENT_UT_EQ(l2, A57_L2, "and the largest L2 it allows");
	JENT_UT_EQ(l3, 0, "and no L3: its cluster cache is the L2");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xd05\n", &l1, &l2, &l3);
	JENT_UT_EQ(l2, A55_L2, "a Cortex-A55 has the largest L2 its TRM allows");
	JENT_UT_EQ(l3, A55_L3, "and the largest L3 its DSU does");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xd85\n", &l1, &l2, &l3);
	JENT_UT_EQ(l3, X925_L3, "a Cortex-X925 the largest L3 of a DSU-120");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xd4f\n", &l1, &l2, &l3);
	JENT_UT_EQ(l3, 0, "a Neoverse V2, connected directly, has no L3");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xd03\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, A53_L1, "a Cortex-A53 has the largest L1 its TRM allows");
	JENT_UT_EQ(l2, A53_L2, "and the largest L2, which is optional");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xfff\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "a core type not listed has none");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t: 0xc0f\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 32768, "an ARMv7 Cortex-A15 is listed as well");
	JENT_UT_EQ(l2, 4194304, "with its largest L2");

	cpuinfo_lines(cpuinfo_msm8992, &l1, &l2, &l3);
	JENT_UT_EQ(l1, A53_L1, "the larger L1 of a big.LITTLE pair is taken");
	JENT_UT_EQ(l2, A57_L2, "and the larger L2");

	/* Per level: the L1 from one core type, the L2 from the other. */
	cpuinfo_lines("processor\t: 0\nCPU implementer\t: 0x41\nCPU part\t: 0xd05\n"
		      "processor\t: 4\nCPU implementer\t: 0x41\nCPU part\t: 0xd07\n",
		      &l1, &l2, &l3);
	JENT_UT_EQ(l1, A55_L1, "the largest L1 is kept across core types");
	JENT_UT_EQ(l2, A57_L2, "and so is the largest L2");
	JENT_UT_EQ(l3, A55_L3, "and the largest L3");

	cpuinfo_lines("CPU implementer\t: 0x51\nCPU part\t: 0xd07\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "a part number counts only with its implementer");

	cpuinfo_lines("CPU part\t: 0xd07\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "a part without an implementer counts for nothing");

	cpuinfo_lines("CPU implementer\t: 0x41\nprocessor\t: 1\nCPU part\t: 0xd07\n",
		      &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "an implementer does not carry over to the next CPU");

	cpuinfo_lines("CPU implementer\t: arm\nCPU part\t: 0xd07\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "an implementer that does not parse counts for nothing");

	cpuinfo_lines("CPU implementer\t: 0x41\nCPU part\t:\n", &l1, &l2, &l3);
	JENT_UT_EQ(l1, 0, "and neither does a part that does not");

	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_cpuinfo_file("/nonexistent/jent/proc/cpuinfo",
					&l1, &l2, &l3);
	JENT_UT_TRUE(l1 == 0 && l2 == 0 && l3 == 0,
		     "an absent /proc/cpuinfo reports nothing");

	jent_get_cachesize_cpuinfo(&l1, &l2, &l3);
	printf("  note: /proc/cpuinfo gives L1 %llu, L2 %llu\n",
	       (unsigned long long)l1, (unsigned long long)l2);

	/*
	 * The reader: a Features line longer than its line buffer, which must
	 * be skipped whole rather than read in pieces, and a last line
	 * without its newline.
	 */
	fd = mkstemp(path);
	if (fd < 0) {
		JENT_UT_SKIP("the /proc/cpuinfo reader", "no temporary file");
		return;
	}
	{
		static const char tail[] =
			"processor\t: 0\n"
			"Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp cpuid asimdrdm lrcpc dcpop asimddp\n"
			"CPU implementer\t: 0x41\n"
			"CPU part\t: 0xd07";
		ssize_t w = write(fd, tail, sizeof(tail) - 1);

		close(fd);
		if (w != (ssize_t)sizeof(tail) - 1) {
			unlink(path);
			JENT_UT_SKIP("the /proc/cpuinfo reader",
				     "the temporary file is not writable");
			return;
		}
	}

	l1 = l2 = l3 = UINT64_MAX;
	jent_get_cachesize_cpuinfo_file(path, &l1, &l2, &l3);
	JENT_UT_EQ(l1, A57_L1, "the reader finds the core past a long line");
	JENT_UT_EQ(l2, A57_L2, "with its L2");
	JENT_UT_EQ(l3, 0, "and no L3");

	/* The whole chain, with sysfs gone and the file in place. */
	{
		uint64_t s1, s2, s3;
		const char *saved = jent_test_sysfs_root;

		jent_get_cachesize_sysconf(&s1, &s2, &s3);
		jent_test_sysfs_root = "/nonexistent/jent/sys/devices/system/cpu";
		jent_test_cpuinfo = path;
		jent_get_cachesize_uncached(&l1, &l2, &l3);
		jent_test_sysfs_root = saved;
		jent_test_cpuinfo = "/proc/cpuinfo";

		if (s1 > 0)
			JENT_UT_EQ(l1, s1, "an L1 from sysconf is not second-guessed");
		else
			JENT_UT_EQ(l1, A57_L1,
				   "without sysfs and sysconf the core type answers");
	}

	/*
	 * A sysfs that states the L1 only, as a device tree with d-cache-size
	 * but no L2 node gives: the L1 is a measurement and is never lowered,
	 * and the L2 comes from the core type where sysconf has none.
	 */
	{
		static char root[] = "/tmp/jent-sysfs-l1-XXXXXX";
		char dir[160];
		uint64_t s1, s2, s3;
		const char *saved = jent_test_sysfs_root;

		jent_get_cachesize_sysconf(&s1, &s2, &s3);
		if (mkdtemp(root)) {
			snprintf(dir, sizeof(dir), "%s/cpu0", root);
			mkdir(dir, 0700);
			snprintf(dir, sizeof(dir), "%s/cpu0/cache", root);
			if (!mkdir(dir, 0700) &&
			    !sysfs_index(dir, 0, "Data\n", "1\n", "16K\n")) {
				jent_test_sysfs_root = root;
				jent_test_cpuinfo = path;
				l1 = l2 = l3 = UINT64_MAX;
				jent_get_cachesize_uncached(&l1, &l2, &l3);
				jent_test_sysfs_root = saved;
				jent_test_cpuinfo = "/proc/cpuinfo";

				JENT_UT_EQ(l1, s1 > 16384 ? s1 : 16384,
					   "the L1 sysfs reports is never lowered");
				JENT_UT_EQ(l2, s2 > 0 ? s2 : A57_L2,
					   "the L2 it lacks comes from sysconf or the table");
			}
			sysfs_rmtree(root);
		}
	}
	unlink(path);
}
#else
static void test_cache_cpuinfo(void)
{
	JENT_UT_SKIP("the Arm core types of /proc/cpuinfo",
		     "not the Linux backend on Arm");
}
#endif

/* The "online" CPU list, in every shape the kernel writes and some it cannot. */

int main(void)
{
	test_cache();
	test_cache_roundup();
	test_cache_arm64();
	test_cache_helpers();
	test_cache_sysconf_fallback();
	test_cache_parsers();
	test_sysfs_cache_walk();
	test_cache_cpuinfo();
	test_cache_cpuid_fallback();

	return jent_ut_report("unit-arch-cache");
}
