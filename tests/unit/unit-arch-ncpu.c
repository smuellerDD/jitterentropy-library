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
 * Jitter RNG: unit tests for the CPU count backend.
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
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"

/*
 * The processor group layout and the thread's group affinity, replaceable for
 * the CPU backend alone: more than one processor group takes more than 64
 * logical CPUs, and a gap in a group's active mask a parked or disabled
 * processor, and a test machine is unlikely to have either. <windows.h>
 * came in with the cache backend above, so the real calls are declared here
 * already and the fakes can forward to them.
 */
#ifdef JENT_ARCH_CACHE_WINDOWS
static int ut_groups_fake;
static DWORD ut_groups_len;
static union {
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec;
	BYTE raw[512];
} ut_groups;

static int ut_thread_ga_fake;
static GROUP_AFFINITY ut_thread_ga;

static BOOL WINAPI ut_GetLogicalProcessorInformationEx(
	LOGICAL_PROCESSOR_RELATIONSHIP rel,
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX buf, PDWORD len)
{
	if (!ut_groups_fake || rel != RelationGroup)
		return GetLogicalProcessorInformationEx(rel, buf, len);

	if (!buf || *len < ut_groups_len) {
		*len = ut_groups_len;
		SetLastError(ERROR_INSUFFICIENT_BUFFER);
		return FALSE;
	}
	memcpy(buf, ut_groups.raw, ut_groups_len);
	*len = ut_groups_len;
	return TRUE;
}

static BOOL WINAPI ut_GetThreadGroupAffinity(HANDLE thread,
					     PGROUP_AFFINITY ga)
{
	if (!ut_thread_ga_fake)
		return GetThreadGroupAffinity(thread, ga);

	/* 2: the query itself fails. */
	if (ut_thread_ga_fake == 2) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}

	*ga = ut_thread_ga;
	return TRUE;
}

# define GetLogicalProcessorInformationEx ut_GetLogicalProcessorInformationEx
# define GetThreadGroupAffinity ut_GetThreadGroupAffinity
#else /* JENT_ARCH_CACHE_WINDOWS */

/*
 * sysconf() and, on Linux, the affinity query with its CPU-set allocation,
 * replaceable for the CPU backend alone. No test machine has more CPUs than a
 * cpu_set_t holds, a mask that names none, or a libc that cannot count, so
 * the set that grows on EINVAL and the fallbacks behind an unanswered mask
 * run against these only.
 */
# include <unistd.h>

enum ut_sysconf_mode {
	UT_SYSCONF_REAL = 0,
	UT_SYSCONF_VALUE,	/* ut_sysconf_value */
	UT_SYSCONF_UNKNOWN,	/* -1 with errno untouched */
	UT_SYSCONF_ERRNO,	/* -1 with EINVAL */
};

static enum ut_sysconf_mode ut_sysconf_mode;
static long ut_sysconf_value;

static long ut_sysconf(int name)
{
	switch (ut_sysconf_mode) {
	case UT_SYSCONF_VALUE:
		return ut_sysconf_value;
	case UT_SYSCONF_UNKNOWN:
		return -1;
	case UT_SYSCONF_ERRNO:
		errno = EINVAL;
		return -1;
	case UT_SYSCONF_REAL:
	default:
		return sysconf(name);
	}
}

# ifdef __linux__
#  include <sched.h>

static struct {
	int on;
	size_t need;		/* bits of the kernel's mask, EINVAL below */
	int err;		/* errno for a set that is large enough */
	unsigned int ncpu;	/* the CPUs of the answer */
	unsigned int cpu[3];
} ut_aff;

static int ut_fail_cpu_alloc;

static int ut_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
	unsigned int i;

	if (!ut_aff.on)
		return sched_getaffinity(pid, size, set);

	if (size * 8 < ut_aff.need) {
		errno = EINVAL;
		return -1;
	}
	if (ut_aff.err) {
		errno = ut_aff.err;
		return -1;
	}

	memset(set, 0, size);
	for (i = 0; i < ut_aff.ncpu; i++)
		CPU_SET_S(ut_aff.cpu[i], size, set);
	return 0;
}

/* size_t, as the allocator behind the real CPU_ALLOC() takes. */
static void *ut_cpu_alloc(size_t count)
{
	if (ut_fail_cpu_alloc)
		return NULL;
	return CPU_ALLOC(count);
}

