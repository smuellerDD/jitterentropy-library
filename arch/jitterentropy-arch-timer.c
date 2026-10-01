/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture-specific high-resolution timestamp source.
 *
 *
 * Every backend lives here rather than inline in the header so that the
 * platform headers they need - <windows.h>, <x86intrin.h>, <linux/timex.h> -
 * stay out of the entropy-collection core, which linux_kernel/Kbuild.source
 * compiles at -O0. The kernel's arch headers are only meant to build
 * optimized (inline asm and compile-time asserts that need operands folded to
 * constants), and only the kernel backend below, for architectures without a
 * counter instruction, pulls them in. This file is
 * therefore built with the kernel's normal flags: -O0 protects the measured
 * loops, not the counter read.
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
 * The feature-test macros that make glibc declare clock_gettime() and the
 * CLOCK_* identifiers - POSIX.1b (__USE_POSIX199309), and so hidden by the
 * strict -std=c11 the Makefile uses. The generic fallback below uses them on
 * architectures without a counter instruction. Must be the first line: they
 * have to precede every system header.
 */
#include "jitterentropy-arch-compat.h"

#include "jitterentropy.h"
#include "jitterentropy-arch-timer.h"

/*
 * MSVC names the Arm architectures _M_ARM and _M_ARM64 only; clang and GCC
 * targeting MinGW (llvm-mingw's aarch64-w64-mingw32 and armv7-w64-mingw32)
 * define __aarch64__ and __arm__ instead, and without them here such a build
 * would fall through to the bare cntvct_el0 read below.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(JENT_BAREMETAL) && \
    (defined(_M_ARM) || defined(_M_ARM64) || \
     defined(__arm__) || defined(__aarch64__))
# include <windows.h>
# include <profileapi.h>
# define JENT_ARCH_TIMER_WINDOWS_QPC

#elif defined(_MSC_VER) && defined(_M_ARM64) && !defined(__clang__)
/*
 * MSVC building for a firmware on Arm64 - EDK2 supports that toolchain - has
 * no QueryPerformanceCounter() to call. The counter the aarch64 branch below
 * reads is there all the same; MSVC reaches it through the intrinsic rather
 * than through inline assembly, which it does not have on this target.
 */
# include <intrin.h>	/* _ReadStatusReg(), and ARM64_SYSREG() */
/* <winnt.h> names the register; its encoding is op0 3, op1 3, CRn 14, CRm 0, op2 2. */
# ifndef ARM64_CNTVCT
#  define ARM64_CNTVCT ARM64_SYSREG(3, 3, 14, 0, 2)
# endif
# define JENT_ARCH_TIMER_MSVC_ARM64

#elif defined(__x86_64__) || defined(__i386__) || \
      defined(_M_X64)     || defined(_M_IX86)
# if defined(LINUX_KERNEL) || (defined(_KERNEL) && defined(__FreeBSD__))
/*
 * The kernel gets the same instruction through inline asm rather than through
 * the intrinsic: <x86intrin.h> is a user-space compiler header that kernel
 * code does not include, and the kernel's own rdtsc() lives in <asm/msr.h>,
 * which drags in the cpufeature machinery this file exists to keep away from
 * the core. The FreeBSD kernel, built -nostdinc, has no <x86intrin.h> either.
 *
 * This assumes the TSC exists, as the user-space backend has always done for
 * this architecture. That is architecturally guaranteed on x86_64 and true of
 * every 32-bit CPU from the Pentium onwards; a kernel built for a 486 would
 * need the random_get_entropy() backend instead.
 */
#  define JENT_ARCH_TIMER_X86_ASM
# else
#  define JENT_ARCH_TIMER_X86
#  if defined(_MSC_VER)
#   include <intrin.h>
#  else
#   include <x86intrin.h>
#  endif
# endif

