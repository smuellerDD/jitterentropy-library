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
 * Atomic load and store of the library's process-wide state - the
 * implementation. What these are for and what they promise is in
 * arch/jitterentropy-arch-atomic.h; this file is only which primitive each
 * platform reaches for.
 *
 * One translation unit rather than the inline functions in a header this
 * whole library includes, and that is the point of the arrangement: the kernel
 * primitives arrive with <asm/barrier.h> and <linux/atomic.h> and the Windows
 * ones with <intrin.h>, and a header pulling those in everywhere puts every
 * source in the project one include away from a kernel or a Windows namespace
 * it has no business seeing. Every other back end under arch/ is split this
 * way; this one now is too.
 *
 * The cost is a call where an inline access stood. It is not on any measured
 * path: every one of these runs at initialization, at configuration or once
 * per collector allocation, never inside the loops the noise source times.
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
 * The last is what the code did before these helpers existed: a compiler with
 * neither the builtins nor the intrinsics gets no ordering guarantee beyond
 * the natural width of the access, which is the guarantee these latches were
 * relying on all along. It is a fallback, not a supported concurrency model.
 */

/*
 * As every other back end under arch/: the public header for the integer
 * types the kernel and a hosted build spell differently, and the internal one
 * for the declarations these definitions have to match.
 */
#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#if defined(LINUX_KERNEL) || defined(__KERNEL__)

#include <asm/barrier.h>
#include <linux/atomic.h>

int jent_atomic_load_int(int *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_int(int *ptr, int val)
{
	smp_store_release(ptr, val);
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	smp_store_release(ptr, val);
}

int jent_atomic_exchange_int(int *ptr, int val)
{
	return xchg(ptr, val);
}

jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return smp_load_acquire(ptr);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	smp_store_release(ptr, val);
}

#elif defined(__ATOMIC_ACQUIRE) && (defined(__GNUC__) || defined(__clang__))

int jent_atomic_load_int(int *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_int(int *ptr, int val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
}

/*
 * The one read-modify-write, and on aarch64 the one place a freestanding build
 * can come apart: GCC 10 and later default to -moutline-atomics there, which
 * compiles this into a call to a libgcc helper - __aarch64_swp4_acq_rel - that
 * chooses between the LSE and the LL/SC form at run time through an ifunc. A
 * -nostdlib link has no libgcc, the symbol stays undefined, and the first call
 * jumps into nothing. Such a build wants -mno-outline-atomics, as the kernel
 * uses for the same reason; see the note beside JENT_BAREMETAL in
 * jitterentropy.h.
 */
int jent_atomic_exchange_int(int *ptr, int val)
{
	return __atomic_exchange_n(ptr, val, __ATOMIC_ACQ_REL);
}

/* The builtins take any scalar, a pointer to a function included. */
jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return __atomic_load_n(ptr, __ATOMIC_ACQUIRE);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	__atomic_store_n(ptr, val, __ATOMIC_RELEASE);
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
 * release this back end promises. Nothing here is on a path where that costs
 * anything: every one of them runs at initialization, at configuration or once
 * per collector allocation.
 *
 * _InterlockedOr(ptr, 0) is the load - Windows offers no plain interlocked
 * read, and an OR of zero returns the value without changing it.
 */
int jent_atomic_load_int(int *ptr)
{
	return (int)_InterlockedOr((volatile long *)ptr, 0);
}

void jent_atomic_store_int(int *ptr, int val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return (uint32_t)_InterlockedOr((volatile long *)ptr, 0);
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	(void)_InterlockedExchange((volatile long *)ptr, (long)val);
}

int jent_atomic_exchange_int(int *ptr, int val)
{
	return (int)_InterlockedExchange((volatile long *)ptr, (long)val);
}

/*
 * The pointer-width pair. _InterlockedCompareExchangePointer() against NULL
 * for NULL is the load, for the reason _InterlockedOr(ptr, 0) is above: it
 * returns the value and, unless it already was NULL, writes nothing.
 *
 * The address is cast directly - it is an object pointer whichever type it
 * points to - while the value goes through uintptr_t in both directions. A
 * function pointer and a data pointer are one width on every Windows ABI, but
 * casting between them is what C4054 and C4055 are about, and this build is
 * compiled with /W4. Through an integer wide enough to hold either, MSVC says
 * nothing, and there is no conversion left for it to have an opinion on.
 */
jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return (jent_fnptr)(uintptr_t)_InterlockedCompareExchangePointer(
		(void * volatile *)ptr, NULL, NULL);
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	(void)_InterlockedExchangePointer((void * volatile *)ptr,
					  (void *)(uintptr_t)val);
}

#else /* no atomics available */

int jent_atomic_load_int(int *ptr)
{
	return *(volatile int *)ptr;
}

void jent_atomic_store_int(int *ptr, int val)
{
	*(volatile int *)ptr = val;
}

uint32_t jent_atomic_load_u32(uint32_t *ptr)
{
	return *(volatile uint32_t *)ptr;
}

void jent_atomic_store_u32(uint32_t *ptr, uint32_t val)
{
	*(volatile uint32_t *)ptr = val;
}

/* Not atomic at all here - see the note on this fallback above. */
int jent_atomic_exchange_int(int *ptr, int val)
{
	int old = *(volatile int *)ptr;

	*(volatile int *)ptr = val;

	return old;
}

jent_fnptr jent_atomic_load_fnptr(jent_fnptr *ptr)
{
	return *(jent_fnptr volatile *)ptr;
}

void jent_atomic_store_fnptr(jent_fnptr *ptr, jent_fnptr val)
{
	*(jent_fnptr volatile *)ptr = val;
}

#endif
