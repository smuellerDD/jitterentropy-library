/*
 * Jitter RNG: unit tests for the replaceable timer-less back end
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
 * A program of its own rather than more cases in unit-fault, because the two
 * cannot share a process. Registering a timer-less back end has to happen
 * before the library initializes - the first initialization blocks any further
 * switch - and forcing the internal timer anywhere sets a one-way global that
 * makes every later collector use the counting thread, which is exactly what
 * unit-fault's interposed time source must not be measured through.
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
 * their platform selection cannot be read here, only repeated. See the same
 * block in unit-fault.c, which carries the reasoning in full.
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

static int fi_fail_mmap;
static int fi_fail_mprotect;
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
	if (fi_fail_mprotect) {
		SetLastError(ERROR_INVALID_ADDRESS);
		return FALSE;
	}
	return VirtualProtect(addr, len, protect, old);
}

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
	if (fi_fail_mprotect) {
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

/*
 * Only the cases behind JENT_CONF_ENABLE_INTERNAL_TIMER arm the injection -
 * with the timer off there is no collector allocation here to deny - so both
 * are unused in that configuration rather than dead.
 */
static JENT_UT_MAYBE_UNUSED void fi_arm(unsigned int nth)
{
	fi_fail_alloc = nth;
	fi_alloc_count = 0;
}

static JENT_UT_MAYBE_UNUSED void fi_disarm(void)
{
	fi_fail_alloc = 0;
	fi_alloc_count = 0;
}


/*
 * The system queries the platform backends build their answers from. Each has
 * a documented "cannot tell" reply that the backends have to handle, and none
 * of those replies can be produced on a machine where the query works.
 *
 * The real ones are captured in wrappers defined before the names are taken
 * over, so the fakes can still forward. The Windows backends have no "cannot
 * tell" reply, so there is nothing to interpose there.
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
static JENT_UT_MAYBE_UNUSED int fi_fail_affinity;

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

static JENT_UT_MAYBE_UNUSED int fi_fail_getrandom;

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

#ifndef FI_WINDOWS
# define sysconf fi_sysconf
#endif
#ifdef __linux__
# define sched_getaffinity fi_sched_getaffinity
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
 * The thread call itself, so the thread backend's own failure handling runs
 * rather than being short-circuited above it: on a refused thread it has to
 * release what it had already acquired and leave the started flag clear, so
 * that the matching join does not operate on a thread ID that was never set.
 *
 * JENT_WIN_THREADS is settled by now - jitterentropy.h came in with the ncpu
 * source above - so the backend the library will compile is known here, unlike
 * the memory one further up.
 *
 * All of it is behind JENT_CONF_ENABLE_INTERNAL_TIMER, as the backend is: with
 * the option off none of these functions or types exist, so there is nothing
 * to interpose. The source is still included so this configuration compiles it
 * too.
 */
#ifndef JENT_CONF_ENABLE_INTERNAL_TIMER

#include "jitterentropy-arch-thread.c"

#elif defined(JENT_WIN_THREADS)

#include <process.h>	/* _beginthreadex() */

static int fi_fail_thread_start;

static uintptr_t fi_beginthreadex(void *security, unsigned int stack_size,
				  unsigned int (__stdcall *start)(void *),
				  void *arg, unsigned int initflag,
				  unsigned int *thrdaddr)
{
	if (fi_fail_thread_start) {
		/*
		 * The failure the backend reads back: _beginthreadex() reports
		 * the reason in errno, and EAGAIN is what a refused thread
		 * gives.
		 */
		errno = EAGAIN;
		return 0;
	}
	return _beginthreadex(security, stack_size, start, arg, initflag,
			      thrdaddr);
}

#define _beginthreadex fi_beginthreadex
#define jent_notime_thread_create jent_fi_real_thread_create
#include "jitterentropy-arch-thread.c"
#undef jent_notime_thread_create
#undef _beginthreadex

#else /* JENT_WIN_THREADS */

#include <pthread.h>

static int fi_fail_pthread_create;
static int fi_fail_pthread_attr;

static int fi_pthread_create_real(pthread_t *t, const pthread_attr_t *a,
				  void *(*r)(void *), void *arg)
{
	return pthread_create(t, a, r, arg);
}

static int fi_pthread_attr_init_real(pthread_attr_t *a)
{
	return pthread_attr_init(a);
}

static int fi_pthread_create(pthread_t *t, const pthread_attr_t *a,
			     void *(*r)(void *), void *arg)
{
	if (fi_fail_pthread_create)
		return EAGAIN;
	return fi_pthread_create_real(t, a, r, arg);
}

