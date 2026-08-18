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
 * configuration switches are still open, and the memoized cache geometry. Each
 * of those is a latch or a memo - written once, with the value it would have
 * been given by any other writer, and read by every later caller.
 *
 * That makes a race on them harmless in effect but a data race by the memory
 * model all the same: the compiler is entitled to reload, to widen a store, or
 * to hoist a read out of a branch, and a thread sanitizer reports every one of
 * them, which buries whatever real finding a run has. Accessing them through
 * this header states what they are instead.
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
 * lets exactly one thread write it - which keeps this header to the widths
 * every target has a lock-free access for. A 64-bit atomic would be a call
 * into libatomic on the 32-bit platforms that cannot do one inline.
 *
 * The dispatch is:
 *   - Linux kernel                -> smp_load_acquire() / smp_store_release()
 *                                    and xchg()
 *   - GCC / Clang (any target,    -> __atomic_load_n() / __atomic_store_n() /
 *     the FreeBSD kernel and the      __atomic_exchange_n() with
 *     baremetal builds included)      __ATOMIC_ACQUIRE / _RELEASE / _ACQ_REL
 *   - MSVC                        -> the Interlocked intrinsics
 *   - anything else               -> volatile access
 *
 * The last is what the code did before this header existed: a compiler with
 * neither the builtins nor the intrinsics gets no ordering guarantee beyond
 * the natural width of the access, which is the guarantee these latches were
 * relying on all along. It is a fallback, not a supported concurrency model.
 */

#ifndef _JITTERENTROPY_ARCH_ATOMIC_H
#define _JITTERENTROPY_ARCH_ATOMIC_H

#if defined(LINUX_KERNEL) || defined(__KERNEL__)

#include <asm/barrier.h>
#include <linux/atomic.h>

static inline int jent_atomic_load_int(int *ptr)
{
	return smp_load_acquire(ptr);
}

static inline void jent_atomic_store_int(int *ptr, int val)
{
	smp_store_release(ptr, val);
}

static inline uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return smp_load_acquire(ptr);
}

static inline void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	smp_store_release(ptr, val);
}

static inline int jent_atomic_exchange_int(int *ptr, int val)
{
	return xchg(ptr, val);
}

#elif defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))

static inline int jent_atomic_load_int(int *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void jent_atomic_store_int(int *ptr, int val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

static inline uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

static inline void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

static inline int jent_atomic_exchange_int(int *ptr, int val)
{
	return __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL);
}

#elif defined(_MSC_VER)

#include <intrin.h>

/*
 * MSVC has no __atomic builtins; the Interlocked intrinsics are the primitives
 * and they are typed on long, which is the 32-bit type on every Windows ABI -
 * the same width as the int and the uint32_t latched on here, so the pointer is
 * cast rather than the state being widened. MSVC performs no type-based alias
 * analysis, which is what makes that cast the documented way to use these.
 *
 * Both intrinsics are full barriers, which is stronger than the acquire and
 * release this header promises. Nothing here is on a path where that costs
 * anything: every one of them runs at initialization, at configuration or once
 * per collector allocation.
 *
 * _InterlockedOr(ptr, 0) is the load - Windows offers no plain interlocked
 * read, and an OR of zero returns the value without changing it.
 */
static inline int jent_atomic_load_int(int *ptr)
{
	return (int)_InterlockedOr((volatile long *)ptr, 0);
}

static inline void jent_atomic_store_int(int *ptr, int val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

static inline uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return (uint32_t)_InterlockedOr((volatile long *)ptr, 0);
}

static inline void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

static inline int jent_atomic_exchange_int(int *ptr, int val)
{
	return (int)_InterlockedExchange((volatile long *)ptr, (long)val);
}

#else /* no atomics available */

static inline int jent_atomic_load_int(int *ptr)
{
	return *(volatile int *)ptr;
}

static inline void jent_atomic_store_int(int *ptr, int val)
{
	*(volatile int *)ptr = val;
}

static inline uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return *(volatile uint32_t *)ptr;
}

static inline void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	*(volatile uint32_t *)ptr = val;
}

/* Not atomic at all here - see the note on this fallback above. */
static inline int jent_atomic_exchange_int(int *ptr, int val)
{
	int old = *(volatile int *)ptr;

	*(volatile int *)ptr = val;

	return old;
}

#endif

#endif /* _JITTERENTROPY_ARCH_ATOMIC_H */
