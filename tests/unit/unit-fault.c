/*
 * Jitter RNG: fault injection tests for the failure paths of src/ and arch/
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
 * The library's failure paths - those taken when an allocation or a thread
 * creation does not succeed - are unreachable on a healthy machine and so
 * covered by no other test, while a defect in them is worst: a leak, a double
 * free or a half-built collector, in code that only runs when the system is
 * already under stress.
 *
 * The allocator is interposed rather than the library being given a test hook.
 * These programs absorb the library sources (see CMakeLists.txt here), so
 * renaming jent_zalloc() and jent_zalloc_unlocked() through the preprocessor
 * while arch/jitterentropy-arch-memory.c is compiled hides those definitions
 * under private names and lets this file supply both itself. Every absorbed
 * caller reaches the interposed one, and the shipped library carries no
 * testing conditional.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <stdlib.h>

/*
 * Which system calls there are to interpose is decided inside the arch/
 * sources, and the renaming has to be in place before they are included - so
 * their platform selection cannot be read here, only repeated. This is the
 * first line of arch/jitterentropy-arch-memory.c's dispatch and of every other
 * one in arch/: Windows has no mmap()/mlock() and no sysconf(), and its
 * backends are built on the Win32 calls instead.
 */
#if defined(_MSC_VER) || defined(__MINGW32__)
# define FI_WINDOWS
#endif

/*
 * The kernel calls the secure allocator makes, interposed the same way. They
 * are what fails when the machine is out of memory or will not lock any more
 * of it - RLIMIT_MEMLOCK in a container, the working set quota on Windows -
 * and none of those failures can be produced by asking nicely.
 *
 * The system headers are included before the names are taken over, so the
 * declarations the redirections shadow are already in place.
 */
#ifdef FI_WINDOWS
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif

/*
 * One set of switches for both backends: the mapping call, the protection
 * call and the memory lock, whatever the platform names them.
 */
static int fi_fail_mmap;
static int fi_fail_mprotect;
static unsigned int fi_mprotect_calls;
static int fi_fail_mlock;
static unsigned int fi_mlock_calls;

/*
 * A per-call lock quota in bytes, zero for none. Enough to tell the memory
 * access region from the state around it.
 */
static size_t fi_mlock_quota;

#ifdef FI_WINDOWS

static JENT_UT_MAYBE_UNUSED LPVOID fi_VirtualAlloc(LPVOID addr, SIZE_T len,
						   DWORD type, DWORD protect)
{
	if (fi_fail_mmap) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return NULL;
	}
	return VirtualAlloc(addr, len, type, protect);
}