/*
 * The online CPU list of a libc other than glibc, which sits between the
 * affinity mask and sysconf(): its open() and read(), for a list that is not
 * there and a read that is interrupted or fails.
 */
#  ifndef __GLIBC__
#   include <fcntl.h>
#   define UT_NCPU_SYSFS_FAKES

static int ut_open_fail;
static int ut_read_eintr;	/* reads still to interrupt */
static int ut_read_fail;

static int ut_open(const char *path, int flags)
{
	if (ut_open_fail) {
		errno = ENOENT;
		return -1;
	}
	return open(path, flags);
}

static ssize_t ut_read(int fd, void *buf, size_t len)
{
	if (ut_read_eintr > 0) {
		ut_read_eintr--;
		errno = EINTR;
		return -1;
	}
	if (ut_read_fail) {
		errno = EIO;
		return -1;
	}
	return read(fd, buf, len);
}

#   define open ut_open
#   define read ut_read
#  endif /* __GLIBC__ */

#  define sched_getaffinity ut_sched_getaffinity
#  undef CPU_ALLOC
#  define CPU_ALLOC(n) ut_cpu_alloc(n)
# endif /* __linux__ */

# define sysconf ut_sysconf
#endif /* JENT_ARCH_CACHE_WINDOWS */

#include "jitterentropy-arch-ncpu.c"

/* CPU_ALLOC stays replaced: the fake forwards unless told to fail. */
#ifdef JENT_ARCH_CACHE_WINDOWS
# undef GetThreadGroupAffinity
# undef GetLogicalProcessorInformationEx
#else
# undef sysconf
# ifdef __linux__
#  undef sched_getaffinity
# endif
# ifdef UT_NCPU_SYSFS_FAKES
#  undef read
#  undef open
# endif
#endif

#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

static void test_ncpu(void)
{
	long ncpu = jent_ncpu();

	jent_ut_group("jent_ncpu");

	/*
	 * A negative value is the documented "cannot tell" answer; anything
	 * else must be a count that makes sense for a machine this code is
	 * running on. "Cannot tell" is itself a failure where the backend
	 * always has an answer: the affinity mask backed by sysconf() on
	 * Linux, the thread affinity backed by the active processor count on
	 * Windows, and sysconf() on macOS.
	 */
	jent_ut_checks++;
#if defined(JENT_ARCH_NCPU_WINDOWS) ||					       \
    defined(JENT_ARCH_NCPU_LINUX_AFFINITY) ||				       \
    (defined(JENT_ARCH_NCPU_POSIX) && defined(__APPLE__))
	if (ncpu < 0)
		JENT_UT_FAIL("jent_ncpu returned %ld where the backend always "
			     "counts", ncpu);
#else
	if (ncpu < 0)
		printf("  note: the CPU count is not discoverable here\n");
#endif
	else if (ncpu < 1)
		JENT_UT_FAIL("jent_ncpu returned %ld", ncpu);
	else
		printf("  note: %ld CPUs\n", ncpu);
}

#ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
/* Answer the affinity query: see ut_aff. */
static void ut_aff_answer(size_t need, int err, const unsigned int *cpu,
			  unsigned int n)
{
	memset(&ut_aff, 0, sizeof(ut_aff));
	ut_aff.on = 1;
	ut_aff.need = need;
	ut_aff.err = err;
	ut_aff.ncpu = n;
	if (n)
		memcpy(ut_aff.cpu, cpu, n * sizeof(*cpu));
}

