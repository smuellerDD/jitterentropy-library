/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Out-of-line architecture helpers for the Jitter RNG kernel build.
 *
 * The entropy-collection core (src/jitterentropy-base.c and the other objects
 * built with -O0, see linux_kernel/Kbuild.source) must be compiled with -O0
 * (see the __OPTIMIZE__ guard in src/jitterentropy-base.c). Several kernel
 * headers do not compile at -O0 on modern kernels: for example <linux/slab.h>
 * and <linux/sched.h> reach the asm_inline in <linux/rwsem.h>. The helpers that
 * would otherwise require those headers are therefore implemented here, out of
 * line, and this translation unit is built at the kernel's normal optimization
 * level. The corresponding arch headers only declare the prototypes for the
 * kernel build.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#include <linux/mm.h>		/* kvzalloc(), kvfree_sensitive() */
#include <linux/sched.h>	/* schedule() */
#include <linux/slab.h>		/* kmalloc(), kfree_sensitive() */

#ifdef CONFIG_X86
#include <asm/processor.h>	/* cpuid_count(), boot_cpu_data */
#endif
#ifdef CONFIG_ARM64
#include <linux/preempt.h>	/* preempt_disable()/enable() */
#include <asm/barrier.h>	/* isb() */
#include <asm/sysreg.h>		/* read_sysreg(), write_sysreg() */
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

void *jent_zalloc(size_t len)
{
	void *tmp = kmalloc(len, GFP_KERNEL);

	if (tmp)
		jent_memset_secure(tmp, len);

	return tmp;
}

void *jent_zalloc_large(size_t len)
{
	return kvzalloc(len, GFP_KERNEL);
}

void jent_zfree(void *ptr, size_t len)
{
	(void)len;
	kfree_sensitive(ptr);
}

void jent_zfree_large(void *ptr, size_t len)
{
	kvfree_sensitive(ptr, len);
}

void jent_yield(void)
{
	schedule();
}

/*
 * Data-cache size discovery. The kernel's cacheinfo subsystem
 * (get_cpu_cacheinfo()) is not exported to modules, so the geometry decode in
 * arch/jitterentropy-arch-cache.h is reused and only the architecture-specific
 * register access is provided here: CPUID leaf 4 on x86, the CLIDR/CCSIDR cache
 * ID registers on arm64 (readable at EL1, unlike from userspace). On other
 * architectures report zero so the caller uses its built-in default.
 */
#if defined(CONFIG_X86)

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

#elif defined(CONFIG_ARM64)

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

#else

uint32_t jent_cache_size_roundup(int all_caches)
{
	(void)all_caches;
	return 0;
}

#endif