#elif defined(__aarch64__)
# define JENT_ARCH_TIMER_AARCH64
# ifndef AARCH64_NSTIME_REGISTER
#  define AARCH64_NSTIME_REGISTER "cntvct_el0"
# endif
/*
 * Apple platforms deliberately use the same cntvct_el0 read as every other
 * aarch64 target. clock_gettime_nsec_np(CLOCK_UPTIME_RAW) was used here
 * before on the assumption that the system counter is too coarse on M1+, but
 * that is not the case: CLOCK_UPTIME_RAW *is* the system counter, merely
 * rescaled to nanoseconds by mach_timebase_info() (numer/denom = 125/3 on
 * M-series, i.e. a 24 MHz counter with a ~41.67 ns period). Tracking both
 * sources over 100k samples shows a constant offset and no additional
 * resolution.
 *
 * The rescale is actively harmful: multiplying by 125/3 makes consecutive
 * deltas alternate between 41 and 42, so jent_gcd_analyze() computes a GCD of
 * 1 and concludes there is no fixed increment to divide out, when the real
 * quantum is one ~41.67 ns tick. Reading the register keeps the timestamp
 * unit equal to the hardware quantum, so the GCD health test and the
 * ECOARSETIME detection see the counter as it actually is. It is also a
 * single instruction rather than a libsystem call.
 *
 * CNTVCT_EL0 is readable at EL1 as well, so the kernel takes the same path.
 * It does so without the errata workarounds the kernel applies to its own
 * reads of the register (Cortex-A73 858921 and relatives, where a read can
 * observe a torn value): a counter that occasionally jumps is a defect for a
 * clocksource but not for a jitter measurement, whose health tests watch the
 * distribution of the deltas rather than trusting any single one.
 */

#elif defined(__s390x__)
# define JENT_ARCH_TIMER_S390X
/* memcpy() for the unaligned load in the backend below. */
# ifdef LINUX_KERNEL
#  include <linux/string.h>
# else
#  include <string.h>
# endif

#elif defined(_AIX)
/*
 * Must precede the PowerPC branch: see the dispatch note in
 * arch/jitterentropy-arch-timer.h. GCC and clang expose the timebase through
 * the builtin; xlc does not, and falls back to read_real_time() from
 * <sys/time.h>.
 */
# if defined(__GNUC__) || defined(__clang__)
#  define JENT_ARCH_TIMER_POWERPC
# else
#  include <sys/time.h>
#  define JENT_ARCH_TIMER_AIX_READ_REAL_TIME
# endif

#elif defined(__powerpc) || defined(__powerpc__)
# define JENT_ARCH_TIMER_POWERPC

#elif defined(__riscv) && !(defined(LINUX_KERNEL) && defined(CONFIG_RISCV_M_MODE))
/*
 * Not in a kernel that runs in M-mode (CONFIG_RISCV_M_MODE, the NOMMU builds
 * for K210-class parts): there is no SBI below it to emulate the time CSR, and
 * on the cores that do not implement it in hardware rdtime traps into the very
 * kernel executing it. Such a kernel reads the CLINT's mtime register through
 * get_cycles(), and the random_get_entropy() branch further down takes that.
 *
 * Outside the Linux kernel, M-mode is currently not supported. A freestanding
 * (JENT_BAREMETAL) build running in M-mode takes this branch and reads the
 * time CSR with rdtime, which traps on the cores that do not implement it in
 * hardware - the same problem as above, with no SBI below M-mode to emulate
 * the CSR either.
 */
# define JENT_ARCH_TIMER_RISCV
/*
 * The "time" CSR is the platform timer and is reliably accessible from
 * user mode on Linux (the kernel enables [s|m]counteren.TM). The "cycle"
 * CSR has higher resolution but is not always user-readable. Override
 * RISCV_NSTIME_INSN (and RISCV_NSTIME_INSN_HI on RV32) to switch sources,
 * e.g. to "rdcycle" / "rdcycleh".
 *
 * Be aware that the "time" CSR is driven by the platform timer, which on
 * currently shipping RISC-V hardware ticks at 1 MHz (10 MHz on a few boards).
 * That is a ~1 us quantum, coarse enough that jent_entropy_init() commonly
 * rejects the timer with ECOARSETIME. "rdcycle" resolves that where it is
 * permitted, but it is deliberately not the default: when the supervisor has
 * not set [s|m]counteren.CY - which is the norm, as the cycle counter is a
 * side-channel - the instruction traps and the process dies with SIGILL.
 * Silently coarse timestamps that are detected and reported by the health
 * tests are the safer default; a crash is not. See README.md.
 */
# ifndef RISCV_NSTIME_INSN
#  define RISCV_NSTIME_INSN "rdtime"
# endif
# if __riscv_xlen < 64
#  ifndef RISCV_NSTIME_INSN_HI
#   define RISCV_NSTIME_INSN_HI "rdtimeh"
#  endif
# endif

