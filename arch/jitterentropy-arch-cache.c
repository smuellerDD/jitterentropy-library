/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific data-cache size discovery.
 *
 * Definition of jent_cache_size_roundup() (declared in
 * arch/jitterentropy-arch-cache.h). See that header for the dispatch rationale.
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
 * GetLogicalProcessorInformationEx() and RelationCache are declared by the
 * Windows SDK only when the translation unit asks for Windows 7 or newer.
 * mingw-w64 in particular has defaulted to older values across its releases,
 * so the minimum is stated here rather than left to the toolchain; it has to
 * precede every system header, including the <windows.h> included below. An
 * externally supplied, higher value is left alone.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

/*
 * The kernel's cacheinfo subsystem (get_cpu_cacheinfo()) is not exported to
 * modules, so the sizes are read directly with CPUID (x86) or the CLIDR/CCSIDR
 * cache ID registers (arm64, readable at EL1). These headers must not reach the
 * -O0 core, which is why this discovery is out of line here.
 */
#include <linux/cpu.h>		/* cpus_read_lock()/unlock() */
#include <linux/cpumask.h>	/* for_each_online_cpu() */
#include <linux/smp.h>		/* smp_call_function_single() */
#ifdef CONFIG_X86
/*
 * cpuid_count() has moved twice. It used to live in <asm/processor.h>; the x86
 * CPUID centralisation split it out into <asm/cpuid.h>, and that header then
 * became the directory <asm/cpuid/api.h>. <asm/processor.h> carried the API
 * along for a while, but once the circular dependency between the two was
 * resolved it was reduced to including <asm/cpuid/types.h> - types only, no
 * cpuid_count() - which is what broke this file on 7.2-rc.
 *
 * boot_cpu_data still comes from <asm/processor.h> in every one of those
 * arrangements, so that include stays unconditional and only the CPUID API is
 * probed for. Kernels predating the split resolve cpuid_count() from it as
 * before, which is why no version test is needed here.
 */
#if defined(__has_include)
# if __has_include(<asm/cpuid/api.h>)
#  include <asm/cpuid/api.h>	/* cpuid_count() */
# elif __has_include(<asm/cpuid.h>)
#  include <asm/cpuid.h>	/* cpuid_count() */
# endif
#endif
#include <asm/processor.h>	/* boot_cpu_data */
#endif
#ifdef CONFIG_ARM64
#include <asm/barrier.h>	/* isb() */
#include <asm/sysreg.h>		/* read_sysreg(), write_sysreg() */
#endif
# define JENT_ARCH_CACHE_LINUX_KERNEL

#else /* LINUX_KERNEL */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
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
#elif defined(_AIX)
# include <sys/systemcfg.h>
# define JENT_ARCH_CACHE_AIX
#elif (defined(__x86_64__) || defined(__i386__)) && \
      (defined(__GNUC__) || defined(__clang__))
/*
 * Generic x86 fallback: read the geometry straight out of CPUID. This is the
 * backend for every x86 platform that has no OS interface of its own here -
 * the BSDs (which do not export data-cache sizes through sysctl in any uniform
 * way), DragonFly, Solaris/illumos, Haiku and Cygwin. It deliberately sits
 * after the OS-specific branches so that a platform which does have a better
 * interface keeps using it.
 *
 * Cygwin is handled here rather than by the Windows branch above. It is a
 * POSIX environment throughout the rest of this library, and mixing the Win32
 * cache query into it was the one place that pulled <windows.h> into an
 * otherwise POSIX translation unit.
 */
# include <cpuid.h>
# define JENT_ARCH_CACHE_CPUID
#endif

#endif /* LINUX_KERNEL */

/*
 * Combine the discovered L1/L2/L3 data-cache sizes into the memory working-set
 * size: the smallest power of two strictly greater than the summed cache size,
 * or 0 when nothing was discovered.
 */
static uint32_t jent_cache_roundup_from_sizes(long l1, long l2, long l3,
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

	if (cache_size == 0)
		return 0;

	/* Force the output_size to be of the form (bounding_power_of_2 - 1). */
	cache_size |= (cache_size >> 1);
	cache_size |= (cache_size >> 2);
	cache_size |= (cache_size >> 4);
	cache_size |= (cache_size >> 8);
	cache_size |= (cache_size >> 16);

	/* smallest power of 2 strictly greater than the summed cache size */
	return cache_size + 1;
}

