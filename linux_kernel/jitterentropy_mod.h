/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel module handling for Jitter RNG.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#ifndef _JITTERENTROPY_MOD_H
#define _JITTERENTROPY_MOD_H

/*
 * Check an OSR and flags pair the module is about to allocate collectors
 * with: the osr/flags module parameters at load, testing_osr/testing_flags on
 * every open of the debugfs test interface. Refuses what the kernel build
 * cannot run, and what the library would only report as a failed startup -
 * which panics a fips=1 kernel over a configuration error - or as a NULL
 * allocation indistinguishable from memory running out:
 *
 * - an OSR above JENT_MAX_OSR (a lower one is raised by the library),
 * - undefined flag bits and flag fields above their maximum,
 * - JENT_FORCE_INTERNAL_TIMER, as the kernel has no internal timer,
 * - JENT_DISABLE_MEMORY_ACCESS in FIPS or NTG.1 mode, fips=1 included.
 *
 * The module load bounds the memory size further, see jent_mod_init().
 *
 * @prefix names the parameters in the log message ("" or "testing_").
 * Return: 0, or -EINVAL after logging why.
 */
int jent_mod_check_config(unsigned int osr, unsigned int flags,
			  const char *prefix);

#endif /* _JITTERENTROPY_MOD_H */
