/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific data-cache size discovery.
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
 * The feature-test macros that make glibc declare O_CLOEXEC, and the Windows
 * SDK version that declares GetLogicalProcessorInformationEx() and
 * RelationCache. Must be the first line: both have to precede every system
 * header, the <windows.h> included below among them.
 */
#include "jitterentropy-arch-compat.h"

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

#elif defined(_KERNEL) && defined(__FreeBSD__)

/*
 * The FreeBSD kernel exports no uniform cache geometry either, so x86 reads
 * CPUID as every other BSD build does - through the kernel's own primitives,
 * as <cpuid.h> is a user-space compiler header that a -nostdinc kernel build
 * does not have. Elsewhere there is no discovery, as in the fallback at the
 * end of this file.
 */
#include <sys/param.h>
#include <sys/systm.h>
# if defined(__x86_64__) || defined(__i386__)
#  include <machine/cpufunc.h>	/* do_cpuid(), cpuid_count() */
#  define JENT_ARCH_CACHE_FREEBSD_KERNEL_CPUID
# endif

#else /* LINUX_KERNEL */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * The OS interfaces are skipped on a baremetal target - there is no sysfs to
 * read and no sysctl to call. The chain does not end there: the generic CPUID
 * backend further down needs no operating system at all and is exactly what
 * such a build wants, so the branches are made unreachable rather than the
 * whole chain being cut short.
 */
#if !defined(JENT_BAREMETAL) && (defined(_MSC_VER) || defined(__MINGW32__))
# include <windows.h>
# define JENT_ARCH_CACHE_WINDOWS
#elif !defined(JENT_BAREMETAL) && defined(__linux__)
# include <unistd.h>
# include <fcntl.h>
# include <errno.h>
# include <limits.h>
# include <stdio.h>
# define JENT_ARCH_CACHE_LINUX
# include "jitterentropy-arch-cloexec.h"
/*
 * CPUID answers where sysfs and sysconf do not - musl has no _SC_LEVEL* and a
 * container may hide the sysfs cache tree - see jent_get_cachesize_uncached().
 */
# if (defined(__x86_64__) || defined(__i386__)) && \
     (defined(__GNUC__) || defined(__clang__))
#  include <cpuid.h>
#  define JENT_ARCH_CACHE_LINUX_CPUID
# endif
#elif !defined(JENT_BAREMETAL) && defined(__APPLE__)
# include <sys/sysctl.h>
# define JENT_ARCH_CACHE_APPLE
#elif !defined(JENT_BAREMETAL) && defined(_AIX)
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

/* Saturating: a garbage level must not wrap the sum into a small one. */
static uint64_t jent_cache_size_add(uint64_t sum, uint64_t size)
{
	return (size > ~(uint64_t)0 - sum) ? ~(uint64_t)0 : sum + size;
}

/*
 * Combine the discovered L1/L2/L3 data-cache sizes into the memory working-set
 * size: the smallest power of two strictly greater than the summed cache size,
 * or 0 when nothing was discovered. A sum from 2^63 on has no such power in a
 * uint64_t and gives 2^63; the caller caps the memory size far below anyway.
 */
static uint64_t jent_cache_roundup_from_sizes(uint64_t l1, uint64_t l2,
					      uint64_t l3, int all_caches)
{
	uint64_t sum = l1;

	if (all_caches) {
		sum = jent_cache_size_add(sum, l2);
		sum = jent_cache_size_add(sum, l3);
	}

	if (sum == 0)
		return 0;
	if (sum >= ((uint64_t)1 << 63))
		return (uint64_t)1 << 63;

	/* Force the sum to be of the form (bounding_power_of_2 - 1). */
	sum |= (sum >> 1);
	sum |= (sum >> 2);
	sum |= (sum >> 4);
	sum |= (sum >> 8);
	sum |= (sum >> 16);
	sum |= (sum >> 32);

	/* smallest power of 2 strictly greater than the summed cache size */
	return sum + 1;
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
static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
				       uint64_t *l3);

/* log2 of a power of two; 0 stands for "no size", as 2^0 is none. */
static uint32_t jent_cache_size_bits(uint64_t size)
{
	uint32_t bits = 0;

	while (size >>= 1)
		bits++;
	return bits;
}