static int fi_pthread_attr_init(pthread_attr_t *a)
{
	if (fi_fail_pthread_attr)
		return ENOMEM;
	return fi_pthread_attr_init_real(a);
}

/* libcs may define these as macros themselves (NetBSD does). */
#undef pthread_create
#undef pthread_attr_init
#define pthread_create fi_pthread_create
#define pthread_attr_init fi_pthread_attr_init
#define jent_notime_thread_create jent_fi_real_thread_create
#include "jitterentropy-arch-thread.c"
#undef jent_notime_thread_create
#undef pthread_attr_init
#undef pthread_create

#endif /* JENT_WIN_THREADS */

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

/*
 * The timer-less back end is replaceable: a caller that has its own counting
 * thread registers it with jent_entropy_switch_notime_impl(). Everything here
 * has to run before anything else initializes the library, which blocks the
 * switch for the rest of the process - that block is itself what keeps a
 * caller from losing its timer half way through a run.
 *
 * The replacement below delegates to the built-in one: that goes through the
 * externally-registered path while keeping the counting thread real - a stub
 * would let jent_notime_settick() succeed with nothing incrementing the
 * counter, and the first read would spin forever.
 */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
static unsigned int fi_custom_init_calls;
static unsigned int fi_custom_fini_calls;

/*
 * A context of the handler's own, smaller than the built-in one: the library
 * must not read it as the built-in layout (ASan catches it if it does).
 */
struct fi_custom_ctx {
	void *builtin;
};

static unsigned int fi_custom_starts, fi_custom_stops;

/*
 * An init that fails after having stored a context it then released - legal
 * for a handler, as the library may make nothing of what a failed init left
 * behind. Its fini is never to see that pointer.
 */
static int fi_custom_init_fail;
static void *fi_custom_failed_ctx;
static unsigned int fi_custom_fini_failed_calls;

static int fi_custom_init(void **ctx)
{
	struct fi_custom_ctx *c = malloc(sizeof(*c));

	fi_custom_init_calls++;
	if (!c)
		return -1;
	if (fi_custom_init_fail) {
		*ctx = c;
		fi_custom_failed_ctx = c;
		free(c);
		return -1;
	}
	if (jent_notime_thread_builtin.jent_notime_init(&c->builtin)) {
		free(c);
		return -1;
	}
	*ctx = c;
	return 0;
}

static unsigned int fi_custom_fini_null_calls;

static void fi_custom_fini(void *ctx)
{
	struct fi_custom_ctx *c = ctx;

	fi_custom_fini_calls++;
	/* A handler is only ever handed back the context its init stored. */
	if (!c) {
		fi_custom_fini_null_calls++;
		return;
	}
	/* Compared only, never dereferenced: it has been freed. */
	if (fi_custom_failed_ctx && c == fi_custom_failed_ctx) {
		fi_custom_fini_failed_calls++;
		return;
	}
	jent_notime_thread_builtin.jent_notime_fini(c->builtin);
	free(c);
}

static int fi_custom_start(void *ctx, jent_notime_start_routine r, void *arg)
{
	struct fi_custom_ctx *c = ctx;
	int ret = jent_notime_thread_builtin.jent_notime_start(c->builtin, r,
								arg);

	if (!ret)
		fi_custom_starts++;
	return ret;
}

static void fi_custom_stop(void *ctx)
{
	struct fi_custom_ctx *c = ctx;

	fi_custom_stops++;
	jent_notime_thread_builtin.jent_notime_stop(c->builtin);
}

static struct jent_notime_thread fi_custom_thread = {
	fi_custom_init, fi_custom_fini, fi_custom_start, fi_custom_stop
};

