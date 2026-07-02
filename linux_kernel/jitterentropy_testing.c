/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test interface for Jitter RNG.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 */

#include <crypto/rng.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/uaccess.h>

#include "jitterentropy-noise.h"
#include "jitterentropy.h"

#define JENT_TEST_RINGBUFFER_SIZE	(1<<10)
#define JENT_TEST_RINGBUFFER_MASK	(JENT_TEST_RINGBUFFER_SIZE - 1)

#define JENT_TEST_COMMON (1<<14)
#define JENT_TEST_HASHLOOP (1<<15)
#define JENT_TEST_MEMACCLOOP (1<<16)

struct jent_testing {
	u64 jent_testing_rb[JENT_TEST_RINGBUFFER_SIZE];
	u32 rb_reader;
	atomic_t rb_writer;
	atomic_t jent_testing_enabled;
	spinlock_t lock;
	wait_queue_head_t read_wait;
};

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

/*************************** Generic Data Handling ****************************/

/*
 * boot variable:
 * 0 ==> No boot test, gathering of runtime data allowed
 * 1 ==> Boot test enabled and ready for collecting data, gathering runtime
 *	 data is disabled
 * 2 ==> Boot test completed and disabled, gathering of runtime data is
 *	 disabled
 */

static void jent_testing_reset(struct jent_testing *data)
{
	unsigned long lock_flags;

	spin_lock_irqsave(&data->lock, lock_flags);
	data->rb_reader = 0;
	atomic_set(&data->rb_writer, 0);
	spin_unlock_irqrestore(&data->lock, lock_flags);
}

static void jent_testing_data_init(struct jent_testing *data, u32 boot)
{
	/*
	 * The boot time testing implies we have a running test. If the
	 * caller wants to clear it, he has to unset the boot_test flag
	 * at runtime via sysfs to enable regular runtime testing
	 */
	if (boot)
		return;

	jent_testing_reset(data);
	atomic_set(&data->jent_testing_enabled, 1);
	//pr_warn("Enabling data collection\n");
}

static void jent_testing_fini(struct jent_testing *data, u32 boot)
{
	/* If we have boot data, we do not reset yet to allow data to be read */
	if (boot)
		return;

	atomic_set(&data->jent_testing_enabled, 0);
	jent_testing_reset(data);
	//pr_warn("Disabling data collection\n");
}

static bool jent_testing_store(struct jent_testing *data, u64 value,
			       u32 *boot)
{
	unsigned long lock_flags;

	if (!atomic_read(&data->jent_testing_enabled) && (*boot != 1))
		return false;

	spin_lock_irqsave(&data->lock, lock_flags);

	/*
	 * Disable entropy testing for boot time testing after ring buffer
	 * is filled.
	 */
	if (*boot) {
		if (((u32)atomic_read(&data->rb_writer)) >
		     JENT_TEST_RINGBUFFER_SIZE) {
			*boot = 2;
			pr_warn_once("One time data collection test disabled\n");
			spin_unlock_irqrestore(&data->lock, lock_flags);
			return false;
		}

		if (atomic_read(&data->rb_writer) == 1)
			pr_warn("One time data collection test enabled\n");
	}

	data->jent_testing_rb[((u32)atomic_read(&data->rb_writer)) &
			      JENT_TEST_RINGBUFFER_MASK] = value;
	atomic_inc(&data->rb_writer);

	/* If writer starts to overtake reader, push the reader */
	if ((u32)atomic_read(&data->rb_writer) == data->rb_reader)
		data->rb_reader++;

	spin_unlock_irqrestore(&data->lock, lock_flags);

	if (wq_has_sleeper(&data->read_wait))
		wake_up_interruptible(&data->read_wait);

	return true;
}

static bool jent_testing_have_data(struct jent_testing *data)
{
	return ((((u32)atomic_read(&data->rb_writer)) &
		 JENT_TEST_RINGBUFFER_MASK) !=
		 (data->rb_reader & JENT_TEST_RINGBUFFER_MASK));
}

static int jent_testing_reader(struct jent_testing *data, u32 *boot,
			       u8 *outbuf, u32 outbuflen)
{
	unsigned long lock_flags;
	int collected_data = 0;

	while (outbuflen) {
		u32 writer = (u32)atomic_read(&data->rb_writer);

		spin_lock_irqsave(&data->lock, lock_flags);

		/* We have no data or reached the writer. */
		if (!writer || (writer == data->rb_reader)) {

			spin_unlock_irqrestore(&data->lock, lock_flags);

			/*
			 * Now we gathered all boot data, enable regular data
			 * collection.
			 */
			if (*boot) {
				*boot = 0;
				goto out;
			}

			wait_event_interruptible(data->read_wait,
						 jent_testing_have_data(data));
			if (signal_pending(current)) {
				collected_data = -ERESTARTSYS;
				goto out;
			}

			continue;
		}

		/* We copy out word-wise */
		if (outbuflen < sizeof(u64)) {
			spin_unlock_irqrestore(&data->lock, lock_flags);
			goto out;
		}

		memcpy(outbuf, &data->jent_testing_rb[data->rb_reader &
						      JENT_TEST_RINGBUFFER_MASK],
		       sizeof(u64));

		data->rb_reader++;

		spin_unlock_irqrestore(&data->lock, lock_flags);

		outbuf += sizeof(u64);
		outbuflen -= sizeof(u64);
		collected_data += sizeof(u64);
	}

out:
	return collected_data;
}

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