static JENT_UT_MAYBE_UNUSED BOOL fi_VirtualProtect(LPVOID addr, SIZE_T len,
						   DWORD protect, PDWORD old)
{
	fi_mprotect_calls++;
	if (fi_fail_mprotect &&
	    (unsigned int)fi_fail_mprotect == fi_mprotect_calls) {
		SetLastError(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	return VirtualProtect(addr, len, protect, old);
}

/*
 * The one refusal that is expected on a healthy machine: VirtualLock() charges
 * its pages against the process minimum working set and reports
 * ERROR_WORKING_SET_QUOTA once that budget is gone. Unlike mlock() there is no
 * second class of failure to distinguish, so the allocator tolerates every one
 * of them unless the caller demanded secure memory - which is why this needs
 * no equivalent of fi_mlock_errno below.
 */
static JENT_UT_MAYBE_UNUSED BOOL fi_VirtualLock(LPVOID addr, SIZE_T len)
{
	fi_mlock_calls++;
	if (fi_fail_mlock || (fi_mlock_quota && len > fi_mlock_quota)) {
		SetLastError(ERROR_WORKING_SET_QUOTA);
		return FALSE;
	}
	return VirtualLock(addr, len);
}

#else /* FI_WINDOWS */

static int fi_mlock_errno = EPERM;

static JENT_UT_MAYBE_UNUSED void *fi_mmap(void *addr, size_t len, int prot,
					  int flags, int fd, off_t off)
{
	if (fi_fail_mmap) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return mmap(addr, len, prot, flags, fd, off);
}

static JENT_UT_MAYBE_UNUSED int fi_mprotect(void *addr, size_t len, int prot)
{
	fi_mprotect_calls++;
	if (fi_fail_mprotect &&
	    (unsigned int)fi_fail_mprotect == fi_mprotect_calls) {
		errno = EACCES;
		return -1;
	}
	return mprotect(addr, len, prot);
}

static JENT_UT_MAYBE_UNUSED int fi_mlock(const void *addr, size_t len)
{
	fi_mlock_calls++;
	if (fi_fail_mlock) {
		errno = fi_mlock_errno;
		return -1;
	}
	if (fi_mlock_quota && len > fi_mlock_quota) {
		errno = ENOMEM;
		return -1;
	}
	return mlock(addr, len);
}

/*
 * sysconf(), the source of the POSIX backends' page size, CPU count and cache
 * sizes, each with a documented "cannot tell" reply the backends have to
 * handle and that no machine where the query works produces.
 *
 * Defined here rather than with the other system queries below because the
 * allocator is the first source that calls it: a redirection that only comes
 * into force after jitterentropy-arch-memory.c has been compiled leaves
 * jent_pagesize() on the real sysconf(), and the page size fallback checks
 * then pass whatever the fallback does.
 */
enum fi_sysconf_mode {
	FI_SYSCONF_REAL = 0,
	FI_SYSCONF_FAIL,	/* -1, the "unknown" reply */
	FI_SYSCONF_ZERO,	/* 0, which is not a usable count */
	FI_SYSCONF_HUGE,	/* more CPUs than any topology has */
};

static enum fi_sysconf_mode fi_sysconf_mode;

static long fi_sysconf_real(int name)
{
	return sysconf(name);
}

static long fi_sysconf(int name)
{
	switch (fi_sysconf_mode) {
	case FI_SYSCONF_FAIL:
		errno = EINVAL;
		return -1;
	case FI_SYSCONF_ZERO:
		return 0;
	case FI_SYSCONF_HUGE:
		return 1L << 20;
	case FI_SYSCONF_REAL:
	default:
		return fi_sysconf_real(name);
	}
}

#endif /* FI_WINDOWS */

/*
 * Compile the real allocator under a private name, with its kernel calls
 * redirected. The header it includes declares jent_zalloc() and
 * jent_zalloc_unlocked(), which are renamed with them, so the declarations and
 * the definitions still agree.
 */
#define jent_zalloc jent_fi_real_zalloc
#define jent_zalloc_unlocked jent_fi_real_zalloc_unlocked
#ifdef FI_WINDOWS
# define VirtualAlloc fi_VirtualAlloc
# define VirtualProtect fi_VirtualProtect
# define VirtualLock fi_VirtualLock
#else
# define mmap fi_mmap
# define mprotect fi_mprotect
# define mlock fi_mlock
# define sysconf fi_sysconf
#endif
/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-memory.c"
#ifdef FI_WINDOWS
# undef VirtualLock
# undef VirtualProtect
# undef VirtualAlloc
#else
# undef sysconf
# undef mlock
# undef mprotect
# undef mmap
#endif
#undef jent_zalloc_unlocked
#undef jent_zalloc

/*
 * Fail the n-th allocation from now on, counting from 1. Zero disables the
 * injection. Only one allocation is failed per arming, so that the collector
 * is built up to a chosen point and only then denied its next allocation -
 * which is what walks the cleanup paths one stage at a time. Both allocators
 * count, the memory access region being one of the stages.
 */
static unsigned int fi_fail_alloc;
static unsigned int fi_alloc_count;

static int fi_deny_alloc(void)
{
	fi_alloc_count++;

	return fi_fail_alloc && fi_alloc_count == fi_fail_alloc;
}

void *jent_zalloc(size_t len, unsigned int flags)
{
	if (fi_deny_alloc())
		return NULL;

	return jent_fi_real_zalloc(len, flags);
}

void *jent_zalloc_unlocked(size_t len)
{
	if (fi_deny_alloc())
		return NULL;

	return jent_fi_real_zalloc_unlocked(len);
}

static void fi_arm(unsigned int nth)
{
	fi_fail_alloc = nth;
	fi_alloc_count = 0;
}

static void fi_disarm(void)
{
	fi_fail_alloc = 0;
	fi_alloc_count = 0;
}

/* How many allocations an operation makes when nothing is denied. */
static unsigned int fi_count_allocs(void (*op)(void))
{
	fi_disarm();
	op();
	return fi_alloc_count;
}

/*
 * The system queries the platform backends build their answers from. Each has
 * a documented "cannot tell" reply that the backends have to handle, and none
 * of those replies can be produced on a machine where the query works.
 *
 * The real ones are captured in wrappers defined before the names are taken
 * over, so the fakes can still forward.
 *
 * sysconf() and the affinity query are the POSIX backends' sources. On
 * Windows they are GetThreadGroupAffinity() and GetActiveProcessorCount(),
 * which fail with FALSE and with a count of zero.
 */
#ifndef FI_WINDOWS
#include <sched.h>
#endif
/*
 * The same glibc floor arch/jitterentropy-arch-random.c tests before reaching
 * for the header: <sys/random.h> and the getrandom() wrapper are glibc 2.25,
 * and on anything older the include is a build failure rather than a link
 * error. Below it the UUID backend takes its /dev/urandom path, where there is
 * no getrandom() call left to interpose, so the cases guarded by
 * FI_HAVE_GETRANDOM have nothing to do anyway.
 */
#if defined(__linux__) && defined(__GLIBC__) && \
    (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
# include <sys/random.h>
# define FI_HAVE_GETRANDOM
#endif

#ifndef FI_WINDOWS
/* fi_sysconf() itself is defined above, ahead of the allocator. */
static int fi_fail_affinity;
static int fi_empty_affinity;
#else /* FI_WINDOWS */

static int fi_fail_affinity;
static int fi_empty_affinity;
static int fi_fail_processor_count;

static BOOL fi_GetThreadGroupAffinity_real(HANDLE thread, PGROUP_AFFINITY ga)
{
	return GetThreadGroupAffinity(thread, ga);
}

static BOOL fi_GetThreadGroupAffinity(HANDLE thread, PGROUP_AFFINITY ga)
{
	if (fi_fail_affinity) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (!fi_GetThreadGroupAffinity_real(thread, ga))
		return FALSE;
	/* Succeeds, but names no CPU - not a usable count either. */
	if (fi_empty_affinity)
		ga->Mask = 0;
	return TRUE;
}

static DWORD fi_GetActiveProcessorCount_real(WORD group)
{
	return GetActiveProcessorCount(group);
}

static DWORD fi_GetActiveProcessorCount(WORD group)
{
	/* Zero is the documented failure reply. */
	if (fi_fail_processor_count)
		return 0;
	return fi_GetActiveProcessorCount_real(group);
}
#endif /* FI_WINDOWS */

/*
 * The errno a failing getrandom() reports, zero for the real call. Which one
 * decides the fallback: ENOSYS goes on to /dev/urandom, EAGAIN - a pool that
 * is not ready yet - does not.
 */
static int fi_fail_getrandom;

#ifdef __linux__
static int fi_sched_getaffinity_real(pid_t pid, size_t size, cpu_set_t *set)
{
	return sched_getaffinity(pid, size, set);
}

/* A kernel CPU mask wider than a cpu_set_t: a set of that size is too small. */
static int fi_small_affinity;

static int fi_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
	if (fi_fail_affinity) {
		errno = EPERM;
		return -1;
	}
	if (fi_small_affinity && size <= sizeof(cpu_set_t)) {
		errno = EINVAL;
		return -1;
	}
	if (fi_empty_affinity) {
		/* Succeeds, but names no CPU - not a usable count either. */
		memset(set, 0, size);
		return 0;
	}
	return fi_sched_getaffinity_real(pid, size, set);
}
#endif

#ifdef FI_HAVE_GETRANDOM
static ssize_t fi_getrandom_real(void *buf, size_t len, unsigned int flags)
{
	return getrandom(buf, len, flags);
}

static ssize_t fi_getrandom(void *buf, size_t len, unsigned int flags)
{
	if (fi_fail_getrandom) {
		errno = fi_fail_getrandom;
		return -1;
	}
	return fi_getrandom_real(buf, len, flags);
}
#endif

/*
 * The kernel FIPS indicator. Whether the machine has it on decides two
 * branches in the collector setup, and it is not something a test can turn on.
 */
static int fi_force_fips_enabled;

/*
 * The CPU-set allocation the affinity query needs. CPU_ALLOC is a macro over
 * an allocator, so a machine that is out of memory at that moment is the only
 * way its failure path runs.
 */
#if defined(__linux__) && defined(CPU_ALLOC)
static int fi_fail_cpu_alloc;

/*
 * size_t, as __sched_cpualloc() behind the real CPU_ALLOC() takes: the callers
 * pass an unsigned count, and an int parameter would make every one of them a
 * signedness conversion.
 */
static void *fi_cpu_alloc(size_t count)
{
	if (fi_fail_cpu_alloc)
		return NULL;
	return CPU_ALLOC(count);
}
# define FI_HAVE_CPU_ALLOC
#endif

#ifndef FI_WINDOWS
# define sysconf fi_sysconf
#endif
#ifdef __linux__
# define sched_getaffinity fi_sched_getaffinity
#endif
#ifdef FI_HAVE_CPU_ALLOC
# undef CPU_ALLOC
# define CPU_ALLOC(n) fi_cpu_alloc(n)
#endif
#ifdef FI_HAVE_GETRANDOM
# define getrandom fi_getrandom
#endif

/*
 * The two things the timer-less mode depends on and cannot do anything about:
 * how many CPUs there are, and whether a thread can be created. Interposed
 * ahead of the sources that call them, by the same renaming. A machine with
 * one CPU and a machine that has run out of threads are both configurations
 * the library has to handle and neither is one a test can be run on.
 */
#ifdef FI_WINDOWS
# define GetThreadGroupAffinity fi_GetThreadGroupAffinity
# define GetActiveProcessorCount fi_GetActiveProcessorCount
#endif
#define jent_ncpu jent_fi_real_ncpu
#include "jitterentropy-arch-ncpu.c"
#undef jent_ncpu
#ifdef FI_WINDOWS
# undef GetActiveProcessorCount
# undef GetThreadGroupAffinity
#endif

/*
 * The whole thread back-end - the context type, the start routine type and
 * every function over them - is behind JENT_CONF_ENABLE_INTERNAL_TIMER, in
 * arch/jitterentropy-arch-thread.h as well as in the source. With the option
 * off there is no thread creation left to fail, so the interposition and the
 * override below are compiled out with it rather than referring to types that
 * this configuration does not declare.
 */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
#define jent_notime_thread_create jent_fi_real_thread_create
#include "jitterentropy-arch-thread.c"
#undef jent_notime_thread_create
#else
#include "jitterentropy-arch-thread.c"
#endif

/*
 * The time source, interposed for the same reason. Everything the startup
 * self test decides - that the timer is absent, too coarse, not monotonic, or
 * produces nothing but stuck measurements - it decides from what this returns,
 * and a machine whose timer is any of those things is one the library refuses
 * to run on at all.
 */
#define jent_get_nstime jent_fi_real_get_nstime
#include "jitterentropy-arch-timer.c"
#undef jent_get_nstime

enum fi_time_mode {
	FI_TIME_REAL = 0,	/* forward to the platform */
	FI_TIME_ZERO,		/* a counter that never leaves zero */
	FI_TIME_CONSTANT,	/* a counter that does not move */
	FI_TIME_BACKWARDS,	/* a counter that runs down by a fixed amount */
	FI_TIME_FIXED_STEP,	/* moves up by the same amount every time */
};

static enum fi_time_mode fi_time;
static uint64_t fi_time_value;


void jent_get_nstime(uint64_t *out)
{
	switch (fi_time) {
	case FI_TIME_ZERO:
		*out = 0;
		return;
	case FI_TIME_CONSTANT:
		*out = 0x4242424242424242ULL;
		return;
	case FI_TIME_BACKWARDS:
		fi_time_value -= 4096;
		*out = fi_time_value;
		return;
	case FI_TIME_FIXED_STEP:
		fi_time_value += 4096;
		*out = fi_time_value;
		return;
	case FI_TIME_REAL:
	default:
		jent_fi_real_get_nstime(out);
		return;
	}
}

static void fi_time_set(enum fi_time_mode mode)
{
	fi_time = mode;
	fi_time_value = 0x8000000000000000ULL;
}

/* Negative reports the count as undiscoverable; 0 forwards to the real one. */
static long fi_ncpu;

long jent_ncpu(void)
{
	if (fi_ncpu)
		return fi_ncpu;
	return jent_fi_real_ncpu();
}

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
static int fi_fail_thread_create;
/* Thread creations granted before fi_fail_thread_create denies them. */
static unsigned int fi_thread_create_grant;

int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine start_routine,
			      void *arg)
{
	if (fi_fail_thread_create) {
		if (!fi_thread_create_grant)
			return -EAGAIN;
		fi_thread_create_grant--;
	}
	return jent_fi_real_thread_create(ctx, start_routine, arg);
}
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

int jent_fips_enabled(void);

#include "jitterentropy-sha3.c"
#include "jitterentropy-gcd.c"
#include "jitterentropy-health.c"
#include "jitterentropy-noise.c"
#include "jitterentropy-timer.c"
#include "jitterentropy-base.c"
#include "jitterentropy-uuid.c"
#include "jitterentropy-status.c"

#include "jitterentropy-arch-cache.c"
/* The Windows FIPS policy query, faked to report "on" or to fail. */
#if defined(FI_WINDOWS) && !defined(LIBGCRYPT) && !defined(AWSLC) && \
    !defined(OPENSSL)
# include <bcrypt.h>
# define FI_HAVE_BCRYPT_FIPS

enum fi_bcrypt_fips_mode {
	FI_BCRYPT_FIPS_REAL = 0,
	FI_BCRYPT_FIPS_FAIL,	/* an error status, with TRUE written anyway */
	FI_BCRYPT_FIPS_ON,	/* the policy is enabled */
};

static enum fi_bcrypt_fips_mode fi_bcrypt_fips_mode;

static NTSTATUS fi_BCryptGetFipsAlgorithmMode_real(BOOLEAN *enabled)
{
	return BCryptGetFipsAlgorithmMode(enabled);
}

static NTSTATUS fi_BCryptGetFipsAlgorithmMode(BOOLEAN *enabled)
{
	switch (fi_bcrypt_fips_mode) {
	case FI_BCRYPT_FIPS_FAIL:
		*enabled = TRUE;
		return (NTSTATUS)0xC0000001L;	/* STATUS_UNSUCCESSFUL */
	case FI_BCRYPT_FIPS_ON:
		*enabled = TRUE;
		return 0;
	case FI_BCRYPT_FIPS_REAL:
	default:
		return fi_BCryptGetFipsAlgorithmMode_real(enabled);
	}
}
# define BCryptGetFipsAlgorithmMode fi_BCryptGetFipsAlgorithmMode
#endif

/*
 * The rename is scoped to this one include: the sources above call
 * jent_fips_enabled() and must reach the override below, not the real one.
 */
#define jent_fips_enabled jent_fi_real_fips_enabled
#include "jitterentropy-arch-fips.c"
#undef jent_fips_enabled
#ifdef FI_HAVE_BCRYPT_FIPS
# undef BCryptGetFipsAlgorithmMode
#endif

int jent_fips_enabled(void)
{
	if (fi_force_fips_enabled)
		return 1;
	return jent_fi_real_fips_enabled();
}
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-random.c"

#ifndef FI_WINDOWS
# undef sysconf
#endif
#ifdef __linux__
# undef sched_getaffinity
#endif
#ifdef FI_HAVE_GETRANDOM
# undef getrandom
#endif

/* Confirms the interposition is in the path at all. */
static void test_injection_works(void)
{
	void *p;

	jent_ut_group("the allocator interposition");

	fi_arm(1);
	p = jent_zalloc(64, 0);
	JENT_UT_TRUE(p == NULL, "the armed allocation fails");

	p = jent_zalloc(64, 0);
	JENT_UT_TRUE(p != NULL, "and only that one");
	jent_zfree(p, 64);

	fi_disarm();
	p = jent_zalloc(64, 0);
	JENT_UT_TRUE(p != NULL, "disarming restores the allocator");
	jent_zfree(p, 64);
}

static unsigned int alloc_flags;

/*
 * Whether the startup self test passes on the platform clock alone, with
 * JENT_DISABLE_INTERNAL_TIMER. main() finds out. A clock the startup rejects
 * - too coarse, say - is the machine's verdict, and a build with the internal
 * timer still initializes through it, so the program runs; but the cases
 * pinned to the platform clock, and the allocation counts that assume the
 * collector is on it, have nothing to measure there.
 */
static int platform_clock = 1;

static void op_collector_alloc(void)
{
	jent_entropy_collector_free(jent_entropy_collector_alloc(0, alloc_flags));
}

/*
 * Deny each allocation the collector makes, one at a time. Whichever one is
 * denied, the result has to be the same: NULL to the caller and nothing left
 * behind. Run under a leak sanitizer this is also what says the partially
 * built state is released rather than dropped.
 */
static void test_collector_alloc_failures(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
	} configs[] = {
		{ 0,				"default" },
		{ JENT_DISABLE_MEMORY_ACCESS,	"no memory access" },
		{ JENT_FORCE_FIPS,		"FIPS mode" },
		{ JENT_MAX_MEMSIZE_1MB,		"a fixed memory size" },
	};
	size_t c;

	jent_ut_group("every allocation of the collector is denied in turn");

	for (c = 0; c < sizeof(configs) / sizeof(configs[0]); c++) {
		unsigned int total, nth, survived = 0, usable = 0, expect;

		alloc_flags = configs[c].flags;
		total = fi_count_allocs(op_collector_alloc);

		if (!total) {
			JENT_UT_SKIP(configs[c].name, "no collector to allocate");
			continue;
		}

		/*
		 * Where secure memory is forced and the machine will not lock
		 * any, the locked allocation is the first and fails: the
		 * sequence ends there.
		 */
		if (JENT_UT_SECURE_MEM_FORCED(configs[c].flags) &&
		    !jent_ut_memlock_available()) {
			JENT_UT_SKIP(configs[c].name,
				     "no lockable memory on this machine");
			continue;
		}

		/*
		 * What the configuration implies: the struct rand_data, and
		 * the memory access region unless that noise source is turned
		 * off. Asserted rather than printed, so that an allocation
		 * appearing or disappearing - a denial arm that then walks
		 * past the end of the sequence and tests nothing - is noticed
		 * here and not in a number nobody reads.
		 */
		expect = (configs[c].flags & JENT_DISABLE_MEMORY_ACCESS) ?
			 1 : 2;

		for (nth = 1; nth <= total; nth++) {
			struct rand_data *ec;
			char buf[32];

			fi_arm(nth);
			ec = jent_entropy_collector_alloc(0, configs[c].flags);
			fi_disarm();

			if (!ec)
				continue;

			/*
			 * Not a failure in itself: an allocation the collector
			 * can do without (the GCD history of the startup test,
			 * say) leaves a usable collector. But usable is the
			 * claim, and it has to be made good on - a half-built
			 * collector handed back to the caller would land here
			 * just the same, and used to be counted as a pass.
			 */
			survived++;
			if ((!(configs[c].flags & JENT_DISABLE_MEMORY_ACCESS) &&
			     !ec->mem))
				printf("  %s: the collector surviving denial "
				       "%u of %u has no memory access region\n",
				       configs[c].name, nth, total);
			else if (jent_read_entropy(ec, buf, sizeof(buf)) !=
				 (ssize_t)sizeof(buf))
				printf("  %s: the collector surviving denial "
				       "%u of %u produces no entropy\n",
				       configs[c].name, nth, total);
			else
				usable++;

			jent_entropy_collector_free(ec);
		}

		printf("  %-22s %2u allocations, %u survived a denial\n",
		       configs[c].name, total, survived);

		/*
		 * A collector on the internal timer, because the platform
		 * clock is rejected, allocates its counting thread's context
		 * besides.
		 */
		if (platform_clock)
			JENT_UT_EQ(total, expect,
				   "the collector makes the allocations its "
				   "configuration implies");
		else
			JENT_UT_SKIP(configs[c].name,
				     "the startup rejects the platform clock, "
				     "so the counts include the internal "
				     "timer's");
		JENT_UT_EQ(usable, survived,
			   "a denied allocation gives back NULL or a working "
			   "collector, never a half-built one");
	}
}

