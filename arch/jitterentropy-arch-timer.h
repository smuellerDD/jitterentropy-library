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
 * Provides jent_get_nstime() implemented via the highest-resolution counter
 * available on the target. The dispatch order is:
 *
 *   - Windows ARM / ARM64 (MSVC / MinGW) -> QueryPerformanceCounter()
 *   - x86 / x86_64       -> __rdtsc() intrinsic
 *                           (<intrin.h> on MSVC, <x86intrin.h> elsewhere)
 *   - aarch64            -> mrs <reg> (cntvct_el0 by default), or
 *                           clock_gettime_nsec_np(CLOCK_UPTIME_RAW) on Apple
 *   - s390x              -> stcke inline asm
 *   - powerpc            -> __builtin_ppc_get_timebase()
 *   - riscv              -> rdtime (RV64), or rdtimeh/rdtime retry pair (RV32);
 *                           override via RISCV_NSTIME_INSN[_HI] to use rdcycle
 *   - Linux kernel       -> random_get_entropy and ktime_get_ns as fallback
 *   - generic fallback   -> mach_absolute_time() on Mach,
 *                           read_real_time() on AIX,
 *                           clock_gettime(CLOCK_REALTIME) elsewhere
 *
 * Where a compiler intrinsic exists for the underlying instruction we use it
 * instead of inline asm. Inline asm is retained only on architectures (s390x,
 * aarch64, riscv) where no portable intrinsic covers the same register or
 * instruction.
 */

#ifndef _JITTERENTROPY_ARCH_TIMER_H
#define _JITTERENTROPY_ARCH_TIMER_H

#ifdef LINUX_KERNEL

#include <linux/ktime.h>	/* ktime_t (required by timekeeping.h on older kernels) */
#include <linux/time.h>
#include <linux/timekeeping.h>	/* ktime_get_ns() */
#include <linux/timex.h>	/* random_get_entropy() */
# define JENT_ARCH_TIMER_LINUX_KERNEL

#else /* LINUX_KERNEL */

#include <stdint.h>

#if (defined(_MSC_VER) || defined(__MINGW32__)) && \
    (defined(_M_ARM) || defined(_M_ARM64))
# include <windows.h>
# include <profileapi.h>
# define JENT_ARCH_TIMER_WINDOWS_QPC

#elif defined(__x86_64__) || defined(__i386__) || \
      defined(_M_X64)     || defined(_M_IX86)
# define JENT_ARCH_TIMER_X86
# if defined(_MSC_VER)
#  include <intrin.h>
# else
#  include <x86intrin.h>
# endif

#elif defined(__aarch64__)
# define JENT_ARCH_TIMER_AARCH64
# ifndef AARCH64_NSTIME_REGISTER
#  define AARCH64_NSTIME_REGISTER "cntvct_el0"
# endif
# ifdef __MACH__
/*
 * On modern Apple platforms (M1+), the system counter is too coarse.
 * Use clock_gettime_nsec_np(CLOCK_UPTIME_RAW) instead.
 */
#  include <time.h>
#  define JENT_ARCH_TIMER_AARCH64_APPLE
# endif

#elif defined(__s390x__)
# define JENT_ARCH_TIMER_S390X

#elif defined(__powerpc) || defined(__powerpc__)
# define JENT_ARCH_TIMER_POWERPC

#elif defined(__riscv)
# define JENT_ARCH_TIMER_RISCV
/*
 * The "time" CSR is the platform timer and is reliably accessible from
 * user mode on Linux (the kernel enables [s|m]counteren.TM). The "cycle"
 * CSR has higher resolution but is not always user-readable. Override
 * RISCV_NSTIME_INSN (and RISCV_NSTIME_INSN_HI on RV32) to switch sources,
 * e.g. to "rdcycle" / "rdcycleh".
 */
# ifndef RISCV_NSTIME_INSN
#  define RISCV_NSTIME_INSN "rdtime"
# endif
# if __riscv_xlen < 64
#  ifndef RISCV_NSTIME_INSN_HI
#   define RISCV_NSTIME_INSN_HI "rdtimeh"
#  endif
# endif

#else /* generic fallback */
# define JENT_ARCH_TIMER_GENERIC
# include <time.h>
# ifdef __MACH__
#  include <mach/mach_time.h>
# endif
#endif

#endif /* LINUX_KERNEL */