static void test_ncpu_linux_affinity(void)
{
	/* The set the first query is made with, which is not allocated. */
	const unsigned int fixed = (unsigned int)sizeof(cpu_set_t) * 8;
	static const unsigned int holes[] = { 0, 2, 5 };
	unsigned int cpus[2];
	long count, highest;

	jent_ut_group("the Linux affinity mask");

	/* A set, not a range: the highest CPU is not the count minus one. */
	ut_aff_answer(0, 0, holes, 3);
	JENT_UT_EQ(jent_ncpu(), 3, "a mask with holes counts its CPUs");
	JENT_UT_EQ(jent_cpu_highest(), 5,
		   "and its highest CPU is the last of them");

	cpus[0] = fixed - 1;
	ut_aff_answer(0, 0, cpus, 1);
	JENT_UT_EQ(jent_ncpu(), 1, "a mask of the last CPU of the set counts");
	JENT_UT_EQ(jent_cpu_highest(), (long)fixed - 1,
		   "and that CPU is found");

	/* A kernel mask wider than the default set: the set grows ... */
	cpus[0] = 1;
	cpus[1] = fixed + fixed / 2;
	ut_aff_answer((size_t)fixed * 2, 0, cpus, 2);
	JENT_UT_EQ(jent_ncpu(), 2, "a mask beyond the default set is counted");
	JENT_UT_EQ(jent_cpu_highest(), (long)cpus[1],
		   "and its highest CPU lies beyond that set");

	/* ... up to the largest, whose last CPU is the highest handled ... */
	cpus[0] = JENT_NCPU_SET_MAX - 1;
	ut_aff_answer(JENT_NCPU_SET_MAX, 0, cpus, 1);
	JENT_UT_EQ(jent_cpu_highest(), (long)JENT_NCPU_SET_MAX - 1,
		   "the largest set is reached");

	/* ... and no further. */
	ut_aff_answer((size_t)JENT_NCPU_SET_MAX + 1, 0, cpus, 1);
	JENT_UT_EQ(jent_affinity_mask(&count, &highest), -EINVAL,
		   "a mask beyond the largest set is refused");

	ut_aff_answer(0, EPERM, NULL, 0);
	JENT_UT_EQ(jent_affinity_mask(&count, &highest), -EPERM,
		   "a denied query reports its error");
	ut_aff_answer((size_t)fixed * 2, EPERM, NULL, 0);
	JENT_UT_EQ(jent_affinity_mask(&count, &highest), -EPERM,
		   "and so does one denied once the set has grown");

	ut_aff_answer((size_t)fixed * 2, 0, holes, 3);
	ut_fail_cpu_alloc = 1;
	JENT_UT_EQ(jent_affinity_mask(&count, &highest), -ENOMEM,
		   "no memory for a larger set is reported");
	ut_fail_cpu_alloc = 0;

	/* A mask naming no CPU is read, and answers neither question. */
	ut_aff_answer(0, 0, NULL, 0);
	count = highest = 7;
	JENT_UT_EQ(jent_affinity_mask(&count, &highest), 0,
		   "an empty mask is read");
	JENT_UT_EQ(count, 0, "it counts no CPU");
	JENT_UT_EQ(highest, -1, "and has no highest one");

	ut_aff_answer((size_t)fixed * 2, 0, NULL, 0);
	count = highest = 7;
	JENT_UT_EQ(jent_affinity_mask(&count, &highest), 0,
		   "an empty mask in a grown set is read");
	JENT_UT_EQ(count, 0, "it counts no CPU either");
	JENT_UT_EQ(highest, -1, "and has no highest one");

	ut_aff.on = 0;
}
#else
static void test_ncpu_linux_affinity(void)
{
	JENT_UT_SKIP("the Linux affinity mask", "not the Linux CPU backend");
}
#endif

#ifdef JENT_ARCH_NCPU_POSIX
/*
 * What is left once the affinity mask answers nothing, or where there is
 * none: the system's count, with the CPU numbers taken as its dense range.
 */
static void test_ncpu_fallback(void)
{
	long real = jent_ncpu();
	long ncpu;

	jent_ut_group("the CPU count without an affinity mask");

#ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
	ut_aff_answer(0, EPERM, NULL, 0);
#endif

	ncpu = jent_ncpu();
	JENT_UT_TRUE(ncpu > 0, "the system still counts its CPUs");
	JENT_UT_EQ(jent_cpu_highest(), ncpu - 1,
		   "and the highest CPU is the last of that range");

#ifdef JENT_ARCH_NCPU_LINUX_SYSFS
	/* The online list answered that. Without it, sysconf() is left. */
	ut_open_fail = 1;
#endif
	ut_sysconf_mode = UT_SYSCONF_VALUE;
	ut_sysconf_value = 8;
	JENT_UT_EQ(jent_ncpu(), 8, "the count is what sysconf() reports");
	JENT_UT_EQ(jent_cpu_highest(), 7, "and the highest CPU one below it");

#ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
	ut_aff_answer(0, 0, NULL, 0);
	JENT_UT_EQ(jent_ncpu(), 8, "an empty mask leaves the count to it too");
	JENT_UT_EQ(jent_cpu_highest(), 7, "and the highest CPU");
#endif

	ut_sysconf_value = 0;
	JENT_UT_EQ(jent_ncpu(), -EFAULT, "a count of zero is an error");
	JENT_UT_EQ(jent_cpu_highest(), -EFAULT,
		   "which the highest CPU passes on");

	/* -1 without an errno is "unknown": what errno held is no cause. */
	ut_sysconf_mode = UT_SYSCONF_UNKNOWN;
	errno = ERANGE;
	JENT_UT_EQ(jent_ncpu(), -EFAULT,
		   "an unknown count is an error of its own");

	ut_sysconf_mode = UT_SYSCONF_ERRNO;
	JENT_UT_EQ(jent_ncpu(), -EINVAL,
		   "a failing sysconf() reports its errno");
	JENT_UT_EQ(jent_cpu_highest(), -EINVAL,
		   "which the highest CPU passes on as well");

	ut_sysconf_mode = UT_SYSCONF_REAL;
#ifdef JENT_ARCH_NCPU_LINUX_SYSFS
	ut_open_fail = 0;
#endif

#ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
	ut_aff.on = 0;
#endif
	JENT_UT_EQ(jent_ncpu(), real, "the real count comes back afterwards");
}
#else
static void test_ncpu_fallback(void)
{
	JENT_UT_SKIP("the CPU count without an affinity mask",
		     "not the POSIX CPU backend");
}
#endif

