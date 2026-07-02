/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific data-cache size discovery.
 *
 * Definition of jent_cache_size_roundup() (declared in
 * arch/jitterentropy-arch-cache.h). See that header for the dispatch rationale.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

/*
 * The kernel's cacheinfo subsystem (get_cpu_cacheinfo()) is not exported to
 * modules, so the sizes are read directly with CPUID (x86) or the CLIDR/CCSIDR
 * cache ID registers (arm64, readable at EL1). These headers must not reach the
 * -O0 core, which is why this discovery is out of line here.
 */
#ifdef CONFIG_X86
#include <asm/processor.h>	/* cpuid_count(), boot_cpu_data */
#endif
#ifdef CONFIG_ARM64
#include <linux/preempt.h>	/* preempt_disable()/enable() */
#include <asm/barrier.h>	/* isb() */
#include <asm/sysreg.h>		/* read_sysreg(), write_sysreg() */
#endif
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

/*
 * x86 data-cache discovery via CPUID leaf 4 (deterministic cache parameters,
 * Intel SDM Vol. 2A; also supported by modern AMD CPUs), shared by every x86
 * backend that can issue CPUID. The instruction is issued through the supplied
 * callback so each environment can plug in its own primitive (the userspace
 * __get_cpuid_count() from <cpuid.h>, the kernel's cpuid_count(), ...). The
 * callback returns non-zero on success and must fail when the leaf is
 * unsupported, matching __get_cpuid_count().
 *
 *   EAX[ 4: 0]  cache type (1 = data, 2 = instruction, 3 = unified)
 *   EAX[ 7: 5]  cache level (1, 2, 3, ...)
 *   EBX[11: 0]  L = system coherency line size - 1
 *   EBX[21:12]  P = physical line partitions - 1
 *   EBX[31:22]  W = ways of associativity - 1
 *   ECX         S = number of sets - 1
 * Total size = (W + 1) * (P + 1) * (L + 1) * (S + 1).
 */
typedef int (*jent_cpuid_count_t)(unsigned int leaf, unsigned int subleaf,
				  unsigned int *eax, unsigned int *ebx,
				  unsigned int *ecx, unsigned int *edx);

static inline uint32_t jent_cache_size_roundup_cpuid(jent_cpuid_count_t cpuid,
						     int all_caches)
{
	long l1 = 0, l2 = 0, l3 = 0;
	uint32_t cache_size;
	unsigned int sub;

	for (sub = 0; sub < 16; sub++) {
		unsigned int eax, ebx, ecx, edx;
		unsigned int cache_type, cache_level;
		unsigned int ways, partitions, line_size, sets;
		long size;

		if (!cpuid(4, sub, &eax, &ebx, &ecx, &edx))
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
		 * caches; only the data cache (type 1) is relevant here. L2/L3
		 * are usually unified, so accept data or unified.
		 */
		if (cache_level == 1 && cache_type == 1 && l1 == 0)
			l1 = size;
		else if (cache_level == 2 && l2 == 0)
			l2 = size;
		else if (cache_level == 3 && l3 == 0)
			l3 = size;
	}

	cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);
	if (cache_size == 0)
		return 0;

	/* smallest power of 2 strictly greater than the summed cache size */
	return cache_size + 1;
}

/*
 * AArch64 data-cache discovery via the cache ID registers, shared by any
 * EL1-capable backend. CLIDR_EL1 gives the cache type per level and CCSIDR_EL1
 * (selected via CSSELR_EL1) the geometry of the selected cache. Those registers
 * are only accessible at EL1, so userspace (EL0) cannot use this - the Linux
 * kernel backend can. The caller passes the CLIDR_EL1 value and the FEAT_CCIDX
 * indication (wider CCSIDR fields) and supplies the CCSIDR_EL1 read for a given
 * (1-based) level through the callback. See Arm ARM (DDI 0487), CLIDR_EL1 /
 * CCSIDR_EL1.
 */
typedef uint64_t (*jent_read_ccsidr_t)(unsigned int level);

