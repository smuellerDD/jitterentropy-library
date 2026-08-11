/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific thread handling for the internal timer.
 *
 * Definitions of jent_thread_pin_to_cpu(), jent_notime_thread_create() and
 * jent_notime_thread_join() (declared in arch/jitterentropy-arch-thread.h). See
 * that header for the dispatch rationale. This is userspace-only; the Linux
 * kernel build does not use the internal timer thread.
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
 * _GNU_SOURCE exposes the Linux CPU-affinity interfaces used below on glibc
 * (the CPU_* set macros, sched_setaffinity(), pthread_setaffinity_np()). It is
 * defined here, in the translation unit that needs it, rather than in the
 * public jitterentropy.h so the installed header does not impose a feature-test
 * macro on consumers; it must precede every system header.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

/*
 * GetLogicalProcessorInformationEx(), RelationGroup, SetThreadGroupAffinity()
 * and the GROUP_AFFINITY / GROUP_RELATIONSHIP structs are declared by the
 * Windows SDK only when the translation unit asks for Windows 7 or newer.
 * mingw-w64 has defaulted to older values across its releases, so the minimum
 * is stated here rather than left to the toolchain; like _GNU_SOURCE above it
 * must precede every system header, including the <windows.h> included below.
 * An externally supplied, higher value is left alone.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"
#include "jitterentropy-arch-thread.h"	/* not pulled in by internal.h */

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

#if defined(JENT_ARCH_THREAD_HOSTED)

#include <errno.h>

/* CPU pinning back-end selection */
#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# include <stddef.h>	/* offsetof() */
# include <stdlib.h>	/* malloc(), free() */
# define JENT_ARCH_THREAD_PIN_WINDOWS
#elif defined(__linux__)
# include <sched.h>
/* CPU_ALLOC() below is __sched_cpualloc() on glibc, but calloc() on musl. */
# include <stdlib.h>
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

#ifdef JENT_WIN_THREADS
/*
 * <windows.h> already came in with the pinning back-end above - the Win32
 * thread back-end is only ever selected on the same platforms - but it is
 * named again so this block does not silently depend on that.
 */
# include <windows.h>
# include <process.h>	/* _beginthreadex() */
# include <stdint.h>	/* uintptr_t */
#endif

/*
 * Pin the calling thread to a single logical CPU.
 *
 * Returns 0 on success or a negative errno on failure. The request is
 * advisory from the caller's point of view: the internal timer keeps
 * working even when pinning is unavailable or rejected.
 */
