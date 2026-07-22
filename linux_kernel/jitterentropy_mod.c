/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel module handling for Jitter RNG.
 *
 * Defines the module parameters shared by the kernel interfaces and ties the
 * interfaces (crypto API, hwrng, character device, procfs status, test
 * interface) together in the module init/exit paths.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

/*
 * MODULE_ALIAS_CRYPTO() lives in crypto/algapi.h on kernels >= 6.4 and in
 * linux/crypto.h before; crypto/algapi.h includes linux/crypto.h, so this
 * single include covers the whole supported kernel range. Only needed for
 * the crypto API alias emitted at the bottom of this file.
 */
#ifdef CONFIG_EXTERNAL_JITTERENTROPY_KCAPI
#include <crypto/algapi.h>
#endif
#include <linux/fips.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "jitterentropy.h"
#include "jitterentropy_chardev.h"
#include "jitterentropy_compat.h"
#include "jitterentropy_hwrng.h"
#include "jitterentropy_kcapi.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_testing.h"

/*
 * Kernel module options.
 *
 * osr, flags and verbose are non-static as they are shared with the kernel
 * interfaces: osr and flags with the crypto API, hwrng and character device
 * interfaces, verbose with the crypto API and test interfaces (see
 * jitterentropy_kcapi.c and jitterentropy_testing.c).
 */
unsigned int osr = 0;
int flags = 0;
unsigned int verbose = 0;

/*
 * Shortcut parameters for common operation modes. They are folded into the
 * shared flags value in jent_mod_init(), so the effective configuration is
 * visible in the flags sysfs file; the numeric flag bits do not need to be
 * known to request the modes.
 */
static bool ntg1 = false;
static bool force_fips = false;

module_param(osr, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(osr, "Jitter RNG OSR parameter");
module_param(flags, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(flags, "Jitter RNG flags parameter");
module_param(verbose, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(verbose, "Jitter RNG verbose logging");
module_param(ntg1, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(ntg1, "Enable AIS 20/31 NTG.1 compliant operation (shortcut for the JENT_NTG1 bit in flags)");
module_param(force_fips, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(force_fips, "Force FIPS compliant operation (shortcut for the JENT_FORCE_FIPS bit in flags)");

static int __init jent_mod_init(void)
{
	int ret = 0;

	/*
	 * Fold the shortcut parameters into the shared flags value before any
	 * user of it runs: the interfaces (crypto API, hwrng, character
	 * device) allocate their collectors from flags at open/instantiation
	 * time, and the sysfs flags file then reports the effective value.
	 */
	if (ntg1)
		flags |= JENT_NTG1;
	if (force_fips)
		flags |= JENT_FORCE_FIPS;

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		/* Handle permanent health test error */
		if (fips_enabled)
			panic("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);

		pr_info("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);
		return -EFAULT;
	}

	jent_proc_init();

	ret = jent_kcapi_init();
	if (ret)
		goto err;

	ret = jent_hwrng_init();
	if (ret)
		goto err_crypto;

	/*
	 * Register the character device after all other fallible steps:
	 * misc_register() makes /dev/jitterentropy openable by userspace
	 * (e.g. udev) immediately, and misc_deregister() does not wait for
	 * open files. If a later init step failed after the device went live,
	 * an already-open file would keep pointing into a module that init
	 * failure frees. The hwrng registration is safe to precede it as
	 * hwrng_unregister() drains all readers before returning.
	 */
	ret = jent_chardev_init();
	if (ret)
		goto err_hwrng;

	/*
	 * Register the debugfs test interface as the very last step: like the
	 * character device, the file is visible to userspace immediately and
	 * debugfs_remove_recursive() does not wait for open files, so it must
	 * not be created while a later init step could still fail (an open
	 * file would outlive the module memory freed on init failure). Being
	 * after jent_entropy_init_ex() also keeps its reads from racing the
	 * library initialization. jent_testing_init() itself cannot fail, so
	 * no unwind is needed for it.
	 */
	jent_testing_init();

	return 0;

err_hwrng:
	jent_hwrng_exit();
err_crypto:
	jent_kcapi_exit();
err:
	jent_proc_exit();
	return ret;
}

static void __exit jent_mod_exit(void)
{
	/*
	 * Tear down in reverse order of the registrations in jent_mod_init().
	 * The relative order of the character device, hwrng and procfs teardown
	 * matters: both interfaces remove their status files below the shared
	 * /proc/jitterentropy directory, so their exits must run before
	 * jent_proc_exit() removes that directory recursively (a later
	 * proc_remove() on an already-removed entry would act on freed memory).
	 */
	jent_testing_exit();
	jent_chardev_exit();
	jent_hwrng_exit();
	jent_kcapi_exit();
	jent_proc_exit();
}

module_init(jent_mod_init);
module_exit(jent_mod_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Non-physical True Random Number Generator based on CPU Jitter");
/*
 * The crypto API name depends on the build mode (see jent_alg.base.cra_name
 * in jitterentropy_kcapi.c). Alias the matching name so the algorithm can be
 * auto-loaded on request via the kernel crypto API. Only emitted when the
 * crypto API interface is compiled in: the alias must not advertise an
 * algorithm this build does not register.
 */
#ifdef CONFIG_EXTERNAL_JITTERENTROPY_KCAPI
# ifdef CONFIG_BUILTIN_JITTERENTROPY
MODULE_ALIAS_CRYPTO("jitterentropy_rng");
# else
MODULE_ALIAS_CRYPTO("jitter_rng");
# endif
#endif /* CONFIG_EXTERNAL_JITTERENTROPY_KCAPI */
