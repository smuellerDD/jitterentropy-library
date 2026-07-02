/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 *
 * License
 * =======
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
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
 * Architecture / OS-specific thread handling for the internal timer.
 *
 * The internal ("notime") timer spawns a helper thread that does nothing
 * but increment a counter. This header isolates the two pieces of code
 * that differ between platforms:
 *
 *   1. Thread creation / joining. jent_notime_thread_create() /
 *      jent_notime_thread_join() hide the back-end behind a single
 *      signature and a single context struct (struct jent_notime_ctx).
 *
 *   2. Pinning the calling thread to a single logical CPU via
 *      jent_thread_pin_to_cpu(). Keeping the counting thread on one CPU
 *      avoids inter-core migration of the running counter.
 *
 * Three execution environments are distinguished:
 *
 *   - Hosted userspace (JENT_ARCH_THREAD_HOSTED): a full C library is
 *     present. Threads use POSIX threads or C11 threads (selected by
 *     JENT_PTHREAD, see jitterentropy.h) and pinning uses the native OS
 *     affinity API:
 *        - Windows  -> SetThreadGroupAffinity() (resolves the CPU index
 *                      across processor groups, so CPUs beyond 64 are
 *                      reachable)
 *        - Linux    -> sched_setaffinity(2)
 *        - Apple    -> thread_policy_set(THREAD_AFFINITY_POLICY) (an
 *                      affinity hint, ignored on Apple Silicon)
 *        - FreeBSD  -> cpuset_setaffinity(2)
 *        - NetBSD   -> pthread_setaffinity_np(3)
 *        - OpenBSD  -> unsupported (no public affinity API)
 *
 *   - Linux kernel module (JENT_ARCH_THREAD_LINUX_KERNEL, __KERNEL__),
 *     FreeBSD kernel module (JENT_ARCH_THREAD_FREEBSD_KERNEL, _KERNEL) and
 *     baremetal / EFI targets (JENT_ARCH_THREAD_BAREMETAL, e.g. gnu-efi):
 *     no hosted C library and no userspace threading or affinity APIs are
 *     available. The built-in thread handler therefore degrades to a stub
 *     and pinning is a no-op; such builds are expected to register their
 *     own thread handler via jent_entropy_switch_notime_impl().
 *
 * Pinning is best-effort: callers treat a negative return as "not pinned"
 * and continue, so an unsupported platform or a denied request is never
 * fatal.
 */

#ifndef _JITTERENTROPY_ARCH_THREAD_H
#define _JITTERENTROPY_ARCH_THREAD_H

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/*
 * Execution environment selection. A freestanding target is the Linux
 * kernel (__KERNEL__), the FreeBSD kernel (_KERNEL) or a baremetal/EFI
 * environment. The latter is detected from -ffreestanding
 * (__STDC_HOSTED__ == 0, as used by gnu-efi and similar baremetal
 * toolchains) or requested explicitly with JENT_BAREMETAL. The FreeBSD
 * kernel is matched before the generic baremetal test because it also
 * builds freestanding. Everything else is treated as hosted userspace.
 */
#if defined(__KERNEL__)
# define JENT_ARCH_THREAD_LINUX_KERNEL
#elif defined(_KERNEL) && defined(__FreeBSD__)
# define JENT_ARCH_THREAD_FREEBSD_KERNEL
#elif defined(JENT_BAREMETAL) || \
      (defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0))
# define JENT_ARCH_THREAD_BAREMETAL
#else
# define JENT_ARCH_THREAD_HOSTED
#endif

#if defined(JENT_ARCH_THREAD_HOSTED)

#include <errno.h>

