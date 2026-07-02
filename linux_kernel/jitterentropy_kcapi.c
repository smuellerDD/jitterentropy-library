/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel crypto API interface for Jitter RNG.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */


#include <crypto/hash.h>
#include <crypto/sha3.h>
#include <linux/fips.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <crypto/internal/rng.h>

#include "jitterentropy.h"
#include "jitterentropy_chardev.h"
#include "jitterentropy_hwrng.h"
#include "jitterentropy_testing.h"

/*
 * Kernel module options.
 *
 * osr and flags are non-static as they are shared with the (optional)
 * character device interface in jitterentropy_chardev.c.
 */
unsigned int osr = 0;
int flags = 0;
static unsigned int verbose = 0;

module_param(osr, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(osr, "Jitter RNG OSR parameter");
module_param(flags, int, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(flags, "Jitter RNG flags parameter");
module_param(verbose, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(verbose, "Jitter RNG verbose logging");

/***************************************************************************
 * Kernel crypto API interface
 ***************************************************************************/

struct jitterentropy {
	struct mutex jent_lock;
	struct rand_data *entropy_collector;
};

static void jent_kcapi_cleanup(struct crypto_tfm *tfm)
{
	struct jitterentropy *rng = crypto_tfm_ctx(tfm);

	mutex_lock(&rng->jent_lock);

	if (rng->entropy_collector)
		jent_entropy_collector_free(rng->entropy_collector);
	rng->entropy_collector = NULL;

	mutex_unlock(&rng->jent_lock);
}

static int jent_kcapi_log(struct jitterentropy *rng)
{
	char *buf;
	int ret;

	if (!verbose || !rng->entropy_collector)
		return 0;

#define JENT_STATUS_BUF_SIZE 1000
	buf = kzalloc(JENT_STATUS_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return PTR_ERR(buf);

	mutex_lock(&rng->jent_lock);
	ret = jent_status(rng->entropy_collector, buf, JENT_STATUS_BUF_SIZE);
	mutex_unlock(&rng->jent_lock);
	if (ret)
		goto err;

	pr_notice_ratelimited("%s\n", buf);

err:
	kfree(buf);
	return ret;
}

static int jent_kcapi_init(struct crypto_tfm *tfm)
{
	struct jitterentropy *rng = crypto_tfm_ctx(tfm);
	int ret = 0;

	mutex_init(&rng->jent_lock);

	rng->entropy_collector = jent_entropy_collector_alloc(osr, flags);

	if (!rng->entropy_collector) {
		ret = -ENOMEM;
		goto err;
	}

	ret = jent_kcapi_log(rng);
	if (ret)
		goto err;

	return 0;

err:
	if (rng->entropy_collector) {
		jent_entropy_collector_free(rng->entropy_collector);
		rng->entropy_collector = NULL;
	}
	return ret;
}

static int jent_kcapi_random(struct crypto_rng *tfm,
			     const u8 *src, unsigned int slen,
			     u8 *rdata, unsigned int dlen)
{
	struct jitterentropy *rng = crypto_rng_ctx(tfm);
	struct rand_data *ec;
	int ret = 0;

	mutex_lock(&rng->jent_lock);

	ec = rng->entropy_collector;
	ret = jent_read_entropy_safe(&rng->entropy_collector, rdata, dlen);

	switch (ret) {
	case -6:
	case -7:
	case -8:
	case -10:
		/* Handle permanent health test error */
		/*
		 * If the kernel was booted with fips=1, it implies that
		 * the entire kernel acts as a FIPS 140 module. In this case
		 * an SP800-90B permanent health test error is treated as
		 * a FIPS module error.
		 */
		if (fips_enabled)
			panic("Jitter RNG permanent health test failure\n");

		pr_err("Jitter RNG permanent health test failure\n");
		ret = -EFAULT;
		break;
	case -1:
	case -4:
		/* Handle generic errors */
		ret = -EINVAL;
		break;
	default:
		/* Handle unexpected errors */
		ret = -EFAULT;
		break;
	}

	mutex_unlock(&rng->jent_lock);

	if (verbose && ec != rng->entropy_collector) {
		/*
		 * The entropy collector was reallocated
		 *
		 * Do not honor the return code here: In case logging was
		 * unsuccessful, so be it.
		 */
		jent_kcapi_log(rng);
	}

	return ret;
}

static int jent_kcapi_reset(struct crypto_rng *tfm,
			    const u8 *seed, unsigned int slen)
{
	return 0;
}

/*
 * If the code is compiled as part of the kernel, use jitterentropy_rng as name.
 * Otherwise use "jitter_rng" as name as otherwise we have a name clash with
 * the existing old in-kernel variant.
 */
static struct rng_alg jent_alg = {
	.generate		= jent_kcapi_random,
	.seed			= jent_kcapi_reset,
	.seedsize		= 0,
	.base			= {
#ifdef CONFIG_BUILTIN_JITTERENTROPY
		.cra_name               = "jitterentropy_rng",
		.cra_driver_name        = "jitterentropy_rng",
#else
		.cra_name               = "jitter_rng",
		.cra_driver_name        = "jitter_rng",
#endif
		.cra_priority           = 100,
		.cra_ctxsize            = sizeof(struct jitterentropy),
		.cra_module             = THIS_MODULE,
		.cra_init               = jent_kcapi_init,
		.cra_exit               = jent_kcapi_cleanup,
	}
};

static int __init jent_mod_init(void)
{
	int ret = 0;

	jent_testing_init();

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		/* Handle permanent health test error */
		if (fips_enabled)
			panic("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);

		jent_testing_exit();
		pr_info("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);
		return -EFAULT;
	}

	ret = crypto_register_rng(&jent_alg);
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
	crypto_unregister_rng(&jent_alg);
err:
	jent_testing_exit();
	return ret;
}

static void __exit jent_mod_exit(void)
{
	jent_hwrng_exit();
	jent_chardev_exit();
	jent_testing_exit();
	crypto_unregister_rng(&jent_alg);
}

module_init(jent_mod_init);
module_exit(jent_mod_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_DESCRIPTION("Non-physical True Random Number Generator based on CPU Jitter");
MODULE_ALIAS_CRYPTO("jitterentropy_rng");