/*
 * The same for the startup self test, which allocates its own collector and a
 * GCD history and has to release both on every exit.
 */
static void op_time_entropy_init(void)
{
	(void)jent_time_entropy_init(JENT_MIN_OSR,
				     JENT_DISABLE_INTERNAL_TIMER);
}

static void test_init_failures(void)
{
	unsigned int nth, total, emem = 0, other = 0, swallowed = 0;

	jent_ut_group("the startup self test under allocation failure");

	/*
	 * Exactly as many arms as the startup makes allocations. A fixed
	 * range covered the ones past the end with nothing at all - the
	 * denial never fired - and every one of those counted as a pass,
	 * which is also how the loop below would have read a denial the
	 * startup swallowed and reported success for.
	 */
	total = fi_count_allocs(op_time_entropy_init);
	JENT_UT_NE(total, 0, "the startup self test allocates");

	for (nth = 1; nth <= total; nth++) {
		int ret;

		fi_arm(nth);
		ret = jent_time_entropy_init(JENT_MIN_OSR,
					     JENT_DISABLE_INTERNAL_TIMER);
		fi_disarm();

		if (ret == EMEM)
			emem++;
		else if (ret)
			other++;
		else
			swallowed++;
	}

	JENT_UT_EQ(emem, total, "every denied allocation is reported as EMEM");
	JENT_UT_EQ(other, 0, "and never as some other failure");
	JENT_UT_EQ(swallowed, 0, "and never reported as success");

	/* And that it still passes once nothing is denied. */
	fi_disarm();
	if (platform_clock)
		JENT_UT_EQ(jent_time_entropy_init(JENT_MIN_OSR,
						  JENT_DISABLE_INTERNAL_TIMER),
			   0, "the self test passes again with the allocator "
			      "restored");
	else
		JENT_UT_SKIP("the self test passing again",
			     "the startup rejects the platform clock");
}

