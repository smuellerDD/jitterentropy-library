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
 * or 0 when the platform offers no way to discover it.
 *
 * Dispatch:
 *   - Linux            -> sysconf(_SC_LEVEL{1,2,3}_*) with /sys/devices fallback
 *   - macOS            -> sysctlbyname("hw.l{1d,2,3}cachesize")
 *   - Windows / Cygwin -> GetLogicalProcessorInformation
 *   - {Open,Free,Net}BSD x86 -> CPUID leaf 4 (deterministic cache parameters)
 *   - {Open,Free,Net}BSD aarch64 / riscv -> zero stub (no EL0-readable source)
 *   - AIX              -> _system_configuration (dcache_size / L2_cache_size)
 *   - Linux Kernel     -> return 0
 *   - other            -> return 0
 */

#ifndef _JITTERENTROPY_ARCH_CACHE_H
#define _JITTERENTROPY_ARCH_CACHE_H

#ifdef LINUX_KERNEL

#include <linux/cacheinfo.h>
# define JENT_ARCH_CACHE_LINUX_KERNEL

#else /* LINUX_KERNEL */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__MINGW32__) || defined(__CYGWIN__)
# include <windows.h>
# define JENT_ARCH_CACHE_WINDOWS
#elif defined(__linux__)
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
# include <limits.h>
# include <stdio.h>
# define JENT_ARCH_CACHE_LINUX
#elif defined(__APPLE__)
# include <sys/sysctl.h>
# define JENT_ARCH_CACHE_APPLE
#elif (defined(__OpenBSD__) || defined(__FreeBSD__) || defined(__NetBSD__)) && \
      (defined(__x86_64__) || defined(__i386__) ||                            \
       defined(__aarch64__) || defined(__riscv))
# define JENT_ARCH_CACHE_BSD
# if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#  define JENT_ARCH_CACHE_BSD_CPUID
# endif
#elif defined(_AIX)
# include <sys/systemcfg.h>
# define JENT_ARCH_CACHE_AIX
#endif

#endif /* LINUX_KERNEL */

static inline uint32_t jent_cache_size_to_memory(long l1, long l2, long l3,
						 int all_caches)
{
	uint32_t cache_size = 0;

	/* Cache size reported by system */
	if (l1 > 0)
		cache_size += (uint32_t)l1;
	if (all_caches) {
		if (l2 > 0)
			cache_size += (uint32_t)l2;
		if (l3 > 0)
			cache_size += (uint32_t)l3;
	}

	/* Force the output_size to be of the form (bounding_power_of_2 - 1). */
	cache_size |= (cache_size >> 1);
	cache_size |= (cache_size >> 2);
	cache_size |= (cache_size >> 4);
	cache_size |= (cache_size >> 8);
	cache_size |= (cache_size >> 16);

	return cache_size;
}

#if defined(JENT_ARCH_CACHE_LINUX) || defined(JENT_ARCH_CACHE_APPLE)

#ifdef JENT_ARCH_CACHE_LINUX

/*
 * The _SC_LEVEL*_*CACHE_SIZE selectors are a glibc extension. musl, uClibc
 * and similar libcs do not define them - check each level individually so a
 * libc that only exposes some of them still gets used for those, and so a
 * libc with no support at all (musl) silently leaves the values at zero
 * and lets the sysfs fallback below take over.
 *
 * We also defensively clamp negative returns to zero: a libc may define
 * the constant but have its sysconf() reply with -1 / EINVAL at runtime.
 */
static inline void jent_get_cachesize_sysconf(long *l1, long *l2, long *l3)
{
	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

# ifdef _SC_LEVEL1_DCACHE_SIZE
	{
		long v = sysconf(_SC_LEVEL1_DCACHE_SIZE);
		if (v > 0)
			*l1 = v;
	}
# endif
# ifdef _SC_LEVEL2_CACHE_SIZE
	{
		long v = sysconf(_SC_LEVEL2_CACHE_SIZE);
		if (v > 0)
			*l2 = v;
	}
# endif
# ifdef _SC_LEVEL3_CACHE_SIZE
	{
		long v = sysconf(_SC_LEVEL3_CACHE_SIZE);
		if (v > 0)
			*l3 = v;
	}
# endif
}