#elif defined(__sparc__) && defined(__arch64__)
/*
 * The SPARC V9 %tick register counts CPU clock cycles and is readable from
 * user mode unless the supervisor sets %tick.NPT. 32-bit SPARC is not covered:
 * "rd %tick" yields a 64-bit result that needs a two-register split there, and
 * the remaining hardware is not worth the separate path.
 */
# define JENT_ARCH_TIMER_SPARC64

#elif defined(__loongarch64)
/*
 * LoongArch64 has a constant-frequency counter read with rdtime.d, which
 * returns the counter in the first register and its stable counter ID in the
 * second. The ID is discarded.
 */
# define JENT_ARCH_TIMER_LOONGARCH64

#elif defined(LINUX_KERNEL)
/*
 * The architecture offers no counter instruction of its own, so the kernel's
 * raw cycle counter is read through random_get_entropy(). Not ktime_get_ns():
 * that rescales the clocksource by mult/shift, so constant ticks of a 24 MHz
 * counter come out as 41, 42, 41 ns deltas - the harm described for aarch64
 * above: the GCD analysis finds 1 and jent_stuck() credits deterministic
 * samples.
 *
 * random_get_entropy() is an unsigned long and often narrower still (32-bit
 * kernels, the MIPS count register, masked clocksources). jent_delta() only
 * handles a 64-bit wrap, so each wrap of a narrow counter gives one bogus huge
 * delta - once every few seconds to minutes. The delta stays the true one
 * minus a constant modulo 2^64, so it carries the same information, but the
 * stuck test misjudges the two or three samples around it. The startup test
 * drops such a backwards step from the GCD analysis and tolerates three. No
 * stateless fix exists: the counter width is not known here.
 *
 * MIPS and m68k read random_get_entropy_fallback(), the raw cycles of the
 * current clocksource, instead: their random_get_entropy() is not a clock
 * everywhere - MIPS without a usable c0_count mixes the TLB Random index into
 * it, m68k asks a machine hook, on Amiga the video beam position. The
 * function exists since 5.19 and the 5.10.119 and 5.15.44 backports; the
 * module does not build for these two on older kernels. On those, other
 * architectures without get_cycles() read 0 from random_get_entropy(), and
 * the startup test rejects that clock.
 */
# define JENT_ARCH_TIMER_LINUX_KERNEL
# include <linux/timex.h>	/* random_get_entropy(), ..._fallback() */

#elif defined(_KERNEL) && defined(__FreeBSD__)
/*
 * The FreeBSD kernel counterpart of the branch above (armv7 is what reaches
 * it): get_cyclecount() is the machine-dependent raw counter, the one random(4)
 * stamps its own harvested events with, and not a clock rescaled to
 * nanoseconds - see the Linux kernel branch for why that matters.
 */
# define JENT_ARCH_TIMER_FREEBSD_KERNEL
# include <sys/param.h>
# include <sys/systm.h>
# include <machine/cpu.h>	/* get_cyclecount() */

#elif defined(JENT_BAREMETAL)
/*
 * A freestanding build on an architecture none of the branches above has a
 * counter instruction for - 32-bit Arm, MIPS, the Cortex-M profile, ... The
 * generic fallback below is no answer there: it calls clock_gettime(), which a
 * freestanding target does not have - or has as a newlib stub that fails, or
 * one returning a millisecond tick - and none of that is a statement that the
 * target was ported.
 *
 * With the internal timer compiled in, the platform timer reads 0 instead. The
 * startup test rejects that with ENOTIME, and the library runs on the internal
 * timer - which on such a target means the thread handler the integrator
 * registers with jent_entropy_switch_notime_impl(). Without it no clock is
 * left at all, and that is a build error rather than a collector that only
 * finds out at runtime.
 */
# ifndef JENT_CONF_ENABLE_INTERNAL_TIMER
#  error "JENT_BAREMETAL: no counter instruction is known for this architecture - add one to arch/jitterentropy-arch-timer.c or enable the internal timer"
# endif
# define JENT_ARCH_TIMER_NONE

#else /* generic fallback */
# define JENT_ARCH_TIMER_GENERIC
# include <time.h>
#endif

#ifdef JENT_CONF_ENABLE_MOCK_TIMER
/*
 * The mocked time source. See arch/jitterentropy-arch-timer.h for what it is
 * for and why it is not compiled unless asked for.
 *
 * Deliberately not guarded by a lock or an atomic: the callback is registered
 * before the collector that uses it is allocated, and replaying a recording
 * through the Jitter RNG is a single-threaded operation - the same one
 * jent_read_entropy() already requires per collector.
 */