/* The GCD helpers report the denial rather than dereferencing NULL. */
static void test_gcd_failures(void)
{
	jent_ut_group("the GCD self test under allocation failure");

	fi_arm(1);
	JENT_UT_EQ(jent_gcd_selftest(0), EMEM,
		   "jent_gcd_selftest reports EMEM");
	fi_disarm();

	fi_arm(1);
	JENT_UT_TRUE(jent_gcd_init(1000, 0) == NULL,
		     "jent_gcd_init reports the denial");
	fi_disarm();

	JENT_UT_EQ(jent_gcd_selftest(0), 0,
		   "and passes again with the allocator restored");
}

/*
 * The mark that records "a startup has passed in this process" is written once
 * and never taken back: it is set only by a startup that passed, and a failing
 * one neither sets nor clears it.
 *
 * Both halves matter, and they used to be one bug each. The mark was set on
 * entry to the startup, as a guard against a startup allocating a collector
 * that runs another startup - so a failure had to clear it again, and that
 * store retracted a verdict a concurrent thread had already established,
 * sending it to redo the work and possibly fail on a transient condition its
 * own process had already passed. The recursion is handled where it happens
 * now (the measure_clock argument of the internal allocation), which leaves
 * this mark free to mean only what it says.
 *
 * main() establishes the mark before any case runs, so what this program
 * reaches is the retraction half: whatever the mark is on entry, a failed
 * initialization leaves it exactly so. That a failure cannot set it is
 * structural - jent_entropy_init_common_post() stores only for ret == 0.
 */
static void test_failed_init_unmarks_selftest(void)
{
	struct rand_data *ec;
	int marked = jent_startup_passed(JENT_CLOCK_PLATFORM);
	int marked_notime = jent_startup_passed(JENT_CLOCK_NOTIME);

	jent_ut_group("a failed initialization does not change the mark");

	fi_arm(1);
	JENT_UT_EQ(jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER), EMEM,
		   "a denied GCD self test fails the initialization");
	fi_disarm();
	JENT_UT_EQ(jent_startup_passed(JENT_CLOCK_PLATFORM), marked,
		   "and leaves the mark as it found it");
	JENT_UT_EQ(jent_startup_passed(JENT_CLOCK_NOTIME), marked_notime,
		   "on both clocks");

	fi_arm(1);
	JENT_UT_EQ(jent_entropy_init(), EMEM,
		   "the same through jent_entropy_init");
	fi_disarm();
	JENT_UT_EQ(jent_startup_passed(JENT_CLOCK_PLATFORM), marked,
		   "which leaves it untouched just as well");
	JENT_UT_EQ(jent_startup_passed(JENT_CLOCK_NOTIME), marked_notime,
		   "on both clocks");

	if (!platform_clock) {
		JENT_UT_SKIP("an allocation afterwards",
			     "the startup rejects the platform clock");
		return;
	}
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	JENT_UT_TRUE(ec != NULL,
		     "and an allocation afterwards still succeeds");
	jent_entropy_collector_free(ec);
}

/*
 * The recovery of jent_read_entropy_safe() reallocates the collector. When
 * that reallocation is denied, the original collector must be left intact and
 * the health failure returned - the caller is left with a collector in an
 * error state, not with a dangling pointer. And an intermittent one: memory
 * that ran out says nothing about the noise source, so the next call, with
 * memory to be had again, recovers.
 */
static void test_recovery_alloc_failure(void)
{
	struct rand_data *ec, *before;
	char buf[32];
	unsigned int total;
	unsigned int nth;
	unsigned int leaked = 0, wrong = 0, stuck = 0;

	jent_ut_group("recovery under allocation failure");

	/* How many allocations one recovery makes. */
	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("recovery", JENT_FORCE_FIPS);
		return;
	}
	ec->health_failure = JENT_RCT_FAILURE;
	fi_disarm();
	jent_read_entropy_safe(&ec, buf, sizeof(buf));
	total = fi_alloc_count;
	jent_entropy_collector_free(ec);

	for (nth = 1; nth <= total; nth++) {
		ssize_t ret;

		ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
		if (!ec)
			continue;

		before = ec;
		ec->health_failure = JENT_RCT_FAILURE;

		fi_arm(nth);
		ret = jent_read_entropy_safe(&ec, buf, sizeof(buf));
		fi_disarm();

		if (ret < 0) {
			/*
			 * The reallocation was denied: the failure is returned
			 * as it was, intermittent, and the caller's pointer
			 * still names the original collector - which the next
			 * call, allocations granted, replaces.
			 */
			if (ret != JENT_ERR_RCT)
				wrong++;
			if (ec != before)
				leaked++;
			if (jent_read_entropy_safe(&ec, buf, sizeof(buf)) !=
			    (ssize_t)sizeof(buf))
				stuck++;
		} else if (ret != (ssize_t)sizeof(buf)) {
			wrong++;
		}

		jent_entropy_collector_free(ec);
	}

	JENT_UT_EQ(wrong, 0, "a denied recovery returns the health failure");
	JENT_UT_EQ(leaked, 0,
		   "and leaves the caller's collector pointer unchanged");
	JENT_UT_EQ(stuck, 0,
		   "and usable: the next call, memory granted, recovers");
	printf("  note: denied each of %u allocations of a recovery\n", total);
}

/*
 * The whole point of denying one allocation at a time is that the cleanup path
 * runs. Reading back from a collector allocated afterwards is what says the
 * library is still in a working state rather than merely not having crashed.
 */
static void test_still_usable_afterwards(void)
{
	struct rand_data *ec;
	char buf[64];

	jent_ut_group("the library still works after the injected failures");

	fi_disarm();

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		/*
		 * Say what the startup test objected to, not only that nothing
		 * came back: an allocation here runs the whole self test again
		 * - test_alloc_runs_failing_selftest() cleared the flag that
		 * would have skipped it - and reports neither of the two paths
		 * it tries in turn.
		 */
		printf("  note: startup without the internal timer gives %d\n",
		       jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER));
		printf("  note: startup with the internal timer gives %d\n",
		       jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER));

		JENT_UT_FAIL("%s", "no collector after the injected failures");
		return;
	}

	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   (ssize_t)sizeof(buf), "entropy is produced");

	jent_entropy_collector_free(ec);
}

/*
 * The secure allocator when the kernel refuses. Each refusal has to leave
 * nothing mapped and nothing locked - the mapping is established before the
 * lock is attempted, so a failure after that point has a mapping to undo.
 */
