/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test interface for Jitter RNG.
 *
 * The debugfs file jent_raw_hires provides the raw noise data of the Jitter
 * RNG: each open allocates a dedicated Jitter RNG instance, each read drives
 * its measure_jitter operation and returns one u64 per measurement holding
 * the time delta as consumed by the health tests and the entropy pool (i.e.
 * including the division by the common timer GCD). This mirrors the user
 * space recording logic in
 * tests/raw-entropy/recording_userspace/jitterentropy-hashtime.c.
 *
 * The file also implements the JENT_IOCSTATUS ioctl known from the character
 * device (see jitterentropy_uapi.h), returning the JSON status string of the
 * Jitter RNG instance bound to the open file description, and the test-
 * interface-only JENT_IOCLOOPCNT ioctl, overriding the loop count applied to
 * the raw noise measurements of that instance.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "jitterentropy-internal.h"
#include "jitterentropy-noise.h"
#include "jitterentropy.h"
#include "jitterentropy_testing.h"
#include "jitterentropy_uapi.h"

/*
 * Serialize extract sessions: a concurrent reader would run its own timing
 * measurements and thereby perturb the measurements of the other session, and
 * the jent_testing_log() bookkeeping (logged_osr/logged_flags) would be
 * interleaved.
 */
static DEFINE_MUTEX(jent_testing_read_lock);

#define JENT_TEST_HASHLOOP (1<<15)
#define JENT_TEST_MEMACCLOOP (1<<16)

static struct dentry *jent_raw_debugfs_root = NULL;

/*
 * The tester can define the OSR as well as the flags used to perform the
 * testing with. The module_param below allows them to be also updated while
 * the module is inserted. As for each new open of the test interface file
 * a new Jitter RNG instance is allocated with the given OSR/flags, the
 * tester can perform the following without loading/unloading the Jitter RNG:
 *
 * 1. insmod jitter_rng.ko
 * 2. getrawentropy to collect data with default OSR/flags
 * 3.a. echo ... > /sys/module/jitter_rng/parameters/testing_osr
 * 3.b. echo ... > /sys/module/jitter_rng/parameters/testing_flags
 * 4. getrawentropy to collect data with updated OSR/flags
 * 5. Go back to step 3 and attempt testing with yet other parameters.
 */
static unsigned int testing_osr = 0;
static int testing_flags = 0;