/*
 * Per-backend data-cache size discovery, defined exactly once below for the
 * active platform. Every backend reports the same three values - the L1 data,
 * L2 and L3 cache size in bytes, each 0 when that level is not discoverable -
 * and leaves both the all_caches selection and the round-up to the common code
 * above. Backends that enumerate several CPUs report the largest cache seen at
 * each level.
 *
 * This is the uncached discovery: it is called once, from the memoising entry
 * point below, and must not be called directly.
 */
static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3);

/*
 * Enumerating every CPU's data-cache geometry - the sysfs walk in userspace, a
 * cross-CPU call per online CPU in the kernel - is comparatively expensive, and
 * the answer is fixed for the life of the process / module. Run the discovery
 * once, on the first call, derive both the L1-only (all_caches == 0) and the
 * all-levels (all_caches == 1) working-set size from that single result, and
 * return the cached answers afterwards so that repeated
 * jent_entropy_collector_alloc() calls do not each trigger a full CPU walk and
 * the latency spike it brings.
 *
 * The two results are deterministic, so the library's usual "initialise once
 * before concurrent use" contract (see jent_entropy_init()) makes this safe. A
 * thread still racing the very first call at worst recomputes the same values;
 * a 32-bit slot is read/written atomically, so a racing reader observes either
 * the old zero (harmless - the caller then falls back to its default size) or
 * the final value, never a torn one.
 */
uint32_t jent_cache_size_roundup(int all_caches)
{
	static uint32_t cached[2];
	static int cached_valid;

	if (!cached_valid) {
		long l1 = 0, l2 = 0, l3 = 0;

		jent_get_cachesize_uncached(&l1, &l2, &l3);
		cached[0] = jent_cache_roundup_from_sizes(l1, l2, l3, 0);
		cached[1] = jent_cache_roundup_from_sizes(l1, l2, l3, 1);
		cached_valid = 1;
	}

	return cached[!!all_caches];
}

/*
 * Compiled only for the backends that use it, so the other platforms do not
 * carry dead code (and clang's -Wunused-function noise) around.
 *
 * x86 data-cache discovery via the deterministic cache parameters leaf, shared
 * by every x86 backend that can issue CPUID. The instruction is issued through
 * the supplied callback so each environment can plug in its own primitive (the
 * userspace __get_cpuid_count() from <cpuid.h>, the kernel's cpuid_count(),
 * ...). The callback returns non-zero on success and must fail when the leaf is
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
#if defined(JENT_ARCH_CACHE_CPUID) || \
    (defined(JENT_ARCH_CACHE_LINUX_KERNEL) && defined(CONFIG_X86))

/* Intel SDM Vol. 2A, CPUID leaf 4: deterministic cache parameters. */
#define JENT_CPUID_LEAF_CACHE		0x00000004U
/* AMD APM Vol. 3, CPUID Fn8000_001D: the same layout, on AMD and Hygon. */
#define JENT_CPUID_LEAF_CACHE_EXT	0x8000001DU

typedef int (*jent_cpuid_count_t)(unsigned int leaf, unsigned int subleaf,
				  unsigned int *eax, unsigned int *ebx,
				  unsigned int *ecx, unsigned int *edx);

/* Walk the subleaves of @leaf; returns non-zero if any cache was found. */
static inline int jent_cache_sizes_cpuid_leaf(jent_cpuid_count_t cpuid,
					      unsigned int leaf,
					      long *l1, long *l2, long *l3)
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

		if (!cpuid(leaf, sub, &eax, &ebx, &ecx, &edx))
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
		if (cache_level == 1 && cache_type == 1 && *l1 == 0)
			*l1 = size;
		else if (cache_level == 2 && *l2 == 0)
			*l2 = size;
		else if (cache_level == 3 && *l3 == 0)
			*l3 = size;
	}

	return (*l1 != 0 || *l2 != 0 || *l3 != 0);
}

