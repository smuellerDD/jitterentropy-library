/*
 * Copyright (C) 2019 - 2026, Stephan Mueller <smueller@chronox.de>
 *
 * License: see LICENSE file in root directory
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE, ALL OF
 * WHICH ARE HEREBY DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#include "jitterentropy.h"

#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/*
 * Parse a complete numeric option value. A plain strtoul(str, NULL, 10) turns
 * a typo (or a follow-up option consumed as value) into 0 and the tool would
 * silently record with a configuration different from what was requested.
 */
static int parse_ulong(const char *str, unsigned long *val)
{
	char *endptr;

	errno = 0;
	*val = strtoul(str, &endptr, 10);
	if (endptr == str || *endptr != '\0' || errno != 0) {
		printf("Invalid numeric value \"%s\"\n", str);
		return 1;
	}
	return 0;
}

int main(int argc, char * argv[])
{
	unsigned long long size, rounds;
	int ret = 0;
	unsigned int flags = 0, osr = 0;
	struct rand_data *ec_nostir;
	char status[4096];
	int hex = 0;
	size_t i;

	if (argc < 2) {
		printf("%s <number of measurements> [--ntg1|--force-fips|--disable-memory-access|--disable-internal-timer|--force-internal-timer|--all-caches|--osr <OSR>|--max-mem <NUM>|--hloopcnt <NUM>|--hex]\n", argv[0]);
		return 1;
	}

	{
		char *endp;

		/* Reject non-numeric input instead of treating it as 0. */
		rounds = strtoull(argv[1], &endp, 10);
		if (endp == argv[1] || *endp != '\0' || rounds >= ULLONG_MAX) {
			fprintf(stderr, "Invalid rounds value %s\n", argv[1]);
			return 1;
		}
	}
	argc--;
	argv++;

	while (argc > 1) {
		if (!strncmp(argv[1], "--ntg1", 6))
			flags |= JENT_NTG1;
		else if (!strncmp(argv[1], "--force-fips", 12))
			flags |= JENT_FORCE_FIPS;
		else if (!strncmp(argv[1], "--disable-memory-access", 23))
			flags |= JENT_DISABLE_MEMORY_ACCESS;
		else if (!strncmp(argv[1], "--disable-internal-timer", 24))
			flags |= JENT_DISABLE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--force-internal-timer", 22))
			flags |= JENT_FORCE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--all-caches", 12))
			flags |= JENT_CACHE_ALL;
		else if (!strncmp(argv[1], "--osr", 5)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("OSR value missing\n");
				return 1;
			}

			if (parse_ulong(argv[1], &val) || val >= UINT_MAX)
				return 1;
			osr = (unsigned int)val;
		} else if (!strncmp(argv[1], "--max-mem", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Maximum memory value missing\n");
				return 1;
			}

			if (parse_ulong(argv[1], &val))
				return 1;
			switch (val) {
			case 0:
				/* Allow to set no option */
				break;
			case 1:
				flags |= JENT_MAX_MEMSIZE_1kB;
				break;
			case 2:
				flags |= JENT_MAX_MEMSIZE_2kB;
				break;
			case 3:
				flags |= JENT_MAX_MEMSIZE_4kB;
				break;
			case 4:
				flags |= JENT_MAX_MEMSIZE_8kB;
				break;
			case 5:
				flags |= JENT_MAX_MEMSIZE_16kB;
				break;
			case 6:
				flags |= JENT_MAX_MEMSIZE_32kB;
				break;
			case 7:
				flags |= JENT_MAX_MEMSIZE_64kB;
				break;
			case 8:
				flags |= JENT_MAX_MEMSIZE_128kB;
				break;
			case 9:
				flags |= JENT_MAX_MEMSIZE_256kB;
				break;
			case 10:
				flags |= JENT_MAX_MEMSIZE_512kB;
				break;
			case 11:
				flags |= JENT_MAX_MEMSIZE_1MB;
				break;
			case 12:
				flags |= JENT_MAX_MEMSIZE_2MB;
				break;
			case 13:
				flags |= JENT_MAX_MEMSIZE_4MB;
				break;
			case 14:
				flags |= JENT_MAX_MEMSIZE_8MB;
				break;
			case 15:
				flags |= JENT_MAX_MEMSIZE_16MB;
				break;
			case 16:
				flags |= JENT_MAX_MEMSIZE_32MB;
				break;
			case 17:
				flags |= JENT_MAX_MEMSIZE_64MB;
				break;
			case 18:
				flags |= JENT_MAX_MEMSIZE_128MB;
				break;
			case 19:
				flags |= JENT_MAX_MEMSIZE_256MB;
				break;
			case 20:
				flags |= JENT_MAX_MEMSIZE_512MB;
				break;
			default:
				printf("Unknown maximum memory value\n");
				return 1;
			}
		} else if (!strncmp(argv[1], "--hloopcnt", 10)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Hash loop count value missing\n");
				return 1;
			}

			if (parse_ulong(argv[1], &val))
				return 1;
			switch (val) {
			case 0:
				flags |= JENT_HASHLOOP_1;
				break;
			case 1:
				flags |= JENT_HASHLOOP_2;
				break;
			case 2:
				flags |= JENT_HASHLOOP_4;
				break;
			case 3:
				flags |= JENT_HASHLOOP_8;
				break;
			case 4:
				flags |= JENT_HASHLOOP_16;
				break;
			case 5:
				flags |= JENT_HASHLOOP_32;
				break;
			case 6:
				flags |= JENT_HASHLOOP_64;
				break;
			case 7:
				flags |= JENT_HASHLOOP_128;
				break;
			default:
				printf("Unknown hashloop value\n");
				return 1;
			}
		} else if (!strncmp(argv[1], "--hex", 5)) {
			hex = 1;
		} else {
			printf("Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		printf("The initialization failed with error code %d\n", ret);
		return ret;
	}

	ec_nostir = jent_entropy_collector_alloc(osr, flags);
	if (!ec_nostir) {
		printf("Jitter RNG handle cannot be allocated\n");
		return 1;
	}

	if (jent_status(ec_nostir, status, sizeof(status))) {
		printf("Cannot obtain status information\n");
		ret = 1;
		goto out;
	}

	fprintf(stderr, "%s", status);

	for (size = 0; size < rounds; size++) {
		uint8_t tmp[32];

		if (0 > jent_read_entropy_safe(&ec_nostir, (char*)tmp, sizeof(tmp))) {
			fprintf(stderr, "FIPS 140-3 health test failed\n");
			ret = 1;
			goto out;
		}
		/*
		 * Treat output errors as fatal: consumers pipe this data into
		 * files for analysis, and a silently truncated stream with
		 * exit code 0 would be processed as if complete.
		 */
		if (hex) {
			for (i = 0; i < sizeof(tmp); ++i) {
				if (fprintf(stdout, "%02X",
					    (unsigned int)tmp[i]) < 0) {
					fprintf(stderr, "Can't output data\n");
					ret = 1;
					goto out;
				}
			}
		} else {
			if (fwrite(&tmp, sizeof(tmp), 1, stdout) != 1) {
				fprintf(stderr, "Can't output data\n");
				ret = 1;
				goto out;
			}
		}
	}

	/*
	 * Most of the output above only fills the stdio buffer; the final
	 * chunk would be flushed inside exit(), where a write failure (e.g.
	 * ENOSPC) is silently discarded. Flush explicitly so a truncated
	 * stream cannot terminate with exit code 0.
	 */
	if (fflush(stdout) != 0) {
		fprintf(stderr, "Can't output data\n");
		ret = 1;
		goto out;
	}

	ret = 0;

out:
	jent_entropy_collector_free(ec_nostir);
	return ret;
}
