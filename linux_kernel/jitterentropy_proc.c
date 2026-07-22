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
#include "jitterentropy-internal.h"	/* JENT_MIN_OSR */
#include "jitterentropy_proc.h"

struct proc_dir_entry *jent_proc_dir;

/*
 * The effective flags and OSR values shared by the kernel interfaces,
 * including the folded-in shortcut parameters (see jitterentropy_mod.c).
 */
extern unsigned int flags;
extern unsigned int osr;

/*
 * Machine-readable variants reported via /proc/jitterentropy/config/flags_raw
 * and /proc/jitterentropy/config/osr: the plain values without any decoration,
 * directly
 * reusable as the flags= and osr= module parameters (kernel parameter parsing
 * accepts the 0x prefix).
 */
static int jent_proc_flags_raw_show(struct seq_file *m, void *v)
{
	seq_printf(m, "0x%08x\n", flags);

	return 0;
}

/*
 * Compile-time presence of the optional kernel interfaces, reported as plain
 * 0/1 via /proc/jitterentropy/interfaces/{kcapi,hwrng,chardev,testing} so what this
 * module build provides can be checked without knowing its Kbuild.config.
 */
static const struct {
	const char *name;
	unsigned int enabled;
} jent_proc_interfaces[] = {
	{ "kcapi",	IS_ENABLED(CONFIG_EXTERNAL_JITTERENTROPY_KCAPI) },
	{ "hwrng",	IS_ENABLED(CONFIG_EXTERNAL_JITTERENTROPY_HWRNG) },
	{ "chardev",	IS_ENABLED(CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV) },
	{ "testing",
	  IS_ENABLED(CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE) },
};

/* Shared show routine; m->private points at the table entry's value. */
static int jent_proc_interface_show(struct seq_file *m, void *v)
{
	const unsigned int *enabled = m->private;

	seq_printf(m, "%u\n", *enabled);

	return 0;
}

/*
 * Quick-check mode indicators reported via /proc/jitterentropy/config/ntg1
 * and /proc/jitterentropy/config/fips as plain 0/1. They report the modes the
 * collectors actually run with (see jent_entropy_collector_alloc_internal()):
 * FIPS compliant operation is in effect when the JENT_FORCE_FIPS flag is set,
 * when the kernel itself runs in FIPS mode, or when NTG.1 mode is enabled
 * (NTG.1 implies FIPS operation).
 */
static int jent_proc_ntg1_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n", !!(flags & JENT_NTG1));

	return 0;
}

static int jent_proc_fips_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n",
		   !!((flags & (JENT_FORCE_FIPS | JENT_NTG1)) ||
		      jent_fips_enabled()));

	return 0;
}

static int jent_proc_osr_show(struct seq_file *m, void *v)
{
	/*
	 * Report the OSR the collectors actually run with: the library raises
	 * any request below JENT_MIN_OSR - including the 0 select-the-default
	 * parameter value - to that minimum (see
	 * ensure_osr_is_at_least_minimal()).
	 */
	seq_printf(m, "%u\n", osr < JENT_MIN_OSR ? JENT_MIN_OSR : osr);

	return 0;
}

/*
 * Human-readable breakdown of the effective flags value, reported via
 * /proc/jitterentropy/config/flags. Only the flag bits with a meaning in this
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

/*
 * The effective-configuration files, grouped below
 * /proc/jitterentropy/config/.
 */
static const struct {
	const char *name;
	int (*show)(struct seq_file *m, void *v);
} jent_proc_config_files[] = {
	{ "flags",	jent_proc_flags_show },
	{ "flags_raw",	jent_proc_flags_raw_show },
	{ "osr",	jent_proc_osr_show },
	{ "ntg1",	jent_proc_ntg1_show },
	{ "fips",	jent_proc_fips_show },
};

int __init jent_proc_init(void)
{
	struct proc_dir_entry *config_dir, *interfaces_dir;
	unsigned int i;

	/*
	 * Without procfs there is nothing to create and jent_proc_dir stays
	 * NULL, which the other interfaces already handle; only a failed
	 * creation on a procfs-enabled kernel is an error.
	 */
	if (!IS_ENABLED(CONFIG_PROC_FS))
		return 0;

	jent_proc_dir = proc_mkdir(JENT_PROC_DIRNAME, NULL);
	if (!jent_proc_dir) {
		pr_warn("jitterentropy: failed to create /proc/%s\n",
			JENT_PROC_DIRNAME);
		return -ENOMEM;
	}

	if (!proc_create_single("statistics", 0444, jent_proc_dir,
				jent_proc_statistics_show)) {
		pr_warn("jitterentropy: failed to create /proc/%s/statistics\n",
			JENT_PROC_DIRNAME);
		goto err;
	}

	if (!proc_create_single("version", 0444, jent_proc_dir,
				jent_proc_version_show)) {
		pr_warn("jitterentropy: failed to create /proc/%s/version\n",
			JENT_PROC_DIRNAME);
		goto err;
	}

	/* Group the effective-configuration files below config/. */
	config_dir = proc_mkdir("config", jent_proc_dir);
	if (!config_dir) {
		pr_warn("jitterentropy: failed to create /proc/%s/config\n",
			JENT_PROC_DIRNAME);
		goto err;
	}
	for (i = 0; i < ARRAY_SIZE(jent_proc_config_files); i++) {
		if (!proc_create_single(jent_proc_config_files[i].name,
					0444, config_dir,
					jent_proc_config_files[i].show)) {
			pr_warn("jitterentropy: failed to create /proc/%s/config/%s\n",
				JENT_PROC_DIRNAME,
				jent_proc_config_files[i].name);
			goto err;
		}
	}

	/* Group the compiled-in interface indicators below interfaces/. */
	interfaces_dir = proc_mkdir("interfaces", jent_proc_dir);
	if (!interfaces_dir) {
		pr_warn("jitterentropy: failed to create /proc/%s/interfaces\n",
			JENT_PROC_DIRNAME);
		goto err;
	}
	for (i = 0; i < ARRAY_SIZE(jent_proc_interfaces); i++) {
		if (!proc_create_single_data(jent_proc_interfaces[i].name,
					     0444, interfaces_dir,
					     jent_proc_interface_show,
					     (void *)&jent_proc_interfaces[i].enabled)) {
			pr_warn("jitterentropy: failed to create /proc/%s/interfaces/%s\n",
				JENT_PROC_DIRNAME,
				jent_proc_interfaces[i].name);
			goto err;
		}
	}

	return 0;

err:
	jent_proc_exit();
	return -ENOMEM;
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