static inline void jent_cache_sizes_cpuid(jent_cpuid_count_t cpuid,
					  long *l1, long *l2, long *l3)
{
	/*
	 * Leaf 4 is Intel's. AMD and Hygon parts leave it empty - it reports
	 * cache type 0 in the first subleaf - and expose the identical structure
	 * through extended leaf 0x8000001D instead (gated by the
	 * TopologyExtensions feature; parts without it, and hypervisors hiding
	 * it, again report cache type 0 and leave the sizes at zero). A guest
	 * sees whichever leaf its host CPU implements, so probe both rather than
	 * dispatching on the vendor ID.
	 */
	if (jent_cache_sizes_cpuid_leaf(cpuid, JENT_CPUID_LEAF_CACHE,
					l1, l2, l3))
		return;

	jent_cache_sizes_cpuid_leaf(cpuid, JENT_CPUID_LEAF_CACHE_EXT,
				    l1, l2, l3);
}

#endif /* JENT_ARCH_CACHE_CPUID || (LINUX_KERNEL && CONFIG_X86) */

#if defined(JENT_ARCH_CACHE_LINUX_KERNEL) && defined(CONFIG_ARM64)

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

static inline void jent_cache_sizes_arm64(uint64_t clidr, int ccidx,
					  jent_read_ccsidr_t ccsidr_fn,
					  long *l1, long *l2, long *l3)
{
	unsigned int level;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

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

		if (level == 1 && *l1 == 0)
			*l1 = size;
		else if (level == 2 && *l2 == 0)
			*l2 = size;
		else if (level == 3 && *l3 == 0)
			*l3 = size;
	}
}

#endif /* JENT_ARCH_CACHE_LINUX_KERNEL && CONFIG_ARM64 */

#if defined(JENT_ARCH_CACHE_LINUX)

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

/*
 * Read a whole sysfs attribute file into @buf (always NUL-terminated on
 * success). Returns the byte count read (> 0) or -1 on any failure. A read
 * that fills the entire buffer is reported as such (rlen == buflen) so callers
 * that care about truncation - the size attribute carries a K/M suffix - can
 * reject it.
 */
static ssize_t jent_read_sysfs_attr(const char *file, char *buf, size_t buflen)
{
	int fd;
	ssize_t rlen;

	memset(buf, 0, buflen);
	fd = open(file, O_RDONLY);
	if (fd < 0)
		return -1;
	do {
		rlen = read(fd, buf, buflen);
	} while (rlen < 0 && errno == EINTR);
	close(fd);
	if (rlen <= 0)
		return -1;
	buf[buflen - 1] = '\0';
	return rlen;
}

/*
 * The three sysfs cache attributes, parsed separately from the reading of
 * them. They are the whole of the interpretation this backend does, they are
 * pure, and split out they can be checked against the shapes the kernel
 * actually produces - and against the malformed ones it must not be fooled by
 * - without a sysfs tree to read.
 */

/* Only data and unified caches are relevant; instruction caches are not. */
static int jent_cache_type_is_data(const char *buf)
{
	return !strncmp(buf, "Data", 4) || !strncmp(buf, "Unified", 7);
}

/*
 * The "level" attribute, e.g. "1". Returns 0 and the level in @level, or -1
 * when the attribute does not name one.
 */
static int jent_parse_cache_level(const char *buf, long *level)
{
	char *endptr;
	long val;

	errno = 0;
	val = strtol(buf, &endptr, 10);
	if (errno != 0 || endptr == buf || val < 1)
		return -1;

	*level = val;
	return 0;
}

/*
 * The "size" attribute, e.g. "32K" or "8M", converted to bytes. @buf is
 * modified. @rlen is what the read returned and @buflen the size of the
 * buffer. Returns 0 and the size in @size, or -1 when the attribute does not
 * name one.
 */
static int jent_parse_cache_size(char *buf, size_t rlen, size_t buflen,
				 long *size)
{
	unsigned int shift = 0;
	char *ext, *endptr;
	long val;

	/*
	 * A read filling the entire buffer may have truncated the K/M suffix;
	 * parsing the bare number would undercount the cache size 1024-fold.
	 * Skip it instead.
	 */
	if (rlen >= buflen)
		return -1;

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
		return -1;

	/*
	 * Shifting the suffix in must not overflow. strtol() saturating at
	 * LONG_MAX is rejected above, but a value merely large enough that
	 * << 20 leaves the range is not, and signed overflow is undefined -
	 * so a sysfs attribute reading "9999999999999M" would be a defect in
	 * the reader rather than in what it read.
	 */
	if (val > (LONG_MAX >> shift))
		return -1;

	*size = val << shift;
	return 0;
}