static inline uint32_t jent_cache_size_roundup_arm64(uint64_t clidr, int ccidx,
						     jent_read_ccsidr_t ccsidr_fn,
						     int all_caches)
{
	long l1 = 0, l2 = 0, l3 = 0;
	uint32_t cache_size;
	unsigned int level;

	for (level = 1; level <= 7; level++) {
		/* CLIDR_EL1 holds a 3-bit cache type per level. */
		unsigned int ctype =
			(unsigned int)((clidr >> (3 * (level - 1))) & 0x7);
		unsigned int line;
		unsigned long assoc, sets;
		uint64_t ccsidr;
		long size;

		if (ctype == 0)
			break;		/* no cache at this or higher levels */

		/* Need a data (2), separate I&D (3) or unified (4) cache. */
		if (ctype != 2 && ctype != 3 && ctype != 4)
			continue;

		ccsidr = ccsidr_fn(level);

		line = (unsigned int)(ccsidr & 0x7);	/* log2(line bytes) - 4 */
		if (ccidx) {
			/* FEAT_CCIDX: wider Associativity/NumSets fields. */
			assoc = (unsigned long)((ccsidr >> 3) & 0x1FFFFF) + 1;
			sets  = (unsigned long)((ccsidr >> 32) & 0xFFFFFF) + 1;
		} else {
			assoc = (unsigned long)((ccsidr >> 3) & 0x3FF) + 1;
			sets  = (unsigned long)((ccsidr >> 13) & 0x7FFF) + 1;
		}
		size = (long)(((unsigned long)1 << (line + 4)) * assoc * sets);

		if (level == 1 && l1 == 0)
			l1 = size;
		else if (level == 2 && l2 == 0)
			l2 = size;
		else if (level == 3 && l3 == 0)
			l3 = size;
	}

	cache_size = jent_cache_size_to_memory(l1, l2, l3, all_caches);
	if (cache_size == 0)
		return 0;

	/* smallest power of 2 strictly greater than the summed cache size */
	return cache_size + 1;
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
static void jent_get_cachesize_sysconf(long *l1, long *l2, long *l3)
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

static void jent_get_cachesize_sysfs(long *l1, long *l2, long *l3)
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

static void jent_get_cachesize_sysconf(long *l1, long *l2, long *l3)
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

uint32_t jent_cache_size_roundup(int all_caches)
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

uint32_t jent_cache_size_roundup(int all_caches)
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

uint32_t jent_cache_size_roundup(int all_caches)
{
#ifdef JENT_ARCH_CACHE_BSD_CPUID
	/*
	 * The BSDs do not export data-cache sizes through sysctl in a uniform
	 * way; on x86 read them directly via CPUID leaf 4. __get_cpuid_count()
	 * (from <cpuid.h>) already fails when the leaf is unsupported.
	 */
	return jent_cache_size_roundup_cpuid(__get_cpuid_count, all_caches);
#else
	/*
	 * AArch64 carries the data cache sizes in CCSIDR_EL1, an EL1 register
	 * that the BSD arm64 kernels do not currently emulate for EL0. RISC-V
	 * has no standardised user-mode cache-discovery instruction at all. In
	 * both cases return zero so the caller falls back to its default.
	 */
	(void)all_caches;
	return 0;
#endif /* JENT_ARCH_CACHE_BSD_CPUID */
}

#elif defined(JENT_ARCH_CACHE_AIX)

/*
 * AIX exposes per-CPU cache parameters in the global _system_configuration
 * struct (see <sys/systemcfg.h>): dcache_size for L1 data cache and
 * L2_cache_size for L2. AIX does not provide an L3 size in this struct, so
 * leave it at zero.
 */
uint32_t jent_cache_size_roundup(int all_caches)
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

#elif defined(JENT_ARCH_CACHE_LINUX_KERNEL) && defined(CONFIG_X86)

static int jent_cpuid_count(unsigned int leaf, unsigned int subleaf,
			    unsigned int *eax, unsigned int *ebx,
			    unsigned int *ecx, unsigned int *edx)
{
	if (boot_cpu_data.cpuid_level < (int)leaf)
		return 0;

	cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
	return 1;
}

uint32_t jent_cache_size_roundup(int all_caches)
{
	return jent_cache_size_roundup_cpuid(jent_cpuid_count, all_caches);
}

#elif defined(JENT_ARCH_CACHE_LINUX_KERNEL) && defined(CONFIG_ARM64)

/* Read CCSIDR_EL1 for the data/unified cache at @level (1-based). */
static uint64_t jent_read_ccsidr(unsigned int level)
{
	/* CSSELR_EL1: Level in bits[3:1], InD = 0 selects the data/unified cache. */
	write_sysreg((u64)(level - 1) << 1, csselr_el1);
	isb();
	return read_sysreg(ccsidr_el1);
}

uint32_t jent_cache_size_roundup(int all_caches)
{
	uint64_t clidr;
	int ccidx;
	uint32_t ret;

	/* CSSELR_EL1/CCSIDR_EL1 form a per-PE selector; stay on one CPU. */
	preempt_disable();
	clidr = read_sysreg(clidr_el1);
	/* ID_AA64MMFR2_EL1.CCIDX is bits[23:20]; non-zero => wide CCSIDR format. */
	ccidx = (int)((read_sysreg(id_aa64mmfr2_el1) >> 20) & 0xf);
	ret = jent_cache_size_roundup_arm64(clidr, ccidx, jent_read_ccsidr,
					    all_caches);
	preempt_enable();

	return ret;
}

#else /* no cache discovery available */

uint32_t jent_cache_size_roundup(int all_caches)
{
	(void)all_caches;
	return 0;
}

#endif