static void test_notime_impl_switch(void)
{
	static struct jent_notime_thread partial;
	struct rand_data *ec;
	char buf[32];

	jent_ut_group("replacing the timer-less back end");

	JENT_UT_EQ(jent_entropy_switch_notime_impl(NULL), -EINVAL,
		   "no implementation at all is refused");

	/* Each callback missing in turn: an incomplete one must not be taken. */
	partial = fi_custom_thread;
	partial.jent_notime_init = NULL;
	JENT_UT_EQ(jent_entropy_switch_notime_impl(&partial), -EINVAL,
		   "one with no init is refused");

	partial = fi_custom_thread;
	partial.jent_notime_fini = NULL;
	JENT_UT_EQ(jent_entropy_switch_notime_impl(&partial), -EINVAL,
		   "one with no fini is refused");

	partial = fi_custom_thread;
	partial.jent_notime_start = NULL;
	JENT_UT_EQ(jent_entropy_switch_notime_impl(&partial), -EINVAL,
		   "one with no start is refused");

	partial = fi_custom_thread;
	partial.jent_notime_stop = NULL;
	JENT_UT_EQ(jent_entropy_switch_notime_impl(&partial), -EINVAL,
		   "one with no stop is refused");

	/*
	 * A complete one is taken, and is what the collector then uses. From a
	 * struct wiped right after the call: the library keeps a copy, so
	 * every use below goes through the callbacks registered here.
	 */
	partial = fi_custom_thread;
	JENT_UT_EQ(jent_entropy_switch_notime_impl(&partial), 0,
		   "a complete implementation is accepted");
	memset(&partial, 0, sizeof(partial));

	/* Pinning the counting thread, configured before initialization. */
	JENT_UT_EQ(jent_entropy_set_notime_cpu(1), 0,
		   "the counting thread CPU can be configured beforehand");

	/*
	 * First a collector that never enables the internal timer - before
	 * the one below forces it for the process. Its release, and that of
	 * the startup's own collectors, has no context to tear down.
	 */
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	jent_entropy_collector_free(ec);
	JENT_UT_EQ(fi_custom_fini_calls, 0,
		   "a collector without the internal timer is not torn down "
		   "through the handler");

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_INTERNAL_TIMER);
	if (ec) {
		JENT_UT_NE(fi_custom_init_calls, 0,
			   "the registered implementation is the one used");
		JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
			   (ssize_t)sizeof(buf),
			   "and it produces entropy");
		jent_entropy_collector_free(ec);
		JENT_UT_NE(fi_custom_fini_calls, 0,
			   "and is torn down with the collector");
		JENT_UT_EQ(fi_custom_stops, fi_custom_starts,
			   "stopping each thread it started exactly once");
	} else {
		JENT_UT_NO_COLLECTOR("the registered implementation",
				     JENT_FORCE_INTERNAL_TIMER);
	}

	/*
	 * An init that fails having stored a pointer it already freed. The
	 * collector is declined, and its teardown must not hand that pointer
	 * to fini - a handler freeing on failure would see a double free.
	 * The fini call count is compared over the allocation alone: a later
	 * allocation may well be handed the same address again.
	 */
	{
		unsigned int fini_before = fi_custom_fini_calls;

		fi_custom_init_fail = 1;
		ec = jent_entropy_collector_alloc(0, JENT_FORCE_INTERNAL_TIMER);
		fi_custom_init_fail = 0;
		JENT_UT_TRUE(ec == NULL,
			     "a collector whose timer init fails is declined");
		jent_entropy_collector_free(ec);
		JENT_UT_EQ(fi_custom_fini_failed_calls, 0,
			   "the context a failed init stored is not torn down");
		JENT_UT_EQ(fi_custom_fini_calls, fini_before,
			   "nor is fini called at all without a context");
		fi_custom_failed_ctx = NULL;
	}

	/*
	 * And from here on the switch is denied: allocating the collector
	 * initialized the library, which is the point at which a caller must
	 * not be able to pull its timer out from under it.
	 */
	JENT_UT_EQ(fi_custom_fini_null_calls, 0,
		   "the handler's fini is never called without a context");

	JENT_UT_EQ(jent_entropy_switch_notime_impl(&jent_notime_thread_builtin),
		   -EAGAIN, "switching afterwards is denied");
	JENT_UT_EQ(jent_entropy_set_notime_cpu(0), -EAGAIN,
		   "and so is moving the counting thread");
}
#else
static void test_notime_impl_switch(void)
{
	jent_ut_group("replacing the timer-less back end");
	JENT_UT_SKIP("the timer-less back end", "not compiled in");
}
#endif

/*
 * The timer-less mode on machines it cannot run on. Every one of these is a
 * configuration the library has to decline cleanly rather than a defect: a
 * single CPU leaves the counting thread nowhere to run, and a refused thread
 * is a resource limit.
 */
