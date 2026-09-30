/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * The single-field ioctls (jitterentropy_uapi.h), shared by the character
 * device and the debugfs test interface: both answer the same set, so only the
 * locking is theirs.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#ifndef JITTERENTROPY_IOCTL_H
#define JITTERENTROPY_IOCTL_H

#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/types.h>

#include "jitterentropy.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_uapi.h"

/*
 * Whether jent_ioctl_field_to_user() answers @cmd, so the list is stated once.
 */
bool jent_ioctl_is_field(unsigned int cmd);

/*
 * Answer the single-field ioctl @cmd into the caller's buffer @arg. Without an
 * instance only JENT_IOCVERSION answers, the rest give -ENODATA.
 *
 * @lock is taken around the read of the field only; the copy to userspace can
 * fault and therefore follows the unlock. The collector pointer is read through
 * @ec because the read paths reallocate it on health-test recovery.
 */
long jent_ioctl_field_to_user(struct mutex *lock, struct rand_data **ec,
			      unsigned int cmd, void __user *arg);

/*
 * JENT_IOCSELFTEST, for both interfaces. Unlike everything above it is an
 * action - hence the privilege, which lives here rather than in either
 * dispatcher so the two cannot come to demand different privileges. The
 * character device passes the self test of the instance the ioctl arrived on;
 * the debugfs test interface passes NULL, as its raw-noise instances have no
 * conditioned output to bind a verdict to, and gets the unbound run. Called
 * without the instance lock, which a bound run takes itself.
 */
static inline long jent_ioctl_selftest(struct jent_selftest_instance *st)
{
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	return st ? jent_selftest_instance_run(st) : jent_selftest_run_now();
}

#endif /* JITTERENTROPY_IOCTL_H */