static inline void jent_get_nstime(uint64_t *out)
{
#if defined(JENT_ARCH_TIMER_WINDOWS_QPC)

	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	*out = (uint64_t)ticks.QuadPart;

#elif defined(JENT_ARCH_TIMER_X86)

	*out = (uint64_t)__rdtsc();

#elif defined(JENT_ARCH_TIMER_AARCH64_APPLE)

	*out = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

#elif defined(JENT_ARCH_TIMER_AARCH64)

	uint64_t ctr_val;
	__asm__ __volatile__("mrs %0, " AARCH64_NSTIME_REGISTER : "=r" (ctr_val));
	*out = ctr_val;

#elif defined(JENT_ARCH_TIMER_S390X)

	/*
	 * GCC + STCKE. STCKE command and data format:
	 * z/Architecture - Principles of Operation
	 * http://publibz.boulder.ibm.com/epubs/pdf/dz9zr007.pdf
	 *
	 * The current value of bits 0-103 of the TOD clock is stored in
	 * bytes 1-13 of the sixteen-byte output. Bit 59 (TOD-Clock bit 51)
	 * effectively increments every microsecond; the stepping value of
	 * TOD-clock bit 63 is approximately 244 picoseconds.
	 */
	uint8_t clk[16];
	uint64_t v;

	__asm__ __volatile__("stcke %0" : "=Q" (clk) : : "cc");

	/*
	 * s390x is big-endian, so just copy the relevant 8 bytes. Use memcpy
	 * rather than dereferencing (uint64_t *)(clk + 1): that address is
	 * unaligned and the access would violate strict-aliasing rules (UB the
	 * optimizer may break, even though s390x hardware tolerates the load).
	 */
	memcpy(&v, clk + 1, sizeof(v));
	*out = v;

#elif defined(JENT_ARCH_TIMER_POWERPC)

	*out = (uint64_t)__builtin_ppc_get_timebase();

#elif defined(JENT_ARCH_TIMER_RISCV)

# if __riscv_xlen >= 64
	uint64_t ctr_val;
	__asm__ __volatile__(RISCV_NSTIME_INSN " %0" : "=r" (ctr_val));
	*out = ctr_val;
# else
	/*
	 * RV32: the time CSR is 64 bits wide but only 32 bits are read at a
	 * time. Re-read the high half and retry on rollover so the combined
	 * value is consistent.
	 */
	uint32_t hi, lo, hi2;
	__asm__ __volatile__(
		"1:\n\t"
		RISCV_NSTIME_INSN_HI " %0\n\t"
		RISCV_NSTIME_INSN    " %1\n\t"
		RISCV_NSTIME_INSN_HI " %2\n\t"
		"bne %0, %2, 1b"
		: "=&r" (hi), "=&r" (lo), "=&r" (hi2));
	*out = ((uint64_t)hi << 32) | (uint64_t)lo;
# endif

#elif defined (JENT_ARCH_TIMER_LINUX_KERNEL)

	__u64 tmp = 0;

	tmp = random_get_entropy();

	/*
	 * If random_get_entropy does not return a value, i.e. it is not
	 * implemented for a given architecture, use a clock source.
	 * hoping that there are timers we can work with.
	 */
	if (tmp == 0)
		tmp = ktime_get_ns();

	*out = tmp;

#else /* JENT_ARCH_TIMER_GENERIC */

# ifdef __MACH__
	/*
	 * macOS lacks clock_gettime on older releases. Taken from
	 * http://developer.apple.com/library/mac/qa/qa1398/_index.html
	 */
	*out = mach_absolute_time();
# elif defined(_AIX)
	/*
	 * clock_gettime() on AIX returns a timer value that increments in
	 * steps of 1000.
	 */
	uint64_t tmp = 0;
	timebasestruct_t aixtime;
	read_real_time(&aixtime, TIMEBASE_SZ);
	time_base_to_time(&aixtime, TIMEBASE_SZ);
	tmp = (uint64_t)aixtime.tb_high * 1000000000UL;
	tmp += (uint64_t)aixtime.tb_low;
	*out = tmp;
# else
	/*
	 * We could use CLOCK_MONOTONIC(_RAW), but with CLOCK_REALTIME we
	 * pick up some nice extra entropy once in a while from NTP.
	 */
	uint64_t tmp = 0;
	struct timespec time;

	if (clock_gettime(CLOCK_REALTIME, &time) == 0) {
		tmp = ((uint64_t)time.tv_sec & 0xFFFFFFFF) * 1000000000UL;
		tmp = tmp + (uint64_t)time.tv_nsec;
	}
	*out = tmp;
# endif

#endif
}

#endif /* _JITTERENTROPY_ARCH_TIMER_H */