/* CPU pinning back-end selection */
#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# define JENT_ARCH_THREAD_PIN_WINDOWS
#elif defined(__linux__)
# include <sched.h>
# define JENT_ARCH_THREAD_PIN_LINUX
#elif defined(__APPLE__)
# include <mach/mach.h>
# include <mach/thread_policy.h>
# include <pthread.h>
# define JENT_ARCH_THREAD_PIN_MACOS
#elif defined(__FreeBSD__)
# include <sys/param.h>
# include <sys/cpuset.h>
# include <pthread.h>
# define JENT_ARCH_THREAD_PIN_FREEBSD
#elif defined(__NetBSD__)
# include <sched.h>
# include <pthread.h>
# define JENT_ARCH_THREAD_PIN_NETBSD
#elif defined(__OpenBSD__)
# define JENT_ARCH_THREAD_PIN_OPENBSD
#endif

/* Threading back-end: pthreads or C11 threads (see jitterentropy.h) */
#ifdef JENT_PTHREAD
# include <pthread.h>
struct jent_notime_ctx {
	pthread_attr_t notime_pthread_attr;	/* pthreads library */
	pthread_t notime_thread_id;		/* pthreads thread ID */
	unsigned long notime_cpu;		/* CPU the thread pins to */
	int notime_thread_started;		/* thread successfully created? */
};

typedef void *(*jent_notime_start_routine)(void *);
#else
# include <threads.h>
struct jent_notime_ctx {
	thrd_t notime_thread_id;		/* thread ID */
	unsigned long notime_cpu;		/* CPU the thread pins to */
	int notime_thread_started;		/* thread successfully created? */
};

typedef int (*jent_notime_start_routine)(void *);
#endif

/*
 * Pin the calling thread to a single logical CPU.
 *
 * Returns 0 on success or a negative errno on failure. The request is
 * advisory from the caller's point of view: the internal timer keeps
 * working even when pinning is unavailable or rejected.
 */
static inline int jent_thread_pin_to_cpu(unsigned long cpu)
{
#if defined(JENT_ARCH_THREAD_PIN_WINDOWS)
	/*
	 * A processor group holds at most 64 logical CPUs, so the flat CPU
	 * index is resolved to a (group, in-group bit) pair by walking the
	 * groups. This lets us pin to CPUs beyond 64 on systems that span
	 * multiple processor groups.
	 */
	WORD group_count = GetActiveProcessorGroupCount();
	WORD group;
	unsigned long idx = cpu;

	for (group = 0; group < group_count; group++) {
		DWORD in_group = GetActiveProcessorCount(group);
		GROUP_AFFINITY ga;

		if (idx >= (unsigned long)in_group) {
			idx -= in_group;
			continue;
		}

		ZeroMemory(&ga, sizeof(ga));
		ga.Group = group;
		ga.Mask = (KAFFINITY)1 << idx;
		if (!SetThreadGroupAffinity(GetCurrentThread(), &ga, NULL))
			return -EFAULT;
		return 0;
	}
	return -EINVAL;
#elif defined(JENT_ARCH_THREAD_PIN_LINUX)
	cpu_set_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;
	CPU_ZERO(&set);
	CPU_SET((int)cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -errno;
	return 0;
#elif defined(JENT_ARCH_THREAD_PIN_MACOS)
	thread_affinity_policy_data_t policy;

	/* Affinity tag 0 means "no affinity", so offset the CPU index. */
	policy.affinity_tag = (integer_t)(cpu + 1);
	if (thread_policy_set(pthread_mach_thread_np(pthread_self()),
			      THREAD_AFFINITY_POLICY,
			      (thread_policy_t)&policy,
			      THREAD_AFFINITY_POLICY_COUNT) != KERN_SUCCESS)
		return -EFAULT;
	return 0;
#elif defined(JENT_ARCH_THREAD_PIN_FREEBSD)
	cpuset_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;
	CPU_ZERO(&set);
	CPU_SET((int)cpu, &set);
	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
			       sizeof(set), &set))
		return -errno;
	return 0;