static inline void jent_get_cachesize_sysfs(long *l1, long *l2, long *l3)
{
#define JENT_SYSFS_CACHE_DIR "/sys/devices/system/cpu/cpu0/cache"
	long val;
	unsigned int i;
	char buf[10], file[50];
	int fd = 0;
	ssize_t rlen;

	/* Iterate over all caches */
	for (i = 0; i < 4; i++) {
		unsigned int shift = 0;
		char *ext, *endptr;

		/*
		 * Check the cache type - we are only interested in Unified
		 * and Data caches.
		 */
		memset(buf, 0, sizeof(buf));
		snprintf(file, sizeof(file), "%s/index%u/type",
			 JENT_SYSFS_CACHE_DIR, i);
		fd = open(file, O_RDONLY);
		if (fd < 0)
			continue;
		do {
			rlen = read(fd, buf, sizeof(buf));
		} while (rlen < 0 && errno == EINTR);
		close(fd);
		if (rlen <= 0)
			continue;
		buf[sizeof(buf) - 1] = '\0';

		if (strncmp(buf, "Data", 4) && strncmp(buf, "Unified", 7))
			continue;

		/* Get size of cache */
		memset(buf, 0, sizeof(buf));
		snprintf(file, sizeof(file), "%s/index%u/size",
			 JENT_SYSFS_CACHE_DIR, i);

		fd = open(file, O_RDONLY);
		if (fd < 0)
			continue;
		do {
			rlen = read(fd, buf, sizeof(buf));
		} while (rlen < 0 && errno == EINTR);
		close(fd);
		if (rlen <= 0)
			continue;
		buf[sizeof(buf) - 1] = '\0';

		ext = strstr(buf, "K");
		if (ext) {
			shift = 10;
			*ext = '\0';
		} else {
			ext = strstr(buf, "M");
			if (ext) {
				shift = 20;
				*ext = '\0';
			}
		}

		errno = 0;
		val = strtol(buf, &endptr, 10);
		if (errno != 0 || endptr == buf || val <= 0 || val == LONG_MAX)
			continue;
		val <<= shift;

		if (!*l1)
			*l1 = val;
		else if (!*l2)
			*l2 = val;
		else {
			*l3 = val;
			break;
		}
	}
#undef JENT_SYSFS_CACHE_DIR
}

#endif /* JENT_ARCH_CACHE_LINUX */

#ifdef JENT_ARCH_CACHE_APPLE

static inline void jent_get_cachesize_sysconf(long *l1, long *l2, long *l3)
{
	size_t size;

	size = sizeof(*l1);
	if (sysctlbyname("hw.l1dcachesize", l1, &size, NULL, 0) != 0)
		*l1 = 0;

	size = sizeof(*l2);
	if (sysctlbyname("hw.l2cachesize", l2, &size, NULL, 0) != 0)
		*l2 = 0;

	size = sizeof(*l3);
	if (sysctlbyname("hw.l3cachesize", l3, &size, NULL, 0) != 0)
		*l3 = 0;
}

#endif /* JENT_ARCH_CACHE_APPLE */

static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	uint32_t cache_size = 0;
	long l1 = 0, l2 = 0, l3 = 0;

	jent_get_cachesize_sysconf(&l1, &l2, &l3);

	cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);
#ifdef JENT_ARCH_CACHE_LINUX
	if (cache_size == 0) {
		jent_get_cachesize_sysfs(&l1, &l2, &l3);
		cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);

		if (cache_size == 0)
			return 0;
	}
#endif

	if (cache_size == 0)
		return 0;

	/*
	 * Make the output_size the smallest power of 2 strictly greater
	 * than cache_size.
	 */
	cache_size++;

	return cache_size;
}

#elif defined(JENT_ARCH_CACHE_WINDOWS)

static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	long l1 = 0, l2 = 0, l3 = 0;
	DWORD len = 0;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;
	DWORD count;
	DWORD i;
	uint32_t cache_size;

	/* First call to get buffer size */
	if (!GetLogicalProcessorInformation(NULL, &len) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return 0;

	buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
	if (!buffer)
		return 0;

	/* Second call to retrieve data */
	if (!GetLogicalProcessorInformation(buffer, &len)) {
		free(buffer);
		return 0;
	}

	count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

	for (i = 0; i < count; i++) {
		if (buffer[i].Relationship == RelationCache) {
			CACHE_DESCRIPTOR *cache = &buffer[i].Cache;

			if (cache->Level == 1 && cache->Type == CacheData) {
				l1 = (long)cache->Size;
			} else if (cache->Level == 2 &&
				   (cache->Type == CacheUnified ||
				    cache->Type == CacheData)) {
				l2 = (long)cache->Size;
			} else if (cache->Level == 3 &&
				   (cache->Type == CacheUnified ||
				    cache->Type == CacheData)) {
				l3 = (long)cache->Size;
			}
		}
	}

	free(buffer);

	cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);
	if (cache_size == 0)
		return 0;

	/*
	 * Make the output_size the smallest power of 2 strictly greater
	 * than cache_size.
	 */
	cache_size++;

	return cache_size;
}

