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
#include <linux/types.h>

#include "jitterentropy.h"
#include "jitterentropy_selftest.h"
#include "jitterentropy_uapi.h"

/* One answer, whichever field was asked for. */
struct jent_ioctl_field {
	union {
		__u32 u32;
		struct jent_uuid_ioctl uuid;
		struct jent_output_ioctl output;
	} value;
	size_t size;		/* bytes of @value to copy out */
};

/* Whether jent_ioctl_field_get() answers @cmd, so the list is stated once. */
bool jent_ioctl_is_field(unsigned int cmd);

/*
 * Fill @out for @cmd. Without an @ec only JENT_IOCVERSION answers, the rest
 * give -ENODATA. Call with the interface's lock held; the copy to userspace is
 * the caller's, once it is dropped.
 */
int jent_ioctl_field_get(const struct rand_data *ec, unsigned int cmd,
			 struct jent_ioctl_field *out);

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
