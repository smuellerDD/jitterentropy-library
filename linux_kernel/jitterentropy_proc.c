/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Shared /proc/jitterentropy directory and statistics file for the Jitter RNG
 * kernel interfaces.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include "jitterentropy.h"
#include "jitterentropy_proc.h"

struct proc_dir_entry *jent_proc_dir;

/*
 * The effective flags value shared by the kernel interfaces, including the
 * folded-in shortcut parameters (see jitterentropy_mod.c).
 */
extern unsigned int flags;

/*
 * Human-readable breakdown of the effective flags value, reported via
 * /proc/jitterentropy/flags. Only the flag bits with a meaning in this
 * library version are listed (JENT_DISABLE_STIR and JENT_DISABLE_UNBIAS are
 * unused).
 */
static const struct {
	unsigned int bit;
	const char *label;	/* Column label including the colon. */
} jent_proc_flags_bits[] = {
	{ JENT_DISABLE_MEMORY_ACCESS,	"JENT_DISABLE_MEMORY_ACCESS:" },
	{ JENT_FORCE_INTERNAL_TIMER,	"JENT_FORCE_INTERNAL_TIMER:" },
	{ JENT_DISABLE_INTERNAL_TIMER,	"JENT_DISABLE_INTERNAL_TIMER:" },
	{ JENT_FORCE_FIPS,		"JENT_FORCE_FIPS:" },
	{ JENT_NTG1,			"JENT_NTG1:" },
	{ JENT_CACHE_ALL,		"JENT_CACHE_ALL:" },
};

static int jent_proc_flags_show(struct seq_file *m, void *v)
{
	unsigned int memsize = JENT_FLAGS_TO_MAX_MEMSIZE(flags);
	unsigned int hashloop = JENT_FLAGS_TO_HASHLOOP(flags);
	unsigned int i;

	seq_printf(m, "%-29s0x%08x\n", "flags:", flags);

	for (i = 0; i < ARRAY_SIZE(jent_proc_flags_bits); i++)
		seq_printf(m, "%-29s%s\n", jent_proc_flags_bits[i].label,
			   flags & jent_proc_flags_bits[i].bit ? "on" : "off");

	/*
	 * The memory size field encodes 1 kB << (field - 1); field 0 selects
	 * the automatic cache-size-derived default (see jent_memsize()).
	 */
	if (!memsize)
		seq_printf(m, "%-29sauto (derived from cache size)\n",
			   "max memory size:");
	else if (memsize <= 10)
		seq_printf(m, "%-29s%u kB\n", "max memory size:",
			   1U << (memsize - 1));
	else if (memsize <= JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX))
		seq_printf(m, "%-29s%u MB\n", "max memory size:",
			   1U << (memsize - 11));
	else
		seq_printf(m, "%-29sinvalid (%u)\n", "max memory size:",
			   memsize);

	/*
	 * The hash loop field encodes 1 << field iterations; field 0 selects
	 * the built-in default (see jent_hashloop_cnt()).
	 */
	if (!hashloop)
		seq_printf(m, "%-29sdefault\n", "hash loop count:");
	else
		seq_printf(m, "%-29s%u\n", "hash loop count:",
			   1U << hashloop);

	return 0;
}

/* Library version reported via /proc/jitterentropy/version. */
static int jent_proc_version_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u.%u.%u\n", JENT_MAJVERSION, JENT_MINVERSION,
		   JENT_PATCHLEVEL);

	return 0;
}

/* Statistics reported via /proc/jitterentropy/statistics. */
static atomic_t jent_open_instances = ATOMIC_INIT(0);
static atomic64_t jent_cumulative_opens = ATOMIC64_INIT(0);

void jent_proc_instance_inc(void)
{
	atomic_inc(&jent_open_instances);
	atomic64_inc(&jent_cumulative_opens);
}

void jent_proc_instance_dec(void)
{
	atomic_dec(&jent_open_instances);
}

/*
 * Serialize the module-wide statistics as JSON. Validate the output with
 * "jq -e ." when changing it.
 */
static int jent_proc_statistics_show(struct seq_file *m, void *v)
{
	seq_puts(m, "{\n");
	seq_puts(m, "\t\"charDevice\": {\n");
	seq_printf(m, "\t\t\"openInstances\": %d,\n",
		   atomic_read(&jent_open_instances));
	seq_printf(m, "\t\t\"cumulativeOpens\": %llu\n",
		   (unsigned long long)atomic64_read(&jent_cumulative_opens));
	seq_puts(m, "\t}\n");
	seq_puts(m, "}\n");

	return 0;
}

void __init jent_proc_init(void)
{
	jent_proc_dir = proc_mkdir(JENT_PROC_DIRNAME, NULL);
	if (!jent_proc_dir) {
		pr_warn("jitterentropy: failed to create /proc/%s\n",
			JENT_PROC_DIRNAME);
		return;
	}

	if (!proc_create_single("statistics", 0444, jent_proc_dir,
				jent_proc_statistics_show))
		pr_warn("jitterentropy: failed to create /proc/%s/statistics\n",
			JENT_PROC_DIRNAME);

	if (!proc_create_single("version", 0444, jent_proc_dir,
				jent_proc_version_show))
		pr_warn("jitterentropy: failed to create /proc/%s/version\n",
			JENT_PROC_DIRNAME);

	if (!proc_create_single("flags", 0444, jent_proc_dir,
				jent_proc_flags_show))
		pr_warn("jitterentropy: failed to create /proc/%s/flags\n",
			JENT_PROC_DIRNAME);
}

void jent_proc_exit(void)
{
	/*
	 * Removes the directory and everything still below it (the statistics
	 * file and any interface files whose owners did not run first).
	 */
	proc_remove(jent_proc_dir);
	jent_proc_dir = NULL;
}
