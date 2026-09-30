/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * User space access to the jitter_rng kernel module.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

/* O_CLOEXEC is POSIX.1-2008, hidden by glibc under a strict -std=c11. */
#ifndef _POSIX_C_SOURCE
# define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Beside this file and in the directory above: no include path is needed. */
#include "jitterentropy-kernel.h"
#include "../jitterentropy_uapi.h"

#define JENT_KERNEL_PROC	"/proc/jitterentropy/"

/* The two lengths are one: the header states it without the UAPI header. */
_Static_assert(JENT_KERNEL_UUID_STRLEN == JENT_UUID_IOCTL_LEN,
	       "UUID length differs from the module's");
_Static_assert(JENT_KERNEL_STATUS_LEN == JENT_STATUS_MAX_LEN,
	       "status length differs from the module's");

/*
 * One ioctl, as 0 or a negative errno. glibc declares the request as unsigned
 * long; musl and bionic as int, which the _IOR() numbers do not fit without
 * the cast.
 */
#ifdef __GLIBC__
typedef unsigned long jent_kernel_request;
#else
typedef int jent_kernel_request;
#endif

static int jent_kernel_ioctl(int fd, unsigned long request, void *arg)
{
	return ioctl(fd, (jent_kernel_request)request, arg) ? -errno : 0;
}

int jent_kernel_open(const char *path)
{
	int fd = open(path ? path : JENT_KERNEL_DEVICE, O_RDONLY | O_CLOEXEC);

	return fd < 0 ? -errno : fd;
}

int jent_kernel_close(int fd)
{
	return close(fd) ? -errno : 0;
}

ssize_t jent_kernel_read(int fd, void *buf, size_t len)
{
	size_t done = 0;

	if ((!buf && len) || len > (size_t)SSIZE_MAX)
		return -EINVAL;

	while (done < len) {
		ssize_t r = read(fd, (uint8_t *)buf + done, len - done);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			/* What was read is delivered; the error comes next. */
			return done ? (ssize_t)done : -errno;
		}
		if (r == 0)
			break;
		done += (size_t)r;
	}

	return (ssize_t)done;
}

int jent_kernel_status(int fd, char *buf, size_t buflen)
{
	struct jent_status_ioctl arg;

	if (!buf || !buflen || buflen > UINT32_MAX)
		return -EINVAL;

	memset(&arg, 0, sizeof(arg));
	arg.buf = (uintptr_t)buf;
	arg.length = (uint32_t)buflen;
	return jent_kernel_ioctl(fd, JENT_IOCSTATUS, &arg);
}

int jent_kernel_uuid(int fd, char *buf, size_t buflen)
{
	struct jent_uuid_ioctl arg;
	int ret;

	if (!buf || buflen < JENT_KERNEL_UUID_STRLEN)
		return -EINVAL;

	memset(&arg, 0, sizeof(arg));
	ret = jent_kernel_ioctl(fd, JENT_IOCUUID, &arg);
	if (ret)
		return ret;

	/* Terminated here as well: the string is the kernel's. */
	arg.uuid[sizeof(arg.uuid) - 1] = '\0';
	memcpy(buf, arg.uuid, sizeof(arg.uuid));
	return 0;
}

/* The ioctls that answer with one __u32. */
static int jent_kernel_u32(int fd, unsigned long request, unsigned int *val)
{
	uint32_t v = 0;
	int ret;

	if (!val)
		return -EINVAL;
	ret = jent_kernel_ioctl(fd, request, &v);
	if (ret)
		return ret;

	*val = v;
	return 0;
}

int jent_kernel_version(int fd, unsigned int *version)
{
	return jent_kernel_u32(fd, JENT_IOCVERSION, version);
}

int jent_kernel_osr(int fd, unsigned int *osr)
{
	return jent_kernel_u32(fd, JENT_IOCOSR, osr);
}

int jent_kernel_flags(int fd, unsigned int *flags)
{
	return jent_kernel_u32(fd, JENT_IOCFLAGS, flags);
}

int jent_kernel_health_failure(int fd, unsigned int *health_failure)
{
	return jent_kernel_u32(fd, JENT_IOCHEALTH, health_failure);
}

int jent_kernel_reinitializations(int fd, unsigned int *reinitializations)
{
	return jent_kernel_u32(fd, JENT_IOCREINIT, reinitializations);
}

int jent_kernel_hashloops(int fd, unsigned int *hashloops)
{
	return jent_kernel_u32(fd, JENT_IOCHASHLOOPS, hashloops);
}

int jent_kernel_memsize(int fd, size_t *memsize)
{
	uint64_t v = 0;
	int ret;

	if (!memsize)
		return -EINVAL;
	ret = jent_kernel_ioctl(fd, JENT_IOCMEMSIZE, &v);
	if (ret)
		return ret;
	if (v > SIZE_MAX)
		return -EOVERFLOW;

	*memsize = (size_t)v;
	return 0;
}

int jent_kernel_output(int fd, uint64_t *read_invocations,
		       uint64_t *bytes_output)
{
	struct jent_output_ioctl arg;
	int ret;

	memset(&arg, 0, sizeof(arg));
	ret = jent_kernel_ioctl(fd, JENT_IOCOUTPUT, &arg);
	if (ret)
		return ret;

	if (read_invocations)
		*read_invocations = arg.invocations;
	if (bytes_output)
		*bytes_output = arg.bytes;
	return 0;
}

int jent_kernel_selftest(int fd)
{
	return jent_kernel_ioctl(fd, JENT_IOCSELFTEST, NULL);
}

/*
 * The whole of the file @path into @buf, NUL terminated. The path is a
 * parameter so that what this has to handle - a file filling the buffer, one
 * larger than it, none at all - can be produced from a file the caller made.
 */
static int jent_kernel_file(const char *path, char *buf, size_t buflen)
{
	size_t done = 0;
	int fd, ret = 0;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	/* One byte is kept for the NUL. */
	while (done < buflen - 1) {
		ssize_t r = read(fd, buf + done, buflen - 1 - done);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			ret = -errno;
			break;
		}
		if (r == 0)
			break;
		done += (size_t)r;
	}

	/* Full: complete only if the file ends here. */
	if (!ret && done == buflen - 1) {
		char more;
		ssize_t r;

		do {
			r = read(fd, &more, 1);
		} while (r < 0 && errno == EINTR);
		if (r < 0)
			ret = -errno;
		else if (r > 0)
			ret = -EOVERFLOW;
	}
	close(fd);

	buf[ret ? 0 : done] = '\0';
	return ret;
}

int jent_kernel_proc(const char *name, char *buf, size_t buflen)
{
	char path[sizeof(JENT_KERNEL_PROC) + 64];

	/* A name below the directory: nothing leading out of it. */
	if (!name || !*name || name[0] == '/' || strstr(name, "..") ||
	    !buf || !buflen)
		return -EINVAL;
	if ((size_t)snprintf(path, sizeof(path), "%s%s", JENT_KERNEL_PROC,
			     name) >= sizeof(path))
		return -ENAMETOOLONG;

	return jent_kernel_file(path, buf, buflen);
}