/*
 * @cpudir is the sysfs directory holding the per-CPU trees, normally
 * JENT_SYSFS_CPU_DIR. A parameter so the walk can be pointed at a tree the
 * caller built: the shapes handled here - an instruction cache to skip, an
 * attribute that does not parse, a hole in the index numbering - are ones a
 * given machine does not present.
 */
static void jent_get_cachesize_sysfs_dir(const char *cpudir,
					 long *l1, long *l2, long *l3)
{
/*
 * Overridable, as jent_fips_enabled_file() takes its path: pointing the walk
 * at nothing is the only way to reach the sysconf fallback of
 * jent_get_cachesize_uncached() on a machine whose sysfs does answer.
 */
#ifndef JENT_SYSFS_CPU_DIR
# define JENT_SYSFS_CPU_DIR "/sys/devices/system/cpu"
#endif
	long conf, cpu;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

	/*
	 * Enumerate every configured CPU, not just cpu0: on a hybrid CPU
	 * (Intel P-cores + E-cores, Arm big.LITTLE) the per-core data-cache
	 * sizes differ, and the collector normally runs on the more capable
	 * core, so keep the largest cache seen at each level rather than
	 * trusting whatever cpu0 happens to report.
	 *
	 * _SC_NPROCESSORS_CONF counts configured (not merely online) CPUs,
	 * whose sysfs indices lie in [0, conf); offline CPUs have no cache
	 * directory and are simply skipped. Fall back to cpu0 only when the
	 * count is unavailable, and cap the scan so an implausible topology
	 * cannot spin unbounded.
	 */
#ifdef _SC_NPROCESSORS_CONF
	conf = sysconf(_SC_NPROCESSORS_CONF);
#else
	conf = -1;
#endif
	if (conf <= 0)
		conf = 1;
	if (conf > 65536)
		conf = 65536;

	for (cpu = 0; cpu < conf; cpu++) {
		unsigned int idx;

		for (idx = 0; idx < 16; idx++) {
			char buf[32];
			/* the filename buffer is larger than necessary for testing
			 * with artifical sysfs e.g. under /tmp */
			char file[128];
			long *slot, val, level;
			ssize_t rlen;

			/*
			 * Cache type - only Data and Unified are relevant. The
			 * kernel numbers cache indices contiguously from 0, so
			 * a missing index means this CPU has no further caches
			 * (or is offline and exposes no cache directory).
			 */
			snprintf(file, sizeof(file),
				 "%s/cpu%ld/cache/index%u/type",
				 cpudir, cpu, idx);
			if (jent_read_sysfs_attr(file, buf, sizeof(buf)) <= 0)
				break;
			if (!jent_cache_type_is_data(buf))
				continue;

			/* Cache level selects the L1/L2/L3 bucket. */
			snprintf(file, sizeof(file),
				 "%s/cpu%ld/cache/index%u/level",
				 cpudir, cpu, idx);
			if (jent_read_sysfs_attr(file, buf, sizeof(buf)) <= 0)
				continue;
			if (jent_parse_cache_level(buf, &level))
				continue;
			if (level == 1)
				slot = l1;
			else if (level == 2)
				slot = l2;
			else
				slot = l3;

			/* Size of the cache, carrying a K or M suffix. */
			snprintf(file, sizeof(file),
				 "%s/cpu%ld/cache/index%u/size",
				 cpudir, cpu, idx);
			rlen = jent_read_sysfs_attr(file, buf, sizeof(buf));
			if (rlen <= 0)
				continue;
			if (jent_parse_cache_size(buf, (size_t)rlen,
						  sizeof(buf), &val))
				continue;

			/* Keep the largest cache seen at this level. */
			if (val > *slot)
				*slot = val;
		}
	}
}

static void jent_get_cachesize_sysfs(long *l1, long *l2, long *l3)
{
	jent_get_cachesize_sysfs_dir(JENT_SYSFS_CPU_DIR, l1, l2, l3);
}
#undef JENT_SYSFS_CPU_DIR

