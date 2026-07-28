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
 * Architecture-specific high-resolution timestamp source.
 *
 * Provides jent_get_nstime(), which reads the highest-resolution counter
 * available on the target. Every backend is defined in
 * arch/jitterentropy-arch-timer.c; the dispatch order is:
 *
 *   - Windows ARM / ARM64 (MSVC / MinGW) -> QueryPerformanceCounter()
 *   - x86 / x86_64       -> __rdtsc() intrinsic in user space (<intrin.h> on
 *                           MSVC, <x86intrin.h> elsewhere), rdtsc inline asm
 *                           in the kernel
 *   - aarch64            -> mrs <reg> (cntvct_el0 by default), Apple included
 *   - s390x              -> stcke inline asm
 *   - AIX                -> the PowerPC timebase, via the GCC/clang builtin
 *                           where available and read_real_time() otherwise
 *   - powerpc            -> __builtin_ppc_get_timebase()
 *   - riscv              -> rdtime (RV64), or rdtimeh/rdtime retry pair (RV32);
 *                           override via RISCV_NSTIME_INSN[_HI] to use rdcycle
 *   - sparc64            -> rd %tick
 *   - loongarch64        -> rdtime.d
 *   - no counter instruction, Linux kernel
 *                        -> random_get_entropy and ktime_get_ns as fallback
 *   - no counter instruction, user space
 *                        -> mach_absolute_time() on Mach,
 *                           clock_gettime(CLOCK_MONOTONIC) elsewhere
 *
 * The dispatch is by architecture first and by execution environment second:
 * every backend that is a counter read reachable from the instruction set is
 * used in kernel mode exactly as it is in user space. On those architectures
 * the kernel's own random_get_entropy() ends in get_cycles(), i.e. in the very
 * same instruction, so going through it would buy nothing while making the
 * entropy core depend on <linux/timex.h>. Only where the instruction set
 * offers no counter at all does the kernel backend earn its place.
 *
 * Keeping the backends out of this header is what keeps their platform headers
 * - <windows.h>, <x86intrin.h>, the Mach headers, <linux/timex.h> - out of the
 * entropy-collection core, which includes this file through
 * src/jitterentropy-internal.h. That is a hard requirement in the Linux kernel,
 * where linux_kernel/Kbuild.source compiles the core at -O0 and <linux/timex.h>
 * does not survive that; see the note at the top of
 * arch/jitterentropy-arch-timer.c. The out-of-line call costs one branchless
 * call/return per timestamp, which is constant overhead: it delays the
 * measurement uniformly rather than removing jitter from it, and the -O0
 * requirement exists to protect the measured loops, not the counter read.
 */

#ifndef _JITTERENTROPY_ARCH_TIMER_H
#define _JITTERENTROPY_ARCH_TIMER_H

/*
 * Only the fixed-width types the declaration below needs. The platform headers
 * the backends use belong to arch/jitterentropy-arch-timer.c.
 */
#ifdef LINUX_KERNEL
#include <linux/types.h>
#else
#include <stdint.h>
#endif

void jent_get_nstime(uint64_t *out);

#endif /* _JITTERENTROPY_ARCH_TIMER_H */