#elif defined(JENT_ARCH_CACHE_BSD)

#ifdef JENT_ARCH_CACHE_BSD_CPUID

/*
 * The BSDs do not export data-cache sizes through sysctl in a uniform way.
 * On x86 we can read them directly with CPUID leaf 4 (deterministic cache
 * parameters, Intel SDM Vol. 2A; AMD CPUID Specification rev 2.34,
 * fn 0000_0004h).
 *
 * Each successful sub-leaf returns:
 *   EAX[ 4: 0]  cache type (1 = data, 2 = instruction, 3 = unified)
 *   EAX[ 7: 5]  cache level (1, 2, 3, ...)
 *   EBX[11: 0]  L = system coherency line size - 1
 *   EBX[21:12]  P = physical line partitions - 1
 *   EBX[31:22]  W = ways of associativity - 1
 *   ECX         S = number of sets - 1
 *
 * Total size = (W + 1) * (P + 1) * (L + 1) * (S + 1).
 */
static inline void jent_get_cachesize_cpuid(long *l1, long *l2, long *l3)
{
	unsigned int sub;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

	for (sub = 0; sub < 16; sub++) {
		unsigned int eax, ebx, ecx, edx;
		unsigned int cache_type, cache_level;
		unsigned int ways, partitions, line_size, sets;
		long size;

		if (!__get_cpuid_count(4, sub, &eax, &ebx, &ecx, &edx))
			break;

		cache_type = eax & 0x1F;
		if (cache_type == 0)
			break;

		/* Only data (1) and unified (3) caches matter here. */
		if (cache_type != 1 && cache_type != 3)
			continue;

		cache_level = (eax >> 5) & 0x7;
		ways        = ((ebx >> 22) & 0x3FF) + 1;
		partitions  = ((ebx >> 12) & 0x3FF) + 1;
		line_size   = (ebx & 0xFFF) + 1;
		sets        = ecx + 1;
		size = (long)ways * (long)partitions *
		       (long)line_size * (long)sets;

		/*
		 * L1 is typically split into separate data and instruction
		 * caches; only the data cache (type 1) is relevant here.
		 * L2/L3 are usually unified, so accept data or unified.
		 */
		if (cache_level == 1 && cache_type == 1 && *l1 == 0)
			*l1 = size;
		else if (cache_level == 2 && *l2 == 0)
			*l2 = size;
		else if (cache_level == 3 && *l3 == 0)
			*l3 = size;
	}
}

#else /* BSD aarch64 / riscv */

/*
 * AArch64 carries the data cache sizes in CCSIDR_EL1, an EL1 register that
 * the BSD arm64 kernels do not currently emulate for EL0. RISC-V has no
 * standardised user-mode cache-discovery instruction at all - the values
 * live in firmware-supplied device tree / ACPI tables that the BSDs do
 * not surface through a uniform sysctl. In both cases leave the cache
 * sizes at zero so the caller falls back to its built-in default.
 */
static inline void jent_get_cachesize_cpuid(long *l1, long *l2, long *l3)
{
	*l1 = 0;
	*l2 = 0;
	*l3 = 0;
}

#endif /* JENT_ARCH_CACHE_BSD_CPUID */

static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	long l1 = 0, l2 = 0, l3 = 0;
	uint32_t cache_size;

	jent_get_cachesize_cpuid(&l1, &l2, &l3);

	cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);
	if (cache_size == 0)
		return 0;

	/*
	 * Make the output_size the smallest power of 2 strictly greater
	 * than cache_size.
	 */
	cache_size++;

	return cache_size;
}

#elif defined(JENT_ARCH_CACHE_AIX)

/*
 * AIX exposes per-CPU cache parameters in the global _system_configuration
 * struct (see <sys/systemcfg.h>): dcache_size for L1 data cache and
 * L2_cache_size for L2. AIX does not provide an L3 size in this struct, so
 * leave it at zero.
 */
static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	long l1 = (long)_system_configuration.dcache_size;
	long l2 = (long)_system_configuration.L2_cache_size;
	long l3 = 0;
	uint32_t cache_size;

	cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);
	if (cache_size == 0)
		return 0;

	/*
	 * Make the output_size the smallest power of 2 strictly greater
	 * than cache_size.
	 */
	cache_size++;

	return cache_size;
}

#elif defined(JENT_ARCH_CACHE_LINUX_KERNEL)

static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	(void)all_caches;
	return 0;
}

#else /* no cache discovery available */

static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	(void)all_caches;
	return 0;
}

#endif

#endif /* _JITTERENTROPY_ARCH_CACHE_H */