static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	long s1 = 0, s2 = 0, s3 = 0;

	/*
	 * Prefer the sysfs scan: it enumerates every CPU and therefore captures
	 * the largest (performance-core) data cache on a hybrid part, whereas
	 * glibc's sysconf reflects only the single core its one-shot CPUID probe
	 * happened to run on.
	 */
	jent_get_cachesize_sysfs(l1, l2, l3);
	if (*l1 > 0)
		return;

	/*
	 * No L1 data cache found - sysfs is unavailable (not mounted, a
	 * restricted container, ...) or does not describe the caches. Fall back
	 * to sysconf, keeping the larger value per level so a partial sysfs
	 * result is never made worse.
	 */
	jent_get_cachesize_sysconf(&s1, &s2, &s3);
	if (s1 > *l1)
		*l1 = s1;
	if (s2 > *l2)
		*l2 = s2;
	if (s3 > *l3)
		*l3 = s3;
}

#elif defined(JENT_ARCH_CACHE_APPLE)

/*
 * Return the first of @names that resolves, or 0 when none does.
 *
 * The value is read into a uint64_t rather than straight into the caller's
 * long: the hw.* cache sysctls are 64 bit, so a 32-bit build passing
 * sizeof(long) == 4 would be rejected with ENOMEM and lose the size entirely.
 * A kernel answering with a narrower type is still handled - the destination
 * is zeroed first and every Apple target is little-endian, so a short write
 * lands in the low bytes.
 */
static long jent_sysctl_cachesize(const char *const *names, size_t nnames)
{
	size_t i;

	for (i = 0; i < nnames; i++) {
		uint64_t val = 0;
		size_t len = sizeof(val);

		if (sysctlbyname(names[i], &val, &len, NULL, 0) != 0)
			continue;
		if (len != sizeof(val) && len != sizeof(uint32_t))
			continue;
		if (val == 0 || val > (uint64_t)LONG_MAX)
			continue;

		return (long)val;
	}

	return 0;
}

static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	/*
	 * Apple Silicon is heterogeneous, and the flat hw.l1dcachesize /
	 * hw.l2cachesize report the *least* capable cluster - the efficiency
	 * cores. On an M-series part that is 64 kB / 4 MB where the
	 * performance cores have 128 kB / 12 MB. Sizing the memory access
	 * working set from the efficiency core understates it for the
	 * performance cores the collector normally runs on, so ask for the
	 * perflevel0 (most performant) cluster first.
	 *
	 * The perflevel* names only exist on Apple Silicon; on Intel Macs and
	 * on homogeneous parts the lookup falls through to the flat names.
	 */
	static const char *const l1_names[] = {
		"hw.perflevel0.l1dcachesize",
		"hw.l1dcachesize"
	};
	static const char *const l2_names[] = {
		"hw.perflevel0.l2cachesize",
		"hw.l2cachesize"
	};
	/*
	 * Apple Silicon exposes no L3 size through sysctl at all (the SLC is
	 * not reported); these names only resolve on older Intel Macs. A
	 * missing L3 simply leaves the value at zero.
	 */
	static const char *const l3_names[] = {
		"hw.perflevel0.l3cachesize",
		"hw.l3cachesize"
	};

	*l1 = jent_sysctl_cachesize(l1_names, JENT_ARRAY_SIZE(l1_names));
	*l2 = jent_sysctl_cachesize(l2_names, JENT_ARRAY_SIZE(l2_names));
	*l3 = jent_sysctl_cachesize(l3_names, JENT_ARRAY_SIZE(l3_names));
}

#elif defined(JENT_ARCH_CACHE_WINDOWS)

/*
 * GetLogicalProcessorInformationEx() rather than the older
 * GetLogicalProcessorInformation(): the latter only ever describes processor
 * group 0, so on a machine with more than 64 logical CPUs - and on the
 * heterogeneous parts where the per-level scan below actually matters - every
 * cache outside that first group was invisible.
 *
 * The Ex variant returns a packed sequence of variable-length records, so it
 * has to be walked by each record's own Size field instead of being indexed
 * like an array.
 */
