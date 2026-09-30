/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * The JSON status document of an instance (jent_status()), rendered the three
 * ways the kernel interfaces need it: into a userspace buffer for the
 * JENT_IOCSTATUS ioctl, into a seq_file for a /proc status export and into the
 * kernel log. The character device, the hwrng, the crypto API and the debugfs
 * test interface all render the same document, so only the locking and the
 * instance are theirs.
 *
 * Every renderer reads the collector pointer through @ec rather than taking it
 * by value: the read paths reallocate the collector on health-test recovery,
 * and @lock is the lock that path takes, so the instance cannot be replaced or
 * freed while jent_status() walks it.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#ifndef _JITTERENTROPY_STATUS_H
#define _JITTERENTROPY_STATUS_H

#include <linux/mutex.h>
#include <linux/seq_file.h>
#include <linux/types.h>

#include "jitterentropy.h"

/*
 * The JENT_IOCSTATUS handler, @arg pointing at the caller's struct
 * jent_status_ioctl. Takes @lock itself.
 *
 * Returns 0, or -EFAULT, -ENOMEM, -ERESTARTSYS, -EIO. A buffer too small for
 * the document gives -EOVERFLOW with the required size reported back in the
 * length field - part of the uapi contract, hence stated once here.
 */
long jent_status_to_user(struct mutex *lock, struct rand_data **ec,
			 void __user *arg);

/* The show routine of a /proc status export. Takes @lock itself. */
int jent_status_seq_show(struct seq_file *m, struct mutex *lock,
			 struct rand_data **ec);

/*
 * Emit the document to the kernel log, one printk per line: printk truncates
 * records at about 1 kB, which the multi-line JSON document exceeds.
 *
 * @lock is taken around jent_status() only, or NULL when the caller already
 * holds the instance lock. Any rate limiting or change detection gating the
 * emission stays with the caller. Best effort: the return value reports the
 * failure, callers are free to ignore it.
 */
int jent_status_to_log(struct mutex *lock, struct rand_data **ec);

#endif /* _JITTERENTROPY_STATUS_H */
