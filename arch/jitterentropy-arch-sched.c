/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific scheduler yield.
 *
 * Definition of jent_yield() (declared in arch/jitterentropy-arch-sched.h). It
 * combines a CPU-level pause hint with an OS-level scheduler yield; see that
 * header for the dispatch rationale.
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
#elif defined(_M_ARM64) || defined(_M_ARM)
/*
 * Windows on ARM. MSVC defines neither __aarch64__ nor __arm__, so without
 * these two macros the branch below did not match and a Windows-on-ARM build
 * emitted no yield hint at all - the same pair jitterentropy-arch-timer.c
 * matches for its QueryPerformanceCounter back-end. The hint goes through the
 * intrinsic because MSVC rejects inline asm on ARM targets entirely.
 */
# include <intrin.h>
# define JENT_ARCH_SCHED_PAUSE_ARM_INTRIN
#elif defined(__aarch64__) || \
      (defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 7)
# define JENT_ARCH_SCHED_PAUSE_ARM
#elif defined(__powerpc) || defined(__powerpc__)
# define JENT_ARCH_SCHED_PAUSE_POWERPC
#elif defined(__riscv)
# define JENT_ARCH_SCHED_PAUSE_RISCV
#endif

#endif /* LINUX_KERNEL */

void jent_yield(void)
{
#if defined(JENT_ARCH_SCHED_PAUSE_X86)
	_mm_pause();
#elif defined(JENT_ARCH_SCHED_PAUSE_ARM_INTRIN)
	__yield();
#elif defined(JENT_ARCH_SCHED_PAUSE_ARM)
	__asm__ __volatile__("yield" ::: "memory");
#elif defined(JENT_ARCH_SCHED_PAUSE_POWERPC)
	__asm__ __volatile__("or 27,27,27" ::: "memory");
#elif defined(JENT_ARCH_SCHED_PAUSE_RISCV)
	/*
	 * The Zihintpause "pause" hint, spelled as its raw encoding rather than
	 * as the mnemonic. The assembler only accepts "pause" when the
	 * extension is enabled in the target's -march string, which it usually
	 * is not; the instruction itself is architecturally a FENCE hint and so
	 * executes as a no-op on hardware that predates Zihintpause. Emitting
	 * the encoding directly therefore works everywhere.
	 */
	__asm__ __volatile__(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
#endif

#if defined(JENT_ARCH_SCHED_OS_WINDOWS)
	SwitchToThread();
#elif defined(JENT_ARCH_SCHED_OS_POSIX)
	(void)sched_yield();
#elif defined(JENT_ARCH_SCHED_LINUX_KERNEL)
	schedule();
#endif
}