#if defined(JENT_ARCH_NCPU_LINUX_SYSFS)
/* Write @len bytes of @list to @path and return what the reader makes of it. */
static long ut_online_file(const char *path, const char *list, size_t len)
{
	FILE *f = fopen(path, "w");

	if (!f)
		return 0;
	fwrite(list, 1, len, f);
	fclose(f);
	return jent_ncpu_sysfs_file(path);
}

static void test_ncpu_parse(void)
{
	static const struct {
		const char *list;
		long want;
		const char *what;
	} cases[] = {
		{ "0\n",		1,	"a single CPU" },
		{ "0-3\n",		4,	"one contiguous range" },
		{ "0,2-5,8\n",		6,	"ranges with holes" },
		{ "0-0\n",		1,	"a range of one" },
		{ "0-3",		4,	"a list without a newline" },
		{ "0,\n",		1,	"a trailing comma" },
		/* JENT_NCPU_SET_MAX is 65536 CPUs, numbered up to 65535. */
		{ "65535\n",		1,	"the highest CPU a set holds" },
		{ "0-65535\n",		65536,	"the largest set in full" },
		{ "65536\n",		-EINVAL, "a CPU beyond that set" },
		{ "0-65536\n",		-EINVAL, "a range beyond that set" },
		{ "0,1-x\n",		-EINVAL, "a range with no end number" },
		{ "99999999999999999999\n",
					-EINVAL, "a number no long holds" },
		{ "0-99999999999999999999\n",
					-EINVAL, "a range ending in one" },
		{ "\n",		-EINVAL, "an empty list" },
		{ "",			-EINVAL, "no list at all" },
		{ "x\n",		-EINVAL, "a non-numeric list" },
		{ "-1\n",		-EINVAL, "a negative CPU number" },
		{ "3-1\n",		-EINVAL, "a range that runs backwards" },
		{ "0-\n",		-EINVAL, "a range with no end" },
		/*
		 * Parses, but names more CPUs than a signed long can count -
		 * rejected before the width is added rather than overflowed
		 * into it.
		 */
		{ "0-9223372036854775807\n",
					-EINVAL, "a range wider than a CPU set" },
	};
	size_t i;
	char path[] = "/tmp/jent-ncpu-XXXXXX";
	/* As many bytes as one read of the file takes, CPUs 0 and 1. */
	char full[255];
	int fd;

	jent_ut_group("the online CPU list");

	for (i = 0; i < JENT_ARRAY_SIZE(cases); i++)
		JENT_UT_EQ(jent_ncpu_parse_online(cases[i].list), cases[i].want,
			   cases[i].what);

	/* And the reading of it. */
	JENT_UT_TRUE(jent_ncpu_sysfs_file("/nonexistent/jent/online") < 0,
		     "an unreadable list is an error");

	fd = mkstemp(path);
	if (fd < 0) {
		JENT_UT_SKIP("an empty online list", "no temporary file");
		return;
	}
	close(fd);
	JENT_UT_TRUE(jent_ncpu_sysfs_file(path) < 0,
		     "an empty list file is an error");

	{
		FILE *f = fopen(path, "w");

		if (f) {
			fputs("0-7\n", f);
			fclose(f);
			JENT_UT_EQ(jent_ncpu_sysfs_file(path), 8,
				   "a list of eight CPUs is counted");
		}
	}

	/*
	 * A list that fills the read is complete only if it ends there: without
	 * the newline the rest of it was not read, and what was is not counted.
	 */
	memset(full, '0', sizeof(full));
	full[sizeof(full) - 3] = ',';
	full[sizeof(full) - 2] = '1';
	full[sizeof(full) - 1] = '\n';
	JENT_UT_EQ(ut_online_file(path, full, sizeof(full)), 2,
		   "a list filling the read and ending in it is counted");
	JENT_UT_EQ(ut_online_file(path, full, sizeof(full) - 1), 2,
		   "and so is one short of filling it");
	full[sizeof(full) - 1] = '1';
	JENT_UT_EQ(ut_online_file(path, full, sizeof(full)), -EINVAL,
		   "a list cut off by the read is refused");

	ut_read_eintr = 2;
	JENT_UT_EQ(ut_online_file(path, "0-7\n", 4), 8,
		   "an interrupted read is repeated");
	JENT_UT_EQ(ut_read_eintr, 0, "for as often as it is interrupted");
	ut_read_fail = 1;
	JENT_UT_EQ(ut_online_file(path, "0-7\n", 4), -EIO,
		   "a failing read is an error");
	ut_read_fail = 0;

	remove(path);
}
#else
static void test_ncpu_parse(void)
{
	JENT_UT_SKIP("the online CPU list", "not the sysfs CPU backend");
}
#endif

