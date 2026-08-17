/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test tool for the single-field ioctls of the Jitter RNG kernel interfaces.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * Calls all seven single-field ioctls and checks each answer against the same
 * field of the JENT_IOCSTATUS document. They are two spellings of one state,
 * so a disagreement is the defect this looks for.
 *
 * Serves both interfaces: an empty "uuid" in the document is what selects the
 * -ENODATA expectation for JENT_IOCUUID on a raw test instance.
 *
 * Usage: jitterentropy-chardev-fields [<device file>]
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* For the JENT_* flag and health failure bits the JSON is checked against. */
#include "jitterentropy.h"
#include "jitterentropy_uapi.h"

static int failures;

static void fail(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));

static void fail(const char *fmt, ...)
{
	va_list ap;

	fputs("FAIL: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	failures++;
}

/*
 * jent_status() has a fixed layout, so these are string searches, not a JSON
 * parse. Each takes a region, which is how the repeated key names inside the
 * healthFailure and output objects are told apart.
 */

/* The text after "<key>": within [hay, end), or NULL. */
static const char *find_value(const char *hay, const char *end, const char *key)
{
	char pattern[64];
	const char *p;

	snprintf(pattern, sizeof(pattern), "\"%s\":", key);
	p = strstr(hay, pattern);
	if (!p || (end && p >= end))
		return NULL;

	p += strlen(pattern);
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/* The body of the object "<key>": { ... }, as a [start, end) pair. */
static int find_object(const char *hay, const char *key,
		       const char **start, const char **end)
{
	const char *p = find_value(hay, NULL, key);
	int depth = 0;

	if (!p || *p != '{')
		return -1;

	*start = p;
	for (; *p; p++) {
		if (*p == '{')
			depth++;
		else if (*p == '}' && --depth == 0) {
			*end = p;
			return 0;
		}
	}
	return -1;
}

static int json_u64(const char *hay, const char *end, const char *key,
		    uint64_t *out)
{
	const char *p = find_value(hay, end, key);

	if (!p || *p < '0' || *p > '9')
		return -1;
	*out = strtoull(p, NULL, 10);
	return 0;
}

static int json_bool(const char *hay, const char *end, const char *key,
		     int *out)
{
	const char *p = find_value(hay, end, key);

	if (!p)
		return -1;
	if (!strncmp(p, "true", 4))
		*out = 1;
	else if (!strncmp(p, "false", 5))
		*out = 0;
	else
		return -1;
	return 0;
}

static int json_str(const char *hay, const char *end, const char *key,
		    char *buf, size_t buflen)
{
	const char *p = find_value(hay, end, key);
	const char *q;

	if (!p || *p != '"')
		return -1;
	p++;
	q = strchr(p, '"');
	if (!q || (size_t)(q - p) >= buflen)
		return -1;
	memcpy(buf, p, (size_t)(q - p));
	buf[q - p] = '\0';
	return 0;
}

/* One JSON boolean against one bit of a mask the ioctl reported. */
static void check_bit(const char *json, const char *end, const char *key,
		      uint32_t mask, uint32_t bit, const char *what)
{
	int json_set;

	if (json_bool(json, end, key, &json_set)) {
		fail("%s: no \"%s\" in the status document", what, key);
		return;
	}
	if (json_set != !!(mask & bit))
		fail("%s: %s is %s in the status document but %s in the ioctl",
		     what, key, json_set ? "true" : "false",
		     (mask & bit) ? "set" : "clear");
}

/* Fetch the status document; the caller frees it. */
static char *get_status(int fd)
{
	struct jent_status_ioctl status = { .buf = 0, .length = 0 };
	char *buf;

	if (ioctl(fd, JENT_IOCSTATUS, &status) == 0 || errno != EOVERFLOW) {
		perror("JENT_IOCSTATUS (probe)");
		return NULL;
	}
	if (!status.length || status.length > JENT_STATUS_MAX_LEN) {
		fprintf(stderr, "implausible required length %u\n",
			status.length);
		return NULL;
	}

	buf = calloc(1, status.length);
	if (!buf) {
		perror("calloc");
		return NULL;
	}

	status.buf = (uintptr_t)buf;
	if (ioctl(fd, JENT_IOCSTATUS, &status)) {
		perror("JENT_IOCSTATUS");
		free(buf);
		return NULL;
	}
	return buf;
}

int main(int argc, char *argv[])
{
	const char *devfile = "/dev/jitterentropy";
	const char *obj, *obj_end;
	struct jent_output_ioctl output;
	struct jent_uuid_ioctl uuid;
	char json_uuid[JENT_UUID_IOCTL_LEN];
	char *json;
	uint64_t n;
	uint32_t val;
	unsigned int maj, min, patch;
	int fd, have_uuid;

	if (argc > 1)
		devfile = argv[1];

	fd = open(devfile, O_RDONLY);
	if (fd < 0) {
		perror(devfile);
		return EXIT_FAILURE;
	}

	json = get_status(fd);
	if (!json) {
		close(fd);
		return EXIT_FAILURE;
	}

	/* A property of the library; the document spells it out with dots. */
	if (ioctl(fd, JENT_IOCVERSION, &val)) {
		fail("JENT_IOCVERSION: %s", strerror(errno));
	} else {
		char ver[32];

		printf("version:          %u\n", val);
		if (json_str(json, NULL, "version", ver, sizeof(ver)) ||
		    sscanf(ver, "%u.%u.%u", &maj, &min, &patch) != 3)
			fail("no usable \"version\" in the status document");
		else if (val != maj * 1000000 + min * 10000 + patch * 100)
			fail("version: ioctl says %u, status document says %s",
			     val, ver);
	}

	/* Present exactly when the instance went through startup. */
	have_uuid = !json_str(json, NULL, "uuid", json_uuid,
			      sizeof(json_uuid)) && json_uuid[0];
	if (ioctl(fd, JENT_IOCUUID, &uuid)) {
		if (have_uuid)
			fail("JENT_IOCUUID: %s, but the status document has "
			     "\"%s\"", strerror(errno), json_uuid);
		else if (errno != ENODATA)
			fail("JENT_IOCUUID on an instance with no UUID: "
			     "expected ENODATA, got %s", strerror(errno));
		else
			printf("uuid:             none (ENODATA, as expected "
			       "for a raw instance)\n");
	} else {
		printf("uuid:             %.*s\n", JENT_UUID_IOCTL_LEN,
		       (char *)uuid.uuid);
		if (!have_uuid)
			fail("JENT_IOCUUID answered but the status document "
			     "carries no UUID");
		else if (strncmp((char *)uuid.uuid, json_uuid,
				 sizeof(json_uuid)))
			fail("uuid: ioctl says \"%.*s\", status document says "
			     "\"%s\"", JENT_UUID_IOCTL_LEN, (char *)uuid.uuid,
			     json_uuid);
	}

	/* JENT_IOCOSR against configuration.osr. */
	if (ioctl(fd, JENT_IOCOSR, &val)) {
		fail("JENT_IOCOSR: %s", strerror(errno));
	} else {
		printf("osr:              %u\n", val);
		if (json_u64(json, NULL, "osr", &n))
			fail("no \"osr\" in the status document");
		else if (n != val)
			fail("osr: ioctl says %u, status document says %"
			     PRIu64, val, n);
	}

	/* JENT_IOCREINIT against reinitializations. */
	if (ioctl(fd, JENT_IOCREINIT, &val)) {
		fail("JENT_IOCREINIT: %s", strerror(errno));
	} else {
		printf("reinitializations: %u\n", val);
		if (json_u64(json, NULL, "reinitializations", &n))
			fail("no \"reinitializations\" in the status document");
		else if (n != val)
			fail("reinitializations: ioctl says %u, status "
			     "document says %" PRIu64, val, n);
	}

	/* configuration.flags reports the same value one bit at a time. */
	if (ioctl(fd, JENT_IOCFLAGS, &val)) {
		fail("JENT_IOCFLAGS: %s", strerror(errno));
	} else {
		printf("flags:            0x%08x\n", val);
		if (find_object(json, "flags", &obj, &obj_end)) {
			fail("no \"flags\" object in the status document");
		} else {
			check_bit(obj, obj_end, "JENT_DISABLE_MEMORY_ACCESS",
				  val, JENT_DISABLE_MEMORY_ACCESS, "flags");
			check_bit(obj, obj_end, "JENT_FORCE_INTERNAL_TIMER",
				  val, JENT_FORCE_INTERNAL_TIMER, "flags");
			check_bit(obj, obj_end, "JENT_DISABLE_INTERNAL_TIMER",
				  val, JENT_DISABLE_INTERNAL_TIMER, "flags");
			check_bit(obj, obj_end, "JENT_FORCE_FIPS",
				  val, JENT_FORCE_FIPS, "flags");
			check_bit(obj, obj_end, "JENT_NTG1",
				  val, JENT_NTG1, "flags");
			check_bit(obj, obj_end, "JENT_CACHE_ALL",
				  val, JENT_CACHE_ALL, "flags");
			check_bit(obj, obj_end, "JENT_FORCE_SECURE_MEM",
				  val, JENT_FORCE_SECURE_MEM, "flags");
		}
	}

	/* healthFailure's per-test booleans are the same bits. */
	if (ioctl(fd, JENT_IOCHEALTH, &val)) {
		fail("JENT_IOCHEALTH: %s", strerror(errno));
	} else {
		static const struct {
			const char *object;
			uint32_t bit;
		} tests[] = {
			{ "apt",	JENT_APT_FAILURE },
			{ "rct",	JENT_RCT_FAILURE },
			{ "rctMemory",	JENT_RCT_MEM_FAILURE },
			{ "lag",	JENT_LAG_FAILURE },
		};
		size_t i;

		printf("healthFailure:    0x%08x\n", val);
		for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
			/* The lag predictor is compiled out of some builds. */
			if (find_object(json, tests[i].object, &obj, &obj_end)) {
				if (tests[i].bit != JENT_LAG_FAILURE)
					fail("no \"%s\" object in the status "
					     "document", tests[i].object);
				continue;
			}
			check_bit(obj, obj_end, "intermittent", val,
				  tests[i].bit, tests[i].object);
			check_bit(obj, obj_end, "permanent", val,
				  JENT_PERMANENT_FAILURE(tests[i].bit),
				  tests[i].object);
		}
	}

	/* Nothing is read in between, so the two must agree exactly. */
	if (ioctl(fd, JENT_IOCOUTPUT, &output)) {
		fail("JENT_IOCOUTPUT: %s", strerror(errno));
	} else {
		/* Cast, not PRIu64: __u64 is unsigned long long. */
		printf("output:           %llu invocations, %llu bytes\n",
		       (unsigned long long)output.invocations,
		       (unsigned long long)output.bytes);
		if (find_object(json, "output", &obj, &obj_end)) {
			fail("no \"output\" object in the status document");
		} else {
			if (json_u64(obj, obj_end, "invocations", &n) ||
			    n != output.invocations)
				fail("output.invocations: ioctl says %llu",
				     (unsigned long long)output.invocations);
			if (json_u64(obj, obj_end, "bytes", &n) ||
			    n != output.bytes)
				fail("output.bytes: ioctl says %llu",
				     (unsigned long long)output.bytes);
		}
	}

	/*
	 * The self test is module-wide rather than a field of this instance,
	 * so the status document has nothing to compare it against. What is
	 * checked is the privilege rule: it runs for a caller holding
	 * CAP_SYS_ADMIN and is refused to one without it. euid 0 stands in for
	 * the capability - a non-root caller that holds it through a file
	 * capability would see this check fail, which is not how this tool is
	 * run.
	 */
	if (geteuid()) {
		if (ioctl(fd, JENT_IOCSELFTEST) != -1 || errno != EPERM)
			fail("JENT_IOCSELFTEST: not refused with EPERM although unprivileged");
		else
			puts("selfTest:         refused without CAP_SYS_ADMIN");
	} else if (ioctl(fd, JENT_IOCSELFTEST)) {
		fail("JENT_IOCSELFTEST: %s", strerror(errno));
	} else {
		puts("selfTest:         passed");
	}

	/* An unimplemented command of the same magic must be rejected. */
	if (ioctl(fd, _IOR(JENT_IOC_MAGIC, 0x7f, __u32), &val) != -1 ||
	    errno != ENOTTY)
		fail("an unimplemented ioctl was not rejected with ENOTTY");

	free(json);
	close(fd);

	if (failures) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	puts("all fields agree with the status document");
	return EXIT_SUCCESS;
}
