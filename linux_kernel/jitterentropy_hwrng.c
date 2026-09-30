/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Hardware RNG (hwrng) interface for Jitter RNG.
 *
 * This registers the Jitter RNG with the kernel hw_random framework, exposing
 * it as /dev/hwrng (when selected as the current hwrng) and optionally letting
 * the kernel hwrng entropy thread (rngd in-kernel) feed the random pool.
 *
 * A single Jitter RNG instance is allocated when the hwrng is registered (at
 * module load) and freed when it is unregistered (at module unload). read
 * requests are served from that instance and serialized via a mutex.
 *
 * The whole interface can be disabled at compile time by not setting the
 * CONFIG_EXTERNAL_JITTERENTROPY_HWRNG configuration option (see Kbuild.config).
 * In that case the stubs in jitterentropy_hwrng.h are used.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/hw_random.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "jitterentropy.h"
#include "jitterentropy_error.h"
#include "jitterentropy_hwrng.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_status.h"

/*
 * The OSR and flags used to allocate the Jitter RNG instance are shared with
 * the crypto API interface and are configurable via the module parameters of
 * the same name (see jitterentropy_mod.c).
 */
extern unsigned int jent_osr;
extern unsigned int jent_flags;

/*
 * Entropy quality declared to the hw_random framework, expressed as the number
 * of bits of estimated entropy per 1024 bits of output (maximum 1024).
 *
 * The meaning of the default of 0 depends on the kernel version:
 *
 * - Kernels < 6.2: the device is registered without contributing to the
 *   kernel random pool automatically; the Jitter RNG output is still
 *   available via /dev/hwrng.
 *
 * - Kernels >= 6.2: the hw_random core promotes a driver quality of 0 to the
 *   global default (module parameter rng_core.default_quality, 1024 unless
 *   overridden), so the in-kernel hwrng thread will feed the random pool with
 *   full entropy credit. A per-driver opt-out no longer exists; boot with
 *   rng_core.default_quality=0 or lower /sys/class/misc/hw_random/rng_quality
 *   at runtime to prevent the automatic crediting. The promoted quality
 *   also ranks the device above every hwrng declaring less, hardware TRNGs
 *   included, when the core picks the current rng - see the hwrng section
 *   of README.md. When the promoted quality takes effect, a one-time notice
 *   is logged (see jent_hwrng_report_promotion()).
 *
 * The default stays 0 nonetheless: no value avoids both the promotion and the
 * credit, and a fixed non-zero one would decide for every system which
 * hardware RNG the Jitter RNG outranks.
 *
 * Set a non-zero value (e.g. 1024, as the Jitter RNG is designed to deliver
 * full-entropy, conditioned output) to declare the quality explicitly.
 */
static unsigned int hwrng_quality = 0;
module_param(hwrng_quality, uint, S_IRUSR | S_IRGRP | S_IROTH);
/*
 * The promotion of quality 0 exists only since kernel 6.2, so only builds for
 * those kernels carry the hint - on older kernels it would describe behavior
 * the running kernel does not have.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
MODULE_PARM_DESC(hwrng_quality,
		 "Jitter RNG hwrng entropy quality (bits per 1024 bits, 0..1024; the hw_random core promotes 0 to rng_core.default_quality)");
#else
MODULE_PARM_DESC(hwrng_quality,
		 "Jitter RNG hwrng entropy quality (bits per 1024 bits, 0..1024; 0 registers without crediting entropy to the random pool)");
#endif

/* State backing the single hwrng instance. */
struct jent_hwrng_ctx {
	struct mutex lock;
	struct rand_data *entropy_collector;
	/* This instance's own periodic self test. */
	struct jent_selftest_instance selftest;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
	/* The hwrng_quality=0 promotion notice was emitted, under lock. */
	bool promotion_reported;
#endif
};

