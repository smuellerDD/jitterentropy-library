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
 * renaming jent_zalloc() through the preprocessor while
 * arch/jitterentropy-arch-memory.c is compiled hides that definition under a
 * private name and lets this file supply jent_zalloc() itself. Every absorbed
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
	if (fi_fail_mlock) {
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
	if (fi_fail_mlock) {
		errno = fi_mlock_errno;
		return -1;
	}
	return mlock(addr, len);
}

#endif /* FI_WINDOWS */

/*
 * Compile the real allocator under a private name, with its kernel calls
 * redirected. The header it includes declares jent_zalloc(), which is renamed
 * with it, so the declaration and the definition still agree.
 */
#define jent_zalloc jent_fi_real_zalloc
#ifdef FI_WINDOWS
# define VirtualAlloc fi_VirtualAlloc
# define VirtualProtect fi_VirtualProtect
# define VirtualLock fi_VirtualLock
#else
# define mmap fi_mmap
# define mprotect fi_mprotect
# define mlock fi_mlock
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
# undef mlock
# undef mprotect
# undef mmap
#endif
#undef jent_zalloc

/*
 * Fail the n-th allocation from now on, counting from 1. Zero disables the
 * injection. Only one allocation is failed per arming, so that the collector
 * is built up to a chosen point and only then denied its next allocation -
 * which is what walks the cleanup paths one stage at a time.
 */
static unsigned int fi_fail_alloc;
static unsigned int fi_alloc_count;

