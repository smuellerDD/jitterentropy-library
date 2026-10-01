/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Shared /proc/jitterentropy directory and statistics file for the Jitter RNG
 * kernel interfaces.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/atomic.h>
#include <linux/fips.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include "jitterentropy.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_selftest.h"

struct proc_dir_entry *jent_proc_dir;

/*
 * The effective flags and OSR values shared by the kernel interfaces,
 * including the folded-in shortcut parameters (see jitterentropy_mod.c).
 */
extern unsigned int jent_flags;
extern unsigned int jent_osr;

/*
 * Machine-readable variants reported via /proc/jitterentropy/config/flags_raw
 * and /proc/jitterentropy/config/osr: the plain values without any decoration,
 * directly
 * reusable as the flags= and osr= module parameters (kernel parameter parsing
 * accepts the 0x prefix).
 */
static int jent_proc_flags_raw_show(struct seq_file *m, void *v)
{
	seq_printf(m, "0x%08x\n", jent_flags);

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

/*
 * Shared show routine; m->private points at the table entry's value.
 *
 * This and the other show routines passed only to proc_create_single*() are
 * __maybe_unused: without CONFIG_PROC_FS those are stubs that discard the
 * routine, which would leave it unreferenced (-Wunused-function, fatal with
 * CONFIG_WERROR).
 */
static int __maybe_unused jent_proc_interface_show(struct seq_file *m, void *v)
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
	seq_printf(m, "%u\n", !!(jent_flags & JENT_NTG1));

	return 0;
}

static int jent_proc_fips_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u\n",
		   !!((jent_flags & (JENT_FORCE_FIPS | JENT_NTG1)) ||
		      fips_enabled));

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
	seq_printf(m, "%u\n",
		   jent_osr < JENT_MIN_OSR ? JENT_MIN_OSR : jent_osr);

	return 0;
}

/*
 * Human-readable breakdown of the effective flags value, reported via
 * /proc/jitterentropy/config/flags. Every flag bit a loaded module can carry
 * is listed: the module refuses to load with JENT_FORCE_INTERNAL_TIMER, which
 * would always read off. JENT_DISABLE_STIR and JENT_DISABLE_UNBIAS are
 * accepted but have no effect in this library version, which a set bit says.
 */
static const struct {
	unsigned int bit;
	const char *label;	/* Column label including the colon. */
	bool unused;		/* Accepted, but without effect. */
} jent_proc_flags_bits[] = {
	{ JENT_DISABLE_STIR,		"JENT_DISABLE_STIR:", true },
	{ JENT_DISABLE_UNBIAS,		"JENT_DISABLE_UNBIAS:", true },
	{ JENT_DISABLE_MEMORY_ACCESS,	"JENT_DISABLE_MEMORY_ACCESS:" },
	{ JENT_DISABLE_INTERNAL_TIMER,	"JENT_DISABLE_INTERNAL_TIMER:" },
	{ JENT_FORCE_FIPS,		"JENT_FORCE_FIPS:" },
	{ JENT_NTG1,			"JENT_NTG1:" },
	{ JENT_CACHE_ALL,		"JENT_CACHE_ALL:" },
	{ JENT_FORCE_SECURE_MEM,	"JENT_FORCE_SECURE_MEM:" },
};

static int jent_proc_flags_show(struct seq_file *m, void *v)
{
	unsigned int memsize = JENT_FLAGS_TO_MAX_MEMSIZE(jent_flags);
	unsigned int hashloop = JENT_FLAGS_TO_HASHLOOP(jent_flags);
	unsigned int i;

	seq_printf(m, "%-29s0x%08x\n", "flags:", jent_flags);

	for (i = 0; i < ARRAY_SIZE(jent_proc_flags_bits); i++)
		seq_printf(m, "%-29s%s\n", jent_proc_flags_bits[i].label,
			   !(jent_flags & jent_proc_flags_bits[i].bit) ? "off" :
			   jent_proc_flags_bits[i].unused ? "on (no effect)" :
				"on");

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
	 * The hash loop field encodes 1 << (field - 1) iterations; field 0
	 * selects the built-in default (see jent_hashloop_cnt()).
	 */
	if (!hashloop)
		seq_printf(m, "%-29sdefault\n", "hash loop count:");
	else if (hashloop <= JENT_FLAGS_TO_HASHLOOP(JENT_MAX_HASHLOOP))
		seq_printf(m, "%-29s%u\n", "hash loop count:",
			   1U << (hashloop - 1));
	else
		seq_printf(m, "%-29sinvalid (%u)\n", "hash loop count:",
			   hashloop);

	return 0;
}

/* Library version reported via /proc/jitterentropy/version. */
static int __maybe_unused jent_proc_version_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%u.%u.%u\n", JENT_MAJVERSION, JENT_MINVERSION,
		   JENT_PATCHLEVEL);

	return 0;
}

/* Statistics reported via /proc/jitterentropy/statistics. */
static atomic_t jent_open_instances = ATOMIC_INIT(0);
static atomic64_t jent_cumulative_opens = ATOMIC64_INIT(0);

/*
 * Take a slot for a new instance, refusing beyond @max (0 = unlimited). Taken
 * before the collector is allocated, so cumulativeOpens counts admitted opens.
 *
 * Compare-and-swap rather than increment-then-undo: with the latter two opens
 * racing for the last free slot could both see the count above @max and both
 * be refused, although one of them fits. Here a slot is only ever taken if it
 * is free, so exactly the free slots are admitted.
 */
bool jent_proc_instance_inc(unsigned int max)
{
	int cur = atomic_read(&jent_open_instances);

	do {
		/*
		 * Unsigned: @max is a module parameter and may exceed INT_MAX;
		 * capped there so the increment cannot wrap. A count raised
		 * beyond @max by exempt opens is refused as well.
		 */
		if (max &&
		    (unsigned int)cur >= min_t(unsigned int, max, INT_MAX))
			return false;
	} while (!atomic_try_cmpxchg(&jent_open_instances, &cur, cur + 1));

	atomic64_inc(&jent_cumulative_opens);
	return true;
}

void jent_proc_instance_dec(void)
{
	atomic_dec(&jent_open_instances);
}

/*
 * Serialize the module-wide statistics as JSON. Validate the output with
 * "jq -e ." when changing it.
 */
static int __maybe_unused jent_proc_statistics_show(struct seq_file *m, void *v)
{
	struct jent_selftest_stats selftest;

	jent_selftest_get_stats(&selftest);

	seq_puts(m, "{\n");
	seq_puts(m, "\t\"charDevice\": {\n");
	seq_printf(m, "\t\t\"openInstances\": %d,\n",
		   atomic_read(&jent_open_instances));
	seq_printf(m, "\t\t\"cumulativeOpens\": %llu\n",
		   (unsigned long long)atomic64_read(&jent_cumulative_opens));
	seq_puts(m, "\t},\n");
	seq_puts(m, "\t\"selfTest\": {\n");
	seq_printf(m, "\t\t\"intervalSeconds\": %u,\n", selftest.interval);
	seq_printf(m, "\t\t\"runs\": %llu,\n",
		   (unsigned long long)selftest.runs);
	seq_printf(m, "\t\t\"failures\": %llu\n",
		   (unsigned long long)selftest.failures);
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

	/*
	 * Root only: the instance counts reveal other users' activity. The
	 * configuration files below stay world readable.
	 */
	if (!proc_create_single("statistics", 0400, jent_proc_dir,
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
