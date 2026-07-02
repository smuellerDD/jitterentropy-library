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
 * Architecture / OS-specific scheduler yield.
 *
 * Provides jent_yield() which combines a CPU-level pause hint (to ease
 * SMT and power contention while the caller is busy-waiting) with an
 * OS-level scheduler yield. The two phases are dispatched independently.
 *
 * CPU pause hint:
 *   - x86 / x86_64           -> _mm_pause() intrinsic
 *   - aarch64                -> 'yield' instruction
 *   - arm (ARMv7+)           -> 'yield' instruction
 *   - powerpc                -> 'or 27,27,27' (low-priority hint)
 *   - Linux kernel           -> schedule()
 *   - other (s390x, riscv,   -> no hint
 *     unknown)
 *
 * OS yield:
 *   - Windows (MSVC / MinGW)             -> SwitchToThread()
 *   - hosted Unix-like (Linux, BSDs,     -> sched_yield()
 *     Apple, AIX, Solaris/illumos,
 *     Haiku, Cygwin)
 *   - other (e.g. baremetal)             -> no-op
 *
 * The CPU hint mirrors what YieldProcessor() does on Windows (which
 * expands to the architecture's pause/yield instruction). On baremetal
 * targets with no scheduler we still emit the CPU hint so a busy-wait
 * loop does not pin SMT siblings unnecessarily.
 */

#ifndef _JITTERENTROPY_ARCH_SCHED_H
#define _JITTERENTROPY_ARCH_SCHED_H

/*
 * Combine a CPU-level pause hint (to ease SMT and power contention while the
 * caller is busy-waiting) with an OS-level scheduler yield. Defined in
 * arch/jitterentropy-arch-sched.c.
 */
void jent_yield(void);

#endif /* _JITTERENTROPY_ARCH_SCHED_H */
