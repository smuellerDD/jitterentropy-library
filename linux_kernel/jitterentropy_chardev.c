/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Character device interface for Jitter RNG.
 *
 * This registers a misc character device (/dev/jitterentropy). For every
 * open() a dedicated Jitter RNG entropy collector is allocated; it is freed
 * again on the matching release(). read() delivers entropy bytes obtained
 * from that per-open instance. O_NONBLOCK reads return -EAGAIN instead of
 * waiting for a concurrent reader of the same instance, are capped at one
 * internal buffer (a short read) per call, and leave the recovery from an
 * intermittent health test failure to the next blocking read.
 *
 * The whole interface can be disabled at compile time by not setting the
 * CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV configuration option (see
 * Kbuild.config). In that case the stubs in jitterentropy_chardev.h are used.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "jitterentropy.h"
#include "jitterentropy_chardev.h"
#include "jitterentropy_error.h"
#include "jitterentropy_ioctl.h"
#include "jitterentropy_proc.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_status.h"
#include "jitterentropy_uapi.h"

/*
 * The OSR and flags used to allocate the per-open Jitter RNG instances are
 * shared with the crypto API interface and are configurable via the module
 * parameters of the same name (see jitterentropy_mod.c).
 */
extern unsigned int jent_osr;
extern unsigned int jent_flags;

/*
 * Largest chunk handed to jent_read_entropy_safe() in one iteration. The
 * instance lock is only held per chunk, so this also bounds how long a single
 * reader can stall other users (readers, ioctl, procfs) of the same instance.
 */
#define JENT_CHARDEV_READ_BUF_SIZE 32

/*
 * Concurrent open instances allowed an unprivileged caller, 0 for unlimited.
 * Every open allocates a collector of hundreds of kB (up to 512 MB with
 * JENT_CACHE_ALL), so the world-readable device needs a bound. It is global,
 * so one caller can hold every slot and keep others out (ENFILE); restrict the
 * device to a group, or set 0 and rely on the memory cgroup accounting, where
 * that matters.
 */