uint64_t jent_cache_size_roundup(int all_caches)
{
	/*
	 * The exponents, not the sizes: 32-bit atomics are what every platform
	 * has (see arch/jitterentropy-arch-atomic.h).
	 */
	static uint32_t cached[2];
	static int cached_valid;
	uint32_t bits;

	if (!jent_atomic_load_int(&cached_valid)) {
		uint64_t l1 = 0, l2 = 0, l3 = 0;

		jent_get_cachesize_uncached(&l1, &l2, &l3);
		jent_atomic_store_u32(&cached[0], jent_cache_size_bits(
			jent_cache_roundup_from_sizes(l1, l2, l3, 0)));
		jent_atomic_store_u32(&cached[1], jent_cache_size_bits(
			jent_cache_roundup_from_sizes(l1, l2, l3, 1)));
		jent_atomic_store_int(&cached_valid, 1);
	}

	bits = jent_atomic_load_u32(&cached[!!all_caches]);
	return bits ? (uint64_t)1 << bits : 0;
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
#if defined(JENT_ARCH_CACHE_CPUID) || defined(JENT_ARCH_CACHE_LINUX_CPUID) || \
    (defined(JENT_ARCH_CACHE_LINUX_KERNEL) && defined(CONFIG_X86)) || \
    defined(JENT_ARCH_CACHE_FREEBSD_KERNEL_CPUID)

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
					      uint64_t *l1, uint64_t *l2,
					      uint64_t *l3)
{
	unsigned int sub;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

	for (sub = 0; sub < 16; sub++) {
		unsigned int eax, ebx, ecx, edx;
		unsigned int cache_type, cache_level;
		uint64_t ways, partitions, line_size, sets, size;

		if (!cpuid(leaf, sub, &eax, &ebx, &ecx, &edx))
			break;

		cache_type = eax & 0x1F;
		if (cache_type == 0)
			break;

		/* Only data (1) and unified (3) caches matter here. */
		if (cache_type != 1 && cache_type != 3)
			continue;

		/*
		 * W, P and L are bounded by their field widths (1024, 1024 and
		 * 4096 at most), but S is the whole of ECX, so S + 1 alone
		 * needs 33 bits and the product reaches 2^64. Formed in long,
		 * as this once was, it is signed overflow - undefined behavior
		 * - from 2^63 on, and a hypervisor or emulator answering leaf 4
		 * with EBX = 0xFFFFFFFF, ECX = 0xFFFFFFFE is all it takes to
		 * get there. The Linux kernel backend runs this inside an IPI,
		 * where a UBSAN kernel would splat.
		 *
		 * So S = 0xFFFFFFFF, whose increment needs the 33rd bit, is
		 * dropped outright - no cache has 2^32 sets, and it is what a
		 * leaf filled with all-ones reports - and the rest is computed
		 * unsigned at 64 bit, where the remaining worst case
		 * (2^32 * (2^32 - 1)) cannot overflow either.
		 */
		if (ecx == 0xFFFFFFFFU)
			continue;

		cache_level = (eax >> 5) & 0x7;
		ways        = (uint64_t)((ebx >> 22) & 0x3FF) + 1;
		partitions  = (uint64_t)((ebx >> 12) & 0x3FF) + 1;
		line_size   = (uint64_t)(ebx & 0xFFF) + 1;
		sets        = (uint64_t)ecx + 1;
		size = ways * partitions * line_size * sets;

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
					  uint64_t *l1, uint64_t *l2,
					  uint64_t *l3)
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

#endif /* x86 CPUID, in user space or a kernel */

#if defined(JENT_ARCH_CACHE_CPUID) || defined(JENT_ARCH_CACHE_LINUX_CPUID)
/*
 * The user-space CPUID primitive for jent_cache_sizes_cpuid(). This is what
 * __get_cpuid_count() does, spelled out because <cpuid.h> gained that helper
 * only with GCC 7 - RHEL 7 builds with 4.8 - while __get_cpuid_max() and
 * __cpuid_count() are there in every version that has the header. A leaf
 * beyond the maximum of its range (basic or extended) fails, as the caller
 * requires, and on i386 so does a CPU without the instruction.
 */
static int jent_cpuid_count_user(unsigned int leaf, unsigned int subleaf,
				 unsigned int *eax, unsigned int *ebx,
				 unsigned int *ecx, unsigned int *edx)
{
	unsigned int max = __get_cpuid_max(leaf & 0x80000000U, NULL);

	if (max == 0 || max < leaf)
		return 0;

	__cpuid_count(leaf, subleaf, *eax, *ebx, *ecx, *edx);
	return 1;
}
#endif /* JENT_ARCH_CACHE_CPUID || JENT_ARCH_CACHE_LINUX_CPUID */

/*
 * JENT_UT_CACHE_ARM64 compiles the decoder below into a unit test as well: it
 * is pure, and the geometries it has to see through - a hypervisor's among
 * them - are not ones a test machine presents, nor one that runs a kernel.
 */
#if (defined(JENT_ARCH_CACHE_LINUX_KERNEL) && defined(CONFIG_ARM64)) || \
    defined(JENT_UT_CACHE_ARM64)

/*
 * AArch64 data-cache discovery via the cache ID registers, shared by any
 * EL1-capable backend. CLIDR_EL1 gives the cache type per level and CCSIDR_EL1
 * (selected via CSSELR_EL1) the geometry of the selected cache. Those registers
 * are only accessible at EL1, so userspace (EL0) cannot use this - the Linux
 * kernel backend can. The caller passes the CLIDR_EL1 value and the FEAT_CCIDX
 * indication (wider CCSIDR fields) and supplies the CCSIDR_EL1 read for a given
 * (1-based) level through the callback. See Arm ARM (DDI 0487), CLIDR_EL1 /
 * CCSIDR_EL1.
 *
 * The Arm ARM is explicit that CCSIDR_EL1 does not describe the actual
 * caches - it gives the parameters of the set/way maintenance loop, which is
 * all it is architected for - and a hypervisor is free to report whatever
 * makes that loop cheap. KVM does so from Linux 6.3 on: a guest sees the
 * host's cache levels, each with one set and one way of the minimum line
 * size, i.e. 64 bytes per level. Summed and rounded up, that is a memory
 * block of 256 bytes where the real caches hold megabytes.
 *
 * A data or unified cache below JENT_CACHE_ARM64_MIN is taken for such a
 * report and left at zero - unknown - so the default memory size applies
 * rather than one sized after nothing. No AArch64 core has so small a data
 * cache at any level: the smallest L1 data cache any Cortex-A TRM permits is
 * 8 KiB.
 */
#define JENT_CACHE_ARM64_MIN	4096

typedef uint64_t (*jent_read_ccsidr_t)(unsigned int level);

static inline void jent_cache_sizes_arm64(uint64_t clidr, int ccidx,
					  jent_read_ccsidr_t ccsidr_fn,
					  uint64_t *l1, uint64_t *l2,
					  uint64_t *l3)
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
		uint64_t assoc, sets, ccsidr, size;

		if (ctype == 0)
			break;		/* no cache at this or higher levels */

		/* Need a data (2), separate I&D (3) or unified (4) cache. */
		if (ctype != 2 && ctype != 3 && ctype != 4)
			continue;

		ccsidr = ccsidr_fn(level);

		line = (unsigned int)(ccsidr & 0x7);	/* log2(line bytes) - 4 */
		if (ccidx) {
			/* FEAT_CCIDX: wider Associativity/NumSets fields. */
			assoc = ((ccsidr >> 3) & 0x1FFFFF) + 1;
			sets  = ((ccsidr >> 32) & 0xFFFFFF) + 1;
		} else {
			assoc = ((ccsidr >> 3) & 0x3FF) + 1;
			sets  = ((ccsidr >> 13) & 0x7FFF) + 1;
		}
		/* The fields are narrow enough that this stays below 2^57. */
		size = ((uint64_t)1 << (line + 4)) * assoc * sets;

		/* Not a cache, but a maintenance loop bound - see above. */
		if (size < JENT_CACHE_ARM64_MIN)
			continue;

		if (level == 1 && *l1 == 0)
			*l1 = size;
		else if (level == 2 && *l2 == 0)
			*l2 = size;
		else if (level == 3 && *l3 == 0)
			*l3 = size;
	}
}