#elif defined(JENT_ARCH_THREAD_PIN_NETBSD)
	cpuset_t *set = cpuset_create();
	int ret;

	if (!set)
		return -ENOMEM;
	cpuset_zero(set);
	if (cpuset_set((cpuid_t)cpu, set)) {
		cpuset_destroy(set);
		return -EINVAL;
	}
	ret = pthread_setaffinity_np(pthread_self(), cpuset_size(set), set);
	cpuset_destroy(set);
	return ret ? -ret : 0;
#elif defined(JENT_ARCH_THREAD_PIN_OPENBSD)
	/* OpenBSD intentionally exposes no thread-to-CPU affinity API. */
	(void)cpu;
	return -ENOTSUP;
#else
	(void)cpu;
	return -ENOSYS;
#endif
}

/*
 * Spawn the counting thread running start_routine(arg).
 *
 * Returns 0 on success or a negative errno on failure.
 */
static inline int jent_notime_thread_create(struct jent_notime_ctx *ctx,
					    jent_notime_start_routine routine,
					    void *arg)
{
#ifdef JENT_PTHREAD
	int ret = -pthread_attr_init(&ctx->notime_pthread_attr);

	if (ret)
		return ret;
	ret = -pthread_create(&ctx->notime_thread_id,
			      &ctx->notime_pthread_attr, routine, arg);
	if (ret) {
		/*
		 * The thread was not created: destroy the attr (otherwise it
		 * leaks) and leave notime_thread_started clear so the matching
		 * join does not operate on an uninitialized thread ID.
		 */
		pthread_attr_destroy(&ctx->notime_pthread_attr);
		return ret;
	}
	ctx->notime_thread_started = 1;
	return 0;
#else
	switch (thrd_create(&ctx->notime_thread_id, routine, arg)) {
	case thrd_success:
		ctx->notime_thread_started = 1;
		return 0;
	case thrd_nomem:
		return -ENOMEM;
	case thrd_timedout:
		return -ETIMEDOUT;
	case thrd_busy:
		return -EBUSY;
	case thrd_error:
	default:
		return -EINVAL;
	}
#endif
}

/* Wait for the counting thread to terminate and release its resources. */
static inline void jent_notime_thread_join(struct jent_notime_ctx *ctx)
{
	/*
	 * Nothing to do if the thread was never created. Joining a zeroed/
	 * uninitialized thread ID is undefined behavior (e.g. a crash or
	 * ESRCH) and would be reached on every thread-creation failure.
	 */
	if (!ctx->notime_thread_started)
		return;

#ifdef JENT_PTHREAD
	pthread_join(ctx->notime_thread_id, NULL);
	pthread_attr_destroy(&ctx->notime_pthread_attr);
#else
	thrd_join(ctx->notime_thread_id, NULL);
#endif
	ctx->notime_thread_started = 0;
}

#else /* freestanding: LINUX_KERNEL / FREEBSD_KERNEL / BAREMETAL */

/*
 * Freestanding targets have no hosted C library to spawn threads or set
 * CPU affinity. The built-in handler is a stub - such builds register
 * their own thread handler via jent_entropy_switch_notime_impl() - and
 * pinning is a no-op. Error returns avoid <errno.h>, which may be absent.
 */
struct jent_notime_ctx {
	unsigned long notime_cpu;		/* CPU the thread pins to */
};

typedef int (*jent_notime_start_routine)(void *);

static inline int jent_thread_pin_to_cpu(unsigned long cpu)
{
	(void)cpu;
	return -1;
}

static inline int jent_notime_thread_create(struct jent_notime_ctx *ctx,
					    jent_notime_start_routine routine,
					    void *arg)
{
	(void)ctx;
	(void)routine;
	(void)arg;
	return -1;
}

static inline void jent_notime_thread_join(struct jent_notime_ctx *ctx)
{
	(void)ctx;
}

#endif /* JENT_ARCH_THREAD_HOSTED */

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

#endif /* _JITTERENTROPY_ARCH_THREAD_H */