void *jent_zalloc(size_t len, unsigned int flags)
{
	fi_alloc_count++;

	if (fi_fail_alloc && fi_alloc_count == fi_fail_alloc)
		return NULL;

	return jent_fi_real_zalloc(len, flags);
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
 * sysconf() and the affinity query are the POSIX backends' sources. The
 * Windows ones have no "cannot tell" reply to fake - jent_ncpu() there cannot
 * return an error at all - so there is nothing to interpose and the cases
 * below skip.
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
enum fi_sysconf_mode {
	FI_SYSCONF_REAL = 0,
	FI_SYSCONF_FAIL,	/* -1, the "unknown" reply */
	FI_SYSCONF_ZERO,	/* 0, which is not a usable count */
	FI_SYSCONF_HUGE,	/* more CPUs than any topology has */
};

static enum fi_sysconf_mode fi_sysconf_mode;
static int fi_fail_affinity;
static int fi_empty_affinity;

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

static int fi_fail_getrandom;

#ifdef __linux__
static int fi_sched_getaffinity_real(pid_t pid, size_t size, cpu_set_t *set)
{
	return sched_getaffinity(pid, size, set);
}

static int fi_sched_getaffinity(pid_t pid, size_t size, cpu_set_t *set)
{
	if (fi_fail_affinity) {
		errno = EPERM;
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
		errno = ENOSYS;
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
#define jent_ncpu jent_fi_real_ncpu
#include "jitterentropy-arch-ncpu.c"
#undef jent_ncpu

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

int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine start_routine,
			      void *arg)
{
	if (fi_fail_thread_create)
		return -EAGAIN;
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
/*
 * The rename is scoped to this one include: the sources above call
 * jent_fips_enabled() and must reach the override below, not the real one.
 */
#define jent_fips_enabled jent_fi_real_fips_enabled
#include "jitterentropy-arch-fips.c"
#undef jent_fips_enabled

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
		unsigned int total, nth, survived = 0;

		alloc_flags = configs[c].flags;
		total = fi_count_allocs(op_collector_alloc);

		if (!total) {
			JENT_UT_SKIP(configs[c].name, "no collector to allocate");
			continue;
		}

		for (nth = 1; nth <= total; nth++) {
			struct rand_data *ec;

			fi_arm(nth);
			ec = jent_entropy_collector_alloc(0, configs[c].flags);
			fi_disarm();

			if (ec) {
				/*
				 * Not a failure in itself: an allocation the
				 * collector can do without (the GCD history of
				 * the startup test, say) leaves a usable
				 * collector. It must still be intact.
				 */
				survived++;
				jent_entropy_collector_free(ec);
			}
		}

		printf("  %-22s %2u allocations, %u survived a denial\n",
		       configs[c].name, total, survived);
		jent_ut_checks++;
	}
}

/*
 * The same for the startup self test, which allocates its own collector and a
 * GCD history and has to release both on every exit.
 */
static void test_init_failures(void)
{
	unsigned int nth, emem = 0, other = 0;

	jent_ut_group("the startup self test under allocation failure");

	for (nth = 1; nth <= 8; nth++) {
		int ret;

		fi_arm(nth);
		ret = jent_time_entropy_init(JENT_MIN_OSR,
					     JENT_DISABLE_INTERNAL_TIMER);
		fi_disarm();

		if (ret == EMEM)
			emem++;
		else if (ret)
			other++;
	}

	JENT_UT_NE(emem, 0, "a denied allocation is reported as EMEM");
	JENT_UT_EQ(other, 0, "and never as some other failure");

	/* And that it still passes once nothing is denied. */
	fi_disarm();
	JENT_UT_EQ(jent_time_entropy_init(JENT_MIN_OSR,
					  JENT_DISABLE_INTERNAL_TIMER), 0,
		   "the self test passes again with the allocator restored");
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

/* The hash state allocation, which the collector cannot do without. */
static void test_sha3_alloc_failure(void)
{
	void *hash_state = (void *)0x1;

	jent_ut_group("the hash state under allocation failure");

	fi_arm(1);
	JENT_UT_NE(jent_sha3_alloc(&hash_state, 0), 0,
		   "jent_sha3_alloc reports the denial");
	fi_disarm();

	JENT_UT_EQ(jent_sha3_alloc(&hash_state, 0), 0,
		   "and succeeds again with the allocator restored");
	jent_sha3_dealloc(hash_state);
}

/*
 * The recovery of jent_read_entropy_safe() reallocates the collector. When
 * that reallocation is denied, the original collector must be left intact and
 * the health failure returned - the caller is left with a collector in an
 * error state, not with a dangling pointer.
 */
static void test_recovery_alloc_failure(void)
{
	struct rand_data *ec, *before;
	char buf[32];
	unsigned int total;
	unsigned int nth;
	unsigned int leaked = 0, wrong = 0;

	jent_ut_group("recovery under allocation failure");

	/* How many allocations one recovery makes. */
	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_SKIP("recovery", "no collector");
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
			 * and the caller's pointer still names the original
			 * collector.
			 */
			if (ret != JENT_ERR_RCT)
				wrong++;
			if (ec != before)
				leaked++;
		} else if (ret != (ssize_t)sizeof(buf)) {
			wrong++;
		}

		jent_entropy_collector_free(ec);
	}

	JENT_UT_EQ(wrong, 0, "a denied recovery returns the health failure");
	JENT_UT_EQ(leaked, 0,
		   "and leaves the caller's collector pointer unchanged");
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
		 * it tries in turn. This being the last case in the program,
		 * the one-way global that asking for the internal timer sets
		 * has nothing left to affect.
		 */
		printf("  note: internal timer already forced: %d\n",
		       jent_notime_forced());
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
#ifdef JENT_ARCH_MEM_POSIX_MLOCK
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

	fi_fail_mlock = 0;
	fi_mlock_errno = EPERM;

	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	JENT_UT_TRUE(p != NULL, "and the allocator works again afterwards");
	jent_zfree(p, 4096);
#elif defined(JENT_ARCH_MEM_WINDOWS)
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
	fi_fail_mlock = 0;

	p = jent_fi_real_zalloc(4096, JENT_FORCE_SECURE_MEM);
	JENT_UT_TRUE(p != NULL, "and the allocator works again afterwards");
	jent_zfree(p, 4096);
#else
	jent_ut_group("the secure allocator when the kernel refuses");
	JENT_UT_SKIP("the secure allocator", "not a mapping backend");
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
	JENT_UT_EQ(jent_time_entropy_init(JENT_MIN_OSR,
					  JENT_DISABLE_INTERNAL_TIMER), 0,
		   "the real timer passes again");
}

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
	/*
	 * GetActiveProcessorCount() has no failure reply, so there is no
	 * unanswerable count to produce here - only the real one to confirm.
	 */
	JENT_UT_SKIP("the unanswerable CPU count",
		     "the Windows backend has no query that can decline");
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
	/* No CPU set to ask with: the query is skipped, not attempted. */
	fi_fail_cpu_alloc = 1;
	fi_sysconf_mode = FI_SYSCONF_REAL;
	JENT_UT_TRUE(jent_ncpu() > 0,
		     "a failed CPU-set allocation falls back to the system count");
	fi_fail_cpu_alloc = 0;
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
		long l1 = -1, l2 = -1, l3 = -1;

		fi_sysconf_mode = FI_SYSCONF_FAIL;
		jent_get_cachesize_sysconf(&l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "an unanswerable L1 size is zero");
		JENT_UT_EQ(l2, 0, "an unanswerable L2 size is zero");
		JENT_UT_EQ(l3, 0, "an unanswerable L3 size is zero");

		/*
		 * And the CPU count the sysfs walk bounds itself with: neither
		 * an unusable answer nor an implausible one may let it run
		 * away.
		 */
		fi_sysconf_mode = FI_SYSCONF_ZERO;
		l1 = -1;
		jent_get_cachesize_sysfs_dir("/nonexistent/jent/cpu",
					     &l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "a CPU count of zero is handled");

		fi_sysconf_mode = FI_SYSCONF_HUGE;
		l1 = -1;
		jent_get_cachesize_sysfs_dir("/nonexistent/jent/cpu",
					     &l1, &l2, &l3);
		JENT_UT_EQ(l1, 0, "an implausible CPU count is capped");

		fi_sysconf_mode = FI_SYSCONF_REAL;
	}