static unsigned int max_instances = 256;
module_param(max_instances, uint, S_IRUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(max_instances,
		 "Maximum concurrent unprivileged /dev/jitterentropy instances (0: unlimited)");

static bool jent_chardev_instance_get(void)
{
	if (jent_proc_instance_inc(max_instances))
		return true;

	/*
	 * Exempt, so a device filled by unprivileged callers stays usable. Only
	 * asked once the limit would refuse, so an open that fits neither logs
	 * an LSM audit denial nor marks the caller PF_SUPERPRIV.
	 */
	return capable(CAP_SYS_RESOURCE) && jent_proc_instance_inc(0);
}

static void jent_chardev_instance_put(void)
{
	jent_proc_instance_dec();
}

/*
 * Subdirectory /proc/jitterentropy/instances holding one status file per open
 * character-device instance. NULL if procfs is unavailable.
 */
static struct proc_dir_entry *jent_chardev_proc_dir;
#define JENT_CHARDEV_PROC_DIRNAME "instances"

/* Per-open state: each open() gets its own Jitter RNG instance. */
struct jent_chardev_ctx {
	struct mutex lock;
	struct rand_data *entropy_collector;
	/* Per-instance status file /proc/jitterentropy/instances/<uuid>. */
	struct proc_dir_entry *proc;
	/* This instance's own periodic self test. */
	struct jent_selftest_instance selftest;
};

/*
 * Emit the JSON status string of a single open instance, exported read-only as
 * /proc/jitterentropy/instances/<id>. The shared renderer holds the instance
 * lock (as read()/ioctl() do) so the collector cannot be reallocated on
 * health-test recovery while jent_status() runs. __maybe_unused: without
 * CONFIG_PROC_FS proc_create_single_data() discards it.
 */
static int __maybe_unused jent_chardev_instance_status_show(struct seq_file *m,
							    void *v)
{
	struct jent_chardev_ctx *ctx = m->private;

	return jent_status_seq_show(m, &ctx->lock, &ctx->entropy_collector);
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

	if (jent_entropy_collector_uuid(ctx->entropy_collector, name,
					sizeof(name)))
		return;

	ctx->proc = proc_create_single_data(name, 0400, jent_chardev_proc_dir,
					    jent_chardev_instance_status_show,
					    ctx);
	if (!ctx->proc)
		pr_warn("jitterentropy: failed to create /proc/%s/%s/%s\n",
			JENT_PROC_DIRNAME, JENT_CHARDEV_PROC_DIRNAME, name);
}

static int jent_chardev_open(struct inode *inode, struct file *file)
{
	struct jent_chardev_ctx *ctx;

	if (!jent_chardev_instance_get())
		return -ENFILE;

	ctx = kvzalloc(sizeof(*ctx), GFP_KERNEL_ACCOUNT);
	if (!ctx) {
		jent_chardev_instance_put();
		return -ENOMEM;
	}

	mutex_init(&ctx->lock);

	ctx->entropy_collector =
		jent_entropy_collector_alloc(jent_osr, jent_flags);
	if (!ctx->entropy_collector) {
		/*
		 * The allocation also fails when the collector's startup gives
		 * up - its health tests exhausted the oversampling rates on a
		 * poor clock - which the NULL does not tell apart from memory
		 * running out. Say so, rather than leave it to the ENOMEM.
		 */
		pr_warn_ratelimited("jitterentropy: no entropy collector for /dev/jitterentropy: its startup failed or memory ran out\n");
		mutex_destroy(&ctx->lock);
		kvfree(ctx);
		jent_chardev_instance_put();
		return -ENOMEM;
	}

	file->private_data = ctx;

	jent_selftest_instance_init(&ctx->selftest, &ctx->lock,
				    &ctx->entropy_collector);

	/* Publish its status under /proc/jitterentropy/instances/<uuid>. */
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

	/* Likewise before the free; waits for a self test run in progress. */
	jent_selftest_instance_exit(&ctx->selftest);

	if (ctx->entropy_collector)
		jent_entropy_collector_free(ctx->entropy_collector);
	mutex_destroy(&ctx->lock);
	kvfree(ctx);
	file->private_data = NULL;

	jent_chardev_instance_put();

	return 0;
}

static ssize_t jent_chardev_read(struct file *file, char __user *buf,
				 size_t nbytes, loff_t *ppos)
{
	struct jent_chardev_ctx *ctx = file->private_data;
	/*
	 * Small and fixed (see JENT_CHARDEV_READ_BUF_SIZE), and this is a
	 * read(2) handler called straight from vfs_read(), so the frame it
	 * adds is affordable and an allocation per read is not worth its error
	 * path. Wiped before returning, as the kvfree_sensitive() it replaces
	 * did.
	 */
	u8 tmp[JENT_CHARDEV_READ_BUF_SIZE];
	ssize_t ret = 0;

	if (!ctx)
		return -EFAULT;

	if (!nbytes)
		return 0;

	/*
	 * A non-blocking reader must not sleep on the instance lock and is
	 * capped at one buffer so the time spent generating stays bounded.
	 * Userspace observes an ordinary short read and is expected to retry.
	 */
	if (file->f_flags & O_NONBLOCK)
		nbytes = min_t(size_t, nbytes, JENT_CHARDEV_READ_BUF_SIZE);

	while (nbytes) {
		size_t towork = min_t(size_t, nbytes,
				      JENT_CHARDEV_READ_BUF_SIZE);
		bool nonblock = file->f_flags & O_NONBLOCK;
		ssize_t rc;

		/*
		 * Jitter entropy collection is CPU-bound and slow. Take the
		 * lock per chunk so a large request cannot monopolize the
		 * instance: concurrent readers, the status ioctl, the
		 * per-instance proc file and this instance's own self test
		 * run get a chance between chunks.
		 */
		if (nonblock) {
			if (!mutex_trylock(&ctx->lock)) {
				/*
				 * Preserve an already accumulated partial
				 * count (reachable when O_NONBLOCK is set
				 * mid-read via fcntl()), like every other
				 * error path in this loop.
				 */
				if (ret == 0)
					ret = -EAGAIN;
				break;
			}
		} else if (mutex_lock_interruptible(&ctx->lock)) {
			if (ret == 0)
				ret = -ERESTARTSYS;
			break;
		}

		/*
		 * jent_read_entropy_safe() recovers from an intermittent
		 * health-test failure by reallocating the collector, hence
		 * the indirection, and running its startup again, which a
		 * non-blocking reader must not wait for. jent_read_entropy()
		 * returns that failure instead; it is sticky, so the next
		 * blocking read recovers it.
		 *
		 * Either returns the number of generated bytes (== towork)
		 * or a negative error code on a permanent/generic failure -
		 * JENT_ERR_SELFTEST after a failed run of this instance's
		 * self test included - or on an intermittent one not
		 * recovered.
		 */
		if (nonblock)
			rc = jent_read_entropy(ctx->entropy_collector, tmp,
					       towork);
		else
			rc = jent_read_entropy_safe(&ctx->entropy_collector,
						    tmp, towork);
		mutex_unlock(&ctx->lock);

		if (rc < 0) {
			/*
			 * Map the error; panics under FIPS if the health
			 * test failure is permanent.
			 */
			int err = jent_map_user_read_error(rc);

			if (ret == 0)
				ret = err;
			break;
		}

		/* tmp is private to this call, no lock needed to copy out. */
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

	memzero_explicit(tmp, sizeof(tmp));
	return ret;
}

static long jent_chardev_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct jent_chardev_ctx *ctx = file->private_data;

	if (!ctx)
		return -EFAULT;

	/*
	 * The status and field handlers take ctx->lock themselves: the read
	 * path may reallocate the collector on health-test recovery, so it must
	 * not be read unlocked.
	 */
	switch (cmd) {
	case JENT_IOCSTATUS:
		return jent_status_to_user(&ctx->lock, &ctx->entropy_collector,
					   (void __user *)arg);
	case JENT_IOCSELFTEST:
		/* The run of this instance; it takes ctx->lock itself. */
		return jent_ioctl_selftest(&ctx->selftest);
	default:
		if (jent_ioctl_is_field(cmd))
			return jent_ioctl_field_to_user(&ctx->lock,
							&ctx->entropy_collector,
							cmd,
							(void __user *)arg);
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

/*
 * Whether misc_register() succeeded. A failure here does not fail the module
 * load (see jent_mod_init()), so the module exit runs with the device never
 * registered - and misc_deregister() of such a device walks a list_head that
 * was never linked.
 */
static bool jent_chardev_registered;

int __init jent_chardev_init(void)
{
	int ret;

	/*
	 * Before the device can be opened: an open() racing the module load
	 * would otherwise get an instance without its status file.
	 *
	 * Non-fatal: the device works without the per-instance status export
	 * (and jent_proc_dir is NULL without CONFIG_PROC_FS).
	 */
	if (jent_proc_dir) {
		/* Root only: the file names are the instances' UUIDs. */
		jent_chardev_proc_dir = proc_mkdir_mode(JENT_CHARDEV_PROC_DIRNAME,
							0500, jent_proc_dir);
		if (!jent_chardev_proc_dir)
			pr_warn("jitterentropy: failed to create /proc/%s/%s\n",
				JENT_PROC_DIRNAME, JENT_CHARDEV_PROC_DIRNAME);
	}

	ret = misc_register(&jent_chardev_misc);
	if (ret) {
		pr_err("jitterentropy: failed to register character device: %d\n",
		       ret);
		proc_remove(jent_chardev_proc_dir);
		jent_chardev_proc_dir = NULL;
		return ret;
	}

	jent_chardev_registered = true;

	pr_info("jitterentropy: character device /dev/%s registered\n",
		jent_chardev_misc.name);

	return 0;
}

void jent_chardev_exit(void)
{
	/*
	 * The misc device's fops hold a module reference for every open file, so
	 * the module cannot be unloaded while instances exist. By the time this
	 * runs there are therefore no per-instance files left below the
	 * directory, and removing it cannot race a release().
	 *
	 * Only undo a registration that happened: a failed jent_chardev_init()
	 * leaves the module loaded without the device, and already removed the
	 * proc directory it may have created.
	 */
	if (jent_chardev_registered) {
		misc_deregister(&jent_chardev_misc);
		jent_chardev_registered = false;
	}

	proc_remove(jent_chardev_proc_dir);
	jent_chardev_proc_dir = NULL;
}