static void test_notime_failures(void)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	void *ctx = NULL;

	jent_ut_group("the internal timer where it cannot run");

	/* One CPU: the counting thread would share it with the consumer. */
	fi_ncpu = 1;
	JENT_UT_NE(jent_notime_init(&ctx), 0,
		   "a single CPU is declined");
	fi_ncpu = 0;

	/* No CPU count at all is passed through as the error it is. */
	fi_ncpu = -ENODEV;
	JENT_UT_EQ(jent_notime_init(&ctx), -ENODEV,
		   "an undiscoverable CPU count is passed through");
	fi_ncpu = 0;

	/*
	 * The context allocation, on two CPUs whatever this machine has: the
	 * CPU count is asked first, and one declines before any allocation.
	 */
	fi_ncpu = 2;
	fi_arm(1);
	JENT_UT_EQ(jent_notime_init(&ctx), -ENOMEM,
		   "a denied context allocation is reported");
	fi_disarm();
	fi_ncpu = 0;

	/*
	 * And a collector that asks for the timer when no thread starts. The
	 * thread is started per read, not per collector - but the allocation
	 * runs the startup, which is a read of its own, so it is declined.
	 */
	fi_fail_thread_create = 1;
	{
		struct rand_data *ec =
			jent_entropy_collector_alloc(0,
						     JENT_FORCE_INTERNAL_TIMER);

		JENT_UT_TRUE(ec == NULL,
			     "a collector with no timer thread is declined");
		jent_entropy_collector_free(ec);
	}
	fi_fail_thread_create = 0;

	/*
	 * A collector allocated while the thread still started, read once it
	 * no longer does: the read must fail rather than measure nothing.
	 */
	{
		struct rand_data *ec =
			jent_entropy_collector_alloc(0,
						     JENT_FORCE_INTERNAL_TIMER);
		char buf[32];

		if (ec) {
			fi_fail_thread_create = 1;
			JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
				   JENT_ERR_NOTIME,
				   "a read with no timer thread is reported");
			fi_fail_thread_create = 0;
			jent_entropy_collector_free(ec);
		} else {
			JENT_UT_NO_COLLECTOR("a read with no timer thread",
					     JENT_FORCE_INTERNAL_TIMER);
		}
	}

	/* Working again once nothing is forced. */
	ctx = NULL;
	if (!jent_notime_init(&ctx)) {
		JENT_UT_TRUE(ctx != NULL, "the timer initializes again");
		jent_notime_fini(ctx);
	} else if (!jent_ut_strict() && jent_notime_cpus_missing()) {
		JENT_UT_SKIP("the internal timer",
			     "this machine has no CPU for the counting thread");
	} else {
		JENT_UT_FAIL("%s: the timer does not initialize once nothing "
			     "is forced", "the internal timer");
	}
#else
	jent_ut_group("the internal timer where it cannot run");
	JENT_UT_SKIP("the internal timer", "not compiled in");
#endif
}

/*
 * The thread backend when the system will not give it a thread. Both refusals
 * have to leave the context in a state the matching teardown can handle - a
 * join on a thread that was never created is what would go wrong.
 */
static void test_thread_create_failures(void)
{
#if defined(JENT_CONF_ENABLE_INTERNAL_TIMER) && defined(JENT_PTHREAD)
	void *ctx = NULL;
	int ret;

	jent_ut_group("the thread backend when no thread can be created");

	if (jent_notime_init(&ctx)) {
		JENT_UT_SKIP("the thread backend",
			     "the internal timer does not initialize here");
		return;
	}

	fi_fail_pthread_attr = 1;
	ret = jent_fi_real_thread_create(ctx, jent_notime_sample_timer, NULL);
	fi_fail_pthread_attr = 0;
	JENT_UT_NE(ret, 0, "refused thread attributes are reported");
	JENT_UT_EQ(((struct jent_notime_ctx *)ctx)->notime_thread_started, 0,
		   "and leave no thread marked as started");

	fi_fail_pthread_create = 1;
	ret = jent_fi_real_thread_create(ctx, jent_notime_sample_timer, NULL);
	fi_fail_pthread_create = 0;
	JENT_UT_NE(ret, 0, "a refused thread is reported");
	JENT_UT_EQ(((struct jent_notime_ctx *)ctx)->notime_thread_started, 0,
		   "and leaves no thread marked as started");

	/*
	 * The teardown after a refusal must be safe - this is the join that
	 * would otherwise be handed a thread ID that was never set. Beyond not
	 * crashing, it is to leave the context as it found it.
	 */
	jent_notime_thread_join(ctx);
	JENT_UT_EQ(((struct jent_notime_ctx *)ctx)->notime_thread_started, 0,
		   "the teardown after a refusal skips the join");

	jent_notime_fini(ctx);
#elif defined(JENT_CONF_ENABLE_INTERNAL_TIMER) && defined(JENT_WIN_THREADS)
	void *ctx = NULL;
	int ret;

	jent_ut_group("the thread backend when no thread can be created");

	if (jent_notime_init(&ctx)) {
		JENT_UT_SKIP("the thread backend",
			     "the internal timer does not initialize here");
		return;
	}

	/*
	 * No attribute object to refuse here - the Win32 backend has only the
	 * one call - so this is the whole of its failure path.
	 */
	fi_fail_thread_start = 1;
	ret = jent_fi_real_thread_create(ctx, jent_notime_sample_timer, NULL);
	fi_fail_thread_start = 0;
	JENT_UT_NE(ret, 0, "a refused thread is reported");
	JENT_UT_EQ(((struct jent_notime_ctx *)ctx)->notime_thread_started, 0,
		   "and leaves no thread marked as started");

	/*
	 * The teardown after a refusal must be safe - this is the join that
	 * would otherwise be handed a handle that was never opened. Beyond not
	 * crashing, it is to leave the context as it found it.
	 */
	jent_notime_thread_join(ctx);
	JENT_UT_EQ(((struct jent_notime_ctx *)ctx)->notime_thread_started, 0,
		   "the teardown after a refusal skips the join");

	jent_notime_fini(ctx);
#else
	jent_ut_group("the thread backend when no thread can be created");
	JENT_UT_SKIP("the thread backend", "no thread backend is compiled in");
#endif
}

