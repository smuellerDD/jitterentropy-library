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
 * configuration switches are still open, the memoized cache geometry, and the
 * registered FIPS failure callback. All but the last are a latch or a memo -
 * written once, with the value it would have been given by any other writer,
 * and read by every later caller.
 *
 * That makes a race on them harmless in effect but a data race by the memory
 * model all the same: the compiler is entitled to reload, to widen a store, or
 * to hoist a read out of a branch, and a thread sanitizer reports every one of
 * them, which buries whatever real finding a run has. Accessing them through
 * these helpers states what they are instead.
 *
 */

#ifndef _JITTERENTROPY_ARCH_ATOMIC_H
#define _JITTERENTROPY_ARCH_ATOMIC_H

typedef void (*jent_fnptr)(void);

JENT_INTERNAL
int jent_atomic_load_int(const int *ptr);
JENT_INTERNAL
void jent_atomic_store_int(int *ptr, int val);

JENT_INTERNAL
uint32_t jent_atomic_load_u32(const uint32_t *ptr);
JENT_INTERNAL
uint32_t jent_atomic_inc_u32(uint32_t *ptr);
/* Store val if *ptr is old; returns what *ptr held, old on success. */
JENT_INTERNAL
uint32_t jent_atomic_cmpxchg_u32(uint32_t *ptr, uint32_t old, uint32_t val);
JENT_INTERNAL
void jent_atomic_store_u32(uint32_t *ptr, uint32_t val);

JENT_INTERNAL
jent_fnptr jent_atomic_load_fnptr(const jent_fnptr *ptr);
JENT_INTERNAL
void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val);

#endif /* _JITTERENTROPY_ARCH_ATOMIC_H */