static void test_secure_memory_failures(void)
{
/* The crypto libraries allocate themselves: no mapping and no lock here. */
#if defined(LIBGCRYPT) || defined(OPENSSL) || defined(AWSLC)
	jent_ut_group("the secure allocator when the kernel refuses");
	JENT_UT_SKIP("the secure allocator", "the crypto library's allocator");
#elif defined(JENT_ARCH_MEM_POSIX_MLOCK)
	int lockable = jent_ut_memlock_available();
	void *p;

	jent_ut_group("the secure allocator when the kernel refuses");

	fi_fail_mmap = 1;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	fi_fail_mmap = 0;
	JENT_UT_TRUE(p == NULL, "a refused mapping is reported");

	/*
	 * Both guard pages, one at a time: the second call is only reached
	 * when the first succeeded, and a failure there has a mapping and a
	 * protected page to undo.
	 */
	fi_mprotect_calls = 0;
	fi_fail_mprotect = 1;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	fi_fail_mprotect = 0;
	JENT_UT_TRUE(p == NULL, "a refused leading guard page is reported");

	fi_mprotect_calls = 0;
	fi_fail_mprotect = 2;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	fi_fail_mprotect = 0;
	JENT_UT_TRUE(p == NULL, "a refused trailing guard page is reported");

	/*
	 * A refused lock is fatal only when the caller demanded secure memory.
	 * Without that demand the three limit errnos are tolerated and the
	 * allocation succeeds unlocked, because the alternative is no entropy
	 * at all on a machine with a small RLIMIT_MEMLOCK.
	 */
	fi_fail_mlock = 1;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	JENT_UT_TRUE(p == NULL, "a refused lock is fatal when secure memory is demanded");

	fi_mlock_errno = EPERM;
	p = jent_fi_real_zalloc(4096, 0);
	JENT_UT_TRUE(p != NULL, "EPERM without that demand is tolerated");
	jent_zfree(p, 4096);

	fi_mlock_errno = ENOMEM;
	p = jent_fi_real_zalloc(4096, 0);
	JENT_UT_TRUE(p != NULL, "and so is ENOMEM");
	jent_zfree(p, 4096);

	fi_mlock_errno = EAGAIN;
	p = jent_fi_real_zalloc(4096, 0);
	JENT_UT_TRUE(p != NULL, "and EAGAIN");
	jent_zfree(p, 4096);

	/* Any other errno is a real failure even without the demand. */
	fi_mlock_errno = EINVAL;
	p = jent_fi_real_zalloc(4096, 0);
	JENT_UT_TRUE(p == NULL, "but an unexpected errno is not");

	/* The unlocked allocation does not ask for the lock at all. */
	fi_mlock_calls = 0;
	p = jent_fi_real_zalloc_unlocked(4096);
	JENT_UT_TRUE(p != NULL && fi_mlock_calls == 0,
		     "the unlocked allocation does not ask for the lock");
	jent_zfree(p, 4096);

	fi_fail_mlock = 0;
	fi_mlock_errno = EPERM;

	if (!lockable) {
		JENT_UT_SKIP("the allocator afterwards",
			     "this machine locks no memory (RLIMIT_MEMLOCK)");
		return;
	}
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	JENT_UT_TRUE(p != NULL, "and the allocator works again afterwards");
	jent_zfree(p, 4096);
#elif defined(JENT_ARCH_MEM_WINDOWS)
	int lockable = jent_ut_memlock_available();
	void *p;

	jent_ut_group("the secure allocator when the kernel refuses");

	fi_fail_mmap = 1;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	fi_fail_mmap = 0;
	JENT_UT_TRUE(p == NULL, "a refused reservation is reported");

	/*
	 * One protection call, not two: the region is committed inaccessible
	 * and only the payload is raised, so the guard pages need no call of
	 * their own. A failure there has a reservation to undo.
	 */
	fi_mprotect_calls = 0;
	fi_fail_mprotect = 1;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	fi_fail_mprotect = 0;
	JENT_UT_TRUE(p == NULL, "a refused payload protection is reported");

	/*
	 * A refused lock is fatal only when the caller demanded secure memory.
	 * Without that demand the allocation succeeds unlocked, because the
	 * alternative is no entropy at all on a machine whose working set
	 * quota does not cover the memory block.
	 */
	fi_fail_mlock = 1;
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	JENT_UT_TRUE(p == NULL,
		     "a refused lock is fatal when secure memory is demanded");

	p = jent_fi_real_zalloc(4096, 0);
	JENT_UT_TRUE(p != NULL, "and tolerated without that demand");
	jent_zfree(p, 4096);

	/* The unlocked allocation does not ask for the lock at all. */
	fi_mlock_calls = 0;
	p = jent_fi_real_zalloc_unlocked(4096);
	JENT_UT_TRUE(p != NULL && fi_mlock_calls == 0,
		     "the unlocked allocation does not ask for the lock");
	jent_zfree(p, 4096);
	fi_fail_mlock = 0;

	if (!lockable) {
		JENT_UT_SKIP("the allocator afterwards",
			     "this machine locks no memory (RLIMIT_MEMLOCK)");
		return;
	}
	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	JENT_UT_TRUE(p != NULL, "and the allocator works again afterwards");
	jent_zfree(p, 4096);
#else
	jent_ut_group("the secure allocator when the kernel refuses");
	JENT_UT_SKIP("the secure allocator", "not a mapping backend");
#endif
}

/*
 * A lock quota below the memory access region but above the collector state,
 * such as Android's 64 KiB RLIMIT_MEMLOCK. The region is never locked, so a
 * compliance-mode collector is still allocated.
 */
static void test_lock_quota_below_region(void)
{
#if defined(JENT_ARCH_MEM_POSIX_MLOCK) || defined(JENT_ARCH_MEM_WINDOWS)
	/* Pinned well above the quota, whatever the cache geometry derives. */
	const unsigned int flags = JENT_FORCE_FIPS | JENT_MAX_MEMSIZE_1MB;
	struct rand_data *ec;
	char buf[32];

	jent_ut_group("a lock quota below the memory access region");

	if (!jent_ut_memlock_available()) {
		JENT_UT_SKIP("the lock quota",
			     "this machine locks no memory (RLIMIT_MEMLOCK)");
		return;
	}

	fi_mlock_quota = 64 * 1024;
	ec = jent_entropy_collector_alloc(0, flags);
	JENT_UT_TRUE(ec != NULL, "a FIPS collector is allocated under it");
	if (ec) {
		JENT_UT_TRUE(ec->memmask + 1 > fi_mlock_quota,
			     "with a region the quota would refuse");
		JENT_UT_TRUE(jent_read_entropy_safe(&ec, buf, sizeof(buf)) ==
			     (ssize_t)sizeof(buf), "and generates");
		jent_entropy_collector_free(ec);
	}
	fi_mlock_quota = 0;
#else
	jent_ut_group("a lock quota below the memory access region");
	JENT_UT_SKIP("the lock quota", "not a mapping backend");
#endif
}

/*
 * The startup self test against timers that are not usable. Each of these is
 * a property of the machine that the library has to detect and refuse on,
 * because every bit of entropy it produces comes from the timer.
 */
static void test_startup_rejects_bad_timers(void)
{
	static const struct {
		enum fi_time_mode mode;
		int expect;
		const char *what;
	} modes[] = {
		{ FI_TIME_ZERO,		ENOTIME,
		  "a timer stuck at zero" },
		{ FI_TIME_CONSTANT,	ECOARSETIME,
		  "a timer that does not move" },
		/*
		 * A counter moving by the same amount every time has a second
		 * derivative of zero, so every measurement is stuck and the
		 * RCT reaches its cutoff before the stuck-count check does.
		 */
		{ FI_TIME_FIXED_STEP,	ERCT,
		  "a timer that only ever steps by one amount" },
		/*
		 * A timer running down is caught by the monotonicity check
		 * before the repetition count test can fire - it comes first
		 * and compares the readings the two measurements ended on.
		 *
		 * ESTUCK stays out of reach here: a delta spans several
		 * readings, so no per-reading pattern produces a chosen
		 * proportion of stuck measurements. unit-mock reaches the
		 * cases that need every reading controlled.
		 */
		{ FI_TIME_BACKWARDS,	ENOMONOTONIC,
		  "a timer running down by one amount" },
	};
	size_t i;

	jent_ut_group("the startup self test against unusable timers");

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		int ret;

		fi_time_set(modes[i].mode);
		ret = jent_time_entropy_init(JENT_MIN_OSR,
					     JENT_DISABLE_INTERNAL_TIMER);
		fi_time_set(FI_TIME_REAL);

		JENT_UT_EQ(ret, modes[i].expect, modes[i].what);
	}

	/* And that a real timer still passes afterwards. */
	if (platform_clock)
		JENT_UT_EQ(jent_time_entropy_init(JENT_MIN_OSR,
						  JENT_DISABLE_INTERNAL_TIMER),
			   0, "the real timer passes again");
	else
		JENT_UT_SKIP("the real timer passing again",
			     "the startup rejects the platform clock");
}

