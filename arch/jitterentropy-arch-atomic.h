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
 * Atomic load and store of the library's process-wide state.
 *
 * An entropy collector belongs to one user and needs no synchronization of its
 * own. What is shared is the state around the instances: whether the startup
 * self tests have run, whether the internal timer has been forced, whether the
 * configuration switches are still open, and the memoized cache geometry. Each of
 * those is a latch or a memo - written once, with the value it would have been
 * given by any other writer, and read by every later caller.
 *
 * That makes a race on them harmless in effect but a data race by the memory
 * model all the same: the compiler is entitled to reload, to widen a store, or
 * to hoist a read out of a branch, and a thread sanitizer reports every one of
 * them, which buries whatever real finding a run has. Accessing them through
 * these helpers states what they are instead.
 *
 * Provided are a load and a store per width the library latches on:
 *
 *   - jent_atomic_load_int()  / jent_atomic_store_int()  for the flags,
 *   - jent_atomic_load_u32()  / jent_atomic_store_u32()  for the memos.
 *
 * The loads take a plain pointer rather than a pointer to const: the kernel's
 * smp_load_acquire() derives a local of the operand's type, which a const
 * qualifier breaks on the releases that predate __unqual_scalar_typeof(), and
 * the Windows intrinsics would have to cast the qualifier away again.
 *
 * The load acquires and the store releases, which is what the one-time state
 * needs: a thread that sees a latch set must also see everything the thread
 * that set it published beforehand - the self test verdict published by
 * jent_selftest_run stands for the timer GCD and the switch blocks established
 * with it.
 *
 * jent_atomic_exchange_int() is the one read-modify-write, and it exists for
 * the one place that has to know whether it was the thread that set a latch:
 * the common timer GCD is a 64-bit value, and a claim taken on a 32-bit flag
 * lets exactly one thread write it - which keeps these helpers to the widths
 * every target has a lock-free access for. A 64-bit atomic would be a call
 * into libatomic on the 32-bit platforms that cannot do one inline.
 */

#ifndef _JITTERENTROPY_ARCH_ATOMIC_H
#define _JITTERENTROPY_ARCH_ATOMIC_H

/*
 * All defined in arch/jitterentropy-arch-atomic.c, which is the only place the
 * kernel and the Windows atomic headers are included. That is why they are
 * out-of-line functions rather than the inline definitions that used to stand
 * here: nothing on these paths is measured, and every source in the project
 * includes this header.
 */
int jent_atomic_load_int(int *ptr);
void jent_atomic_store_int(int *ptr, int val);

uint32_t jent_atomic_load_u32(uint32_t *ptr);
void jent_atomic_store_u32(uint32_t *ptr, uint32_t val);

int jent_atomic_exchange_int(int *ptr, int val);

#endif /* _JITTERENTROPY_ARCH_ATOMIC_H */