#define JENT_STATUS_BUF_SIZE 1000
	buf = kzalloc(JENT_STATUS_BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = jent_status(ec, buf, JENT_STATUS_BUF_SIZE);
	if (ret < 0)
		goto err;

	pr_notice_ratelimited("%s\n", buf);

err:
	kfree(buf);
	return ret;
}

static ssize_t jent_testing_extract_user(
	struct file *file, char __user *buf, size_t nbytes, loff_t *ppos,
	struct jent_testing *data, u32 *boot,
	int (*reader)(u8 *outbuf, u32 outbuflen))
{
	struct rand_data *ec = NULL;
	ssize_t ret = 0;
	int large_request = (nbytes > 256);
	u8 *tmp = NULL, *tmp_aligned;
	u8 random[32];

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

	ec = jent_entropy_collector_alloc(testing_osr, testing_flags);
	if (!ec) {
		ret = -ENOMEM;
		goto out;
	}

	jent_testing_log(ec);

	/*
	 * The intention of this interface is for collecting at least
	 * 1000 samples due to the SP800-90B requirements. However, due to
	 * memory and performance constraints, it is not desirable to allocate
	 * 8000 bytes of memory. Instead, we allocate space for only 125
	 * samples, which will allow the user to collect all 1000 samples using
	 * 8 calls to this interface.
	 */
#define JENT_TESTING_READ_BUF_SIZE (125 * sizeof(u64) + sizeof(u64))
	tmp = kmalloc(JENT_TESTING_READ_BUF_SIZE, GFP_KERNEL);
	if (!tmp) {
		ret = -ENOMEM;
		goto out;
	}

	tmp_aligned = PTR_ALIGN(tmp, sizeof(u64));

	while (nbytes) {
		loff_t ppos = 0;
		ssize_t count;
		u32 i;

		if (large_request && need_resched()) {
			if (signal_pending(current)) {
				if (ret == 0)
					ret = -ERESTARTSYS;
				break;
			}
			schedule();
		}

		i = min_t(u32, nbytes, JENT_TESTING_READ_BUF_SIZE);

		/* Enable recording of data */
		jent_testing_data_init(data, *boot);

		/*
		 * Trigger the Jitter RNG to collect data. The timing
		 * information is then recorded here.
		 */
		if (testing_flags & (JENT_TEST_COMMON | JENT_TEST_HASHLOOP |
				     JENT_TEST_MEMACCLOOP )) {
			unsigned int ctr;

			/* Prime */
			if (measure_jitter == jent_measure_jitter)
				jent_measure_jitter(ec, 0, NULL);
			for (ctr = 0; ctr < i / sizeof(u64); ctr++) {
				/* Disregard stuck indicator */
				measure_jitter(ec, 0, NULL);
			}

		} else {
			int rc = jent_read_entropy(ec, random, sizeof(random));
			if (rc < 0) {
				jent_testing_fini(data, *boot);
				ret = -EFAULT;
				goto out;
			}
		}

		/* Read the data into our buffer */
		i = reader(tmp_aligned, i);

		/* Disable recording */
		jent_testing_fini(data, *boot);

		if (i <= 0) {
			if (i < 0)
				ret = i;
			break;
		}

		count = simple_read_from_buffer(buf, i, &ppos, tmp_aligned,
						JENT_TESTING_READ_BUF_SIZE);
		if (count < 0) {
			ret = count;
			goto out;
		}

		nbytes -= i;
		buf += i;
		ret += i;
	}

	if (ret > 0)
		*ppos += ret;

out:
	if (ec)
		jent_entropy_collector_free(ec);
	if (tmp)
		kfree_sensitive(tmp);
	return ret;
}

/************** Raw High-Resolution Timer Entropy Data Handling **************/

static u32 boot_raw_hires_test = 0;
module_param(boot_raw_hires_test, uint, 0644);
MODULE_PARM_DESC(boot_raw_hires_test,
		 "Enable gathering boot time high resolution timer entropy of the first Jitter RNG entropy events");

static struct jent_testing jent_raw_hires = {
	.rb_reader = 0,
	.rb_writer = ATOMIC_INIT(0),
	.lock      = __SPIN_LOCK_UNLOCKED(jent_raw_hires.lock),
	.read_wait = __WAIT_QUEUE_HEAD_INITIALIZER(jent_raw_hires.read_wait)
};

int jent_raw_hires_entropy_store(__u64 value)
{
	return jent_testing_store(&jent_raw_hires, value, &boot_raw_hires_test);
}
EXPORT_SYMBOL(jent_raw_hires_entropy_store);

static int jent_raw_hires_entropy_reader(u8 *outbuf, u32 outbuflen)
{
	return jent_testing_reader(&jent_raw_hires, &boot_raw_hires_test,
				   outbuf, outbuflen);
}

static ssize_t jent_raw_hires_read(struct file *file, char __user *to,
				   size_t count, loff_t *ppos)
{
	return jent_testing_extract_user(file, to, count, ppos,
					 &jent_raw_hires, &boot_raw_hires_test,
					 jent_raw_hires_entropy_reader);
}

static const struct file_operations jent_raw_hires_fops = {
	.owner = THIS_MODULE,
	.read = jent_raw_hires_read,
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
