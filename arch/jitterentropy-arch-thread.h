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
 * but increment a counter. This header declares the two pieces of code
 * that differ between platforms and defines the context struct they use;
 * the definitions live in arch/jitterentropy-arch-thread.c.
 *
 *   1. Thread creation / joining. jent_notime_thread_create() /
 *      jent_notime_thread_join() hide the back-end behind a single
 *      signature and a single context struct (struct jent_notime_ctx).
 *
 *   2. Pinning the calling thread to a single logical CPU via
 *      jent_thread_pin_to_cpu(). Keeping the counting thread on one CPU
 *      avoids inter-core migration of the running counter.
 *
 * Three execution environments are distinguished: hosted userspace
 * (JENT_ARCH_THREAD_HOSTED, POSIX or C11 threads plus the native affinity
 * API) and the freestanding Linux kernel / FreeBSD kernel / baremetal
 * targets (stub handler, pinning is a no-op). Pinning is best-effort:
 * callers treat a negative return as "not pinned" and continue.
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
#if defined(__KERNEL__) || defined(LINUX_KERNEL)
	/*
	 * Match both the kernel's own __KERNEL__ and the build-system macro
	 * LINUX_KERNEL used by every other arch file, so a TU compiled with
	 * only one of them cannot pair the kernel memory backend with the
	 * hosted thread backend.
	 */
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
# ifdef __STDC_NO_THREADS__
	/*
	 * No auto-fallback to pthreads here: JENT_PTHREAD changes the callback
	 * signature of the public struct jent_notime_thread (jitterentropy.h),
	 * so it must be selected consistently by the build system for every
	 * translation unit including external consumers, not silently by this
	 * header.
	 */
#  error "C11 <threads.h> is unavailable (__STDC_NO_THREADS__); build with -DJENT_PTHREAD and link against pthreads instead"
# endif
# include <threads.h>
struct jent_notime_ctx {
	thrd_t notime_thread_id;		/* thread ID */
	unsigned long notime_cpu;		/* CPU the thread pins to */
	int notime_thread_started;		/* thread successfully created? */
};

typedef int (*jent_notime_start_routine)(void *);
#endif

#else /* freestanding: LINUX_KERNEL / FREEBSD_KERNEL / BAREMETAL */

struct jent_notime_ctx {
	unsigned long notime_cpu;		/* CPU the thread pins to */
};

typedef int (*jent_notime_start_routine)(void *);

#endif /* JENT_ARCH_THREAD_HOSTED */

/* Definitions in arch/jitterentropy-arch-thread.c. */
int jent_thread_pin_to_cpu(unsigned long cpu);
int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine routine,
			      void *arg);
void jent_notime_thread_join(struct jent_notime_ctx *ctx);

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

#endif /* _JITTERENTROPY_ARCH_THREAD_H */