static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	DWORD len = 0;
	BYTE *buffer, *pos, *end;
	/* Bytes that must be readable before Relationship and Size are touched. */
	const size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
				    Cache);
	/* ... and before the Cache member itself is read. */
	const size_t cache_rec = hdr + sizeof(CACHE_RELATIONSHIP);

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

	/* First call to get buffer size */
	if (!GetLogicalProcessorInformationEx(RelationCache, NULL, &len) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return;

	buffer = (BYTE *)malloc(len);
	if (!buffer)
		return;

	/* Second call to retrieve data */
	if (!GetLogicalProcessorInformationEx(
			RelationCache,
			(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer,
			&len)) {
		free(buffer);
		return;
	}

	/*
	 * One record is reported per cache, so every level shows up as many
	 * times as the machine has such caches. Keep the largest of each level
	 * rather than whatever happens to be enumerated last: on a
	 * heterogeneous CPU (Intel P-cores and E-cores report different L2
	 * sizes) the picked value would otherwise depend on enumeration order,
	 * and a memory region sized after the smaller cache would still fit
	 * into the larger one. On a homogeneous machine all entries of a level
	 * are equal and the result is unchanged.
	 */
	pos = buffer;
	end = buffer + len;
	while ((size_t)(end - pos) >= hdr) {
		PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec =
			(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)pos;
		CACHE_RELATIONSHIP *cache;
		long size;

		/*
		 * A zero or oversized Size would make the walk spin or read
		 * past the buffer; treat either as the end of the data.
		 */
		if (rec->Size < hdr || (size_t)(end - pos) < rec->Size)
			break;

		if (rec->Relationship != RelationCache) {
			pos += rec->Size;
			continue;
		}

		/*
		 * The bound checked above covers Relationship and Size only,
		 * while the Cache member reaches past hdr. A record claiming
		 * to describe a cache in less space than a CACHE_RELATIONSHIP
		 * occupies is malformed, and reading it would run off the end
		 * of the buffer when it is the last record; stop rather than
		 * trust the remainder of the walk. The pointer is formed only
		 * after this check, so it never even points past the
		 * allocation.
		 */
		if (rec->Size < cache_rec)
			break;

		cache = &rec->Cache;
		size = (long)cache->CacheSize;

		if (cache->Level == 1 && cache->Type == CacheData) {
			if (size > *l1)
				*l1 = size;
		} else if (cache->Level == 2 &&
			   (cache->Type == CacheUnified ||
			    cache->Type == CacheData)) {
			if (size > *l2)
				*l2 = size;
		} else if (cache->Level == 3 &&
			   (cache->Type == CacheUnified ||
			    cache->Type == CacheData)) {
			if (size > *l3)
				*l3 = size;
		}

		pos += rec->Size;
	}

	free(buffer);
}

#elif defined(JENT_ARCH_CACHE_CPUID)

static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	/*
	 * __get_cpuid_count() (from <cpuid.h>) already fails when the leaf is
	 * unsupported, which is what jent_cache_sizes_cpuid() relies on to
	 * probe the Intel and the AMD/Hygon leaf in turn.
	 *
	 * Unlike the sysfs and kernel backends this reads only the CPU the
	 * caller happens to be running on, so on a hybrid part it reports that
	 * core's geometry rather than the largest in the system. Every platform
	 * routed here lacks a portable way to enumerate the others; the result
	 * is a working-set size that is correct for some core rather than none.
	 */
	jent_cache_sizes_cpuid(__get_cpuid_count, l1, l2, l3);
}

#elif defined(JENT_ARCH_CACHE_AIX)

/*
 * AIX exposes per-CPU cache parameters in the global _system_configuration
 * struct (see <sys/systemcfg.h>): dcache_size for L1 data cache and
 * L2_cache_size for L2. AIX does not provide an L3 size in this struct, so
 * leave it at zero.
 */
static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	*l1 = (long)_system_configuration.dcache_size;
	*l2 = (long)_system_configuration.L2_cache_size;
	*l3 = 0;
}

#elif defined(JENT_ARCH_CACHE_LINUX_KERNEL) && \
      (defined(CONFIG_X86) || defined(CONFIG_ARM64))

struct jent_cpu_cache_sizes {
	long l1, l2, l3;
};

#ifdef CONFIG_X86