/*
 * The entry points of the timer-less back end, called directly with the
 * arguments its own callers never produce. Each is a guard that only matters
 * when something above it has already gone wrong.
 */
static void test_notime_entry_guards(void)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	struct rand_data ec;
	void *ctx = NULL;

	jent_ut_group("the timer-less entry points");

	/* Starting a counting thread with no context to start it in. */
	JENT_UT_EQ(jent_notime_thread_builtin.jent_notime_start(
			   NULL, jent_notime_sample_timer, NULL), -EINVAL,
		   "starting with no context is refused");

	/*
	 * Stopping one is a no-op rather than a fault. There is no state to
	 * inspect - with no context there is nothing it could have changed -
	 * so not crashing is the whole of the check.
	 */
	jent_notime_thread_builtin.jent_notime_stop(NULL);
	JENT_UT_TRUE(1, "stopping with no context is a no-op");

	/* The counting routine with no context still has to prime its counter. */
	memset(&ec, 0, sizeof(ec));
	ec.notime_interrupt = 1;
	ec.notime_timer = 0x5a5a;
	jent_notime_sample_timer(&ec);
	JENT_UT_EQ(ec.notime_timer, 0,
		   "the counting routine primes its counter and returns when "
		   "interrupted");

	/*
	 * Ticking a collector that does not use the internal timer. Both calls
	 * are made unconditionally by jent_read_entropy(), so both have to be
	 * no-ops there.
	 */
	memset(&ec, 0, sizeof(ec));
	ec.enable_notime = 0;
	JENT_UT_EQ(jent_notime_settick(&ec), 0,
		   "ticking a collector without the timer is a no-op");
	JENT_UT_EQ(ec.notime_running, 0,
		   "and starts no thread");
	{
		unsigned int stops = fi_custom_stops;

		/* Marked running, so only the timer check keeps it out. */
		ec.notime_running = 1;
		jent_notime_unsettick(&ec);
		JENT_UT_EQ(ec.notime_interrupt, 0,
			   "and so is unticking it");
		JENT_UT_EQ(fi_custom_stops, stops,
			   "which stops no thread");
	}

	/* And the context lifecycle on its own. */
	if (!jent_notime_init(&ctx)) {
		JENT_UT_TRUE(ctx != NULL, "a context is allocated");
		jent_notime_fini(ctx);
	} else {
		JENT_UT_SKIP("the context lifecycle",
			     "the internal timer does not initialize here");
	}
	jent_notime_fini(NULL);
#else
	jent_ut_group("the timer-less entry points");
	JENT_UT_SKIP("the timer-less entry points", "not compiled in");
#endif
}

/*
 * The CPU the counting thread picks when none was configured. It is the
 * highest one of the affinity the thread starts with - that of the reader that
 * starts it - and not of the thread that allocated the collector: pinned by
 * the latter, the counter would land on a CPU the reader is kept off, as
 * sched_setaffinity() does not stop at the mask the thread inherited.
 *
 * Played out on this thread: the context is made with the full mask, the
 * thread is then narrowed to its two lowest CPUs, and the counting routine is
 * run on it. It has to end up on the higher of the two.
 *
 * Before the switch test below, which takes the builtin handler away: only
 * that one's context is pinned from.
 */
