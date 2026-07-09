/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test tool for the /dev/jitterentropy ioctl interface.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 *
 * Fetches the JSON status string of the Jitter RNG instance bound to the
 * open file description via JENT_IOCSTATUS and prints it to stdout. The
 * required buffer size is probed with a zero-length buffer first, which
 * exercises the EOVERFLOW path of the ioctl and its length report.
 *
 * Usage: jitterentropy-chardev-status [<device file>]
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "jitterentropy_uapi.h"

int main(int argc, char *argv[])
{
	const char *devfile = "/dev/jitterentropy";
	struct jent_status_ioctl status = { .buf = 0, .length = 0 };
	char *buf = NULL;
	uint32_t required;
	int fd, ret = EXIT_FAILURE;

	if (argc > 1)
		devfile = argv[1];

	fd = open(devfile, O_RDONLY);
	if (fd < 0) {
		perror(devfile);
		return EXIT_FAILURE;
	}

	/* A zero-length buffer must fail with EOVERFLOW ... */
	if (ioctl(fd, JENT_IOCSTATUS, &status) == 0) {
		fprintf(stderr,
			"JENT_IOCSTATUS with zero-length buffer succeeded\n");
		goto out;
	}
	if (errno != EOVERFLOW) {
		perror("JENT_IOCSTATUS (probe)");
		goto out;
	}

	/* ... and report the required size including the terminating NUL. */
	required = status.length;
	if (!required || required > JENT_STATUS_MAX_LEN) {
		fprintf(stderr, "implausible required length %u\n", required);
		goto out;
	}

	buf = calloc(1, required);
	if (!buf) {
		perror("calloc");
		goto out;
	}

	status.buf = (uintptr_t)buf;
	status.length = required;
	if (ioctl(fd, JENT_IOCSTATUS, &status)) {
		perror("JENT_IOCSTATUS");
		goto out;
	}

	if (status.length != required ||
	    strnlen(buf, required) != (size_t)required - 1) {
		fprintf(stderr,
			"inconsistent status length %u (probed %u)\n",
			status.length, required);
		goto out;
	}

	puts(buf);
	ret = EXIT_SUCCESS;

out:
	free(buf);
	close(fd);
	return ret;
}
