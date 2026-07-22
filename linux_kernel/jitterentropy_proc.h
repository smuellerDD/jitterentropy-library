/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Shared /proc/jitterentropy directory for the Jitter RNG kernel interfaces.
 *
 * The core module owns the /proc/jitterentropy directory and a statistics file
 * below it. Optional interfaces (e.g. the hwrng status) place their own files
 * under the exported @jent_proc_dir parent.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _JITTERENTROPY_PROC_H
#define _JITTERENTROPY_PROC_H

#include <linux/init.h>
#include <linux/proc_fs.h>

/* Name of the shared /proc directory. */
#define JENT_PROC_DIRNAME "jitterentropy"

/*
 * Parent directory for all Jitter RNG proc files. NULL if procfs is not
 * available (CONFIG_PROC_FS off) or the directory could not be created; callers
 * placing files below it must check for NULL first.
 */
extern struct proc_dir_entry *jent_proc_dir;

/*
 * Create/remove /proc/jitterentropy and the files below it. A creation
 * failure on a procfs-enabled kernel is returned as -ENOMEM after removing
 * anything already created; with CONFIG_PROC_FS off the init succeeds and
 * jent_proc_dir stays NULL.
 */
int __init jent_proc_init(void);
void jent_proc_exit(void);

/*
 * Account for character-device instances (one Jitter RNG entropy collector per
 * open file description). Reflected in /proc/jitterentropy/statistics.
 */
void jent_proc_instance_inc(void);
void jent_proc_instance_dec(void);

#endif /* _JITTERENTROPY_PROC_H */