#if defined(JENT_ARCH_CACHE_LINUX)
/*
 * A small sysfs cache tree for the walk to read, so that the CPU count it
 * bounds itself with has something to be right or wrong about. cpu1 carries
 * the larger caches, the way the performance cores of a hybrid part do; the
 * third CPU is described with fi_cpu_tree_cpus below.
 */
#include <dirent.h>
#include <sys/stat.h>

static char fi_cpu_tree[] = "/tmp/jent-fault-cpu-XXXXXX";

static int fi_cpu_attr(const char *cache, unsigned int idx, const char *name,
		       const char *value)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/index%u", cache, idx);
	if (mkdir(path, 0700) && errno != EEXIST)
		return -1;

	snprintf(path, sizeof(path), "%s/index%u/%s", cache, idx, name);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fputs(value, f);
	fclose(f);
	return 0;
}

/*
 * The CPUs of the tree. The third is numbered at JENT_NCPU_SET_MAX, the bound
 * the walk holds itself to, and has the largest caches of all: a walk that
 * honours the bound never reads it, and one that follows an implausible CPU
 * count past it does - which is the only way the bound shows in the answer.
 */
static const unsigned int fi_cpu_tree_cpus[] = { 0, 1, JENT_NCPU_SET_MAX };
static const char *const fi_cpu_tree_l1[] = { "32K\n", "64K\n", "128K\n" };
static const char *const fi_cpu_tree_l2[] = { "512K\n", "2M\n", "4M\n" };

static void fi_rm_cpu_tree(void)
{
	unsigned int cpu, idx;
	char path[256];
	static const char *attrs[] = { "type", "level", "size" };
	size_t a, i;

	for (i = 0; i < JENT_ARRAY_SIZE(fi_cpu_tree_cpus); i++) {
		cpu = fi_cpu_tree_cpus[i];
		for (idx = 0; idx < 2; idx++) {
			for (a = 0; a < JENT_ARRAY_SIZE(attrs); a++) {
				snprintf(path, sizeof(path),
					 "%s/cpu%u/cache/index%u/%s",
					 fi_cpu_tree, cpu, idx, attrs[a]);
				unlink(path);
			}
			snprintf(path, sizeof(path),
				 "%s/cpu%u/cache/index%u", fi_cpu_tree, cpu,
				 idx);
			rmdir(path);
		}
		snprintf(path, sizeof(path), "%s/cpu%u/cache", fi_cpu_tree, cpu);
		rmdir(path);
		snprintf(path, sizeof(path), "%s/cpu%u", fi_cpu_tree, cpu);
		rmdir(path);
	}
	rmdir(fi_cpu_tree);
}

/* 0 when the tree stands, nonzero when it could not be built. */
static int fi_build_cpu_tree(void)
{
	size_t i;

	if (!mkdtemp(fi_cpu_tree))
		return -1;

	for (i = 0; i < JENT_ARRAY_SIZE(fi_cpu_tree_cpus); i++) {
		unsigned int cpu = fi_cpu_tree_cpus[i];
		char cache[192];

		snprintf(cache, sizeof(cache), "%s/cpu%u", fi_cpu_tree, cpu);
		if (mkdir(cache, 0700))
			goto err;
		snprintf(cache, sizeof(cache), "%s/cpu%u/cache", fi_cpu_tree,
			 cpu);
		if (mkdir(cache, 0700))
			goto err;

		if (fi_cpu_attr(cache, 0, "type", "Data\n") ||
		    fi_cpu_attr(cache, 0, "level", "1\n") ||
		    fi_cpu_attr(cache, 0, "size", fi_cpu_tree_l1[i]) ||
		    fi_cpu_attr(cache, 1, "type", "Unified\n") ||
		    fi_cpu_attr(cache, 1, "level", "2\n") ||
		    fi_cpu_attr(cache, 1, "size", fi_cpu_tree_l2[i]))
			goto err;
	}

	return 0;

err:
	fi_rm_cpu_tree();
	return -1;
}
#endif /* JENT_ARCH_CACHE_LINUX */

/*
 * The platform queries when the platform will not answer. Each of these has a
 * fallback behind it, and the fallback is the whole point: a container that
 * hides the CPU topology, a libc that does not know its cache sizes, a kernel
 * without getrandom().
 */
