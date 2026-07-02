/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific scheduler yield.
 *
 * Definition of jent_yield() (declared in arch/jitterentropy-arch-sched.h). It
 * combines a CPU-level pause hint with an OS-level scheduler yield; see that
 * header for the dispatch rationale.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

#include <linux/sched.h>	/* schedule() */
# define JENT_ARCH_SCHED_LINUX_KERNEL

#else /* LINUX_KERNEL */

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# define JENT_ARCH_SCHED_OS_WINDOWS
#elif defined(__unix__) || defined(__APPLE__) || defined(_AIX) || \
      defined(__sun)    || defined(__HAIKU__) || defined(__CYGWIN__)
# include <sched.h>
# define JENT_ARCH_SCHED_OS_POSIX
#endif

#if defined(__x86_64__) || defined(__i386__) || \
    defined(_M_X64)     || defined(_M_IX86)
# if defined(_MSC_VER)
#  include <intrin.h>
# else
#  include <x86intrin.h>
# endif
# define JENT_ARCH_SCHED_PAUSE_X86
#elif defined(__aarch64__) || \
      (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
# define JENT_ARCH_SCHED_PAUSE_ARM
#elif defined(__powerpc) || defined(__powerpc__)
# define JENT_ARCH_SCHED_PAUSE_POWERPC
#endif

#endif /* LINUX_KERNEL */

void jent_yield(void)
{
#if defined(JENT_ARCH_SCHED_PAUSE_X86)
	_mm_pause();
#elif defined(JENT_ARCH_SCHED_PAUSE_ARM)
	__asm__ __volatile__("yield" ::: "memory");
#elif defined(JENT_ARCH_SCHED_PAUSE_POWERPC)
	__asm__ __volatile__("or 27,27,27" ::: "memory");
#endif

#if defined(JENT_ARCH_SCHED_OS_WINDOWS)
	SwitchToThread();
#elif defined(JENT_ARCH_SCHED_OS_POSIX)
	(void)sched_yield();
#elif defined(JENT_ARCH_SCHED_LINUX_KERNEL)
	schedule();
#endif
}
