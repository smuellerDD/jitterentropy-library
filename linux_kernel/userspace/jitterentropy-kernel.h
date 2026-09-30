/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * User space access to the jitter_rng kernel module.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

/*
 * The API of libjitterentropy-kernel, which the build offers when asked to
 * (ENABLE_KERNEL_LIB in CMake): the character device of the jitter_rng kernel
 * module, its ioctls of jitterentropy_uapi.h and its files in
 * /proc/jitterentropy, without the caller spelling out any of them.
 *
 * It contains no Jitter RNG of its own and does not link libjitterentropy:
 * every instance lives in the kernel, one per open of the device.
 *
 * An instance is a file descriptor, so that poll(2), fcntl(2) and the like
 * apply to it as to any other. Every function returns a negative errno on
 * failure.
 */

#ifndef _JITTERENTROPY_KERNEL_H
#define _JITTERENTROPY_KERNEL_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JENT_KERNEL_API __attribute__((visibility("default")))

/* The character device of the module. */
#define JENT_KERNEL_DEVICE	"/dev/jitterentropy"

/* A buffer of this size holds the status document. */
#define JENT_KERNEL_STATUS_LEN	4096

/* The canonical UUID string with its NUL. */
#define JENT_KERNEL_UUID_STRLEN	37

/*
 * Open @path, or JENT_KERNEL_DEVICE for NULL: the kernel allocates an instance
 * for the descriptor returned. -ENOMEM also reports startup health tests that
 * failed, -ENFILE the max_instances cap of the module.
 */
JENT_KERNEL_API
int jent_kernel_open(const char *path);

/* Close the descriptor, which frees its instance. */
JENT_KERNEL_API
int jent_kernel_close(int fd);

/*
 * Read @len random bytes into @buf. Returns the number of bytes read: @len,
 * or fewer where a nonblocking descriptor found the instance busy or an error
 * came up after the first byte, which the next call then returns. -EIO is an
 * instance stopped by a health test or self test failure.
 */
JENT_KERNEL_API
ssize_t jent_kernel_read(int fd, void *buf, size_t len);

/*
 * The JSON status document of the instance, as jent_status() writes it, NUL
 * terminated. -EOVERFLOW for a buffer too small.
 */
JENT_KERNEL_API
int jent_kernel_status(int fd, char *buf, size_t buflen);

/* The identifier of the instance; @buflen at least JENT_KERNEL_UUID_STRLEN. */
JENT_KERNEL_API
int jent_kernel_uuid(int fd, char *buf, size_t buflen);

/* The library version of the module, as JENT_VERSION encodes it. */
JENT_KERNEL_API
int jent_kernel_version(int fd, unsigned int *version);

/* The oversampling rate in effect. */
JENT_KERNEL_API
int jent_kernel_osr(int fd, unsigned int *osr);

/* The JENT_* flags of jitterentropy.h the instance runs with. */
JENT_KERNEL_API
int jent_kernel_flags(int fd, unsigned int *flags);

/* The JENT_*_FAILURE bits of the health tests standing, 0 for none. */
JENT_KERNEL_API
int jent_kernel_health_failure(int fd, unsigned int *health_failure);

/* The reads served and the bytes they delivered. Either may be NULL. */
JENT_KERNEL_API
int jent_kernel_output(int fd, uint64_t *read_invocations,
		       uint64_t *bytes_output);

/* The reallocations after an intermittent health test failure. */
JENT_KERNEL_API
int jent_kernel_reinitializations(int fd, unsigned int *reinitializations);

/* The memory access region in bytes, 0 without memory access. */
JENT_KERNEL_API
int jent_kernel_memsize(int fd, size_t *memsize);

/* The hash loop count per measurement. */
JENT_KERNEL_API
int jent_kernel_hashloops(int fd, unsigned int *hashloops);

/*
 * Run the cryptographic self test on the instance. Needs CAP_SYS_ADMIN,
 * -EPERM without. -EFAULT is a failed test, which stops the instance for good.
 */
JENT_KERNEL_API
int jent_kernel_selftest(int fd);

/*
 * Read the file @name below /proc/jitterentropy - "version", "config/osr",
 * "statistics" and the others jitter_rng(4) lists - into @buf, NUL
 * terminated. -EOVERFLOW for a buffer too small, -ENOENT without the module.
 */
JENT_KERNEL_API
int jent_kernel_proc(const char *name, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_KERNEL_H */