static void test_platform_query_failures(void)
{
	jent_ut_group("the platform queries when they cannot answer");

#ifdef FI_WINDOWS
	/* The thread's affinity first, the processors of the machine second. */
	fi_fail_affinity = 1;
	JENT_UT_TRUE(jent_ncpu() > 0,
		     "an unreadable thread affinity falls back to the system count");
	JENT_UT_EQ(jent_cpu_highest(), jent_ncpu() - 1,
		   "and the highest CPU to the count minus one");

	fi_fail_processor_count = 1;
	JENT_UT_TRUE(jent_ncpu() < 0,
		     "an unanswerable CPU count is reported as an error");
	JENT_UT_TRUE(jent_cpu_highest() < 0, "and so is the highest CPU");
	fi_fail_processor_count = 0;
	fi_fail_affinity = 0;

	fi_empty_affinity = 1;
	JENT_UT_TRUE(jent_ncpu() > 0,
		     "an empty affinity mask falls back to the system count");
	fi_empty_affinity = 0;
#else
	/* The CPU count. Whatever it says, it must be a count or an error. */
	fi_fail_affinity = 1;
#ifdef JENT_ARCH_NCPU_LINUX_SYSFS
	/*
	 * A third source sits between the two this interposes: on a Linux libc
	 * that is not glibc, jent_ncpu() reads /sys/devices/system/cpu/online
	 * before it reaches sysconf(). Denying the affinity query and sysconf()
	 * therefore leaves the count answerable, and taking that file away too
	 * would mean interposing open() for the whole translation unit, which
	 * the cache and FIPS backends read through as well.
	 *
	 * What is still asserted is the half that does not depend on the file:
	 * the answer is a count or an error and never zero.
	 */
	fi_sysconf_mode = FI_SYSCONF_FAIL;
	JENT_UT_NE(jent_ncpu(), 0,
		   "a denied affinity query gives a count or an error");
	JENT_UT_SKIP("the unanswerable CPU count",
		     "the sysfs source answers it on this libc");
#else
	fi_sysconf_mode = FI_SYSCONF_FAIL;
	JENT_UT_TRUE(jent_ncpu() < 0,
		     "an unanswerable CPU count is reported as an error");

	fi_sysconf_mode = FI_SYSCONF_ZERO;
	JENT_UT_TRUE(jent_ncpu() < 0, "and so is a count of zero");
#endif /* JENT_ARCH_NCPU_LINUX_SYSFS */

#ifdef FI_HAVE_CPU_ALLOC
	/*
	 * A set is allocated only once the default one proved too small for
	 * the kernel's mask, so that has to be the answer first: a query that
	 * is denied outright never gets to the allocation.
	 */
	{
		long count = 0, highest = -1;

		fi_fail_affinity = 0;
		fi_small_affinity = 1;
		fi_sysconf_mode = FI_SYSCONF_REAL;
		JENT_UT_EQ(jent_affinity_mask(&count, &highest), 0,
			   "a mask wider than the default set is read with "
			   "a larger one");
		JENT_UT_TRUE(count > 0 && highest >= 0,
			     "which answers like the default one");

		fi_fail_cpu_alloc = 1;
		JENT_UT_EQ(jent_affinity_mask(&count, &highest), -ENOMEM,
			   "a failed CPU-set allocation is reported");
		JENT_UT_TRUE(jent_ncpu() > 0,
			     "and falls back to the system count");
		fi_fail_cpu_alloc = 0;
		fi_small_affinity = 0;
	}
#endif

	/* An affinity mask that names no CPU falls through to sysconf. */
	fi_fail_affinity = 0;
	fi_empty_affinity = 1;
	fi_sysconf_mode = FI_SYSCONF_REAL;
	JENT_UT_TRUE(jent_ncpu() > 0,
		     "an empty affinity mask falls back to the system count");
	fi_empty_affinity = 0;

	fi_sysconf_mode = FI_SYSCONF_REAL;
	fi_fail_affinity = 0;
#endif /* FI_WINDOWS */

	JENT_UT_TRUE(jent_ncpu() > 0, "the real count comes back afterwards");

#if defined(JENT_ARCH_CACHE_LINUX)
	/* The cache sizes. A libc that does not know leaves them at zero. */
	{
		uint64_t l1 = UINT64_MAX, l2 = UINT64_MAX, l3 = UINT64_MAX;

		fi_sysconf_mode = FI_SYSCONF_FAIL;
		jent_get_cachesize_sysconf(&l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "an unanswerable L1 size is zero");
		JENT_UT_EQ(l2, 0, "an unanswerable L2 size is zero");
		JENT_UT_EQ(l3, 0, "an unanswerable L3 size is zero");

		/*
		 * And the CPU count the sysfs walk bounds itself with. Against
		 * a tree that does not exist this said nothing at all: the
		 * walk answers zero for a directory it cannot open whatever
		 * the count was, so the clamp it names went untested. Against
		 * a real tree of two CPUs, where only the second reports the
		 * larger caches, the answer says how far the walk got.
		 */
		fi_sysconf_mode = FI_SYSCONF_REAL;
		if (fi_build_cpu_tree()) {
			JENT_UT_SKIP("the CPU count of the cache walk",
				     "no temporary sysfs tree");
		} else {
			/*
			 * Zero is not a usable count. It becomes one, and the
			 * walk then goes on past it for as long as a cpuN
			 * directory exists - which is what musl needs, its
			 * _SC_NPROCESSORS_CONF being the affinity mask rather
			 * than the topology.
			 */
			fi_sysconf_mode = FI_SYSCONF_ZERO;
			l1 = l2 = UINT64_MAX;
			jent_get_cachesize_sysfs_dir(fi_cpu_tree, &l1, &l2,
						     &l3);
			JENT_UT_EQ(l1, 65536,
				   "a CPU count of zero still reaches the "
				   "CPUs behind it");
			JENT_UT_EQ(l2, 2097152, "at every level");

			/*
			 * And an implausible one is capped rather than walked:
			 * a million snprintf/access pairs is not a bound. The
			 * CPU numbered at the bound has the largest caches in
			 * the tree, and a walk that reached it would say so.
			 */
			fi_sysconf_mode = FI_SYSCONF_HUGE;
			l1 = l2 = UINT64_MAX;
			jent_get_cachesize_sysfs_dir(fi_cpu_tree, &l1, &l2,
						     &l3);
			JENT_UT_EQ(l1, 65536,
				   "an implausible CPU count is capped and the "
				   "walk still answers");

			fi_rm_cpu_tree();
		}

		/* A tree that is not there is still no L1, whatever the count. */
		fi_sysconf_mode = FI_SYSCONF_ZERO;
		l1 = UINT64_MAX;
		jent_get_cachesize_sysfs_dir("/nonexistent/jent/cpu",
					     &l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "an absent tree reports no L1");

		fi_sysconf_mode = FI_SYSCONF_REAL;
	}
#endif

#ifdef FI_HAVE_GETRANDOM
	/*
	 * The UUID falls back to /dev/urandom when getrandom() is absent, and
	 * only then. Which path produced it is in the version nibble: 4 from
	 * the device, 8 from the counter - a non-empty string that differs from
	 * the next one is what both of them produce.
	 */
	{
		char a[JENT_UUID_STRLEN], b[JENT_UUID_STRLEN];

		fi_fail_getrandom = ENOSYS;
		jent_uuid_generate(a);
		jent_uuid_generate(b);
		fi_fail_getrandom = 0;

		JENT_UT_EQ(strlen(a), JENT_UUID_STRLEN - 1,
			   "a UUID is still produced without getrandom()");
		JENT_UT_TRUE(strcmp(a, b) != 0,
			     "the fallback does not repeat itself");
		if (access("/dev/urandom", R_OK))
			JENT_UT_SKIP("the /dev/urandom fallback",
				     "no readable /dev/urandom");
		else
			JENT_UT_EQ(a[14], '4',
				   "a getrandom() the kernel lacks falls back "
				   "to /dev/urandom, not to the counter");

		/* A pool that is not ready yet is not read around. */
		fi_fail_getrandom = EAGAIN;
		jent_uuid_generate(a);
		fi_fail_getrandom = 0;
		JENT_UT_EQ(a[14], '8',
			   "an unready pool yields the counter UUID, not "
			   "/dev/urandom");
	}
#endif

#ifndef FI_WINDOWS
	fi_sysconf_mode = FI_SYSCONF_REAL;
	fi_fail_affinity = 0;
#endif
	fi_fail_getrandom = 0;
}

/*
 * A machine with the kernel FIPS indicator on. Every collector then runs the
 * health tests and demands secure memory whether or not the caller asked, and
 * there is no way to ask a machine that does not have it on to behave that
 * way.
 */
static void test_system_fips_mode(void)
{
	struct rand_data *ec;
	char buf[32];

	jent_ut_group("a system with FIPS mode enabled");

	fi_force_fips_enabled = 1;
	ec = jent_entropy_collector_alloc(0, 0);
	fi_force_fips_enabled = 0;

	if (!ec) {
		/* What the system's FIPS mode asks of the collector. */
		JENT_UT_NO_COLLECTOR("system FIPS mode", JENT_FORCE_FIPS);
		return;
	}

	JENT_UT_EQ(ec->is_fips_enabled, 1,
		   "the health tests are on without the caller asking");
	JENT_UT_EQ(ec->flags & JENT_FORCE_SECURE_MEM,
		   JENT_FORCE_SECURE_MEM,
		   "and secure memory is demanded without the caller asking");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   (ssize_t)sizeof(buf), "and entropy is produced");

	jent_entropy_collector_free(ec);
}

/* How the Windows backend reads the FIPS policy query. */
static void test_windows_fips_policy(void)
{
	jent_ut_group("the Windows FIPS policy query");

#ifdef FI_HAVE_BCRYPT_FIPS
	fi_bcrypt_fips_mode = FI_BCRYPT_FIPS_FAIL;
	JENT_UT_EQ(jent_fi_real_fips_enabled(), 0,
		   "a failed query means disabled, whatever it wrote");

	fi_bcrypt_fips_mode = FI_BCRYPT_FIPS_ON;
	JENT_UT_EQ(jent_fi_real_fips_enabled(), 1,
		   "a policy that is on is reported as FIPS mode");

	/*
	 * Against the system's own answer. Compared with jent_fips_enabled()
	 * this said nothing: that is the backend under test, reached through
	 * the override, and agrees with itself whatever the fake left behind.
	 * The "on" fake goes last so that, on the usual machine without the
	 * policy, one left in place is told apart from the real query.
	 */
	fi_bcrypt_fips_mode = FI_BCRYPT_FIPS_REAL;
	{
		BOOLEAN on = FALSE;
		int want = BCRYPT_SUCCESS(fi_BCryptGetFipsAlgorithmMode_real(&on))
			   && on;

		JENT_UT_EQ(jent_fi_real_fips_enabled(), want,
			   "the real query comes back afterwards");
	}
#else
	JENT_UT_SKIP("the Windows FIPS policy query",
		     "not the Windows FIPS backend");
#endif
}