static jent_mock_timer_cb jent_mock_timer;
static void *jent_mock_timer_arg;

int jent_set_mock_timer(jent_mock_timer_cb cb, void *arg)
{
	jent_mock_timer = cb;
	jent_mock_timer_arg = arg;
	return 0;
}

int jent_mock_timer_active(void)
{
	return jent_mock_timer != NULL;
}

#endif /* JENT_CONF_ENABLE_MOCK_TIMER */

void jent_get_nstime(uint64_t *out)
{
#ifdef JENT_CONF_ENABLE_MOCK_TIMER
	/*
	 * The dispatch is inside the one definition rather than wrapping it,
	 * so that this function sits at the same source line whether or not
	 * the mock is compiled in - coverage tooling merging the two builds
	 * would otherwise see one name at two places and refuse.
	 */
	if (jent_mock_timer) {
		jent_mock_timer(jent_mock_timer_arg, out);
		return;
	}
#endif /* JENT_CONF_ENABLE_MOCK_TIMER */

#if defined(JENT_ARCH_TIMER_WINDOWS_QPC)

	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	*out = (uint64_t)ticks.QuadPart;

#elif defined(JENT_ARCH_TIMER_MSVC_ARM64)

	*out = (uint64_t)_ReadStatusReg(ARM64_CNTVCT);

#elif defined(JENT_ARCH_TIMER_X86)

	*out = (uint64_t)__rdtsc();

#elif defined(JENT_ARCH_TIMER_X86_ASM)

	/*
	 * rdtsc always returns the counter split across edx:eax, on x86_64 as
	 * well, where it also clears the upper 32 bits of both registers.
	 */
	uint32_t lo, hi;

	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
	*out = ((uint64_t)hi << 32) | (uint64_t)lo;

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

#elif defined(JENT_ARCH_TIMER_AIX_READ_REAL_TIME)

	/*
	 * AIX without the GCC/clang timebase builtin (xlc). read_real_time()
	 * is used rather than clock_gettime(), whose AIX implementation
	 * advances in steps of 1000.
	 */
	uint64_t tmp = 0;
	timebasestruct_t aixtime;

	read_real_time(&aixtime, TIMEBASE_SZ);
	time_base_to_time(&aixtime, TIMEBASE_SZ);
	tmp = (uint64_t)aixtime.tb_high * 1000000000UL;
	tmp += (uint64_t)aixtime.tb_low;
	*out = tmp;

#elif defined(JENT_ARCH_TIMER_SPARC64)

	uint64_t ctr_val;
	__asm__ __volatile__("rd %%tick, %0" : "=r" (ctr_val));
	*out = ctr_val;

#elif defined(JENT_ARCH_TIMER_LOONGARCH64)

	uint64_t ctr_val, ctr_id;
	__asm__ __volatile__("rdtime.d %0, %1"
			     : "=r" (ctr_val), "=r" (ctr_id));
	*out = ctr_val;

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

#elif defined(JENT_ARCH_TIMER_LINUX_KERNEL)

# if defined(CONFIG_MIPS) || defined(CONFIG_M68K)
	*out = (uint64_t)random_get_entropy_fallback();
# else
	*out = (uint64_t)random_get_entropy();
# endif

#elif defined(JENT_ARCH_TIMER_FREEBSD_KERNEL)

	*out = (uint64_t)get_cyclecount();

#elif defined(JENT_ARCH_TIMER_NONE)

	/* No clock: the startup test reports ENOTIME, see the dispatch above. */
	*out = 0;

#else /* JENT_ARCH_TIMER_GENERIC */

	/*
	 * CLOCK_MONOTONIC is used rather than CLOCK_REALTIME: the realtime
	 * clock is stepped and slewed by adjtime/NTP, which is external
	 * interference rather than entropy the CPU itself produced, and it can
	 * make the timestamp jump backwards. The measured jitter must come
	 * from the execution of the entropy collection loop alone, so the
	 * clock source with the least outside influence is the right one here.
	 */
	uint64_t tmp = 0;
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) == 0) {
		tmp = ((uint64_t)time.tv_sec & 0xFFFFFFFF) * 1000000000UL;
		tmp = tmp + (uint64_t)time.tv_nsec;
	}
	*out = tmp;

#endif
}