#endif

#ifdef FI_HAVE_GETRANDOM
	/* The UUID falls back to /dev/urandom when getrandom() is absent. */
	{
		char a[JENT_UUID_STRLEN], b[JENT_UUID_STRLEN];

		fi_fail_getrandom = 1;
		jent_uuid_generate(a);
		jent_uuid_generate(b);
		fi_fail_getrandom = 0;

		JENT_UT_EQ(strlen(a), JENT_UUID_STRLEN - 1,
			   "a UUID is still produced without getrandom()");
		jent_ut_checks++;
		if (!strcmp(a, b))
			JENT_UT_FAIL("%s", "the fallback repeats itself");
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
		JENT_UT_SKIP("system FIPS mode", "no collector");
		return;
	}

	JENT_UT_EQ(ec->is_fips_enabled, 1,
		   "the health tests are on without the caller asking");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   (ssize_t)sizeof(buf), "and entropy is produced");

	jent_entropy_collector_free(ec);
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
#ifdef JENT_ARCH_MEM_POSIX_MLOCK
	fi_sysconf_mode = FI_SYSCONF_FAIL;
	JENT_UT_TRUE(jent_pagesize() > 0,
		     "an unreportable page size falls back to a usable one");
	fi_sysconf_mode = FI_SYSCONF_ZERO;
	JENT_UT_TRUE(jent_pagesize() > 0, "and so does a page size of zero");
	fi_sysconf_mode = FI_SYSCONF_REAL;
#else
	JENT_UT_SKIP("the page size fallback", "not the mmap/mlock backend");
#endif

	/* The capability query, both ways round. */
	{
		int forced = jent_memory_is_secure(JENT_FORCE_SECURE_MEM);
		int unforced = jent_memory_is_secure(0);

		JENT_UT_TRUE(forced == 0 || forced == 1,
			     "the secure memory query answers a boolean");
		JENT_UT_TRUE(unforced == 0 || unforced == 1,
			     "with and without the flag");
	}
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
	jent_selftest_run = 0;
	fi_time_set(FI_TIME_ZERO);
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	fi_time_set(FI_TIME_REAL);

	JENT_UT_TRUE(ec == NULL,
		     "no collector is handed back when the self test fails");
	jent_entropy_collector_free(ec);

	/* And that a collector can be had again once the timer works. */
	jent_selftest_run = 0;
	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		/*
		 * Same reasoning as in test_still_usable_afterwards(), and the
		 * same limitation: only the path that does not force the
		 * internal timer is asked, because forcing it is a one-way
		 * global and there is still a case to run after this one.
		 */
		printf("  note: internal timer already forced: %d\n",
		       jent_notime_forced());
		printf("  note: startup without the internal timer gives %d\n",
		       jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER));
	}
	JENT_UT_TRUE(ec != NULL, "and one can be had again afterwards");
	jent_entropy_collector_free(ec);
}

/*
 * The startup self test once the internal timer has been forced. A collector
 * that disables it cannot be allocated then, and that refusal used to be
 * reported as EMEM - a machine out of memory rather than a process that has
 * settled on the other timer.
 *
 * Runs last and forces the timer itself: the flag is one-way, so anything
 * after this would see a library that can no longer produce a collector on the
 * platform clock.
 */
static void test_forced_notime_reports_no_timer(void)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	jent_ut_group("the startup self test once the internal timer is forced");

	if (jent_entropy_init_ex(0, JENT_FORCE_INTERNAL_TIMER)) {
		JENT_UT_SKIP("the forced internal timer",
			     "it does not initialise on this machine");
		return;
	}

	JENT_UT_TRUE(jent_notime_forced(),
		     "asking for the internal timer records the choice");

	JENT_UT_EQ(jent_time_entropy_init(JENT_MIN_OSR,
					  JENT_DISABLE_INTERNAL_TIMER),
		   ENOTIME,
		   "a startup that disables it reports no timer, not no memory");
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */
}

int main(void)
{
	jent_ut_setup();

	/*
	 * Get the startup self test out of the way first. It runs once per
	 * process, on whichever allocation happens to be first, and would
	 * otherwise make the allocation counts below depend on the order the
	 * cases run in.
	 */
	if (jent_entropy_init()) {
		printf("the startup self test does not pass on this machine\n");
		return 77;
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
	test_sha3_alloc_failure();
	test_gcd_failures();
	test_collector_alloc_failures();
	test_init_failures();
	test_recovery_alloc_failure();
	test_secure_memory_failures();
	test_platform_query_failures();
	test_system_fips_mode();
	test_allocator_bounds();
	test_alloc_runs_failing_selftest();
	test_still_usable_afterwards();

	/* Last: it forces the internal timer, which cannot be undone. */
	test_forced_notime_reports_no_timer();

	return jent_ut_report("unit-fault");
}