/* The allocator's own bounds, which no ordinary request comes near. */
static void test_allocator_bounds(void)
{
	jent_ut_group("the allocator bounds");

	/*
	 * A length whose page-rounded size plus its guard pages would not fit
	 * in a size_t. Refused rather than wrapped into a small mapping that
	 * the caller then writes past.
	 */
	JENT_UT_TRUE(jent_fi_real_zalloc(SIZE_MAX, JENT_FORCE_SECURE_MEM) == NULL,
		     "a length that cannot be rounded up is refused");
	JENT_UT_TRUE(jent_fi_real_zalloc(SIZE_MAX - 4096, 0) == NULL,
		     "and so is one just under it");

	/*
	 * The page size, which every mapping is rounded to. A system that
	 * cannot report one has to fall back to a sane value rather than round
	 * to zero. Only the mapping backends have one - the malloc fallback
	 * rounds to nothing.
	 */
#if defined(JENT_ARCH_MEM_POSIX_MLOCK) && !defined(LIBGCRYPT) && \
    !defined(OPENSSL) && !defined(AWSLC)
	/*
	 * The fallback by value: "> 0" also holds for -1 cast to a size_t,
	 * which is the very mistake the fallback is there to prevent.
	 */
	fi_sysconf_mode = FI_SYSCONF_FAIL;
	JENT_UT_EQ(jent_pagesize(), 4096,
		   "an unreportable page size falls back to a usable one");
	fi_sysconf_mode = FI_SYSCONF_ZERO;
	JENT_UT_EQ(jent_pagesize(), 4096, "and so does a page size of zero");
	fi_sysconf_mode = FI_SYSCONF_REAL;
	JENT_UT_EQ(jent_pagesize(), (size_t)sysconf(_SC_PAGESIZE),
		   "and the real one comes back afterwards");
#else
	JENT_UT_SKIP("the page size fallback", "not the mmap/mlock backend");
#endif

	/*
	 * The capability query, both ways round: every backend wipes on free,
	 * so the memory is secure whatever the flag and the extras.
	 */
	JENT_UT_EQ(jent_memory_is_secure(JENT_FORCE_SECURE_MEM), 1,
		   "the memory is secure with the flag");
	JENT_UT_EQ(jent_memory_is_secure(0), 1, "and without it");
}

/*
 * The collector allocation when the startup self test has not run and does not
 * pass. The two are separate paths: the allocation runs the self test itself
 * when nothing has, and must decline rather than hand back a collector built
 * on a timer that was never shown to work.
 */
static void test_alloc_runs_failing_selftest(void)
{
	struct rand_data *ec;

	jent_ut_group("allocation when the startup self test fails");

	/* Make the allocation believe no self test has run yet. */
	/*
	 * JENT_DISABLE_INTERNAL_TIMER, so that the dead timer is the only one
	 * there is: without it the initialization falls back to the counting
	 * thread, which is a real timer and would rightly succeed.
	 */
	jent_selftest_run[JENT_CLOCK_PLATFORM] = 0;
	jent_selftest_run[JENT_CLOCK_NOTIME] = 0;
	fi_time_set(FI_TIME_ZERO);
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	fi_time_set(FI_TIME_REAL);

	JENT_UT_TRUE(ec == NULL,
		     "no collector is handed back when the self test fails");
	jent_entropy_collector_free(ec);

	/* And that a collector can be had again once the timer works. */
	jent_selftest_run[JENT_CLOCK_PLATFORM] = 0;
	jent_selftest_run[JENT_CLOCK_NOTIME] = 0;
	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		/* Same reasoning as in test_still_usable_afterwards(). */
		printf("  note: startup without the internal timer gives %d\n",
		       jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER));
	}
	JENT_UT_TRUE(ec != NULL, "and one can be had again afterwards");
	jent_entropy_collector_free(ec);
}

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
/*
 * A compliance-mode collector on the internal timer keeps that clock through a
 * recovery, so the replacement's startup test runs on the internal timer only.
 * A counting thread that cannot be created there is the platform refusing a
 * resource, as a failed allocation is: the intermittent failure is returned
 * and the next call tries again. It used to come back as the ENOTIME of a
 * platform timer that was never tested, which ended the recovery for good.
 */
static void test_recovery_thread_failure_notime(void)
{
	struct rand_data *ec, *before;
	char buf[32];

	jent_ut_group("recovery on the internal timer without a counting thread");

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS |
					     JENT_FORCE_INTERNAL_TIMER);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the denied thread",
				     JENT_FORCE_FIPS | JENT_FORCE_INTERNAL_TIMER);
		return;
	}
	if (!ec->enable_notime) {
		JENT_UT_FAIL("%s: a collector forced onto the internal timer "
			     "does not use it", "the denied thread");
		jent_entropy_collector_free(ec);
		return;
	}

	before = ec;
	ec->health_failure = JENT_RCT_FAILURE;

	/* The read's own counting thread, then none for the replacement. */
	fi_thread_create_grant = 1;
	fi_fail_thread_create = 1;
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)), JENT_ERR_RCT,
		   "a denied counting thread returns the intermittent failure");
	fi_fail_thread_create = 0;
	fi_thread_create_grant = 0;

	JENT_UT_TRUE(ec == before, "the collector was left in place");
	JENT_UT_EQ(ec->health_failure & JENT_PERMANENT_FAILURES, 0,
		   "with no permanent failure raised on it");
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   (ssize_t)sizeof(buf),
		   "and the next call, the thread granted, recovers");

	jent_entropy_collector_free(ec);
}

/*
 * The counting thread needs a second CPU. Without one it is no timer rather
 * than a resource to retry for: ENOTIME from the startup test, and a recovery
 * that ends for good instead of returning an intermittent failure on every
 * call until a CPU comes back.
 */
static void test_notime_without_second_cpu(void)
{
	struct rand_data *ec;
	char buf[32];

	jent_ut_group("the internal timer on a single CPU");

	fi_ncpu = 1;
	JENT_UT_EQ(jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER), ENOTIME,
		   "the forced internal timer is reported missing, not EMEM");
	fi_ncpu = 0;

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS |
					     JENT_FORCE_INTERNAL_TIMER);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the recovery",
				     JENT_FORCE_FIPS | JENT_FORCE_INTERNAL_TIMER);
		return;
	}
	if (!ec->enable_notime) {
		JENT_UT_FAIL("%s: a collector forced onto the internal timer "
			     "does not use it", "the recovery");
		jent_entropy_collector_free(ec);
		return;
	}
	ec->health_failure = JENT_RCT_FAILURE;

	/* The CPUs gone after allocation: an affinity mask that shrank. */
	fi_ncpu = 1;
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_RCT_PERMANENT,
		   "a recovery that cannot run the counting thread ends for good");
	fi_ncpu = 0;
	JENT_UT_TRUE((ec->health_failure & JENT_RCT_FAILURE_PERMANENT) != 0,
		     "with the permanent failure raised on the collector");

	jent_entropy_collector_free(ec);
}
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

int main(void)
{
	int ret;

	jent_ut_setup();

	/*
	 * Get the startup self test out of the way first. It runs once per
	 * process, on whichever allocation happens to be first, and would
	 * otherwise make the allocation counts below depend on the order the
	 * cases run in.
	 *
	 * Only the machine's verdicts make the whole program a skip. A startup
	 * that fails its hash or GCD self test, or runs out of memory it did
	 * not have to lock, is a library that no longer initializes anywhere.
	 */
	ret = jent_entropy_init();
	if (ret && JENT_UT_MACHINE_VERDICT(ret)) {
		printf("the startup self test does not pass on this machine (%d)\n",
		       ret);
		return 77;
	}
	if (ret) {
		printf("unit-fault: the startup self test fails with %d\n", ret);
		return 1;
	}

	/*
	 * That passes through the internal timer where the platform clock is
	 * rejected. The cases pinned to the platform clock need to know.
	 */
	ret = jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER);
	if (ret && JENT_UT_MACHINE_VERDICT(ret)) {
		printf("the startup self test rejects the platform clock (%d)\n",
		       ret);
		platform_clock = 0;
	} else if (ret) {
		printf("unit-fault: the startup self test on the platform clock "
		       "fails with %d\n", ret);
		return 1;
	}

	/*
	 * Before anything else has touched the library's global state.
	 * The startup self test is not re-entrant with respect to it: an
	 * allocation denied inside a previous self test leaves the collector
	 * configuration it had reached, and forcing the internal timer
	 * anywhere sets a one-way global. Both would change what the timers
	 * below are measured through.
	 */
	test_startup_rejects_bad_timers();

	test_injection_works();
	test_gcd_failures();
	test_failed_init_unmarks_selftest();
	test_collector_alloc_failures();
	test_init_failures();
	test_recovery_alloc_failure();
	test_secure_memory_failures();
	test_lock_quota_below_region();
	test_platform_query_failures();
	test_system_fips_mode();
	test_windows_fips_policy();
	test_allocator_bounds();
	test_alloc_runs_failing_selftest();
	test_still_usable_afterwards();

	/* Last: it forces the internal timer, which cannot be undone. */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	test_recovery_thread_failure_notime();
	test_notime_without_second_cpu();
#endif

	return jent_ut_report("unit-fault");
}