static struct jent_hwrng_ctx jent_hwrng_ctx;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
/*
 * Warn once when hwrng_quality=0 is in effect as a non-zero quality, i.e. the
 * hw_random core promoted it to rng_core.default_quality. Called with
 * ctx->lock held.
 *
 * Where the core performs that promotion differs between kernel versions:
 *
 * - 6.2 up to 6.10: only in hwrng_init(), i.e. when the device becomes the
 *   current rng. hwrng_register() compares the raw 0 against the current rng,
 *   so with another hwrng (TPM, virtio-rng, CPU RNG, ...) already current the
 *   device stays at quality 0 after registration and is promoted silently if
 *   selected later via rng_current or when the current rng goes away.
 *
 * - 6.11 and newer (commit 95c0f5c3b8bb "hwrng: core - Fix wrong quality
 *   calculation at hw rng registration", also backported to the 6.6 and 6.10
 *   stable series): already in hwrng_register().
 *
 * A check right after registration therefore misses the first case. The core
 * calls the read callback only for the current rng and only after
 * hwrng_init(), so checking again on every read catches it before (or with)
 * the first output the in-kernel hwrng thread can credit.
 */
static void jent_hwrng_report_promotion(struct jent_hwrng_ctx *ctx,
					struct hwrng *rng)
{
	unsigned short quality = READ_ONCE(rng->quality);

	if (hwrng_quality || !quality || ctx->promotion_reported)
		return;

	ctx->promotion_reported = true;
	pr_notice("jitterentropy: hwrng_quality=0, but the hw_random core credits hwrng '%s' with quality %u; the kernel may credit Jitter RNG output to the random pool (boot with rng_core.default_quality=0 to prevent this) and prefers it as current hwrng over devices of lower quality (write the one to keep to /sys/class/misc/hw_random/rng_current)\n",
		  rng->name, quality);
}
#else
static inline void jent_hwrng_report_promotion(struct jent_hwrng_ctx *ctx,
					       struct hwrng *rng)
{
}
#endif

static int jent_hwrng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct jent_hwrng_ctx *ctx = &jent_hwrng_ctx;
	ssize_t ret;

	if (!max)
		return 0;

	/*
	 * Jitter entropy collection is CPU-bound and slow, and the mutex may be
	 * held by another reader while it generates (bounded by the hwrng core
	 * buffer size per call) or by this instance's own self test run. A
	 * non-blocking caller (wait == false) must not sleep on it; report "no
	 * data available" instead so the hwrng core can fall back or retry. A
	 * blocking caller sleeps interruptibly so a /dev/hwrng reader stays
	 * killable while waiting.
	 */
	if (!wait) {
		if (!mutex_trylock(&ctx->lock))
			return 0;
	} else if (mutex_lock_interruptible(&ctx->lock)) {
		return -ERESTARTSYS;
	}
	jent_hwrng_report_promotion(ctx, rng);
	/*
	 * jent_read_entropy_safe() recovers from an intermittent health-test
	 * failure by reallocating the collector, hence the indirection, and
	 * running its startup again, which a non-blocking caller must not
	 * wait for. jent_read_entropy() returns that failure instead; it is
	 * sticky, so the next blocking read - the hwrng core's fill thread
	 * reads blocking - recovers it.
	 *
	 * Either returns the number of generated bytes (== max) or a negative
	 * error code on a permanent, unrecovered or generic failure -
	 * JENT_ERR_SELFTEST after a failed run of this instance's self test
	 * included. An unrecovered one may clear on a later read, but nothing
	 * says when, so none maps to -EAGAIN; lock contention above is the
	 * only "no data yet".
	 */
	if (wait)
		ret = jent_read_entropy_safe(&ctx->entropy_collector, data,
					     max);
	else
		ret = jent_read_entropy(ctx->entropy_collector, data, max);
	mutex_unlock(&ctx->lock);

	if (ret < 0)
		return jent_map_user_read_error(ret);

	return (int)ret;
}

static struct hwrng jent_hwrng = {
	.name	= "jitterentropy",
	.read	= jent_hwrng_read,
};

/*
 * Status of the global hwrng Jitter RNG instance, exported read-only as
 * /proc/jitterentropy/hwrng_status. Reading it emits the JSON status string
 * produced by jent_status() (version, health-test state, runtime environment
 * and configuration) for the single instance backing /dev/hwrng.
 */
#define JENT_HWRNG_PROC_NAME	"hwrng_status"

static struct proc_dir_entry *jent_hwrng_proc;

/* __maybe_unused: without CONFIG_PROC_FS proc_create_single() discards it. */
static int __maybe_unused jent_hwrng_proc_status_show(struct seq_file *m,
						      void *v)
{
	struct jent_hwrng_ctx *ctx = &jent_hwrng_ctx;

	return jent_status_seq_show(m, &ctx->lock, &ctx->entropy_collector);
}