#ifdef JENT_ARCH_NCPU_WINDOWS
/* The flat CPU number of @bit in @group, or -1 when no CPU has it. */
static long ut_flat_cpu(unsigned short group, unsigned int bit)
{
	DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	unsigned long cpu;

	for (cpu = 0; cpu < n; cpu++) {
		unsigned short g;
		unsigned int b;

		if (!jent_cpu_to_group(cpu, &g, &b) && g == group && b == bit)
			return (long)cpu;
	}
	return -1;
}

/*
 * Under a narrowed process affinity mask the CPU count and the highest CPU
 * follow the mask, and the pin refuses a CPU outside it, which
 * SetThreadGroupAffinity() alone would move the thread to. The mask is
 * restored before returning.
 */
static void test_ncpu_windows_affinity(void)
{
	HANDLE proc = GetCurrentProcess();
	DWORD_PTR pmask, smask;
	GROUP_AFFINITY ga;
	unsigned int lo, hi, bits = 0;
	long flat_lo, flat_hi;

	jent_ut_group("the Windows CPU count under a process affinity mask");

	if (!GetProcessAffinityMask(proc, &pmask, &smask) || !pmask ||
	    !GetThreadGroupAffinity(GetCurrentThread(), &ga)) {
		JENT_UT_SKIP("the affinity mask", "it cannot be read");
		return;
	}

	for (lo = 0; !((pmask >> lo) & 1); lo++)
		;
	for (hi = (unsigned int)(sizeof(pmask) * 8) - 1; !((pmask >> hi) & 1);
	     hi--)
		;
	for (smask = pmask; smask; smask &= smask - 1)
		bits++;

	JENT_UT_EQ(jent_ncpu(), (long)bits,
		   "the count is the CPUs the process may run on");

	if (bits < 2) {
		JENT_UT_SKIP("a narrowed mask", "the process has one CPU");
		return;
	}

	flat_lo = ut_flat_cpu(ga.Group, lo);
	flat_hi = ut_flat_cpu(ga.Group, hi);
	JENT_UT_TRUE(flat_lo >= 0 && flat_hi > flat_lo,
		     "both ends of the mask have a CPU number");
	JENT_UT_EQ(jent_cpu_highest(), flat_hi,
		   "the highest CPU is the top of the mask");

	if (!SetProcessAffinityMask(proc, (DWORD_PTR)1 << lo)) {
		JENT_UT_SKIP("a narrowed mask", "the mask cannot be changed");
		return;
	}

	JENT_UT_EQ(jent_ncpu(), 1, "confined to one CPU, the count is one");
	JENT_UT_EQ(jent_cpu_highest(), flat_lo,
		   "and the highest CPU is that one");

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	JENT_UT_EQ(jent_thread_pin_to_cpu((unsigned long)flat_hi), -EINVAL,
		   "a CPU outside the process affinity is refused");
	JENT_UT_TRUE(GetThreadGroupAffinity(GetCurrentThread(), &ga) &&
		     ga.Mask == ((KAFFINITY)1 << lo),
		     "and the thread stays inside it");
	JENT_UT_EQ(jent_thread_pin_to_cpu((unsigned long)flat_lo), 0,
		   "the CPU inside it is pinned to");
#endif

	JENT_UT_TRUE(SetProcessAffinityMask(proc, pmask),
		     "the process affinity mask is restored");
	JENT_UT_EQ(jent_ncpu(), (long)bits, "and the count follows it back");
}
/*
 * Build a RelationGroup record of @n groups with the given active masks, and
 * declare @claim groups in it - more than @n makes the record too short for
 * the array it announces.
 */
