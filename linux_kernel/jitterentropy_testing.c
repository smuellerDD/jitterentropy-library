/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test interface for Jitter RNG.
 *
 * The debugfs file jent_raw_hires provides the raw noise data of the Jitter
 * RNG: each open allocates a dedicated recording, each read drives its
 * measurements and returns one u64 per measurement holding the time delta as
 * consumed by the health tests and the entropy pool (i.e. including the
 * division by the common timer GCD). Read sizes must be a multiple of the
 * u64 sample size; any other size is rejected with -EINVAL. The recording is
 * the one of the user space recording tools, jitterentropy-record.h of
 * tests/raw-entropy/recording_library.
 *
 * The file also implements the JENT_IOCSTATUS ioctl known from the character
 * device (see jitterentropy_uapi.h), returning the JSON status string of the
 * Jitter RNG instance bound to the open file description, and the test-
 * interface-only JENT_IOCLOOPCNT ioctl, overriding the loop count applied to
 * the raw noise measurements of that instance.
 *
 * On a locked-down kernel the interface is not created, and opens of an
 * already-created file are refused once lockdown is raised at runtime.
 *
 * Copyright (C) 2023 - 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "jitterentropy.h"
#include "jitterentropy-record.h"
#include "jitterentropy_ioctl.h"
#include "jitterentropy_mod.h"
#include "jitterentropy_status.h"
#include "jitterentropy_testing.h"
#include "jitterentropy_uapi.h"

/*
 * Serialize the measurement batches of extract sessions: a concurrent reader
 * would run its own timing measurements and thereby perturb the measurements
 * of the other session. No two batches measure at once; what another reader
 * does between its batches - its user copy and any fault it takes, its
 * allocation - may still run on another CPU beside a batch, as any other load
 * of the machine may. The lock also serializes the jent_testing_log()
 * bookkeeping (logged_osr/logged_flags) and every access to the per-open
 * collector state. It is never held across a user copy, so a reader stalling
 * on a faulting user buffer cannot block the other users of the interface.
 */
static DEFINE_MUTEX(jent_testing_read_lock);

#define JENT_TEST_HASHLOOP (1<<15)
#define JENT_TEST_MEMACCLOOP (1<<16)

static struct dentry *jent_raw_debugfs_root = NULL;

/*
 * The raw noise measurements expose the timing behavior of the kernel and are
 * a pure test vehicle, so the interface has no business on a locked-down
 * kernel: do not create it when the kernel is already locked down at module
 * load, and refuse opens if lockdown was raised afterwards (lockdown can be
 * tightened at runtime, e.g. via /sys/kernel/security/lockdown, but never
 * relaxed). LOCKDOWN_DEBUGFS is the reason the debugfs core itself uses to
 * gate debugfs files, and it is part of the integrity level, so the interface
 * disappears for both lockdown=integrity and lockdown=confidentiality.
 */
static bool jent_testing_locked_down(void)
{
	return security_locked_down(LOCKDOWN_DEBUGFS) != 0;
}

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
static unsigned int testing_flags = 0;

