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
