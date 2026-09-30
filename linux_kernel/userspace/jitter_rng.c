/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * jitter_rng: information about the jitter_rng kernel module, and random
 * bytes from it, through libjitterentropy-kernel.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jitterentropy-kernel.h"

static void usage(const char *prog)
{
	fprintf(stderr,
		"%s [-d <device>] [-i|-s|-t|-r <bytes> [-x]]\n"
		"  -i, --info            the module and an instance of it (default)\n"
		"  -s, --status          the JSON status document of an instance\n"
		"  -r, --random <bytes>  write random bytes to stdout\n"
		"  -x, --hex             as hexadecimal digits\n"
		"  -t, --selftest        run the cryptographic self test\n"
		"  -d, --device <device> instead of " JENT_KERNEL_DEVICE "\n",
		prog);
}

/* One line of /proc/jitterentropy/@name, or why there is none. */
static void info_proc(const char *label, const char *name)
{
	char buf[128];
	int ret = jent_kernel_proc(name, buf, sizeof(buf));

	if (ret) {
		printf("  %-19s unavailable (%s)\n", label, strerror(-ret));
		return;
	}
	buf[strcspn(buf, "\n")] = '\0';
	printf("  %-19s %s\n", label, buf);
}

static void info_u32(const char *label, const char *fmt, int ret,
		     unsigned int val)
{
	printf("  %-19s ", label);
	if (ret)
		printf("unavailable (%s)", strerror(-ret));
	else
		printf(fmt, val);
	putchar('\n');
}

static int info(const char *device)
{
	static const char *const interfaces[] = {
		"kcapi", "hwrng", "chardev", "testing",
	};
	char buf[JENT_KERNEL_UUID_STRLEN];
	uint64_t reads, bytes;
	unsigned int val;
	size_t i, memsize;
	int fd, ret;

	printf("Module\n");
	info_proc("version:", "version");
	info_proc("oversampling rate:", "config/osr");
	info_proc("flags:", "config/flags_raw");
	info_proc("NTG.1 mode:", "config/ntg1");
	info_proc("FIPS mode:", "config/fips");
	printf("  %-19s", "interfaces:");
	for (i = 0; i < sizeof(interfaces) / sizeof(interfaces[0]); i++) {
		char name[32], on[8];

		snprintf(name, sizeof(name), "interfaces/%s", interfaces[i]);
		if (!jent_kernel_proc(name, on, sizeof(on)) && on[0] == '1')
			printf(" %s", interfaces[i]);
	}
	putchar('\n');

	fd = jent_kernel_open(device);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n",
			device ? device : JENT_KERNEL_DEVICE, strerror(-fd));
		return 1;
	}

	printf("Instance\n");
	ret = jent_kernel_uuid(fd, buf, sizeof(buf));
	printf("  %-19s %s\n", "uuid:", ret ? strerror(-ret) : buf);
	ret = jent_kernel_version(fd, &val);
	printf("  %-19s ", "version:");
	if (ret)
		printf("unavailable (%s)\n", strerror(-ret));
	else
		printf("%u.%u.%u\n", val / 1000000, val / 10000 % 100,
		       val / 100 % 100);
	ret = jent_kernel_osr(fd, &val);
	info_u32("oversampling rate:", "%u", ret, val);
	ret = jent_kernel_flags(fd, &val);
	info_u32("flags:", "0x%08x", ret, val);
	ret = jent_kernel_memsize(fd, &memsize);
	printf("  %-19s ", "memory size:");
	if (ret)
		printf("unavailable (%s)\n", strerror(-ret));
	else
		printf("%zu bytes\n", memsize);
	ret = jent_kernel_hashloops(fd, &val);
	info_u32("hash loops:", "%u", ret, val);
	ret = jent_kernel_health_failure(fd, &val);
	info_u32("health failure:", "0x%x", ret, val);
	ret = jent_kernel_reinitializations(fd, &val);
	info_u32("reinitializations:", "%u", ret, val);
	ret = jent_kernel_output(fd, &reads, &bytes);
	printf("  %-19s ", "output:");
	if (ret)
		printf("unavailable (%s)\n", strerror(-ret));
	else
		printf("%" PRIu64 " reads, %" PRIu64 " bytes\n", reads, bytes);

	jent_kernel_close(fd);
	return 0;
}

static int status(int fd)
{
	char buf[JENT_KERNEL_STATUS_LEN];
	int ret = jent_kernel_status(fd, buf, sizeof(buf));

	if (ret) {
		fprintf(stderr, "Cannot obtain the status: %s\n",
			strerror(-ret));
		return 1;
	}
	fputs(buf, stdout);
	return 0;
}

static int selftest(int fd)
{
	int ret = jent_kernel_selftest(fd);

	if (ret) {
		fprintf(stderr, "Cryptographic self test: %s\n",
			strerror(-ret));
		return 1;
	}
	puts("cryptographic self test passed");
	return 0;
}

static int random_bytes(int fd, unsigned long long len, int hex)
{
	uint8_t buf[4096];

	while (len) {
		size_t want = len < sizeof(buf) ? (size_t)len : sizeof(buf);
		ssize_t got = jent_kernel_read(fd, buf, want);
		ssize_t i;

		if (got <= 0) {
			fprintf(stderr, "Reading random data failed: %s\n",
				got ? strerror((int)-got) : "end of file");
			return 1;
		}
		if (hex) {
			for (i = 0; i < got; i++)
				printf("%02x", buf[i]);
		} else if (fwrite(buf, 1, (size_t)got, stdout) !=
			   (size_t)got) {
			break;
		}
		len -= (unsigned long long)got;
	}
	if (hex && !ferror(stdout))
		putchar('\n');

	/* A truncated stream must not exit with 0. */
	if (ferror(stdout) || fflush(stdout)) {
		fprintf(stderr, "Cannot write the random data\n");
		return 1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "info", no_argument, NULL, 'i' },
		{ "status", no_argument, NULL, 's' },
		{ "random", required_argument, NULL, 'r' },
		{ "hex", no_argument, NULL, 'x' },
		{ "selftest", no_argument, NULL, 't' },
		{ "device", required_argument, NULL, 'd' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	const char *device = NULL;
	unsigned long long len = 0;
	int mode = 'i', hex = 0;
	int c, fd, ret;

	while ((c = getopt_long(argc, argv, "isr:xtd:h", opts, NULL)) != -1) {
		char *end;

		switch (c) {
		case 'i':
		case 's':
		case 't':
			mode = c;
			break;
		case 'r':
			mode = c;
			errno = 0;
			len = strtoull(optarg, &end, 10);
			if (errno || end == optarg || *end || *optarg == '-') {
				fprintf(stderr, "Invalid byte count %s\n",
					optarg);
				return 1;
			}
			break;
		case 'x':
			hex = 1;
			break;
		case 'd':
			device = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	if (mode == 'i')
		return info(device);

	fd = jent_kernel_open(device);
	if (fd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n",
			device ? device : JENT_KERNEL_DEVICE, strerror(-fd));
		return 1;
	}

	if (mode == 's')
		ret = status(fd);
	else if (mode == 't')
		ret = selftest(fd);
	else
		ret = random_bytes(fd, len, hex);

	jent_kernel_close(fd);
	return ret;
}
