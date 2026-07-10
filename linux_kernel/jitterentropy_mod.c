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
 * single include covers the whole supported kernel range.
 */
#include <crypto/algapi.h>
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

module_param(osr, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(osr, "Jitter RNG OSR parameter");
module_param(flags, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(flags, "Jitter RNG flags parameter");
module_param(verbose, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(verbose, "Jitter RNG verbose logging");

static int __init jent_mod_init(void)
{
	int ret = 0;

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		/* Handle permanent health test error */
		if (fips_enabled)
			panic("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);

		pr_info("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);
		return -EFAULT;
	}

	jent_proc_init();

	/*
	 * Register the debugfs test interface only after the library
	 * initialization above completed: the file is visible to userspace
	 * immediately, and a read triggers library init paths that must not
	 * run concurrently with jent_entropy_init_ex().
	 */
	jent_testing_init();

	ret = jent_kcapi_init();
	if (ret)
		goto err;

	ret = jent_chardev_init();
	if (ret)
		goto err_crypto;

	ret = jent_hwrng_init();
	if (ret)
		goto err_chardev;

	return 0;

err_chardev:
	jent_chardev_exit();
err_crypto:
	jent_kcapi_exit();
err:
	jent_testing_exit();
	jent_proc_exit();
	return ret;
}

static void __exit jent_mod_exit(void)
{
	jent_hwrng_exit();
	jent_chardev_exit();
	jent_proc_exit();
	jent_testing_exit();
	jent_kcapi_exit();
}

module_init(jent_mod_init);
module_exit(jent_mod_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Non-physical True Random Number Generator based on CPU Jitter");
/*
 * The crypto API name depends on the build mode (see jent_alg.base.cra_name
 * in jitterentropy_kcapi.c). Alias the matching name so the algorithm can be
 * auto-loaded on request via the kernel crypto API.
 */
#ifdef CONFIG_BUILTIN_JITTERENTROPY
MODULE_ALIAS_CRYPTO("jitterentropy_rng");
#else
MODULE_ALIAS_CRYPTO("jitter_rng");
#endif