#if defined(JENT_CONF_ENABLE_INTERNAL_TIMER) && \
    (defined(__linux__) || defined(FI_WINDOWS))
static void test_notime_pin_default(void)
{
	struct rand_data ec;
	void *ctx = NULL;
	long lo = -1, hi = -1, pinned = -1;
	unsigned int n = 0;
#ifdef FI_WINDOWS
	GROUP_AFFINITY orig, narrow, now;
	unsigned int b;
#else
	cpu_set_t orig, narrow, now;
	int b;
#endif

	jent_ut_group("the default CPU of the counting thread");

#ifdef FI_WINDOWS
	if (!GetThreadGroupAffinity(GetCurrentThread(), &orig)) {
		JENT_UT_SKIP("the default CPU", "the affinity is not readable");
		return;
	}
	narrow = orig;
	narrow.Mask = 0;
	for (b = 0; b < sizeof(KAFFINITY) * 8 && n < 2; b++) {
		if ((orig.Mask >> b) & (KAFFINITY)1) {
			narrow.Mask |= (KAFFINITY)1 << b;
			n++;
		}
	}
#else
	if (sched_getaffinity(0, sizeof(orig), &orig)) {
		JENT_UT_SKIP("the default CPU", "the affinity is not readable");
		return;
	}
	CPU_ZERO(&narrow);
	for (b = 0; b < CPU_SETSIZE && n < 2; b++) {
		if (CPU_ISSET((size_t)b, &orig)) {
			CPU_SET((size_t)b, &narrow);
			n++;
		}
	}
#endif
	if (n < 2 || jent_notime_init(&ctx)) {
		JENT_UT_SKIP("the default CPU",
			     "the internal timer does not initialize here");
		return;
	}

	/* The reader's view: the highest of its two CPUs, flat-numbered. */
#ifdef FI_WINDOWS
	if (!SetThreadGroupAffinity(GetCurrentThread(), &narrow, NULL)) {
#else
	if (sched_setaffinity(0, sizeof(narrow), &narrow)) {
#endif
		jent_notime_fini(ctx);
		JENT_UT_SKIP("the default CPU", "the affinity is not settable");
		return;
	}
	hi = jent_cpu_highest();

	memset(&ec, 0, sizeof(ec));
	ec.notime_thread_ctx = ctx;
	ec.notime_interrupt = 1;
	jent_notime_sample_timer(&ec);

#ifdef FI_WINDOWS
	if (GetThreadGroupAffinity(GetCurrentThread(), &now) &&
	    now.Group == narrow.Group && now.Mask &&
	    !(now.Mask & (now.Mask - 1)))
		pinned = jent_cpu_highest();
	for (b = 0; b < sizeof(KAFFINITY) * 8; b++) {
		if ((narrow.Mask >> b) & (KAFFINITY)1) {
			narrow.Mask = (KAFFINITY)1 << b;
			break;
		}
	}
	/* The lower of the two, alone. */
	if (SetThreadGroupAffinity(GetCurrentThread(), &narrow, NULL))
		lo = jent_cpu_highest();
	SetThreadGroupAffinity(GetCurrentThread(), &orig, NULL);
#else
	if (!sched_getaffinity(0, sizeof(now), &now) && CPU_COUNT(&now) == 1)
		pinned = jent_cpu_highest();
	for (b = 0; b < CPU_SETSIZE; b++) {
		if (CPU_ISSET((size_t)b, &narrow)) {
			lo = b;
			break;
		}
	}
	sched_setaffinity(0, sizeof(orig), &orig);
#endif
	jent_notime_fini(ctx);

	JENT_UT_TRUE(hi >= 0, "the narrowed thread has a highest CPU");
	JENT_UT_EQ(pinned, hi,
		   "the counting thread pins to the highest CPU of the thread "
		   "running it");
	JENT_UT_TRUE(lo >= 0 && lo != pinned,
		     "and not to the lower one it could also run on");
}
#else
static void test_notime_pin_default(void)
{
	jent_ut_group("the default CPU of the counting thread");
	JENT_UT_SKIP("the default CPU", "no affinity to narrow here");
}
#endif

int main(void)
{
	test_notime_pin_default();

	/* First of all: initializing the library blocks the switch. */
	test_notime_impl_switch();
	test_notime_failures();
	test_thread_create_failures();
	test_notime_entry_guards();

	return jent_ut_report("unit-notime");
}