#endif /* JENT_ARCH_CACHE_LINUX_KERNEL && CONFIG_ARM64 || JENT_UT_CACHE_ARM64 */

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
static void jent_get_cachesize_sysconf(uint64_t *l1, uint64_t *l2, uint64_t *l3)
{
	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

# ifdef _SC_LEVEL1_DCACHE_SIZE
	{
		long v = sysconf(_SC_LEVEL1_DCACHE_SIZE);
		if (v > 0)
			*l1 = (uint64_t)v;
	}
# endif
# ifdef _SC_LEVEL2_CACHE_SIZE
	{
		long v = sysconf(_SC_LEVEL2_CACHE_SIZE);
		if (v > 0)
			*l2 = (uint64_t)v;
	}
# endif
# ifdef _SC_LEVEL3_CACHE_SIZE
	{
		long v = sysconf(_SC_LEVEL3_CACHE_SIZE);
		if (v > 0)
			*l3 = (uint64_t)v;
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
	fd = open(file, O_RDONLY | JENT_O_CLOEXEC);
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
				 uint64_t *size)
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

	/* Checked before the shift, so the shift cannot overflow. */
	if ((uint64_t)val > (~(uint64_t)0 >> shift))
		return -1;

	*size = (uint64_t)val << shift;
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
					 uint64_t *l1, uint64_t *l2,
					 uint64_t *l3)
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
	 * directory and are simply skipped. musl answers it with the affinity
	 * mask instead, so the scan goes on past conf for as long as a cpuN
	 * directory exists - a pinned process on big.LITTLE would otherwise
	 * see only the cores numbered first. Capped at JENT_NCPU_SET_MAX - the
	 * bound the affinity paths already hold CPU numbers to - so an
	 * implausible topology cannot spin unbounded.
	 */