static void ut_groups_set(const KAFFINITY *masks, WORD n, WORD claim)
{
	const size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
				    Group) +
			   offsetof(GROUP_RELATIONSHIP, GroupInfo);
	PROCESSOR_GROUP_INFO *gi;
	WORD g;

	memset(&ut_groups, 0, sizeof(ut_groups));
	ut_groups.rec.Relationship = RelationGroup;
	ut_groups.rec.Size = (DWORD)(hdr + n * sizeof(PROCESSOR_GROUP_INFO));
	ut_groups.rec.Group.MaximumGroupCount = claim;
	ut_groups.rec.Group.ActiveGroupCount = claim;

	gi = (PROCESSOR_GROUP_INFO *)(ut_groups.raw + hdr);
	for (g = 0; g < n; g++) {
		KAFFINITY m;
		BYTE count = 0;

		for (m = masks[g]; m; m &= m - 1)
			count++;
		gi[g].MaximumProcessorCount = (BYTE)(sizeof(KAFFINITY) * 8);
		gi[g].ActiveProcessorCount = count;
		gi[g].ActiveProcessorMask = masks[g];
	}

	ut_groups_len = ut_groups.rec.Size;
	ut_groups_fake = 1;
}

/*
 * The flat numbering across processor groups: the active processors of all
 * groups in group order, a gap in a mask taking no number. Group 0 has bits 0,
 * 1 and 3 active (CPUs 0-2, bit 2 parked), group 1 bits 1 and 2 (CPUs 3-4).
 */
static void test_ncpu_windows_groups(void)
{
	static const KAFFINITY masks[] = { 0xb, 0x6 };
	static const struct {
		unsigned long cpu;
		unsigned short group;
		unsigned int bit;
	} map[] = {
		{ 0, 0, 0 }, { 1, 0, 1 }, { 2, 0, 3 }, { 3, 1, 1 }, { 4, 1, 2 },
	};
	unsigned short group;
	unsigned int bit;
	long count, highest;
	size_t i;

	jent_ut_group("the Windows CPU numbering across processor groups");

	ut_groups_set(masks, 2, 2);

	for (i = 0; i < JENT_ARRAY_SIZE(map); i++) {
		group = 0xffff;
		bit = ~0U;
		JENT_UT_TRUE(!jent_cpu_to_group(map[i].cpu, &group, &bit) &&
			     group == map[i].group && bit == map[i].bit,
			     "a flat CPU number resolves to its group and bit");
	}
	JENT_UT_EQ(jent_cpu_to_group(5, &group, &bit), -EINVAL,
		   "a CPU beyond the last group has none");

	/* A thread in group 1: its CPUs are numbered after all of group 0's. */
	ut_thread_ga_fake = 1;
	memset(&ut_thread_ga, 0, sizeof(ut_thread_ga));
	ut_thread_ga.Group = 1;
	ut_thread_ga.Mask = 0x6;
	count = highest = 0;
	JENT_UT_EQ(jent_ncpu_thread_affinity(&count, &highest), 0,
		   "the affinity of a thread in the second group is read");
	JENT_UT_EQ(count, 2, "its count is its group's CPUs");
	JENT_UT_EQ(highest, 4, "and its highest CPU is numbered past group 0");
	JENT_UT_EQ(jent_ncpu(), 2, "jent_ncpu() gives that count");
	JENT_UT_EQ(jent_cpu_highest(), 4, "jent_cpu_highest() that CPU");

	/* Affinity bits for processors that are not active count for nothing. */
	ut_thread_ga.Mask = 0x9;
	JENT_UT_EQ(jent_ncpu_thread_affinity(&count, &highest), 0,
		   "a mask naming inactive processors is read");
	JENT_UT_EQ(count, 0, "and holds none of the active ones");
	JENT_UT_EQ(highest, -1, "so it has no highest CPU");

	ut_thread_ga.Group = 0;
	ut_thread_ga.Mask = 0xf;
	JENT_UT_EQ(jent_ncpu_thread_affinity(&count, &highest), 0,
		   "a thread in the first group, across its gap, is read");
	JENT_UT_EQ(count, 3, "the parked processor is not counted");
	JENT_UT_EQ(highest, 2, "and takes no number");

	ut_thread_ga.Group = 2;
	JENT_UT_EQ(jent_ncpu_thread_affinity(&count, &highest), -EFAULT,
		   "a group the layout does not have is refused");
	ut_thread_ga_fake = 0;

	/* A record announcing more groups than it holds. */
	ut_groups_set(masks, 2, 3);
	JENT_UT_EQ(jent_cpu_to_group(0, &group, &bit), -EFAULT,
		   "a group array longer than its record is refused");

	/* A record of another relationship, and ones cut short. */
	ut_groups_set(masks, 2, 2);
	ut_groups.rec.Relationship = RelationProcessorCore;
	JENT_UT_EQ(jent_cpu_to_group(0, &group, &bit), -EFAULT,
		   "a record that is not the group layout is refused");

	ut_groups_set(masks, 2, 2);
	ut_groups.rec.Size = (DWORD)offsetof(
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Group);
	JENT_UT_EQ(jent_cpu_to_group(0, &group, &bit), -EFAULT,
		   "a record too short for the group header is refused");

	ut_groups_set(masks, 2, 2);
	ut_groups_len -= 1;
	JENT_UT_EQ(jent_cpu_to_group(0, &group, &bit), -EFAULT,
		   "a record longer than what was returned is refused");

	ut_groups_set(masks, 2, 2);
	ut_groups_len = 4;
	JENT_UT_EQ(jent_cpu_to_group(0, &group, &bit), -EFAULT,
		   "a reply shorter than a record header is refused");

	ut_groups_fake = 0;
}