module_param(testing_osr, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(testing_osr, "Jitter RNG testing OSR parameter");
module_param(testing_flags, uint, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
MODULE_PARM_DESC(testing_flags, "Jitter RNG testing flags parameter");

static unsigned int logged_osr = 0xffffffff;
static unsigned int logged_flags = 0xffffffff;

/*
 * Verbose logging switch, configurable via the module parameter of the same
 * name (see jitterentropy_mod.c).
 */
extern unsigned int jent_verbose;

/*
 * osr/flags are the snapshot the instance was allocated from, not the live
 * module parameters: a sysfs write since then must not relabel the log.
 */
static int jent_testing_log(struct rand_data *ec, unsigned int osr,
			    unsigned int flags)
{
	if (!jent_verbose)
		return 0;

	if (logged_osr == osr && logged_flags == flags)
		return 0;

	if (!ec)
		return 0;

	/*
	 * Print out status for each test cycle where the Jitter RNG
	 * properties differ.
	 */
	logged_osr = osr;
	logged_flags = flags;

	/*
	 * NULL lock: the caller already holds jent_testing_read_lock, which is
	 * also what serializes the logged_osr/logged_flags bookkeeping above.
	 * The collector of a test instance is never reallocated, so the
	 * indirection the shared renderer takes is a formality here.
	 */
	return jent_status_to_log(NULL, &ec);
}

/************** Raw High-Resolution Timer Entropy Data Handling **************/

/*
 * Per-open state: each open() gets its own recording, allocated with the
 * testing_osr/testing_flags values at open time, which also fix the noise
 * source it measures. @ec is its collector, for the status and field ioctls.
 */
struct jent_testing_ctx {
	struct jent_record *rec;
	struct rand_data *ec;
	/*
	 * Loop count applied to each raw noise measurement, settable via
	 * JENT_IOCLOOPCNT: 0 (the default) selects the loop count the
	 * instance was configured with. Protected by
	 * jent_testing_read_lock; an extract session snapshots it once, so it
	 * cannot change in the middle of the session.
	 */
	u64 loop_cnt;
};

static int jent_testing_open(struct inode *inode, struct file *file)
{
	struct jent_testing_ctx *ctx;
	unsigned int osr;
	unsigned int flags;
	unsigned int lib_flags;
	unsigned int source;
	int ret;

	/*
	 * The file only exists if the kernel was not locked down at module
	 * load; re-check here to also cover lockdown raised at runtime after
	 * the file was created. The debugfs proxy performs the same check on
	 * open for fops carrying an ioctl handler, but do not rely on that
	 * implementation detail.
	 */
	if (jent_testing_locked_down())
		return -EPERM;

	ctx = kvzalloc(sizeof(*ctx), GFP_KERNEL_ACCOUNT);
	if (!ctx)
		return -ENOMEM;

	/*
	 * Snapshot the runtime-writable module parameters once: a concurrent
	 * sysfs write between separate reads could otherwise pair a noise
	 * source with a collector allocated from different flags.
	 */
	flags = READ_ONCE(testing_flags);
	osr = READ_ONCE(testing_osr);

	/*
	 * The recording selectors are the test interface's own; the hash loop
	 * takes precedence when both are set.
	 */
	lib_flags = flags & ~(unsigned int)(JENT_TEST_HASHLOOP |
					    JENT_TEST_MEMACCLOOP);
	if (flags & JENT_TEST_HASHLOOP)
		source = JENT_RECORD_HASHLOOP;
	else if (flags & JENT_TEST_MEMACCLOOP)
		source = JENT_RECORD_MEMACCESS;
	else
		source = JENT_RECORD_COMMON;

	/*
	 * Parameters the module refuses to load with, reported as what they
	 * are rather than as the -ENOMEM the allocation below gives some of
	 * them - JENT_FORCE_INTERNAL_TIMER, which the kernel has no timer for,
	 * among them.
	 */
	ret = jent_mod_check_config(osr, lib_flags, "testing_");
	if (ret) {
		kvfree(ctx);
		return ret;
	}

	/*
	 * The collector comes without the startup entropy collection and its
	 * health-test reset ladder (mirroring the userspace recording tools):
	 * the startup could silently escalate OSR, memory size and hash loop
	 * count, but the recorded raw data must correspond exactly to the
	 * requested testing_osr/testing_flags. It runs the full SP800-90B
	 * health test handling.
	 *
	 * No lock: the allocation, up to the largest memory access region, and
	 * the self tests of the conditioning it runs first touch nothing
	 * another file shares, and would otherwise stall every other read.
	 */
	ret = jent_record_alloc(&ctx->rec, osr, lib_flags, source);
	switch (ret) {
	case JENT_RECORD_OK:
		break;
	case JENT_RECORD_EINVAL:
		/*
		 * The parameters were checked above, which leaves the memory
		 * access loop with no memory region to walk. It would record
		 * zeroes that read() reports as a full capture, and that an
		 * SP800-90B assessment would take for one
		 * (getrawentropy --memaccess --disable-memory-access).
		 */
		pr_warn("jitterentropy: testing_flags requests the memory access loop with memory access disabled - it would record only zeroes\n");
		kvfree(ctx);
		return -EINVAL;
	case JENT_RECORD_ESELFTEST:
		pr_warn("jitterentropy: self test of the conditioning failed\n");
		kvfree(ctx);
		return -EIO;
	default:
		pr_warn("jitterentropy: raw entropy collector allocation failed: out of memory\n");
		kvfree(ctx);
		return -ENOMEM;
	}
	ctx->ec = jent_record_collector(ctx->rec);

	/*
	 * The jent_testing_log() bookkeeping is shared by every file. Only
	 * with something to log: the lock may be held for a whole batch of
	 * another file's measurements, which an open() logging nothing has no
	 * reason to wait for.
	 */
	if (jent_verbose) {
		if (mutex_lock_interruptible(&jent_testing_read_lock)) {
			jent_record_free(ctx->rec);
			kvfree(ctx);
			return -ERESTARTSYS;
		}
		jent_testing_log(ctx->ec, osr, flags);
		mutex_unlock(&jent_testing_read_lock);
	}

	file->private_data = ctx;

	return 0;
}

static int jent_testing_release(struct inode *inode, struct file *file)
{
	struct jent_testing_ctx *ctx = file->private_data;

	if (!ctx)
		return 0;

	jent_record_free(ctx->rec);
	kvfree(ctx);
	file->private_data = NULL;

	return 0;
}

static ssize_t jent_testing_extract_user(struct file *file, char __user *buf,
					 size_t nbytes, loff_t *ppos)
{
	struct jent_testing_ctx *ctx = file->private_data;
	u64 *tmp = NULL;
	unsigned int loop_cnt;
	ssize_t ret = 0;

	/* Defense in depth, matching the ioctl handler: open() sets this. */
	if (!ctx)
		return -EFAULT;

	if (!nbytes)
		return 0;

	/*
	 * The interface delivers whole u64 time stamp samples only: reject
	 * requests that are not a multiple of the sample size. A request
	 * smaller than one sample would fall through the loop below and
	 * return 0, which reads as EOF on an endless stream (e.g. dd with
	 * bs < 8 would record nothing and report success); a trailing
	 * partial sample would be silently dropped as a short read. This
	 * also catches a recording tool built for the u32 sample format of
	 * the vanilla kernel test interface loudly instead of letting it
	 * record garbled data.
	 */
	if (nbytes % sizeof(u64))
		return -EINVAL;

	/*
	 * The intention of this interface is for collecting at least
	 * 1000 samples due to the SP800-90B requirements. With kvmalloc
	 * the allocation of such a larger chunk is no longer an issue.
	 */
#define JENT_TESTING_SAMPLES	1000
#define JENT_TESTING_DATA_SIZE	(JENT_TESTING_SAMPLES * sizeof(u64))
	tmp = kvmalloc(JENT_TESTING_DATA_SIZE, GFP_KERNEL_ACCOUNT);
	if (!tmp)
		return -ENOMEM;

	/*
	 * Snapshot the JENT_IOCLOOPCNT setting under the lock: the whole
	 * extract session records with one consistent loop count, even though
	 * the lock is dropped between the batches below.
	 */
	if (mutex_lock_interruptible(&jent_testing_read_lock)) {
		kvfree(tmp);
		return -ERESTARTSYS;
	}
	/* At most JENT_LOOPCNT_MAX, see jent_testing_ioctl_loopcnt(). */
	loop_cnt = (unsigned int)ctx->loop_cnt;
	mutex_unlock(&jent_testing_read_lock);

	while (nbytes >= sizeof(u64)) {
		u32 samples = (u32)min_t(size_t, nbytes / sizeof(u64),
					 JENT_TESTING_SAMPLES);
		size_t len;
		size_t not_copied;
		bool primed;
		u32 i;

		/* Honor pending signals regardless of the resched state. */
		if (signal_pending(current)) {
			if (ret == 0)
				ret = -ERESTARTSYS;
			break;
		}

		/*
		 * Only one batch of measurements may run at a time, across all
		 * open files, so no other session perturbs it. The lock is held
		 * for the measurements only, not for the copy_to_user() below:
		 * a reader stalling on a faulting user buffer (userfaultfd,
		 * FUSE) must not block every other open, ioctl and read. The
		 * collector state of this file stays consistent, as every
		 * access to it is made under the lock.
		 */
		if (mutex_lock_interruptible(&jent_testing_read_lock)) {
			if (ret == 0)
				ret = -ERESTARTSYS;
			break;
		}

		/*
		 * Prime the measurement so the first recorded delta is not
		 * computed from a stale time stamp (unprimed instance, the gap
		 * spent in copy_to_user() between two rounds, or a reschedule).
		 * The priming runs at the session's loop count, so the hash and
		 * memory access loops the first recorded delta spans run at
		 * that count like every other's; it stays out of the health
		 * tests, whose own work the steady-state deltas also span, as
		 * its delta is not one of the recording.
		 *
		 * A priming is followed by its measurement without a check in
		 * between, so every pass of the loop records a sample: were a
		 * reschedule to discard a priming, a measurement outlasting the
		 * time slice would have the loop prime forever and record
		 * nothing. The longest stretch without a reschedule point is
		 * thus a priming and a measurement, two at the loop count.
		 */
		primed = false;

		for (i = 0; i < samples; i++) {
			/*
			 * Yield and take signals per sample, as one can run
			 * long at a high loop count. A reschedule calls for a
			 * fresh priming, so the scheduling gap is not recorded
			 * as a delta.
			 */
			if (need_resched()) {
				schedule();
				primed = false;
			}

			if (signal_pending(current))
				break;

			if (!primed) {
				jent_record_prime(ctx->rec, loop_cnt);
				primed = true;
			}

			tmp[i] = jent_record_sample(ctx->rec, loop_cnt);
		}

		mutex_unlock(&jent_testing_read_lock);

		/* A signal cut the batch short: deliver what was measured. */
		samples = i;
		len = samples * sizeof(u64);
		if (!len) {
			if (ret == 0)
				ret = -ERESTARTSYS;
			break;
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
	return ret;
}

/*
 * Set the loop count applied to the raw noise measurements of this open
 * instance (see the loop_cnt member of struct jent_testing_ctx). A running
 * extract session keeps recording with the loop count it started with; the
 * update applies from the next read on.
 */
static long jent_testing_ioctl_loopcnt(struct jent_testing_ctx *ctx,
				       void __user *arg)
{
	u64 loop_cnt;

	if (copy_from_user(&loop_cnt, arg, sizeof(loop_cnt)))
		return -EFAULT;

	/* Bounds one uninterruptible measurement, see JENT_LOOPCNT_MAX. */
	if (loop_cnt > JENT_LOOPCNT_MAX)
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

	/*
	 * The status and field handlers take the extract-session lock
	 * themselves, which keeps a concurrent extract session from mutating
	 * the state being reported. Unlike the character device the collector
	 * is never reallocated during the lifetime of the open file, so ctx->ec
	 * itself is stable and the indirection they take is a formality here.
	 *
	 * JENT_IOCSTATUS has the same ABI and semantics as on the character
	 * device; JENT_IOCUUID gives -ENODATA, as ctx->ec is a raw instance and
	 * skips the startup that assigns the UUID.
	 */
	switch (cmd) {
	case JENT_IOCSTATUS:
		return jent_status_to_user(&jent_testing_read_lock, &ctx->ec,
					   (void __user *)arg);
	case JENT_IOCLOOPCNT:
		return jent_testing_ioctl_loopcnt(ctx, (void __user *)arg);
	case JENT_IOCSELFTEST:
		/* Unbound: a raw instance has no conditioned output to stop. */
		return jent_ioctl_selftest(NULL);
	default:
		if (jent_ioctl_is_field(cmd))
			return jent_ioctl_field_to_user(&jent_testing_read_lock,
							&ctx->ec, cmd,
							(void __user *)arg);
		return -ENOTTY;
	}
}

/*
 * The file is created with debugfs_create_file_unsafe(), so the removal
 * protection the full proxy would wrap around every file operation is done
 * here: debugfs_file_get() makes a concurrent debugfs_remove_recursive() wait
 * until the operation returns, and refuses the operation with -EIO - what the
 * proxy returns - once the file is gone. The reason not to use the proxy is
 * compat_ioctl: no kernel from 5.10 to the current one installs a
 * compat_ioctl in debugfs_full_proxy_file_operations, so a 32-bit caller on a
 * 64-bit kernel would get -ENOTTY for every ioctl of the uapi header, whose
 * structures are laid out to be identical for both. With the _unsafe variant
 * the open replaces the file's f_op with these fops, compat_ioctl included.
 *
 * Open is protected by the debugfs open proxy itself, and release must run
 * unprotected, as the full proxy runs it, so an instance allocated by the
 * open is always freed.
 */
static ssize_t jent_testing_debugfs_read(struct file *file, char __user *buf,
					 size_t nbytes, loff_t *ppos)
{
	struct dentry *dentry = file->f_path.dentry;
	ssize_t ret;

	ret = debugfs_file_get(dentry);
	if (unlikely(ret))
		return ret;
	ret = jent_testing_extract_user(file, buf, nbytes, ppos);
	debugfs_file_put(dentry);

	return ret;
}

static long jent_testing_debugfs_ioctl(struct file *file, unsigned int cmd,
				       unsigned long arg)
{
	struct dentry *dentry = file->f_path.dentry;
	long ret;

	ret = debugfs_file_get(dentry);
	if (unlikely(ret))
		return ret;
	ret = jent_testing_ioctl(file, cmd, arg);
	debugfs_file_put(dentry);

	return ret;
}

/*
 * compat_ptr_ioctl() converts the argument and calls .unlocked_ioctl, which
 * is the protected wrapper above: the compat path needs no wrapper of its own.
 */
static const struct file_operations jent_raw_hires_fops = {
	.owner = THIS_MODULE,
	.open = jent_testing_open,
	.release = jent_testing_release,
	.read = jent_testing_debugfs_read,
	.unlocked_ioctl = jent_testing_debugfs_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

/******************************* Initialization *******************************/

int __init jent_testing_init(void)
{
	struct dentry *raw_hires;
	int ret;

	/*
	 * Without debugfs there is nothing to create and every debugfs call
	 * would return -ENODEV; the test interface is a pure debugging aid,
	 * so its absence must not fail the module load.
	 */
	if (!IS_ENABLED(CONFIG_DEBUG_FS)) {
		pr_info("jitterentropy: debugfs not available, not exposing the raw entropy test interface\n");
		return 0;
	}

	if (jent_testing_locked_down()) {
		pr_info("jitterentropy: kernel is locked down, not exposing the raw entropy test interface\n");
		return 0;
	}

	jent_raw_debugfs_root = debugfs_create_dir(KBUILD_MODNAME, NULL);
	if (IS_ERR(jent_raw_debugfs_root)) {
		ret = PTR_ERR(jent_raw_debugfs_root);
		jent_raw_debugfs_root = NULL;

		/*
		 * debugfs compiled in but unavailable at runtime: booting with
		 * debugfs=off refuses every creation with -EPERM (5.10 and
		 * newer), and newer kernels refuse with -ENOENT while debugfs
		 * is not initialized. Skip the interface as without
		 * CONFIG_DEBUG_FS.
		 */
		if (ret == -EPERM || ret == -ENOENT) {
			pr_notice("jitterentropy: debugfs not available (%d), not exposing the raw entropy test interface\n",
				  ret);
			return 0;
		}

		pr_warn("jitterentropy: failed to create debugfs directory %s: %d\n",
			KBUILD_MODNAME, ret);
		return ret;
	}

	/*
	 * The _unsafe variant, so the 32-bit compat ioctl reaches the fops:
	 * they implement the debugfs_file_get()/debugfs_file_put() protocol
	 * themselves, which protects a reader against a concurrent
	 * debugfs_remove_recursive() from jent_testing_exit() as the proxy
	 * would.
	 */
	raw_hires = debugfs_create_file_unsafe("jent_raw_hires", 0400,
					       jent_raw_debugfs_root, NULL,
					       &jent_raw_hires_fops);
	if (IS_ERR(raw_hires)) {
		ret = PTR_ERR(raw_hires);
		pr_warn("jitterentropy: failed to create debugfs file jent_raw_hires: %d\n",
			ret);
		debugfs_remove_recursive(jent_raw_debugfs_root);
		jent_raw_debugfs_root = NULL;
		return ret;
	}

	return 0;
}

void jent_testing_exit(void)
{
	/* NULL if the interface was skipped; debugfs ignores it. */
	debugfs_remove_recursive(jent_raw_debugfs_root);
	jent_raw_debugfs_root = NULL;
}
