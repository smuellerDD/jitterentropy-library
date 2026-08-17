// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * The single-field ioctls. See jitterentropy_ioctl.h.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include "jitterentropy.h"
#include "jitterentropy-internal.h"
#include "jitterentropy_ioctl.h"

bool jent_ioctl_is_field(unsigned int cmd)
{
	switch (cmd) {
	case JENT_IOCUUID:
	case JENT_IOCVERSION:
	case JENT_IOCOSR:
	case JENT_IOCFLAGS:
	case JENT_IOCHEALTH:
	case JENT_IOCOUTPUT:
	case JENT_IOCREINIT:
		return true;
	default:
		return false;
	}
}

static int jent_ioctl_field_u32(struct jent_ioctl_field *out, u32 val)
{
	out->value.u32 = val;
	out->size = sizeof(out->value.u32);
	return 0;
}

int jent_ioctl_field_get(const struct rand_data *ec, unsigned int cmd,
			 struct jent_ioctl_field *out)
{
	/* Zeroed, so no padding carries kernel stack to userspace. */
	memset(out, 0, sizeof(*out));

	if (cmd == JENT_IOCVERSION)
		return jent_ioctl_field_u32(out, jent_version());

	if (!ec)
		return -ENODATA;

	switch (cmd) {
	case JENT_IOCUUID:
		/* The UAPI header states the length itself; it must agree. */
		BUILD_BUG_ON(JENT_UUID_IOCTL_LEN != JENT_UUID_STRLEN);

		if (jent_uuid(ec, (char *)out->value.uuid.uuid,
			      sizeof(out->value.uuid.uuid)))
			return -EIO;

		/*
		 * A raw instance skips the startup that assigns the UUID; say
		 * so rather than hand out an empty string.
		 */
		if (!out->value.uuid.uuid[0])
			return -ENODATA;

		out->size = sizeof(out->value.uuid);
		return 0;
	case JENT_IOCOSR:
		return jent_ioctl_field_u32(out, ec->osr);
	case JENT_IOCFLAGS:
		return jent_ioctl_field_u32(out, ec->flags);
	case JENT_IOCHEALTH:
		return jent_ioctl_field_u32(out, ec->health_failure);
	case JENT_IOCREINIT:
		return jent_ioctl_field_u32(out, ec->reinit_count);
	case JENT_IOCOUTPUT:
		out->value.output.invocations = ec->read_invocations;
		out->value.output.bytes = ec->bytes_output;
		out->size = sizeof(out->value.output);
		return 0;
	default:
		return -ENOTTY;
	}
}
