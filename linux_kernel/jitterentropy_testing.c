/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test interface for Jitter RNG.
 *
 * The debugfs file jent_raw_hires provides the raw noise data of the Jitter
 * RNG: each read drives the measure_jitter operation of a dedicated Jitter RNG
 * instance and returns one u64 per measurement holding the time delta as
 * consumed by the health tests and the entropy pool (i.e. including the
 * division by the common timer GCD). This mirrors the user space recording
 * logic in tests/raw-entropy/recording_userspace/jitterentropy-hashtime.c.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

#include <linux/debugfs.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/uaccess.h>

#include "jitterentropy-internal.h"
#include "jitterentropy-noise.h"
#include "jitterentropy.h"
#include "jitterentropy_testing.h"

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
 * testing with. The module_param below allwos them to be also updated while
 * the module is inserted. As for each new read operation to get test data
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

static int jent_testing_log(struct rand_data *ec)
{
	char *buf;
	int ret;

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

#define JENT_STATUS_BUF_SIZE 4096
	buf = kvzalloc(JENT_STATUS_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = jent_status(ec, buf, JENT_STATUS_BUF_SIZE);
	if (ret < 0)
		goto err;

	pr_notice_ratelimited("%s\n", buf);

err:
	kvfree(buf);
	return ret;
}

/************** Raw High-Resolution Timer Entropy Data Handling **************/

static ssize_t jent_testing_extract_user(struct file *file, char __user *buf,
					 size_t nbytes, loff_t *ppos)
{
	struct rand_data *ec = NULL;
	u64 *tmp = NULL;
	ssize_t ret = 0;
	int large_request = (nbytes > 256);

	unsigned int (*measure_jitter)(struct rand_data *ec,
				       uint64_t loop_cnt,
				       uint64_t *ret_current_delta);

	if (testing_flags & (JENT_TEST_HASHLOOP))
		measure_jitter = jent_measure_jitter_ntg1_sha3;
	else if (testing_flags & (JENT_TEST_MEMACCLOOP))
		measure_jitter = jent_measure_jitter_ntg1_memaccess;
	else
		measure_jitter = jent_measure_jitter;

	if (!nbytes)
		return 0;

	/* Only one extract session may run at a time. */
	if (mutex_lock_interruptible(&jent_testing_read_lock))
		return -ERESTARTSYS;

	/*
	 * Allocate the collector without the startup entropy collection and
	 * its health-test reset ladder (mirroring the userspace recording
	 * tools): the startup could silently escalate OSR, memory size and
	 * hash loop count, but the recorded raw data must correspond exactly
	 * to the requested testing_osr/testing_flags.
	 */
	ec = jent_entropy_collector_alloc_raw(testing_osr, testing_flags);
	if (!ec) {
		/*
		 * The allocation also fails on invalid parameters or a failed
		 * power-up self test, not only on memory shortage.
		 */
		pr_warn("jitterentropy: raw entropy collector allocation failed (out of memory, invalid testing_osr/testing_flags or self-test failure)\n");
		ret = -ENOMEM;
		goto out;
	}

	/*
	 * Match the userspace recording tools: enable the full SP800-90B
	 * health test handling while recording.
	 */
	ec->is_fips_enabled = 1;

	jent_testing_log(ec);

	/*
	 * The intention of this interface is for collecting at least
	 * 1000 samples due to the SP800-90B requirements. With kvmalloc
	 * the allocation of such a larger chunk is no longer an issue.
	 */
#define JENT_TESTING_SAMPLES	1000
#define JENT_TESTING_DATA_SIZE	(JENT_TESTING_SAMPLES * sizeof(u64))
	tmp = kvmalloc(JENT_TESTING_DATA_SIZE, GFP_KERNEL);
	if (!tmp) {
		ret = -ENOMEM;
		goto out;
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
		 */
		if (measure_jitter == jent_measure_jitter)
			jent_measure_jitter(ec, 0, NULL);

		for (i = 0; i < samples; i++) {
			/* Disregard stuck indicator */
			measure_jitter(ec, 0, &tmp[i]);
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

out:
	if (ec)
		jent_entropy_collector_free(ec);
	if (tmp)
		kvfree_sensitive(tmp, JENT_TESTING_DATA_SIZE);
	mutex_unlock(&jent_testing_read_lock);
	return ret;
}

static const struct file_operations jent_raw_hires_fops = {
	.owner = THIS_MODULE,
	.read = jent_testing_extract_user,
};

/******************************* Initialization *******************************/

void jent_testing_init(void)
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