int __init jent_hwrng_init(void)
{
	int ret;

	mutex_init(&jent_hwrng_ctx.lock);

	jent_hwrng_ctx.entropy_collector =
		jent_entropy_collector_alloc(jent_osr, jent_flags);
	if (!jent_hwrng_ctx.entropy_collector) {
		/* As for the character device: startup or memory. */
		pr_warn("jitterentropy: no entropy collector for the hwrng: its startup failed or memory ran out\n");
		mutex_destroy(&jent_hwrng_ctx.lock);
		return -ENOMEM;
	}

	jent_selftest_instance_init(&jent_hwrng_ctx.selftest,
				    &jent_hwrng_ctx.lock,
				    &jent_hwrng_ctx.entropy_collector);

	if (hwrng_quality > 1024)
		pr_warn("jitterentropy: hwrng_quality %u out of range, clamping to 1024\n",
			hwrng_quality);
	jent_hwrng.quality = (unsigned short)min_t(unsigned int, hwrng_quality,
						   1024);

	ret = hwrng_register(&jent_hwrng);
	if (ret) {
		pr_err("jitterentropy: failed to register hwrng device: %d\n",
		       ret);
		jent_selftest_instance_exit(&jent_hwrng_ctx.selftest);
		jent_entropy_collector_free(jent_hwrng_ctx.entropy_collector);
		jent_hwrng_ctx.entropy_collector = NULL;
		mutex_destroy(&jent_hwrng_ctx.lock);
		return ret;
	}

	/*
	 * Report the quality as of registration. On kernels >= 6.2 the core
	 * rewrites the requested value (in particular it promotes 0 to the
	 * rng_core.default_quality global, 1024 unless overridden) - but on
	 * 6.2 up to 6.10 only once the device becomes the current rng, so a 0
	 * here may still be raised later. jent_hwrng_report_promotion() covers
	 * both cases; it runs again on the first read once the device is
	 * current.
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 2, 0)
	if (!hwrng_quality && !READ_ONCE(jent_hwrng.quality))
		pr_info("jitterentropy: hwrng device '%s' registered (quality 0; the hw_random core may raise it to rng_core.default_quality when the device becomes the current rng)\n",
			jent_hwrng.name);
	else
#endif
		pr_info("jitterentropy: hwrng device '%s' registered (effective quality %u)\n",
			jent_hwrng.name, READ_ONCE(jent_hwrng.quality));
	mutex_lock(&jent_hwrng_ctx.lock);
	jent_hwrng_report_promotion(&jent_hwrng_ctx, &jent_hwrng);
	mutex_unlock(&jent_hwrng_ctx.lock);

	/*
	 * Non-fatal: the hwrng device is fully functional without the status
	 * export, so a failure here (or a kernel built without CONFIG_PROC_FS,
	 * where jent_proc_dir is NULL) must not abort registration.
	 */
	if (jent_proc_dir) {
		/* Root only: it reports other users' activity on /dev/hwrng. */
		jent_hwrng_proc = proc_create_single(JENT_HWRNG_PROC_NAME, 0400,
						     jent_proc_dir,
						     jent_hwrng_proc_status_show);
		if (!jent_hwrng_proc)
			pr_warn("jitterentropy: failed to create /proc/%s/%s\n",
				JENT_PROC_DIRNAME, JENT_HWRNG_PROC_NAME);
	}

	return 0;
}

void jent_hwrng_exit(void)
{
	/*
	 * Remove the status export first: proc_remove() waits for any in-flight
	 * reader to leave jent_hwrng_proc_status_show() before returning, so the
	 * collector can subsequently be freed without racing a reader.
	 */
	proc_remove(jent_hwrng_proc);
	jent_hwrng_proc = NULL;

	hwrng_unregister(&jent_hwrng);

	/* Before the collector is freed; waits for a run in progress. */
	jent_selftest_instance_exit(&jent_hwrng_ctx.selftest);

	if (jent_hwrng_ctx.entropy_collector) {
		jent_entropy_collector_free(jent_hwrng_ctx.entropy_collector);
		jent_hwrng_ctx.entropy_collector = NULL;
	}

	mutex_destroy(&jent_hwrng_ctx.lock);
}
