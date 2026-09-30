/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Kernel module handling for Jitter RNG.
 *
 * Defines the module parameters shared by the kernel interfaces and ties the
 * interfaces (crypto API, hwrng, character device, procfs status, test
 * interface) together in the module init/exit paths.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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
#include <linux/log2.h>
#include <linux/module.h>

#include "jitterentropy.h"
#include "jitterentropy_chardev.h"
#include "jitterentropy_compat.h"
#include "jitterentropy_hwrng.h"
#include "jitterentropy_kcapi.h"
#include "jitterentropy_mod.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_testing.h"

/*
 * Kernel module options.
 *
 * osr, flags and verbose are non-static as they are shared with the kernel
 * interfaces: osr and flags with the crypto API, hwrng and character device
 * interfaces and procfs, verbose with the crypto API, self test and test
 * interfaces (see jitterentropy_kcapi.c and jitterentropy_testing.c).
 *
 * Being global, the variables carry the jent_ prefix: built into vmlinux
 * (CONFIG_BUILTIN_JITTERENTROPY) they share one symbol namespace with the
 * whole kernel, where names like flags or verbose risk a multiple definition.
 * module_param_named() keeps the parameter names users set.
 */
unsigned int jent_osr = 0;
unsigned int jent_flags = 0;
unsigned int jent_verbose = 0;

/*
 * Shortcut parameters for common operation modes. They are folded into the
 * shared flags value in jent_mod_init(), so the effective configuration is
 * visible in the flags sysfs file; the numeric flag bits do not need to be
 * known to request the modes.
 */
static bool ntg1 = false;
static bool force_fips = false;
static bool cache_all = false;

/*
 * Size of the memory access region of every instance, in kB: a power of two
 * up to 524288 (512 MB) - 65536 on 32-bit, see JENT_MOD_MAX_MEMSIZE_KB -, 0
 * (the default) leaving it to the library.
 *
 * Unset, the size follows the cache size (hundreds of MB with cache_all) and
 * doubles on every health-test recovery in a compliance mode, up to 512 MB
 * (64 MB on 32-bit);
 * max_instances bounds neither. Setting it pins the size: the recovery raises
 * only the oversampling rate and the hash loop count. It takes precedence
 * over a size given through the flags parameter.
 */
static unsigned int max_memsize;

/*
 * The library's own ceiling for a size it derives (JENT_MAX_AUTO_MEMSIZE,
 * internal to it): 512 MB, or 64 MB on 32-bit, whose vmalloc area (about
 * 128 MB on i386, shared by the whole kernel) makes every larger collector
 * allocation fail with a bare ENOMEM. An explicit size gets the same bound,
 * so it can reach what automatic sizing and recovery reach, and no more.
 */
#if BITS_PER_LONG == 32
#define JENT_MOD_MAX_MEMSIZE	JENT_MAX_MEMSIZE_64MB
#else
#define JENT_MOD_MAX_MEMSIZE	JENT_MAX_MEMSIZE_MAX
#endif
#define JENT_MOD_MAX_MEMSIZE_KB						       \
	(1U << (JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MOD_MAX_MEMSIZE) - 1))

