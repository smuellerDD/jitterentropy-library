/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Userspace API for the Jitter RNG character device (/dev/jitterentropy).
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 */

#ifndef _UAPI_JITTERENTROPY_H
#define _UAPI_JITTERENTROPY_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * Recommended buffer size for the status query. The JSON status string
 * produced by jent_status() comfortably fits into this many bytes including
 * its terminating NUL.
 */
#define JENT_STATUS_MAX_LEN 4096

/*
 * Argument for JENT_IOCSTATUS.
 *
 * @buf:    userspace pointer to a buffer that receives the NUL-terminated
 *          JSON status string of the Jitter RNG instance bound to the open
 *          file description. Encoded as a fixed-width integer so the ABI is
 *          identical for 32- and 64-bit userspace.
 * @length: in  - size of the buffer pointed to by @buf in bytes;
 *          out - number of bytes written including the terminating NUL. If the
 *                supplied buffer is too small the call fails with -EOVERFLOW
 *                and @length is set to the number of bytes required.
 */
struct jent_status_ioctl {
	__aligned_u64 buf;
	__u32 length;
};

#define JENT_IOC_MAGIC 'J'

/* Retrieve the Jitter RNG status string of this open instance. */
#define JENT_IOCSTATUS _IOWR(JENT_IOC_MAGIC, 0x01, struct jent_status_ioctl)

#endif /* _UAPI_JITTERENTROPY_H */