int jent_thread_pin_to_cpu(unsigned long cpu)
{
#if defined(JENT_ARCH_THREAD_PIN_WINDOWS)
	/*
	 * A processor group holds at most 64 logical CPUs, so the flat CPU
	 * index is resolved to a (group, in-group bit) pair by walking the
	 * groups. This lets us pin to CPUs beyond 64 on systems that span
	 * multiple processor groups. The flat index space is the one
	 * jent_ncpu() reports, i.e. the active processors of all groups
	 * concatenated in group order.
	 *
	 * The groups are enumerated with GetLogicalProcessorInformationEx()
	 * rather than counted with GetActiveProcessorGroupCount() /
	 * GetActiveProcessorCount(): the index selects the n-th *active*
	 * processor, and only ActiveProcessorMask says which bit positions
	 * those actually are. Deriving the bit from the count alone assumes
	 * the active processors occupy the lowest bits of the group without a
	 * gap, which stops holding as soon as one is parked or disabled - the
	 * thread would then be pinned to a different CPU than the caller asked
	 * for, or to an inactive one.
	 */
	DWORD len = 0;
	BYTE *buffer;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec;
	GROUP_RELATIONSHIP *groups;
	/* Bytes that must be readable before Relationship and Size are read. */
	const size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
				    Group);
	size_t need;
	unsigned long idx = cpu;
	WORD group;
	int ret = -EINVAL;

	if (!GetLogicalProcessorInformationEx(RelationGroup, NULL, &len) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return -EFAULT;

	buffer = (BYTE *)malloc(len);
	if (!buffer)
		return -ENOMEM;

	if (!GetLogicalProcessorInformationEx(
			RelationGroup,
			(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer,
			&len)) {
		free(buffer);
		return -EFAULT;
	}

	/*
	 * RelationGroup is reported as a single record covering every group,
	 * with the per-group entries as a trailing array. Validate the header,
	 * then the array the announced ActiveGroupCount implies, before either
	 * is dereferenced.
	 */
	rec = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer;
	need = hdr + offsetof(GROUP_RELATIONSHIP, GroupInfo);
	if ((size_t)len < hdr || (size_t)len < rec->Size ||
	    rec->Relationship != RelationGroup || rec->Size < need) {
		free(buffer);
		return -EFAULT;
	}

	groups = &rec->Group;
	need += (size_t)groups->ActiveGroupCount * sizeof(PROCESSOR_GROUP_INFO);
	if (rec->Size < need) {
		free(buffer);
		return -EFAULT;
	}

	for (group = 0; group < groups->ActiveGroupCount; group++) {
		const PROCESSOR_GROUP_INFO *gi = &groups->GroupInfo[group];
		unsigned long seen = 0;
		unsigned int bit;

		if (idx >= (unsigned long)gi->ActiveProcessorCount) {
			idx -= gi->ActiveProcessorCount;
			continue;
		}

		/* The idx-th set bit of this group's active mask. */
		for (bit = 0; bit < (unsigned int)(sizeof(KAFFINITY) * 8);
		     bit++) {
			GROUP_AFFINITY ga;

			if (!((gi->ActiveProcessorMask >> bit) & (KAFFINITY)1))
				continue;
			if (seen++ != idx)
				continue;

			ZeroMemory(&ga, sizeof(ga));
			ga.Group = group;
			ga.Mask = (KAFFINITY)1 << bit;
			ret = SetThreadGroupAffinity(GetCurrentThread(), &ga,
						     NULL) ? 0 : -EFAULT;
			break;
		}
		break;
	}

	free(buffer);
	return ret;
#elif defined(JENT_ARCH_THREAD_PIN_LINUX)
	/*
	 * A cpu_set_t holds CPU_SETSIZE bits - 1024 on glibc - which is a
	 * smaller machine than jent_cpu_highest() can name a CPU of: it grows
	 * its own mask up to JENT_NCPU_SET_MAX, and refusing those CPUs here
	 * would drop the pinning on exactly the machines large enough for the
	 * distinction it draws to matter. Above CPU_SETSIZE the set is
	 * therefore allocated wide enough for @cpu, which is also what
	 * sched_setaffinity(2) wants of a kernel with a larger CPU mask.
	 *
	 * The fixed set below stays for the common case, so the counting
	 * thread - started and stopped on every entropy request - does not
	 * allocate on its way to a CPU.
	 */
	if (cpu < (unsigned long)CPU_SETSIZE) {
		cpu_set_t set;

		CPU_ZERO(&set);
		CPU_SET((size_t)cpu, &set);
		if (sched_setaffinity(0, sizeof(set), &set))
			return -errno;
		return 0;
	} else {
		unsigned int ncpu_set = CPU_SETSIZE;
		cpu_set_t *set;
		size_t size;
		int ret;

		if (cpu >= JENT_NCPU_SET_MAX)
			return -EINVAL;

		while ((unsigned long)ncpu_set <= cpu)
			ncpu_set *= 2;

		size = CPU_ALLOC_SIZE(ncpu_set);
		set = CPU_ALLOC(ncpu_set);
		if (!set)
			return -ENOMEM;

		CPU_ZERO_S(size, set);
		CPU_SET_S((size_t)cpu, size, set);
		ret = sched_setaffinity(0, size, set) ? -errno : 0;
		CPU_FREE(set);

		return ret;
	}
#elif defined(JENT_ARCH_THREAD_PIN_MACOS)
	/*
	 * macOS exposes no CPU pinning API at all. THREAD_AFFINITY_POLICY is
	 * the closest thing, but an affinity tag is only a hint that threads
	 * sharing a tag should share an L2 cache - it does not name a CPU and
	 * does not bind the thread to one. It is used here purely to push the
	 * counting thread into an affinity set of its own (tag 0 means "no
	 * affinity", hence the +1), which is the most the OS allows.
	 *
	 * On Apple Silicon even that is gone: thread_policy_set() returns
	 * KERN_NOT_SUPPORTED for THREAD_AFFINITY_POLICY, so this always fails.
	 * That is reported as -ENOTSUP rather than an error, matching OpenBSD
	 * below. Pinning is advisory, so the internal timer keeps working; the
	 * counting thread simply runs wherever the scheduler puts it.
	 */
	thread_affinity_policy_data_t policy;
	kern_return_t kr;

	/*
	 * affinity_tag is a 32-bit signed integer and tag 0 means "no
	 * affinity": reject CPU indices whose +1 does not fit, instead of
	 * letting the truncated conversion silently request tag 0 (un-pin)
	 * or a negative tag.
	 */
	if (cpu >= INT32_MAX)
		return -EINVAL;

	policy.affinity_tag = (integer_t)(cpu + 1);
	kr = thread_policy_set(pthread_mach_thread_np(pthread_self()),
			       THREAD_AFFINITY_POLICY,
			       (thread_policy_t)&policy,
			       THREAD_AFFINITY_POLICY_COUNT);
	if (kr == KERN_NOT_SUPPORTED)
		return -ENOTSUP;
	if (kr != KERN_SUCCESS)
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

#ifdef JENT_WIN_THREADS
/*
 * Win32 thread entry point.
 *
 * _beginthreadex() wants unsigned __stdcall (*)(void *), which is neither of
 * the signatures the public thread-handler interface offers (struct
 * jent_notime_thread in jitterentropy.h). Rather than adding a third,
 * Windows-only one to that public struct, the routine and its argument travel
 * in the context and this trampoline unpacks them.
 */
static unsigned __stdcall jent_notime_thread_win32(void *arg)
{
	struct jent_notime_ctx *ctx = (struct jent_notime_ctx *)arg;

	return (unsigned)ctx->notime_routine(ctx->notime_arg);
}
#endif

/*
 * Spawn the counting thread running start_routine(arg).
 *
 * Returns 0 on success or a negative errno on failure.
 */
int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine routine,
			      void *arg)
{
#if defined(JENT_PTHREAD)
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
#else /* JENT_WIN_THREADS; the header rejects anything else */
	/*
	 * _beginthreadex() rather than CreateThread(): the counting thread runs
	 * CRT code - jent_thread_pin_to_cpu() above allocates - and a thread
	 * that reaches the CRT without having been started through it leaks the
	 * per-thread state the CRT then initializes lazily. Since the internal
	 * timer starts and stops the thread on every entropy request, that leak
	 * would accumulate for the life of the process. The handle it returns is
	 * a real Win32 HANDLE and is closed by the join below.
	 *
	 * The default stack size (0, i.e. the value in the image header) and no
	 * creation flags are deliberate: the thread only increments a counter,
	 * and it must run immediately rather than be created suspended.
	 */
	uintptr_t handle;

	ctx->notime_routine = routine;
	ctx->notime_arg = arg;

	handle = _beginthreadex(NULL, 0, jent_notime_thread_win32, ctx, 0, NULL);
	if (!handle) {
		/*
		 * _beginthreadex() reports the reason in errno (EAGAIN,
		 * EINVAL, EACCES). It is only meaningful when it was actually
		 * set, hence the fallback.
		 */
		int ret = errno;

		return ret ? -ret : -EAGAIN;
	}

	ctx->notime_thread_id = (void *)handle;
	ctx->notime_thread_started = 1;
	return 0;
#endif
}

/* Wait for the counting thread to terminate and release its resources. */
void jent_notime_thread_join(struct jent_notime_ctx *ctx)
{
	/*
	 * Nothing to do if the thread was never created. Joining a zeroed/
	 * uninitialized thread ID is undefined behavior (e.g. a crash or
	 * ESRCH) and would be reached on every thread-creation failure.
	 */
	if (!ctx->notime_thread_started)
		return;

#if defined(JENT_PTHREAD)
	pthread_join(ctx->notime_thread_id, NULL);
	pthread_attr_destroy(&ctx->notime_pthread_attr);
#else /* JENT_WIN_THREADS */
	/*
	 * The wait is the join; the handle then has to be closed explicitly, as
	 * a Win32 thread handle keeps the (already terminated) thread object
	 * alive until its last reference goes away. Clearing it afterwards keeps
	 * a second join from operating on a closed handle.
	 */
	WaitForSingleObject((HANDLE)ctx->notime_thread_id, INFINITE);
	CloseHandle((HANDLE)ctx->notime_thread_id);
	ctx->notime_thread_id = NULL;
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
int jent_thread_pin_to_cpu(unsigned long cpu)
{
	(void)cpu;
	return -1;
}

int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine routine,
			      void *arg)
{
	(void)ctx;
	(void)routine;
	(void)arg;
	return -1;
}

void jent_notime_thread_join(struct jent_notime_ctx *ctx)
{
	(void)ctx;
}

#endif /* JENT_ARCH_THREAD_HOSTED */

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */
