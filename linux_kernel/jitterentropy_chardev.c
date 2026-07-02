/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Character device interface for Jitter RNG.
 *
 * This registers a misc character device (/dev/jitterentropy). For every
 * open() a dedicated Jitter RNG entropy collector is allocated; it is freed
 * again on the matching release(). read() delivers entropy bytes obtained
 * from that per-open instance.
 *
 * The whole interface can be disabled at compile time by not setting the
 * CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV configuration option (see
 * Kbuild.config). In that case the stubs in jitterentropy_chardev.h are used.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "jitterentropy.h"
#include "jitterentropy_chardev.h"
#include "jitterentropy_error.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_uapi.h"

/*
 * The OSR and flags used to allocate the per-open Jitter RNG instances are
 * shared with the crypto API interface and are configurable via the module
 * parameters of the same name (see jitterentropy_kcapi.c).
 */
extern unsigned int osr;
extern int flags;

/* Largest chunk handed to jent_read_entropy_safe() in one iteration. */
#define JENT_CHARDEV_READ_BUF_SIZE 256

/*
 * Subdirectory /proc/jitter_rng/instances holding one status file per open
 * character-device instance. NULL if procfs is unavailable.
 */
static struct proc_dir_entry *jent_chardev_proc_dir;
#define JENT_CHARDEV_PROC_DIRNAME "instances"

/* Per-open state: each open() gets its own Jitter RNG instance. */
struct jent_chardev_ctx {
	struct mutex lock;
	struct rand_data *entropy_collector;
	/* Per-instance status file /proc/jitter_rng/instances/<uuid>. */
	struct proc_dir_entry *proc;
};

/*
 * Emit the JSON status string of a single open instance, exported read-only as
 * /proc/jitter_rng/instances/<id>. Holds the instance lock (as read()/ioctl()
 * do) so the collector cannot be reallocated on health-test recovery while
 * jent_status() runs.
 */
static int jent_chardev_instance_status_show(struct seq_file *m, void *v)
{
	struct jent_chardev_ctx *ctx = m->private;
	char *buf;
	int ret;

	buf = kvzalloc(JENT_STATUS_MAX_LEN, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	mutex_lock(&ctx->lock);
	if (ctx->entropy_collector)
		ret = jent_status(ctx->entropy_collector, buf,
				  JENT_STATUS_MAX_LEN);
	else
		ret = -1;
	mutex_unlock(&ctx->lock);

	if (ret) {
		kvfree(buf);
		return -EIO;
	}

	seq_puts(m, buf);
	kvfree(buf);
	return 0;
}

/*
 * Create the per-instance status file, named by the instance UUID (stable for
 * the instance's lifetime, so the name stays valid across a collector
 * reallocation). Non-fatal: reads still work without it.
 */
static void jent_chardev_instance_proc_create(struct jent_chardev_ctx *ctx)
{
	char name[JENT_UUID_STRLEN];

	if (!jent_chardev_proc_dir)
		return;

	if (jent_uuid(ctx->entropy_collector, name, sizeof(name)))
		return;

	ctx->proc = proc_create_single_data(name, 0444, jent_chardev_proc_dir,
					    jent_chardev_instance_status_show,
					    ctx);
	if (!ctx->proc)
		pr_warn("jitterentropy: failed to create /proc/%s/%s/%s\n",
			JENT_PROC_DIRNAME, JENT_CHARDEV_PROC_DIRNAME, name);
}

static int jent_chardev_open(struct inode *inode, struct file *file)
{
	struct jent_chardev_ctx *ctx;

	ctx = kvzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_init(&ctx->lock);

	ctx->entropy_collector = jent_entropy_collector_alloc(osr, flags);
	if (!ctx->entropy_collector) {
		mutex_destroy(&ctx->lock);
		kvfree(ctx);
		return -ENOMEM;
	}

	file->private_data = ctx;

	/*
	 * Account for this instance in /proc/jitter_rng/statistics and publish
	 * its status under /proc/jitter_rng/instances/<uuid>.
	 */
	jent_proc_instance_inc();
	jent_chardev_instance_proc_create(ctx);

	return 0;
}

static int jent_chardev_release(struct inode *inode, struct file *file)
{
	struct jent_chardev_ctx *ctx = file->private_data;

	if (!ctx)
		return 0;

	/*
	 * Remove the status file first: proc_remove() waits for any in-flight
	 * reader to leave jent_chardev_instance_status_show() before returning,
	 * so the context and collector can then be freed without racing it.
	 */
	proc_remove(ctx->proc);

	if (ctx->entropy_collector)
		jent_entropy_collector_free(ctx->entropy_collector);
	mutex_destroy(&ctx->lock);
	kvfree(ctx);
	file->private_data = NULL;

	jent_proc_instance_dec();

	return 0;
}

static ssize_t jent_chardev_read(struct file *file, char __user *buf,
				 size_t nbytes, loff_t *ppos)
{
	struct jent_chardev_ctx *ctx = file->private_data;
	u8 *tmp;
	ssize_t ret = 0;

	if (!ctx)
		return -EFAULT;

	if (!nbytes)
		return 0;

	tmp = kvmalloc(JENT_CHARDEV_READ_BUF_SIZE, GFP_KERNEL);
	if (!tmp)
		return -ENOMEM;

	if (mutex_lock_interruptible(&ctx->lock)) {
		ret = -ERESTARTSYS;
		goto out;
	}

	while (nbytes) {
		size_t towork = min_t(size_t, nbytes,
				      JENT_CHARDEV_READ_BUF_SIZE);
		ssize_t rc;

		/*
		 * jent_read_entropy_safe() reallocates the collector on
		 * intermittent health-test failures, hence the indirection.
		 * It returns the number of generated bytes (== towork) or a
		 * negative error code on a permanent/generic failure.
		 */
		rc = jent_read_entropy_safe(&ctx->entropy_collector, tmp,
					    towork);
		if (rc < 0) {
			/* Map the error; may panic under FIPS. */
			int err = jent_map_read_error(rc);

			if (ret == 0)
				ret = err;
			break;
		}

		if (copy_to_user(buf, tmp, rc)) {
			if (ret == 0)
				ret = -EFAULT;
			break;
		}

		nbytes -= rc;
		buf += rc;
		ret += rc;

		/* Honor pending signals regardless of the resched state. */
		if (signal_pending(current)) {
			if (ret == 0)
				ret = -ERESTARTSYS;
			break;
		}

		/* Be cooperative for large requests. */
		if (need_resched())
			cond_resched();
	}

	mutex_unlock(&ctx->lock);

out:
	kvfree_sensitive(tmp, JENT_CHARDEV_READ_BUF_SIZE);
	return ret;
}

/*
 * Serialize the JSON status string of the per-open Jitter RNG instance into a
 * user-provided buffer.
 *
 * The Jitter RNG status is derived from the state of the entropy collector, so
 * the collector lock is held while jent_status() runs: the read path may
 * reallocate the collector on health-test recovery, and this prevents a
 * concurrent read() from freeing it underneath us.
 */
static long jent_chardev_ioctl_status(struct jent_chardev_ctx *ctx,
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

	if (mutex_lock_interruptible(&ctx->lock)) {
		ret = -ERESTARTSYS;
		goto out;
	}

	/*
	 * Without a collector, jent_status() would emit a version-only JSON
	 * stub; report an error instead, matching the per-instance proc file.
	 */
	if (ctx->entropy_collector)
		ret = jent_status(ctx->entropy_collector, buf,
				  JENT_STATUS_MAX_LEN);
	else
		ret = -1;
	mutex_unlock(&ctx->lock);

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

static long jent_chardev_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct jent_chardev_ctx *ctx = file->private_data;

	if (!ctx)
		return -EFAULT;

	switch (cmd) {
	case JENT_IOCSTATUS:
		return jent_chardev_ioctl_status(ctx, (void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations jent_chardev_fops = {
	.owner		= THIS_MODULE,
	.open		= jent_chardev_open,
	.release	= jent_chardev_release,
	.read		= jent_chardev_read,
	.unlocked_ioctl	= jent_chardev_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static struct miscdevice jent_chardev_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "jitterentropy",
	.fops	= &jent_chardev_fops,
	.mode	= 0444,
};

int jent_chardev_init(void)
{
	int ret = misc_register(&jent_chardev_misc);

	if (ret) {
		pr_err("jitterentropy: failed to register character device: %d\n",
		       ret);
		return ret;
	}

	pr_info("jitterentropy: character device /dev/%s registered\n",
		jent_chardev_misc.name);

	/*
	 * Non-fatal: the device works without the per-instance status export
	 * (and jent_proc_dir is NULL without CONFIG_PROC_FS).
	 */
	if (jent_proc_dir) {
		jent_chardev_proc_dir = proc_mkdir(JENT_CHARDEV_PROC_DIRNAME,
						   jent_proc_dir);
		if (!jent_chardev_proc_dir)
			pr_warn("jitterentropy: failed to create /proc/%s/%s\n",
				JENT_PROC_DIRNAME, JENT_CHARDEV_PROC_DIRNAME);
	}

	return 0;
}

void jent_chardev_exit(void)
{
	/*
	 * The misc device's fops hold a module reference for every open file, so
	 * the module cannot be unloaded while instances exist. By the time this
	 * runs there are therefore no per-instance files left below the
	 * directory, and removing it cannot race a release().
	 */
	misc_deregister(&jent_chardev_misc);

	proc_remove(jent_chardev_proc_dir);
	jent_chardev_proc_dir = NULL;
}
