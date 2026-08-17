/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Userspace API for the Jitter RNG character device (/dev/jitterentropy) and
 * the debugfs raw entropy test interface (jent_raw_hires).
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

/*
 * Retrieve the Jitter RNG status string of this open instance. Implemented by
 * both the character device and the debugfs test interface.
 */
#define JENT_IOCSTATUS _IOWR(JENT_IOC_MAGIC, 0x01, struct jent_status_ioctl)

/*
 * Set the measurement loop count of this open instance. Only implemented by
 * the debugfs test interface (the character device rejects it with -ENOTTY).
 *
 * The argument points to a __u64 holding the loop count. A value of 0 (the
 * default of every fresh instance) selects the loop count the instance was
 * configured with; any other value is passed as the loop_cnt parameter of
 * every subsequent raw noise measurement (see the jent_measure_jitter*()
 * functions), overriding the configured hash and memory access loop counts.
 * Values above UINT_MAX are rejected with -EINVAL, mirroring the bound of the
 * userspace recording tools.
 */
#define JENT_IOCLOOPCNT _IOW(JENT_IOC_MAGIC, 0x02, __u64)

/*
 * The single fields of the status document, for callers that want one value
 * and no JSON parser: a second spelling of the same state, not a second
 * source. Both interfaces implement all of them. Those describing an instance
 * give -ENODATA without one, as does JENT_IOCUUID on a raw test instance,
 * which skips the startup that assigns the UUID.
 */

/*
 * The canonical UUID string with its NUL. Stated here so this header stays
 * self-contained; the module checks it against JENT_UUID_STRLEN.
 */
#define JENT_UUID_IOCTL_LEN 37

/* Argument for JENT_IOCUUID: the NUL-terminated instance UUID. */
struct jent_uuid_ioctl {
	__u8 uuid[JENT_UUID_IOCTL_LEN];
};

/* Argument for JENT_IOCOUTPUT: what this instance has delivered so far. */
struct jent_output_ioctl {
	__u64 invocations;	/* jent_read_entropy() calls served */
	__u64 bytes;		/* random bytes delivered to callers */
};

/* The stable per-instance identifier, as the "uuid" status field. */
#define JENT_IOCUUID	_IOR(JENT_IOC_MAGIC, 0x03, struct jent_uuid_ioctl)

/* "version" as jent_version() encodes it: 3.7.1 is 3070100. */
#define JENT_IOCVERSION	_IOR(JENT_IOC_MAGIC, 0x04, __u32)

/* The effective oversampling rate, as "configuration.osr". */
#define JENT_IOCOSR	_IOR(JENT_IOC_MAGIC, 0x05, __u32)

/* "configuration.flags" as a raw mask; the bits are in jitterentropy.h. */
#define JENT_IOCFLAGS	_IOR(JENT_IOC_MAGIC, 0x06, __u32)

/* "healthFailure" as a raw JENT_*_FAILURE mask; zero when nothing fired. */
#define JENT_IOCHEALTH	_IOR(JENT_IOC_MAGIC, 0x07, __u32)

/* The lifetime output counters, as the "output" object. */
#define JENT_IOCOUTPUT	_IOR(JENT_IOC_MAGIC, 0x08, struct jent_output_ioctl)

/* "reinitializations": the UUID is preserved across them. */
#define JENT_IOCREINIT	_IOR(JENT_IOC_MAGIC, 0x09, __u32)

/*
 * Run the cryptographic self test - the SHA3-256 and XDRBG-256 known answer
 * tests of the conditioning component - and report the verdict. Implemented by
 * the character device and the debugfs test interface; takes no argument. An
 * action rather than a query, which is why it requires CAP_SYS_ADMIN and gives
 * -EPERM without it.
 *
 * On the character device the run is that of the instance the ioctl arrives
 * on. Returns 0 when the tests pass. A failure gives -EFAULT and permanently
 * stops the output of this instance - reads on this open file fail from then
 * on, every other instance keeps delivering, and under fips=1 it is a panic. A
 * call made after a failed run of this instance gives -EFAULT without running
 * anything.
 *
 * On the debugfs test interface the run is unbound: its instances record raw
 * noise that never passes the conditioning component, so there is no output to
 * stop, and only the verdict of this run is returned.
 */
#define JENT_IOCSELFTEST _IO(JENT_IOC_MAGIC, 0x0a)

#endif /* _UAPI_JITTERENTROPY_H */