module_param_named(osr, jent_osr, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(osr, "Jitter RNG OSR parameter");
module_param_named(flags, jent_flags, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(flags, "Jitter RNG flags parameter");
module_param_named(verbose, jent_verbose, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(verbose, "Jitter RNG verbose logging");
module_param(ntg1, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(ntg1, "Enable AIS 20/31 NTG.1 compliant operation (shortcut for the JENT_NTG1 bit in flags)");
module_param(force_fips, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(force_fips, "Force FIPS compliant operation (shortcut for the JENT_FORCE_FIPS bit in flags)");
module_param(cache_all, bool, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(cache_all, "Derive the memory access region from the size of all caches instead of L1 only (shortcut for the JENT_CACHE_ALL bit in flags)");
module_param(max_memsize, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_memsize,
		 "Memory access region of an instance in kB, a power of two up to 524288, 65536 on 32-bit (shortcut for the JENT_MAX_MEMSIZE_* bits in flags; 0: derived from the cache size and grown by health-test recovery)");

/*
 * Log each health test failure of a FIPS or NTG.1 instance, including those of
 * a collector startup that gives up: its allocation then only returns NULL,
 * which the interfaces cannot tell apart from memory running out.
 */
static void jent_mod_health_failure(struct rand_data *ec,
				    unsigned int health_failure)
{
	unsigned int bits = health_failure |
			    (health_failure >> JENT_PERMANENT_FAILURE_SHIFT);
	char uuid[JENT_UUID_STRLEN];

	if (jent_entropy_collector_uuid(ec, uuid, sizeof(uuid)))
		uuid[0] = '\0';

	pr_warn_ratelimited("jitterentropy: %s health test failure 0x%x:%s%s%s%s, instance %s, reinit %u, osr %u, hashloop %u, memsize %zu\n",
			    health_failure >> JENT_PERMANENT_FAILURE_SHIFT ?
			    "permanent" : "intermittent",
			    health_failure,
			    bits & JENT_RCT_FAILURE ? " RCT" : "",
			    bits & JENT_APT_FAILURE ? " APT" : "",
			    bits & JENT_LAG_FAILURE ? " lag" : "",
			    bits & JENT_RCT_MEM_FAILURE ? " RCT-mem" : "",
			    uuid[0] ? uuid : "(starting)",
			    jent_entropy_collector_reinitializations(ec),
			    jent_entropy_collector_osr(ec),
			    jent_entropy_collector_hashloops(ec),
			    jent_entropy_collector_memsize(ec));
}

int jent_mod_check_config(unsigned int osr, unsigned int flags,
			  const char *prefix)
{
	/*
	 * Refused here: jent_entropy_init_ex() would report it as a failed
	 * startup, which panics a fips=1 kernel over a configuration error. A
	 * rate below the minimum is raised by the library.
	 */
	if (osr > JENT_MAX_OSR) {
		pr_err("jitterentropy: %sosr %u is above the maximum of %u\n",
		       prefix, osr, (unsigned int)JENT_MAX_OSR);
		return -EINVAL;
	}

	/*
	 * The kernel build has no internal timer to force: every allocation
	 * would fail as if memory ran out.
	 */
	if (flags & JENT_FORCE_INTERNAL_TIMER) {
		pr_err("jitterentropy: %sflags force the internal timer, which is not available in the kernel\n",
		       prefix);
		return -EINVAL;
	}

	/* As the OSR: the compliance startup needs the memory access source. */
	if ((flags & JENT_DISABLE_MEMORY_ACCESS) &&
	    ((flags & (JENT_NTG1 | JENT_FORCE_FIPS)) || fips_enabled)) {
		pr_err("jitterentropy: %sflags disable memory access, which FIPS and NTG.1 mode need\n",
		       prefix);
		return -EINVAL;
	}

	return 0;
}

#if !defined(MODULE) && defined(CONFIG_EXTERNAL_JITTERENTROPY_HWRNG)
/*
 * Built into vmlinux, the hwrng registers from an initcall of its own after
 * every device initcall rather than from jent_mod_init().
 *
 * The object sits below crypto/, which is linked - and so initialized - before
 * drivers/, and the hw_random core of 5.10 and 5.15 initializes from
 * module_init() as well (later kernels moved it to fs_initcall()). From the
 * same level hwrng_register() would run before hwrng_modinit() allocated
 * rng_buffer and rng_fillbuf: the read into the NULL buffer fails, so the
 * early randomness the registration feeds the random pool is lost, and with a
 * non-zero quality the fill thread starts on a failed read and sleeps ten
 * seconds. As a module the hw_random core is either built in or rng-core.ko,
 * which jitter_rng.ko depends on and which is therefore initialized first.
 *
 * Set by jent_mod_init() once the library came up, cleared again if the load
 * fails after all, so the deferred registration only runs on a module that
 * otherwise came up correctly. Only touched from init code - jent_mod_init(),
 * its error path and the initcall below - so it is discarded with it.
 */
static bool jent_mod_hwrng_pending __initdata;

static int __init jent_mod_hwrng_init(void)
{
	jent_mod_hwrng_pending = true;
	return 0;
}

/* Only the error path of jent_mod_init() undoes the registration. */
static void __init jent_mod_hwrng_exit(void)
{
	jent_mod_hwrng_pending = false;
}

/*
 * Not the module's failure, as for the character device: jent_mod_init() has
 * already registered the other interfaces, the crypto API algorithm among them,
 * which cannot be unwound.
 */
static int __init jent_mod_hwrng_late_init(void)
{
	if (jent_mod_hwrng_pending && jent_hwrng_init())
		pr_warn("jitterentropy: hwrng unavailable, continuing without it\n");

	return 0;
}
device_initcall_sync(jent_mod_hwrng_late_init);
#else
static int __init jent_mod_hwrng_init(void)
{
	return jent_hwrng_init();
}

static void __init jent_mod_hwrng_exit(void)
{
	jent_hwrng_exit();
}
#endif

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
		jent_flags |= JENT_NTG1;
	if (force_fips)
		jent_flags |= JENT_FORCE_FIPS;
	if (cache_all)
		jent_flags |= JENT_CACHE_ALL;

	ret = jent_mod_check_config(jent_osr, jent_flags, "");
	if (ret)
		return ret;

	if (max_memsize) {
		/*
		 * The field encodes 1 kB << (field - 1). Other sizes refuse the
		 * load rather than being rounded to one nobody asked for.
		 */
		if (!is_power_of_2(max_memsize) ||
		    max_memsize > JENT_MOD_MAX_MEMSIZE_KB) {
			pr_err("jitterentropy: max_memsize %u kB is not a power of two of at most %u kB\n",
			       max_memsize, JENT_MOD_MAX_MEMSIZE_KB);
			return -EINVAL;
		}

		jent_flags &= ~(unsigned int)JENT_MAX_MEMSIZE_MASK;
		jent_flags |= JENT_MAX_MEMSIZE_TO_FLAGS(ilog2(max_memsize) + 1);
	} else if (JENT_FLAGS_TO_MAX_MEMSIZE(jent_flags) >
		   ilog2(JENT_MOD_MAX_MEMSIZE_KB) + 1) {
		/*
		 * The same bound for a size given through flags. Not part of
		 * jent_mod_check_config(): the test interface records every
		 * size the library accepts, as far as the kernel can allocate
		 * it.
		 */
		pr_err("jitterentropy: flags select a memory size above %u kB\n",
		       JENT_MOD_MAX_MEMSIZE_KB);
		return -EINVAL;
	}

	/* Before jent_entropy_init_ex(): the library refuses it afterwards. */
	ret = jent_set_fips_failure_callback(jent_mod_health_failure);
	if (ret)
		return ret < 0 ? ret : -EINVAL;

	ret = jent_entropy_init_ex(jent_osr, jent_flags);
	if (ret) {
		/* Handle permanent health test error */
		if (fips_enabled)
			panic("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);

		pr_info("jitterentropy: Initialization failed with host not compliant with requirements: %d\n", ret);
		return -EFAULT;
	}

	/*
	 * Validate the interval the interfaces' instances schedule their own
	 * periodic self test runs with, before any of them is registered. The
	 * known answer tests themselves have just run inside
	 * jent_entropy_init_ex() above, which refused the load on failure.
	 */
	jent_selftest_init();

	ret = jent_proc_init();
	if (ret)
		return ret;

	/* Deferred when built into vmlinux, see jent_mod_hwrng_init(). */
	ret = jent_mod_hwrng_init();
	if (ret)
		goto err;

	/*
	 * Neither of the last two registrations can be undone under a user:
	 * crypto_register_rng() lets crypto_alloc_rng() (e.g. an AF_ALG bind)
	 * instantiate a tfm immediately, and crypto_unregister_rng() of an
	 * algorithm with a live tfm hits BUG_ON() on 5.10 (a WARN on newer
	 * kernels), with the module text freed under the tfm either way.
	 * misc_register() makes /dev/jitterentropy openable immediately and
	 * misc_deregister() does not wait for open files. So both come after
	 * every other fallible step, and nothing that can still fail the load
	 * follows them: neither is ever unwound under a user.
	 */
	ret = jent_kcapi_init();
	if (ret)
		goto err_hwrng;

	/*
	 * The character device's failure is not the module's, for the reason
	 * the crypto API registration above must not be unwound: a local
	 * process can bind an AF_ALG rng socket the moment
	 * crypto_register_rng() returns, and unwinding the algorithm under that
	 * tfm is the BUG_ON() described above. The device is a second way to
	 * reach the entropy the crypto API and the hwrng already serve, so a
	 * module that otherwise came up correctly keeps running without it.
	 * jent_chardev_init() has already logged what went wrong, and
	 * jent_chardev_exit() knows not to undo a registration that never
	 * happened.
	 */
	if (jent_chardev_init())
		pr_warn("jitterentropy: character device unavailable, continuing without it\n");

	/*
	 * The debugfs test interface last, and its failure is not the module's.
	 *
	 * It has to come after every step that can still fail the load. Creating
	 * the file earlier opened a window in which a root process could open it
	 * while a later registration was still running - crypto_register_rng()
	 * sleeps waiting on the larval test, so the window is not instantaneous.
	 * open_proxy_open() takes a module reference through fops_get(), and
	 * try_module_get() grants it because module_is_live() is true for a
	 * module still in MODULE_STATE_COMING; the fd's f_op then points into
	 * module text. If the load then failed, do_init_module()'s failure path
	 * calls module_put() and free_module() without waiting for that
	 * reference to drain, and the next read(), ioctl() or close() on the fd
	 * dispatched through a freed function table. debugfs_remove_recursive()
	 * in the unwind does not help: the fd already holds its own reference.
	 *
	 * With nothing fallible left after it the window is gone, which is why
	 * the failure is only reported here. The interface is a pure debugging
	 * aid - it already returns success and skips itself when debugfs is
	 * absent, disabled or the kernel is locked down - so a module that
	 * otherwise came up correctly must not be refused over it.
	 */
	if (jent_testing_init())
		pr_warn("jitterentropy: raw entropy test interface unavailable, continuing without it\n");

	return 0;

err_hwrng:
	jent_mod_hwrng_exit();
err:
	jent_proc_exit();
	return ret;
}

static void __exit jent_mod_exit(void)
{
	/*
	 * Reverse order of the registrations in jent_mod_init(). The character
	 * device and the hwrng must precede jent_proc_exit(): they remove
	 * status files below /proc/jitterentropy, which it removes
	 * recursively, and a proc_remove() on an already-removed entry would
	 * act on freed memory. Each interface stops the self tests of its own
	 * instances as it tears them down.
	 */
	jent_chardev_exit();
	jent_kcapi_exit();
	jent_testing_exit();
	jent_hwrng_exit();
	jent_proc_exit();
}

module_init(jent_mod_init);
module_exit(jent_mod_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Stephan Mueller <smueller@chronox.de>");
MODULE_AUTHOR("Markus Theil <theil.markus@gmail.com>");
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