/*
 * Without a thread affinity to read, or with one that holds no active
 * processor, the machine's processors are counted and numbered densely.
 */
static void test_ncpu_windows_fallback(void)
{
	static const KAFFINITY masks[] = { 0xb, 0x6 };
	long all = (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
	long count, highest;

	jent_ut_group("the Windows CPU count without a thread affinity");

	ut_thread_ga_fake = 2;
	JENT_UT_EQ(jent_ncpu_thread_affinity(&count, &highest), -EFAULT,
		   "a thread affinity that cannot be read is an error");
	JENT_UT_EQ(jent_ncpu(), all,
		   "the count is then the machine's active processors");
	JENT_UT_EQ(jent_cpu_highest(), all - 1,
		   "and the highest CPU the last of them");

	/* An affinity naming only inactive processors of its group. */
	ut_groups_set(masks, 2, 2);
	ut_thread_ga_fake = 1;
	memset(&ut_thread_ga, 0, sizeof(ut_thread_ga));
	ut_thread_ga.Group = 1;
	ut_thread_ga.Mask = 0x9;
	JENT_UT_EQ(jent_ncpu(), all,
		   "an affinity without an active processor falls back too");
	JENT_UT_EQ(jent_cpu_highest(), all - 1, "as does the highest CPU");

	ut_thread_ga_fake = 0;
	ut_groups_fake = 0;
}
#else
static void test_ncpu_windows_affinity(void)
{
	JENT_UT_SKIP("the Windows CPU count under a process affinity mask",
		     "not the Windows CPU backend");
}

static void test_ncpu_windows_groups(void)
{
	JENT_UT_SKIP("the Windows CPU numbering across processor groups",
		     "not the Windows CPU backend");
}

static void test_ncpu_windows_fallback(void)
{
	JENT_UT_SKIP("the Windows CPU count without a thread affinity",
		     "not the Windows CPU backend");
}
#endif

/* The CSPRNG read behind the UUID, against files with known behaviour. */

int main(void)
{
	test_ncpu();
	test_ncpu_linux_affinity();
	test_ncpu_fallback();
	test_ncpu_parse();
	test_ncpu_windows_affinity();
	test_ncpu_windows_groups();
	test_ncpu_windows_fallback();

	return jent_ut_report("unit-arch-ncpu");
}