#ifdef _SC_NPROCESSORS_CONF
	conf = sysconf(_SC_NPROCESSORS_CONF);
#else
	conf = -1;
#endif
	if (conf <= 0)
		conf = 1;
	if (conf > (long)JENT_NCPU_SET_MAX)
		conf = (long)JENT_NCPU_SET_MAX;

	for (cpu = 0; cpu < (long)JENT_NCPU_SET_MAX; cpu++) {
		unsigned int idx;

		if (cpu >= conf) {
			char dir[128];

			snprintf(dir, sizeof(dir), "%s/cpu%ld", cpudir, cpu);
			if (access(dir, F_OK))
				break;
		}

		for (idx = 0; idx < 16; idx++) {
			char buf[32];
			/* the filename buffer is larger than necessary for testing
			 * with artifical sysfs e.g. under /tmp */
			char file[128];
			uint64_t *slot, val;
			long level;
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
			else if (level == 3)
				slot = l3;
			else
				continue;	/* L4 (eDRAM) is no L3 */

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

static void jent_get_cachesize_sysfs(uint64_t *l1, uint64_t *l2, uint64_t *l3)
{
	jent_get_cachesize_sysfs_dir(JENT_SYSFS_CPU_DIR, l1, l2, l3);
}
#undef JENT_SYSFS_CPU_DIR

/* Raise each level to what a later source found, never lowering one. */
static void jent_cache_sizes_merge(uint64_t *l1, uint64_t *l2, uint64_t *l3,
				   uint64_t s1, uint64_t s2, uint64_t s3)
{
	if (s1 > *l1)
		*l1 = s1;
	if (s2 > *l2)
		*l2 = s2;
	if (s3 > *l3)
		*l3 = s3;
}

#if defined(__aarch64__) || defined(__arm__)
/*
 * The largest cache sizes the Technical Reference Manual of an Arm core type
 * allows, keyed by the "CPU implementer" and "CPU part" fields of /proc/cpuinfo
 * - the last resort of the Linux backend on Arm.
 *
 * The cache ID registers CLIDR_EL1 and CCSIDR_EL1 are not readable at EL0, so
 * userspace has only what the kernel publishes. Linux has no cache sysfs on
 * arm64 before 4.0, and since 4.12 ("arm64: cacheinfo: Remove CCSIDR-based
 * cache information probing") the sizes in it are what the device tree or the
 * ACPI PPTT state - often nothing: an Android phone, or a virtual machine whose
 * device tree the hypervisor generated. bionic's sysconf() reports no cache
 * size on arm64 in any Android release, and SELinux keeps an app out of the
 * device tree.
 *
 * Where the TRM lets the licensee choose a size, the entry is the largest it
 * allows, the L2 entry the largest L2 even where the L2 cache is optional or,
 * on the Cortex-A5 and A9, an external L2C-310 controller: the working set
 * derived from them is never smaller than the real caches, so the memory
 * access still misses where the core has the largest caches its design
 * permits. The L3 entry is the largest the DynamIQ Shared Unit the core is
 * designed for supports - 4 MiB for the DSU and DSU-AE, 16 MiB for the
 * DSU-110, 32 MiB for the DSU-120 - and 0 for the cores with none: those
 * whose cluster-shared cache is the L2, and the Neoverse N2 and V2, which
 * connect to the interconnect directly. A system level cache in the
 * interconnect is memory-side, sized by the SoC rather than the core, and not
 * known here.
 *
 * The implementer is part of the key: 0xd01 is a Cortex-A32 under Arm's and a
 * TaiShan V110 under HiSilicon's (0x48). The Cortex-A12 (0xc0d) is left out:
 * Arm folded it into the A17 and withdrew its TRM, so nothing bounds it.
 */
struct jent_arm_core_cache {
	unsigned short implementer;
	unsigned short part;
	unsigned short l1_kib;
	unsigned short l2_kib;
	unsigned short l3_kib;
};

static const struct jent_arm_core_cache jent_arm_core_caches[] = {
	/* Arm Ltd, ARMv7-A */
	{ 0x41, 0xc05, 64, 8192,     0 },	/* Cortex-A5, L2C-310 */
	{ 0x41, 0xc07, 64, 1024,     0 },	/* Cortex-A7 */
	{ 0x41, 0xc08, 32, 1024,     0 },	/* Cortex-A8 */
	{ 0x41, 0xc09, 64, 8192,     0 },	/* Cortex-A9, L2C-310 */
	{ 0x41, 0xc0e, 32, 8192,     0 },	/* Cortex-A17 */
	{ 0x41, 0xc0f, 32, 4096,     0 },	/* Cortex-A15 */

	/* Arm Ltd, ARMv8-A and later */
	{ 0x41, 0xd01, 64, 1024,     0 },	/* Cortex-A32 */
	{ 0x41, 0xd02, 64, 1024,     0 },	/* Cortex-A34 */
	{ 0x41, 0xd03, 64, 2048,     0 },	/* Cortex-A53 */
	{ 0x41, 0xd04, 64, 1024,     0 },	/* Cortex-A35 */
	{ 0x41, 0xd05, 64,  256,  4096 },	/* Cortex-A55 */
	{ 0x41, 0xd06, 64,  256,  4096 },	/* Cortex-A65 */
	{ 0x41, 0xd07, 32, 2048,     0 },	/* Cortex-A57 */
	{ 0x41, 0xd08, 32, 4096,     0 },	/* Cortex-A72 */
	{ 0x41, 0xd09, 64, 8192,     0 },	/* Cortex-A73 */
	{ 0x41, 0xd0a, 64,  512,  4096 },	/* Cortex-A75 */
	{ 0x41, 0xd0b, 64,  512,  4096 },	/* Cortex-A76 */
	{ 0x41, 0xd0c, 64, 1024,  4096 },	/* Neoverse N1 */
	{ 0x41, 0xd0d, 64,  512,  4096 },	/* Cortex-A77 */
	{ 0x41, 0xd0e, 64,  512,  4096 },	/* Cortex-A76AE */
	{ 0x41, 0xd40, 64, 1024,  4096 },	/* Neoverse V1 */
	{ 0x41, 0xd41, 64,  512,  4096 },	/* Cortex-A78 */
	{ 0x41, 0xd42, 64,  512,  4096 },	/* Cortex-A78AE */
	{ 0x41, 0xd43, 64,  256,  4096 },	/* Cortex-A65AE */
	{ 0x41, 0xd44, 64, 1024,  4096 },	/* Cortex-X1 */
	{ 0x41, 0xd46, 64,  512, 16384 },	/* Cortex-A510 */
	{ 0x41, 0xd47, 64,  512, 16384 },	/* Cortex-A710 */
	{ 0x41, 0xd48, 64, 1024, 16384 },	/* Cortex-X2 */
	{ 0x41, 0xd49, 64, 1024,     0 },	/* Neoverse N2, direct connect */
	{ 0x41, 0xd4a, 64,  256,  4096 },	/* Neoverse E1 */
	{ 0x41, 0xd4b, 64,  512,  4096 },	/* Cortex-A78C */
	{ 0x41, 0xd4c, 64, 1024,  4096 },	/* Cortex-X1C */
	{ 0x41, 0xd4d, 64,  512, 16384 },	/* Cortex-A715 */
	{ 0x41, 0xd4e, 64, 1024, 16384 },	/* Cortex-X3 */
	{ 0x41, 0xd4f, 64, 2048,     0 },	/* Neoverse V2, direct connect */
	{ 0x41, 0xd80, 64,  512, 32768 },	/* Cortex-A520 */
	{ 0x41, 0xd81, 64,  512, 32768 },	/* Cortex-A720 */
	{ 0x41, 0xd82, 64, 2048, 32768 },	/* Cortex-X4 */
	{ 0x41, 0xd83, 64, 2048, 32768 },	/* Neoverse V3AE */
	{ 0x41, 0xd84, 64, 3072, 32768 },	/* Neoverse V3 */
	{ 0x41, 0xd85, 64, 3072, 32768 },	/* Cortex-X925 */
	{ 0x41, 0xd87, 64, 1024, 32768 },	/* Cortex-A725 */
	{ 0x41, 0xd88, 64,  512, 32768 },	/* Cortex-A520AE */
	{ 0x41, 0xd89, 64, 1024, 32768 },	/* Cortex-A720AE */

	/*
	 * Qualcomm, for the Arm cores it ships under its own part numbers -
	 * those Linux applies the Arm core's errata to (cpu_errata.c).
	 */
	{ 0x51, 0x801, 64, 2048,     0 },	/* Kryo 2XX Silver: Cortex-A53 */
	{ 0x51, 0x804, 64,  512,  4096 },	/* Kryo 4XX Gold: Cortex-A76 */
	{ 0x51, 0x805, 64,  256,  4096 },	/* Kryo 4XX Silver: Cortex-A55 */
};

/*
 * One line of /proc/cpuinfo. @implementer carries the "CPU implementer" of the
 * block the line belongs to, -1 before one was seen; a "CPU part" line looks
 * the pair up and raises @l1, @l2 and @l3 to the sizes of a core type it
 * knows. Like the sysfs walk, the largest seen at each level is kept.
 */
static void jent_cpuinfo_arm_line(const char *line, long *implementer,
				  uint64_t *l1, uint64_t *l2, uint64_t *l3)
{
	const char *val = strchr(line, ':');
	unsigned long v;
	size_t keylen, i;
	char *endptr;

	if (!val)
		return;

	/* The key, without the padding the file puts before the colon. */
	keylen = (size_t)(val - line);
	while (keylen && (line[keylen - 1] == ' ' || line[keylen - 1] == '\t'))
		keylen--;

	/* A block per CPU; an implementer never carries over to the next. */
	if (keylen == 9 && !strncmp(line, "processor", 9)) {
		*implementer = -1;
		return;
	}

	errno = 0;
	v = strtoul(val + 1, &endptr, 0);
	if (errno != 0 || endptr == val + 1)
		return;

	if (keylen == 15 && !strncmp(line, "CPU implementer", 15)) {
		*implementer = (long)v;
		return;
	}

	if (keylen != 8 || strncmp(line, "CPU part", 8) || *implementer < 0)
		return;

	for (i = 0; i < JENT_ARRAY_SIZE(jent_arm_core_caches); i++) {
		const struct jent_arm_core_cache *c = &jent_arm_core_caches[i];

		if (c->implementer != (unsigned long)*implementer ||
		    c->part != v)
			continue;

		jent_cache_sizes_merge(l1, l2, l3,
				       (uint64_t)c->l1_kib << 10,
				       (uint64_t)c->l2_kib << 10,
				       (uint64_t)c->l3_kib << 10);
		return;
	}
}

/*
 * @path is a parameter for the reason JENT_SYSFS_CPU_DIR is: pointing it at
 * nothing lets a test reach what lies behind it.
 */
static void jent_get_cachesize_cpuinfo_file(const char *path,
					    uint64_t *l1, uint64_t *l2,
					    uint64_t *l3)
{
	/* Longer than any line looked at; the Features line is skipped. */
	char chunk[512], line[64];
	size_t len = 0;
	long implementer = -1;
	int fd, overlong = 0;

	*l1 = 0;
	*l2 = 0;
	*l3 = 0;

	fd = open(path, O_RDONLY | JENT_O_CLOEXEC);
	if (fd < 0)
		return;

	for (;;) {
		ssize_t rlen, i;

		do {
			rlen = read(fd, chunk, sizeof(chunk));
		} while (rlen < 0 && errno == EINTR);
		if (rlen <= 0)
			break;

		for (i = 0; i < rlen; i++) {
			if (chunk[i] != '\n') {
				if (len < sizeof(line) - 1)
					line[len++] = chunk[i];
				else
					overlong = 1;
				continue;
			}

			line[len] = '\0';
			if (!overlong)
				jent_cpuinfo_arm_line(line, &implementer,
						      l1, l2, l3);
			len = 0;
			overlong = 0;
		}
	}
	close(fd);

	/* A last line without its newline. */
	if (len && !overlong) {
		line[len] = '\0';
		jent_cpuinfo_arm_line(line, &implementer, l1, l2, l3);
	}
}

#ifndef JENT_PROC_CPUINFO
# define JENT_PROC_CPUINFO "/proc/cpuinfo"
#endif
static void jent_get_cachesize_cpuinfo(uint64_t *l1, uint64_t *l2, uint64_t *l3)
{
	jent_get_cachesize_cpuinfo_file(JENT_PROC_CPUINFO, l1, l2, l3);
}
#undef JENT_PROC_CPUINFO
#endif /* __aarch64__ || __arm__ */

#ifdef JENT_ARCH_CACHE_LINUX_CPUID
/*
 * Overridable, as JENT_SYSFS_CPU_DIR is: sysconf answers first on glibc, so a
 * test has to replace the instruction to see what the fallback makes of it.
 */
#ifndef JENT_CACHE_CPUID_COUNT
# define JENT_CACHE_CPUID_COUNT jent_cpuid_count_user
#endif
static void jent_get_cachesize_cpuid(uint64_t *l1, uint64_t *l2, uint64_t *l3)
{
	jent_cache_sizes_cpuid(JENT_CACHE_CPUID_COUNT, l1, l2, l3);
}
#undef JENT_CACHE_CPUID_COUNT
#endif /* JENT_ARCH_CACHE_LINUX_CPUID */

static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
{
	uint64_t s1 = 0, s2 = 0, s3 = 0;

	/*
	 * Prefer the sysfs scan: it enumerates every CPU and therefore captures
	 * the largest (performance-core) data cache on a hybrid part, whereas
	 * glibc's sysconf reflects only the single core its one-shot CPUID probe
	 * happened to run on.
	 */
	jent_get_cachesize_sysfs(l1, l2, l3);

	/*
	 * A level still unknown - sysfs is unavailable (not mounted, a
	 * restricted container, ...) or describes only some of the caches.
	 * Fall back to sysconf, keeping the larger value per level so a partial
	 * sysfs result is never made worse.
	 *
	 * Asked per level, as the architecture-specific completions below are:
	 * gated on the L1 alone, a sysfs tree that names the L1 but not the L2
	 * and L3 - which is what a device tree or an ACPI PPTT describing only
	 * the first level leaves behind - would size a JENT_CACHE_ALL collector
	 * from the L1 by itself on every target that has neither of those
	 * completions (ppc64le, s390x, riscv64).
	 */
	if (*l1 == 0 || *l2 == 0 || *l3 == 0) {
		jent_get_cachesize_sysconf(&s1, &s2, &s3);
		jent_cache_sizes_merge(l1, l2, l3, s1, s2, s3);
	}

#if defined(__aarch64__) || defined(__arm__)
	/*
	 * The levels still unknown - all of them, or the L2 and L3 on a device
	 * tree that states only the L1 - take the largest the TRMs of the core
	 * types present allow. A level the system did report is kept: that is
	 * a measurement, the table an upper bound.
	 */
	if (*l1 == 0 || *l2 == 0 || *l3 == 0) {
		jent_get_cachesize_cpuinfo(&s1, &s2, &s3);
		if (*l1 == 0)
			*l1 = s1;
		if (*l2 == 0)
			*l2 = s2;
		if (*l3 == 0)
			*l3 = s3;
	}
#elif defined(JENT_ARCH_CACHE_LINUX_CPUID)
	/*
	 * On x86 the levels still unknown are read out of CPUID, as the
	 * backend of the BSDs does: musl has no _SC_LEVEL* at all, so without
	 * a readable sysfs - a container, a chroot without /sys - nothing else
	 * answers. It describes only the core this runs on, which is why it
	 * comes last and a level found above is kept.
	 */
	if (*l1 == 0 || *l2 == 0 || *l3 == 0) {
		jent_get_cachesize_cpuid(&s1, &s2, &s3);
		if (*l1 == 0)
			*l1 = s1;
		if (*l2 == 0)
			*l2 = s2;
		if (*l3 == 0)
			*l3 = s3;
	}
#endif
}

#elif defined(JENT_ARCH_CACHE_APPLE)

/*
 * Return the first of @names that resolves, or 0 when none does.
 *
 * The hw.* cache sysctls are 64 bit. A kernel answering with a narrower type
 * is still handled - the destination is zeroed first and every Apple target
 * is little-endian, so a short write lands in the low bytes.
 */
static uint64_t jent_sysctl_cachesize(const char *const *names, size_t nnames)
{
	size_t i;

	for (i = 0; i < nnames; i++) {
		uint64_t val = 0;
		size_t len = sizeof(val);

		if (sysctlbyname(names[i], &val, &len, NULL, 0) != 0)
			continue;
		if (len != sizeof(val) && len != sizeof(uint32_t))
			continue;
		if (val == 0)
			continue;

		return val;
	}

	return 0;
}

static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
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
static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
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
		uint64_t size;

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
		size = cache->CacheSize;

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

static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
{
	/*
	 * jent_cpuid_count_user() fails when the leaf is unsupported, which is
	 * what jent_cache_sizes_cpuid() relies on to probe the Intel and the
	 * AMD/Hygon leaf in turn.
	 *
	 * Unlike the sysfs and kernel backends this reads only the CPU the
	 * caller happens to be running on, so on a hybrid part it reports that
	 * core's geometry rather than the largest in the system. Every platform
	 * routed here lacks a portable way to enumerate the others; the result
	 * is a working-set size that is correct for some core rather than none.
	 */
	jent_cache_sizes_cpuid(jent_cpuid_count_user, l1, l2, l3);
}

#elif defined(JENT_ARCH_CACHE_FREEBSD_KERNEL_CPUID)

/*
 * The kernel CPUID primitive for jent_cache_sizes_cpuid(), with the range check
 * of jent_cpuid_count_user(): a leaf beyond the maximum of its range (basic or
 * extended) does not fault but returns another leaf's contents.
 */
static int jent_cpuid_count_kernel(unsigned int leaf, unsigned int subleaf,
				   unsigned int *eax, unsigned int *ebx,
				   unsigned int *ecx, unsigned int *edx)
{
	u_int regs[4];

	do_cpuid(leaf & 0x80000000U, regs);
	if (regs[0] == 0 || regs[0] < leaf)
		return 0;

	cpuid_count(leaf, subleaf, regs);
	*eax = regs[0];
	*ebx = regs[1];
	*ecx = regs[2];
	*edx = regs[3];
	return 1;
}

/* The CPU the caller runs on, as in the user-space CPUID backend above. */
static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
{
	jent_cache_sizes_cpuid(jent_cpuid_count_kernel, l1, l2, l3);
}

#elif defined(JENT_ARCH_CACHE_AIX)

/*
 * AIX exposes per-CPU cache parameters in the global _system_configuration
 * struct (see <sys/systemcfg.h>): dcache_size for L1 data cache and
 * L2_cache_size for L2. AIX does not provide an L3 size in this struct, so
 * leave it at zero.
 */
static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
{
	/* Both fields are signed: a negative one is no size. */
	long dcache = (long)_system_configuration.dcache_size;
	long l2cache = (long)_system_configuration.L2_cache_size;

	*l1 = (dcache > 0) ? (uint64_t)dcache : 0;
	*l2 = (l2cache > 0) ? (uint64_t)l2cache : 0;
	*l3 = 0;
}

#elif defined(JENT_ARCH_CACHE_LINUX_KERNEL) && \
      (defined(CONFIG_X86) || defined(CONFIG_ARM64))

struct jent_cpu_cache_sizes {
	uint64_t l1, l2, l3;
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

/*
 * Read CCSIDR_EL1 for the data/unified cache at @level (1-based).
 *
 * CSSELR_EL1 is put back as found. It is not the host's register alone: on a
 * VHE host before 6.3, KVM loads a guest's EL1 system registers, CSSELR_EL1
 * among them, into the CPU while that vCPU is loaded
 * (kvm_vcpu_load_sysregs_vhe()) and saves them back only on vcpu_put, so the
 * IPI running this can land in between and would otherwise hand the guest our
 * selector. (From 6.3 on KVM emulates the guest's CSSELR_EL1 instead.) KVM's
 * own CCSIDR read of those kernels, get_ccsidr(), keeps an IPI like this one
 * out of its write/read pair by disabling interrupts, so a restore is all it
 * takes to leave it undisturbed as well.
 */
static uint64_t jent_read_ccsidr(unsigned int level)
{
	u64 csselr = read_sysreg(csselr_el1);
	uint64_t ccsidr;

	/* CSSELR_EL1: Level in bits[3:1], InD = 0 selects the data/unified cache. */
	write_sysreg((u64)(level - 1) << 1, csselr_el1);
	isb();
	ccsidr = read_sysreg(ccsidr_el1);

	write_sysreg(csselr, csselr_el1);
	isb();

	return ccsidr;
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

static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
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

static void jent_get_cachesize_uncached(uint64_t *l1, uint64_t *l2,
					uint64_t *l3)
{
	*l1 = 0;
	*l2 = 0;
	*l3 = 0;
}

#endif
