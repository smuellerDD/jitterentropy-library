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
 * Architecture / OS-specific cache size discovery.
 *
 * Provides jent_cache_size_roundup(int all_caches) returning a size that
 * is a power of two strictly greater than the queried data cache size,
 * or 0 when the platform offers no way to discover it. Defined in
 * arch/jitterentropy-arch-cache.c.
 *
 * Every backend there implements the same hook - report the L1 data, L2 and L3
 * cache size, 0 for a level it cannot discover - and the common code does the
 * discovery once and answers from a cache afterwards.
 *
 * Dispatch:
 *   - Linux            -> /sys/devices/system/cpu walk with sysconf(_SC_LEVEL{1,2,3}_*) fallback
 *   - macOS            -> sysctlbyname("hw.l{1d,2,3}cachesize")
 *   - Windows / Cygwin -> GetLogicalProcessorInformation
 *   - {Open,Free,Net}BSD x86 -> CPUID deterministic cache parameters
 *                               (leaf 4, or 0x8000001D on AMD / Hygon)
 *   - {Open,Free,Net}BSD aarch64 / riscv -> zero stub (no EL0-readable source)
 *   - AIX              -> _system_configuration (dcache_size / L2_cache_size)
 *   - Linux Kernel x86 -> CPUID leaf 4 / 0x8000001D, on every online CPU
 *   - Linux Kernel arm64 -> CLIDR_EL1 / CCSIDR_EL1 cache ID registers
 *   - other            -> return 0
 */

#ifndef _JITTERENTROPY_ARCH_CACHE_H
#define _JITTERENTROPY_ARCH_CACHE_H

uint32_t jent_cache_size_roundup(int all_caches);

#endif /* _JITTERENTROPY_ARCH_CACHE_H */
