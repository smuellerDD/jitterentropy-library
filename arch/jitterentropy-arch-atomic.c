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
 * Dispatch:
 *   - Linux kernel                -> smp_load_acquire() / smp_store_release()
 *   - GCC / Clang (any target,    -> __atomic_load_n() / __atomic_store_n()
 *     the FreeBSD kernel and          with __ATOMIC_ACQUIRE / _RELEASE
 *     baremetal builds included)
 *   - MSVC                        -> the Interlocked intrinsics
 *   - anything else               -> C11 <stdatomic.h>
 *
 * Lock-free 32-bit and pointer atomics are required, and there is no fallback
 * for a target without them: ARMv6-M (no LDREX/STREX) or RISC-V without the A
 * extension fails to compile here, as does a compiler offering none of the
 * above. What such a target has instead is a libatomic call, which locks, or
 * a plain access, which is a data race.
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#if defined(LINUX_KERNEL) || defined(__KERNEL__)

#include <asm/barrier.h>
#include <linux/atomic.h>

int jent_atomic_load_int(const int *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_int(int *ptr, int val)
{
	smp_store_release(ptr, val);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return smp_load_acquire(ptr);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	uint32_t old;

	do {
		old = READ_ONCE(*ptr);
	} while (cmpxchg(ptr, old, old + 1) != old);

	return old + 1;
}

uint32_t jent_atomic_cmpxchg_u32(uint32_t *ptr, uint32_t old, uint32_t val)
{
	return cmpxchg(ptr, old, val);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	smp_store_release(ptr, val);
}

jent_fips_failure_cb
jent_atomic_load_fips_cb(const jent_fips_failure_cb *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_fips_cb(jent_fips_failure_cb *ptr,
			       jent_fips_failure_cb val)
{
	smp_store_release(ptr, val);
}

#elif defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))

/*
 * 2 is always lock-free; anything less makes the builtins libatomic calls.
 * clang-cl defines the __CLANG_ macros only.
 */
#if defined(__GCC_ATOMIC_INT_LOCK_FREE) && \
    defined(__GCC_ATOMIC_POINTER_LOCK_FREE)
# define JENT_ATOMIC_LOCK_FREE						       \
	(__GCC_ATOMIC_INT_LOCK_FREE == 2 && __GCC_ATOMIC_POINTER_LOCK_FREE == 2)
#elif defined(__CLANG_ATOMIC_INT_LOCK_FREE) && \
      defined(__CLANG_ATOMIC_POINTER_LOCK_FREE)
# define JENT_ATOMIC_LOCK_FREE						       \
	(__CLANG_ATOMIC_INT_LOCK_FREE == 2 &&				       \
	 __CLANG_ATOMIC_POINTER_LOCK_FREE == 2)
#else
# define JENT_ATOMIC_LOCK_FREE 0
#endif

#if !JENT_ATOMIC_LOCK_FREE
# error "The Jitter RNG requires lock-free 32-bit and pointer atomics"
#endif
#undef JENT_ATOMIC_LOCK_FREE

int jent_atomic_load_int(const int *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_int(int *ptr, int val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	return __atomic_add_fetch(ptr, 1, __ATOMIC_SEQ_CST);
}

uint32_t jent_atomic_cmpxchg_u32(uint32_t *ptr, uint32_t old, uint32_t val)
{
	/* On failure, old is updated to the value found. */
	__atomic_compare_exchange_n(ptr, &old, val, 0, __ATOMIC_SEQ_CST,
				    __ATOMIC_SEQ_CST);
	return old;
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

/* The builtins take any scalar, a pointer to a function included. */
jent_fips_failure_cb
jent_atomic_load_fips_cb(const jent_fips_failure_cb *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_fips_cb(jent_fips_failure_cb *ptr,
			       jent_fips_failure_cb val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

#elif defined(_MSC_VER)

#include <intrin.h>

/*
 * The loads are an OR of 0 - a read-modify-write that stores back what it read
 * - because that is the one Interlocked form that is a full barrier on every
 * target MSVC builds for: a volatile read is an acquire on x86 and x64 but not
 * on Arm64, where /volatile:iso is the default. The const the loads take is
 * cast away for that reason alone. No object handed to them is defined const -
 * they are the library's own writable latches, seen through a const pointer by
 * a reader such as jent_status() - so the store never meets read-only memory.
 */
int jent_atomic_load_int(const int *ptr)
{
	return (int)_InterlockedOr((volatile long *)ptr, 0);
}

void jent_atomic_store_int(int *ptr, int val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return (uint32_t)_InterlockedOr((volatile long *)ptr, 0);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	return (uint32_t)_InterlockedIncrement((volatile long *)ptr);
}

uint32_t jent_atomic_cmpxchg_u32(uint32_t *ptr, uint32_t old, uint32_t val)
{
	return (uint32_t)_InterlockedCompareExchange((volatile long *)ptr,
						     (long)val, (long)old);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

jent_fips_failure_cb
jent_atomic_load_fips_cb(const jent_fips_failure_cb *ptr)
{
	return (jent_fips_failure_cb)(uintptr_t)
		_InterlockedCompareExchangePointer((void * volatile *)ptr,
						   NULL, NULL);
}

void jent_atomic_store_fips_cb(jent_fips_failure_cb *ptr,
			       jent_fips_failure_cb val)
{
	(void)_InterlockedExchangePointer((void * volatile *)ptr,
					  (void *)(uintptr_t)val);
}

#else /* any other compiler: C11 atomics */

#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 201112L || \
    defined(__STDC_NO_ATOMICS__)
# error "The Jitter RNG requires lock-free 32-bit and pointer atomics"
#endif

#include <stdatomic.h>

#if ATOMIC_INT_LOCK_FREE != 2 || ATOMIC_POINTER_LOCK_FREE != 2
# error "The Jitter RNG requires lock-free 32-bit and pointer atomics"
#endif

/*
 * The casts assume that a lock-free atomic shares the plain type's
 * representation (xlc, Oracle Studio). The const of the loads is cast away
 * with them: atomic_load() takes a pointer to const only from C17 on.
 */
int jent_atomic_load_int(const int *ptr)
{
	return atomic_load_explicit((_Atomic int *)ptr, memory_order_acquire);
}

void jent_atomic_store_int(int *ptr, int val)
{
	atomic_store_explicit((_Atomic int *)ptr, val, memory_order_release);
}

uint32_t jent_atomic_load_u32(const uint32_t *ptr)
{
	return atomic_load_explicit((_Atomic uint32_t *)ptr,
				    memory_order_acquire);
}

uint32_t jent_atomic_inc_u32(uint32_t *ptr)
{
	return atomic_fetch_add((_Atomic uint32_t *)ptr, 1) + 1;
}

uint32_t jent_atomic_cmpxchg_u32(uint32_t *ptr, uint32_t old, uint32_t val)
{
	/* On failure, old is updated to the value found. */
	atomic_compare_exchange_strong((_Atomic uint32_t *)ptr, &old, val);
	return old;
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	atomic_store_explicit((_Atomic uint32_t *)ptr, val,
			      memory_order_release);
}

jent_fips_failure_cb
jent_atomic_load_fips_cb(const jent_fips_failure_cb *ptr)
{
	return atomic_load_explicit((_Atomic(jent_fips_failure_cb) *)ptr,
				    memory_order_acquire);
}

void jent_atomic_store_fips_cb(jent_fips_failure_cb *ptr,
			       jent_fips_failure_cb val)
{
	atomic_store_explicit((_Atomic(jent_fips_failure_cb) *)ptr, val,
			      memory_order_release);
}

#endif