static int jent_cpuid_count(unsigned int leaf, unsigned int subleaf,
			    unsigned int *eax, unsigned int *ebx,
			    unsigned int *ecx, unsigned int *edx)
{
	/*
	 * The basic (0x00000000-) and extended (0x80000000-) leaf ranges are
	 * capped separately - by CPUID.0:EAX and CPUID.0x80000000:EAX - which
	 * the kernel keeps in cpuid_level and extended_cpuid_level. Querying
	 * past either cap does not fault but returns another leaf's contents,
	 * so check the one belonging to the requested range.
	 */
	if (leaf & 0x80000000U) {
		if (boot_cpu_data.extended_cpuid_level < leaf)
			return 0;
	} else if (boot_cpu_data.cpuid_level < (int)leaf) {
		return 0;
	}

	cpuid_count(leaf, subleaf, eax, ebx, ecx, edx);
	return 1;
}

/*
 * Runs on the target CPU via smp_call_function_single(), so the whole leaf-4
 * walk stays pinned to that core - the per-level sizes cannot be torn across a
 * migration between a P-core and an E-core.
 */
static void jent_cache_sizes_worker(void *info)
{
	struct jent_cpu_cache_sizes *sizes = info;

	jent_cache_sizes_cpuid(jent_cpuid_count,
			       &sizes->l1, &sizes->l2, &sizes->l3);
}

#else /* CONFIG_ARM64 */

/* Read CCSIDR_EL1 for the data/unified cache at @level (1-based). */
static uint64_t jent_read_ccsidr(unsigned int level)
{
	/* CSSELR_EL1: Level in bits[3:1], InD = 0 selects the data/unified cache. */
	write_sysreg((u64)(level - 1) << 1, csselr_el1);
	isb();
	return read_sysreg(ccsidr_el1);
}

/*
 * Runs on the target PE via smp_call_function_single(). CSSELR_EL1/CCSIDR_EL1
 * form a per-PE selector, so executing the whole sequence on one CPU is what
 * keeps it consistent - and, unlike the former manual preempt_disable(), it
 * also reads each cluster's own geometry on big.LITTLE / DynamIQ parts.
 */
static void jent_cache_sizes_worker(void *info)
{
	struct jent_cpu_cache_sizes *sizes = info;
	uint64_t clidr = read_sysreg(clidr_el1);
	/* ID_AA64MMFR2_EL1.CCIDX is bits[23:20]; non-zero => wide CCSIDR format. */
	int ccidx = (int)((read_sysreg(id_aa64mmfr2_el1) >> 20) & 0xf);

	jent_cache_sizes_arm64(clidr, ccidx, jent_read_ccsidr,
			       &sizes->l1, &sizes->l2, &sizes->l3);
}

#endif /* CONFIG_X86 */

static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	int cpu;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

	/*
	 * Read the cache geometry on every online CPU and keep the largest at
	 * each level. Heterogeneous CPUs - Intel P-cores + E-cores with
	 * differing L2, big.LITTLE / DynamIQ clusters with differing geometry
	 * altogether - would otherwise be sized after the smaller cache, while
	 * the collector normally runs on the more capable core.
	 */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		struct jent_cpu_cache_sizes sizes = { 0, 0, 0 };

		if (smp_call_function_single(cpu, jent_cache_sizes_worker,
					     &sizes, 1))
			continue;

		if (sizes.l1 > *l1)
			*l1 = sizes.l1;
		if (sizes.l2 > *l2)
			*l2 = sizes.l2;
		if (sizes.l3 > *l3)
			*l3 = sizes.l3;
	}
	cpus_read_unlock();
}

#else /* no cache discovery available */

/*
 * Reached by every remaining combination, most notably the non-Linux, non-Apple
 * platforms on a non-x86 CPU: the BSDs on aarch64, powerpc or riscv, and any
 * target whose compiler provides no <cpuid.h>.
 *
 * AArch64 carries the data cache sizes in CCSIDR_EL1, an EL1 register that the
 * BSD arm64 kernels do not currently emulate for EL0 (the Linux kernel backend
 * above can read it because it runs at EL1). RISC-V has no standardised
 * user-mode cache-discovery instruction at all. Reporting nothing makes
 * jent_update_memsize() fall back to JENT_DEFAULT_MEMORY_BITS, which is a
 * conservative working-set size rather than a failure.
 */
static void jent_get_cachesize_uncached(long *l1, long *l2, long *l3)
{
	*l1 = 0;
	*l2 = 0;
	*l3 = 0;
}

#endif