module_param(testing_osr, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(testing_osr, "Jitter RNG testing OSR parameter");
module_param(testing_flags, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(testing_flags, "Jitter RNG testing flags parameter");

static unsigned int logged_osr = 0xffffffff;
static int logged_flags = 0xffffffff;

/*
 * Verbose logging switch, configurable via the module parameter of the same
 * name (see jitterentropy_mod.c).
 */
extern unsigned int verbose;

static int jent_testing_log(struct rand_data *ec)
{
	char *line, *p;
	char *buf;
	int ret;

	if (!verbose)
		return 0;

	if (logged_osr == testing_osr && logged_flags == testing_flags)
		return 0;

	if (!ec)
		return 0;

	/*
	 * Print out status for each test cycle where the Jitter RNG
	 * properties differ.
	 */
	logged_osr = testing_osr;
	logged_flags = testing_flags;

	buf = kvzalloc(JENT_STATUS_MAX_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = jent_status(ec, buf, JENT_STATUS_MAX_LEN);
	if (ret < 0)
		goto err;

	/*
	 * printk truncates records at about 1 kB; emit the multi-line JSON
	 * status line by line so it arrives intact. Rate-limit the status as
	 * a whole, not per line, so an emitted status is never cut short.
	 */
	p = buf;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (*line) {
			pr_notice("%s\n", line);
		}
	}

err:
	kvfree(buf);
	return ret;
}

/************** Raw High-Resolution Timer Entropy Data Handling **************/

/*
 * Per-open state: each open() gets its own Jitter RNG instance, allocated
 * with the testing_osr/testing_flags values at open time. The measurement
 * routine is captured alongside so a testing_flags update between open() and
 * read() cannot make the recording routine disagree with the instance's
 * configuration.
 */
struct jent_testing_ctx {
	struct rand_data *ec;
	unsigned int (*measure_jitter)(struct rand_data *ec,
				       uint64_t loop_cnt,
				       uint64_t *ret_current_delta);
	/*
	 * Loop count applied to each raw noise measurement, settable via
	 * JENT_IOCLOOPCNT: 0 (the default) selects the loop count the
	 * instance was configured with. Protected by
	 * jent_testing_read_lock, so it cannot change in the middle of an
	 * extract session.
	 */
	u64 loop_cnt;
};

static int jent_testing_open(struct inode *inode, struct file *file)
{
	struct jent_testing_ctx *ctx;
	unsigned int osr;
	int flags;

	ctx = kvzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* Serialize the jent_testing_log() bookkeeping. */
	if (mutex_lock_interruptible(&jent_testing_read_lock)) {
		kvfree(ctx);
		return -ERESTARTSYS;
	}

	/*
	 * Snapshot the runtime-writable module parameters once: a concurrent
	 * sysfs write between separate reads could otherwise pair a
	 * measurement routine with a collector allocated from different
	 * flags.
	 */
	flags = testing_flags;
	osr = testing_osr;

	if (flags & (JENT_TEST_HASHLOOP))
		ctx->measure_jitter = jent_measure_jitter_ntg1_sha3;
	else if (flags & (JENT_TEST_MEMACCLOOP))
		ctx->measure_jitter = jent_measure_jitter_ntg1_memaccess;
	else
		ctx->measure_jitter = jent_measure_jitter;

	/*
	 * Allocate the collector without the startup entropy collection and
	 * its health-test reset ladder (mirroring the userspace recording
	 * tools): the startup could silently escalate OSR, memory size and
	 * hash loop count, but the recorded raw data must correspond exactly
	 * to the requested testing_osr/testing_flags.
	 */
	ctx->ec = jent_entropy_collector_alloc_raw(osr, flags);
	if (!ctx->ec) {
		mutex_unlock(&jent_testing_read_lock);
		/*
		 * The allocation also fails on invalid parameters or a failed
		 * power-up self test, not only on memory shortage.
		 */
		pr_warn("jitterentropy: raw entropy collector allocation failed (out of memory, invalid testing_osr/testing_flags or self-test failure)\n");
		kvfree(ctx);
		return -ENOMEM;
	}

	/*
	 * Match the userspace recording tools: enable the full SP800-90B
	 * health test handling while recording.
	 */
	ctx->ec->is_fips_enabled = 1;

	jent_testing_log(ctx->ec);
	mutex_unlock(&jent_testing_read_lock);

	file->private_data = ctx;

	return 0;
}

static int jent_testing_release(struct inode *inode, struct file *file)
{
	struct jent_testing_ctx *ctx = file->private_data;

	if (!ctx)
		return 0;

	jent_entropy_collector_free(ctx->ec);
	kvfree(ctx);
	file->private_data = NULL;

	return 0;
}

static ssize_t jent_testing_extract_user(struct file *file, char __user *buf,
					 size_t nbytes, loff_t *ppos)
{
	struct jent_testing_ctx *ctx = file->private_data;
	struct rand_data *ec = ctx->ec;
	u64 *tmp = NULL;
	u64 loop_cnt;
	ssize_t ret = 0;
	int large_request = (nbytes > 256);

	unsigned int (*measure_jitter)(struct rand_data *ec,
				       uint64_t loop_cnt,
				       uint64_t *ret_current_delta) =
		ctx->measure_jitter;

	if (!nbytes)
		return 0;

	/* Only one extract session may run at a time. */
	if (mutex_lock_interruptible(&jent_testing_read_lock))
		return -ERESTARTSYS;

	/*
	 * Snapshot the JENT_IOCLOOPCNT setting under the lock: the whole
	 * extract session records with one consistent loop count.
	 */
	loop_cnt = ctx->loop_cnt;

	/*
	 * The intention of this interface is for collecting at least
	 * 1000 samples due to the SP800-90B requirements. With kvmalloc
	 * the allocation of such a larger chunk is no longer an issue.
	 */
#define JENT_TESTING_SAMPLES	1000
#define JENT_TESTING_DATA_SIZE	(JENT_TESTING_SAMPLES * sizeof(u64))
	tmp = kvmalloc(JENT_TESTING_DATA_SIZE, GFP_KERNEL);
	if (!tmp) {
		mutex_unlock(&jent_testing_read_lock);
		return -ENOMEM;
	}

	while (nbytes >= sizeof(u64)) {
		u32 samples = (u32)min_t(size_t, nbytes / sizeof(u64),
					 JENT_TESTING_SAMPLES);
		size_t len = samples * sizeof(u64);
		size_t not_copied;
		u32 i;

		/* Honor pending signals regardless of the resched state. */
		if (signal_pending(current)) {
			if (ret == 0)
				ret = -ERESTARTSYS;
			break;
		}

		/* Be cooperative for large requests. */
		if (large_request && need_resched())
			schedule();

		/*
		 * Prime the common measurement (initialize ec->prev_time) so
		 * the first recorded delta is not computed from a stale time
		 * stamp (unprimed instance or the gap spent in copy_to_user()
		 * between two rounds). The NTG.1 hash-loop and memory-access
		 * variants prime themselves and need no separate priming.
		 * The priming uses the configured loop count (loop_cnt 0),
		 * mirroring the userspace recording tools.
		 */
		if (measure_jitter == jent_measure_jitter)
			jent_measure_jitter(ec, 0, NULL);

		for (i = 0; i < samples; i++) {
			/* Disregard stuck indicator */
			measure_jitter(ec, loop_cnt, &tmp[i]);
		}

		not_copied = copy_to_user(buf, tmp, len);

		/* Advance by what was actually copied out. */
		len -= not_copied;
		nbytes -= len;
		buf += len;
		ret += len;

		/*
		 * A short copy means copy_to_user() faulted part-way into the
		 * user buffer; return the short read instead of retrying (and
		 * over-reporting the data actually delivered).
		 */
		if (not_copied) {
			if (ret == 0)
				ret = -EFAULT;
			break;
		}
	}

	if (ret > 0)
		*ppos += ret;

	kvfree_sensitive(tmp, JENT_TESTING_DATA_SIZE);
	mutex_unlock(&jent_testing_read_lock);
	return ret;
}

/*
 * Serialize the JSON status string of the per-open Jitter RNG instance into a
 * user-provided buffer. Same ABI and semantics as the JENT_IOCSTATUS handler
 * of the character device (see jitterentropy_chardev.c).
 *
 * The status is derived from mutable collector state (health test and output
 * accounting), so the extract-session lock is held while jent_status() runs
 * to keep a concurrent extract session from mutating it mid-serialization.
 * Unlike the character device, the collector is never reallocated during the
 * lifetime of the open file, so ctx->ec itself is stable.
 */
static long jent_testing_ioctl_status(struct jent_testing_ctx *ctx,
				      void __user *arg)
{
	struct jent_status_ioctl status;
	char *buf;
	size_t slen;
	long ret;

	if (copy_from_user(&status, arg, sizeof(status)))
		return -EFAULT;

	buf = kvzalloc(JENT_STATUS_MAX_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	if (mutex_lock_interruptible(&jent_testing_read_lock)) {
		ret = -ERESTARTSYS;
		goto out;
	}

	ret = jent_status(ctx->ec, buf, JENT_STATUS_MAX_LEN);
	mutex_unlock(&jent_testing_read_lock);

	if (ret) {
		ret = -EIO;
		goto out;
	}

	/* Number of bytes to copy out, including the terminating NUL. */
	slen = strlen(buf) + 1;

	if (status.length < slen) {
		/* Buffer too small: report the required size to userspace. */
		status.length = slen;
		if (copy_to_user(arg, &status, sizeof(status)))
			ret = -EFAULT;
		else
			ret = -EOVERFLOW;
		goto out;
	}

	if (copy_to_user(u64_to_user_ptr(status.buf), buf, slen)) {
		ret = -EFAULT;
		goto out;
	}

	status.length = slen;
	if (copy_to_user(arg, &status, sizeof(status))) {
		ret = -EFAULT;
		goto out;
	}

	ret = 0;

out:
	kvfree(buf);
	return ret;
}

/*
 * Set the loop count applied to the raw noise measurements of this open
 * instance (see the loop_cnt member of struct jent_testing_ctx). Taking the
 * extract-session lock defers the update until a running extract session has
 * finished.
 */
static long jent_testing_ioctl_loopcnt(struct jent_testing_ctx *ctx,
				       void __user *arg)
{
	u64 loop_cnt;

	if (copy_from_user(&loop_cnt, arg, sizeof(loop_cnt)))
		return -EFAULT;

	/* Mirror the bound of the userspace recording tools. */
	if (loop_cnt > UINT_MAX)
		return -EINVAL;

	if (mutex_lock_interruptible(&jent_testing_read_lock))
		return -ERESTARTSYS;
	ctx->loop_cnt = loop_cnt;
	mutex_unlock(&jent_testing_read_lock);

	return 0;
}

static long jent_testing_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct jent_testing_ctx *ctx = file->private_data;

	if (!ctx)
		return -EFAULT;

	switch (cmd) {
	case JENT_IOCSTATUS:
		return jent_testing_ioctl_status(ctx, (void __user *)arg);
	case JENT_IOCLOOPCNT:
		return jent_testing_ioctl_loopcnt(ctx, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations jent_raw_hires_fops = {
	.owner = THIS_MODULE,
	.open = jent_testing_open,
	.release = jent_testing_release,
	.read = jent_testing_extract_user,
	.unlocked_ioctl = jent_testing_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

/******************************* Initialization *******************************/

void __init jent_testing_init(void)
{
	jent_raw_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);

	debugfs_create_file_unsafe("jent_raw_hires", 0400,
				   jent_raw_debugfs_root, NULL,
				   &jent_raw_hires_fops);
}
EXPORT_SYMBOL(jent_testing_init);

void jent_testing_exit(void)
{
	debugfs_remove_recursive(jent_raw_debugfs_root);
}
EXPORT_SYMBOL(jent_testing_exit);
