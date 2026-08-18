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
static void test_cache(void)
{
	uint32_t one = jent_cache_size_roundup(0);
	uint32_t all = jent_cache_size_roundup(1);

	jent_ut_group("jent_cache_size_roundup");

	printf("  note: single cache %u bytes, all caches %u bytes\n", one, all);

	if (!one && !all) {
		JENT_UT_SKIP("jent_cache_size_roundup",
			     "no cache size is discoverable here");
		return;
	}

	/*
	 * The name says roundup: the value is used as a memory block size and
	 * masked with size - 1 in the access loop, so a value that is not a
	 * power of two would make the mask skip part of the block.
	 */
	if (one)
		JENT_UT_EQ(one & (one - 1), 0, "a single cache size is a power of two");
	if (all)
		JENT_UT_EQ(all & (all - 1), 0, "the total cache size is a power of two");

	if (one && all)
		JENT_UT_TRUE(all >= one,
			     "all caches together are at least one cache");
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

	/* Negative values are the "not discovered" marker, not a size. */
	JENT_UT_EQ(jent_cache_roundup_from_sizes(-1, -1, -1, 1), 0,
		   "negative levels are not sizes");
	JENT_UT_EQ(jent_cache_roundup_from_sizes(32768, -1, -1, 1), 65536,
		   "and are skipped rather than subtracted");

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
		long want;
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
		{ "9999999999999M\n", 0, 0,
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
		long val = -1;
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
		long val = -1;

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
	long l1 = -1, l2 = -1, l3 = -1;
	char buf[64];

	jent_ut_group("the cache discovery helpers");

	/*
	 * The sysconf path. Every value is either a real size or zero for
	 * "this libc does not know"; a negative sysconf() reply must be
	 * clamped rather than passed on as a size.
	 */
	jent_get_cachesize_sysconf(&l1, &l2, &l3);
	JENT_UT_TRUE(l1 >= 0, "the sysconf L1 size is not negative");
	JENT_UT_TRUE(l2 >= 0, "the sysconf L2 size is not negative");
	JENT_UT_TRUE(l3 >= 0, "the sysconf L3 size is not negative");
	printf("  note: sysconf reports L1 %ld, L2 %ld, L3 %ld\n", l1, l2, l3);

	/* The sysfs reader, on a path that does not exist. */
	JENT_UT_TRUE(jent_read_sysfs_attr("/nonexistent/jent/cache/attr", buf,
					  sizeof(buf)) < 0,
		     "an unreadable sysfs attribute is reported as an error");
}

/*
 * Where sysfs answers, the walk finds an L1 and the function returns before
 * the fallback; taking sysfs away is the only way to reach it.
 */
static void test_cache_sysconf_fallback(void)
{
	long l1 = -1, l2 = -1, l3 = -1;
	long s1 = -1, s2 = -1, s3 = -1;
	const char *saved = jent_test_sysfs_root;

	jent_ut_group("the cache sysconf fallback");

	jent_get_cachesize_sysconf(&s1, &s2, &s3);

	jent_test_sysfs_root = "/nonexistent/jent/sys/devices/system/cpu";
	jent_get_cachesize_uncached(&l1, &l2, &l3);
	jent_test_sysfs_root = saved;

	/* Zeros included: musl has no _SC_LEVEL* and none may be invented. */
	JENT_UT_EQ(l1, s1, "the L1 size falls back to sysconf");
	JENT_UT_EQ(l2, s2, "the L2 size falls back to sysconf");
	JENT_UT_EQ(l3, s3, "the L3 size falls back to sysconf");

	/* And the walk itself still answers when the root is real. */
	l1 = l2 = l3 = -1;
	jent_get_cachesize_uncached(&l1, &l2, &l3);
	JENT_UT_TRUE(l1 >= 0 && l2 >= 0 && l3 >= 0,
		     "and the real root is used again afterwards");
	printf("  note: fallback gave L1 %ld, the real root gives L1 %ld\n",
	       s1, l1);
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

static void test_sysfs_cache_walk(void)
{
	char dir[128];
	long l1 = -1, l2 = -1, l3 = -1;

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
	 * does not parse. index6 is absent, which ends the scan.
	 */
	/*
	 * index6 has a type but no level attribute and index7 no size, both of
	 * which the walk has to skip rather than read past; index8 has an
	 * empty type attribute, which reads as no attribute at all.
	 */
	if (sysfs_index(dir, 0, "Instruction\n", "1\n", "32K\n") ||
	    sysfs_index(dir, 1, "Data\n", "1\n", "16K\n") ||
	    sysfs_index(dir, 2, "Unified\n", "2\n", "1M\n") ||
	    sysfs_index(dir, 3, "Unified\n", "3\n", "not-a-size\n") ||
	    sysfs_index(dir, 4, "Data\n", "1\n", "48K\n") ||
	    sysfs_index(dir, 5, "Data\n", "no-level\n", "8K\n") ||
	    sysfs_index(dir, 6, "Data\n", NULL, "8K\n") ||
	    sysfs_index(dir, 7, "Data\n", "1\n", NULL) ||
	    sysfs_index(dir, 8, "", "1\n", "8K\n")) {
		JENT_UT_SKIP("the sysfs cache walk", "the tree is not writable");
		return;
	}

	jent_get_cachesize_sysfs_dir(sysfs_root, &l1, &l2, &l3);

	JENT_UT_EQ(l1, 49152, "the largest L1 data cache is taken");
	JENT_UT_EQ(l2, 1048576, "the unified L2 is taken");
	JENT_UT_EQ(l3, 0, "an L3 whose size does not parse is skipped");

	/* A tree that does not exist leaves every level at zero. */
	l1 = l2 = l3 = -1;
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

		for (idx = 0; idx < 20; idx++) {
			if (sysfs_index(full, idx, "Data\n", "1\n", "4K\n")) {
				ok = 0;
				break;
			}
		}

		if (ok) {
			char root[128];

			snprintf(root, sizeof(root), "%s/many", sysfs_root);
			l1 = -1;
			jent_get_cachesize_sysfs_dir(root, &l1, &l2, &l3);
			JENT_UT_EQ(l1, 4096,
				   "a tree with more indices than are scanned is handled");
		}
	}

	/* index0 missing at all ends the scan immediately. */
	{
		char empty[128];

		snprintf(empty, sizeof(empty), "%s/empty", sysfs_root);
		mkdir(empty, 0700);
		l1 = -1;
		jent_get_cachesize_sysfs_dir(empty, &l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "a tree with no cache directory reports no L1");
	}
}
#else
static void test_sysfs_cache_walk(void)
{
	JENT_UT_SKIP("the sysfs cache walk", "not the sysfs cache backend");
}
#endif

/* The "online" CPU list, in every shape the kernel writes and some it cannot. */

int main(void)
{
	test_cache();
	test_cache_roundup();
	test_cache_helpers();
	test_cache_sysconf_fallback();
	test_cache_parsers();
	test_sysfs_cache_walk();

	return jent_ut_report("unit-arch-cache");
}
