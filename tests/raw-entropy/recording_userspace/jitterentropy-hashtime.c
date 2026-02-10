/*
 * Copyright (C) 2019 - 2025, Stephan Mueller <smueller@chronox.de>
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

#include <inttypes.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "jitterentropy-sha3.c"
#include "jitterentropy-gcd.c"
#include "jitterentropy-health.c"
#include "jitterentropy-noise.c"
#include "jitterentropy-timer.c"
#include "jitterentropy-base.c"

#ifndef REPORT_COUNTER_TICKS
#define REPORT_COUNTER_TICKS 1
#endif

enum jent_es {
	jent_common,		/* Common entropy source */
	jent_hashloop,		/* SHA3 loop exclusively */
	jent_memaccess_loop,	/* Memory access loop exclusively */
};

/***************************************************************************
 * Statistical test logic not compiled for regular operation
 ***************************************************************************/
static int jent_one_test(const char *pathname, unsigned long rounds,
			 unsigned int flags, unsigned int osr,
			 enum jent_es jent_es, unsigned int loopcnt,
			 int report_counter_ticks)
{
	unsigned long size = 0;
	struct rand_data *ec = NULL;
	uint64_t *duration;
#ifdef JENT_TEST_BINARY_OUTPUT
	size_t recordsWritten;
#endif

	FILE *out = NULL;
	int ret = 0;
	unsigned int health_test_result;

	duration = calloc(rounds, sizeof(uint64_t));
	if (!duration)
		return 1;

	printf("Processing %s\n", pathname);

	out = fopen(pathname, "w");
	if (!out) {
		ret = 1;
		goto out;
	}

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		printf("The initialization failed with error code %d\n", ret);
		goto out;
	}
	ec = jent_entropy_collector_alloc(osr, flags);
	if (!ec) {
		ret = 1;
		goto out;
	}

	if (!report_counter_ticks) {
		/*
		 * For this analysis, we want the raw values, not values that
		 * have had common factors removed.
		 */
		ec->jent_common_timer_gcd = 1;
	}

	if (ec->enable_notime) {
		jent_notime_settick(ec);
	}

	/* Enable full SP800-90B health test handling */
	ec->fips_enabled = 1;

	/* Print the size of the memory region. */
#ifdef JENT_RANDOM_MEMACCESS
	(void)jent_memaccess_deterministic;
	printf("Random memory access - Memory size: %" PRIu32 "\n",
	       ec->memmask + 1);
#else
	(void)jent_memaccess_pseudorandom;
	printf("Deterministic memory access - Memory size: %" PRIu32 "\n",
	       ec->memmask + 1);
#endif


	/* Prime the test */
	if (jent_es == jent_common)
		jent_measure_jitter(ec, 0, NULL);
	for (size = 0; size < rounds; size++) {
		/* Disregard stuck indicator */
		switch (jent_es) {
		case jent_hashloop:
			jent_measure_jitter_ntg1_sha3(ec, loopcnt,
						      &duration[size]);
			break;
		case jent_memaccess_loop:
			jent_measure_jitter_ntg1_memaccess(ec, loopcnt,
							   &duration[size]);
			break;
		case jent_common:
		default:
			jent_measure_jitter(ec, loopcnt, &duration[size]);
			break;
		}
	}

#ifdef JENT_TEST_BINARY_OUTPUT
	recordsWritten = fwrite(duration, sizeof(uint64_t), rounds, out);
	if(recordsWritten != rounds) fprintf(stderr, "Can't output data.\n");
	#else
	for (size = 0; size < rounds; size++)
		fprintf(out, "%" PRIu64 "\n", duration[size]);
#endif

	if ((health_test_result = jent_health_failure(ec))) {
		printf("The main context encountered the following health testing failure(s):");
		if (health_test_result & JENT_RCT_FAILURE) printf(" RCT");
		if (health_test_result & JENT_APT_FAILURE) printf(" APT");
		if (health_test_result & JENT_LAG_FAILURE) printf(" Lag");
		printf("\n");
	}

out:
	free(duration);
	if (flags & JENT_FORCE_INTERNAL_TIMER) {
		if (ec)
			jent_notime_unsettick(ec);
	}
	if (out)
		fclose(out);

	if (ec)
		jent_entropy_collector_free(ec);

	return ret;
}

/*
 * Invoke the application.
 *
 * The options allowed for this application are as follows:
 *
 * <rounds per repeat> Number of raw values generated after one reset
 * <number of repeats> Number of resets after one set of data generation is
 * complete (used to generate the SP800-90B restart data matrix)
 * <filename> File to store the output data in
 * --ntg1 Enable flag JENT_NTG1
 * --force-fips Enable flag JENT_FORCE_FIPS
 * --disable-memory-access Enable flag JENT_DISABLE_MEMORY_ACCESS
 * --disable-internal-timer Enable flag JENT_FORCE_INTERNAL_TIMER
 * --force-internal-timer Enable flag JENT_FORCE_INTERNAL_TIMER
 * --osr Apply the given OSR value
 * --loopcnt Apply the given loop count value for the operation (i.e. apply it
 *	     to the respecive used noise source(s))
 * --max-mem Set the memory size of the memory block used for the memory access
 *	     loop
 * --hashloop Perform the measurement of the hash loop only
 * --memaccess Perform the measurement of the memory access loop only
 */
int main(int argc, char * argv[])
{
	const char *file;
	unsigned long i, rounds, repeats;
	unsigned int flags = 0, osr = 0, loopcnt = 0;
	enum jent_es jent_es = jent_common;
	int ret;
	char pathname[4096];

	if (argc < 4) {
		printf("%s <rounds per repeat> <number of repeats> <filename> [--ntg1|--force-fips|--disable-memory-access|--disable-internal-timer|--force-internal-timer|--osr <OSR>|--loopcnt <NUM>|--max-mem <NUM>|--hashloop|--memaccess|--all-caches]\n", argv[0]);
		return 1;
	}

	rounds = strtoul(argv[1], NULL, 10);
	if (rounds >= UINT_MAX)
		return 1;
	argc--;
	argv++;

	repeats = strtoul(argv[1], NULL, 10);
	if (repeats >= UINT_MAX)
		return 1;
	argc--;
	argv++;

	file = argv[1];
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
		else if (!strncmp(argv[1], "--hashloop", 10))
			jent_es |= jent_hashloop;
		else if (!strncmp(argv[1], "--memaccess", 11))
			jent_es |= jent_memaccess_loop;
		else if (!strncmp(argv[1], "--osr", 5)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("OSR value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			if (val >= UINT_MAX)
				return 1;
			osr = (unsigned int)val;
		} else if (!strncmp(argv[1], "--loopcnt", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Loop count value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			if (val >= UINT_MAX)
				return 1;
			loopcnt = (unsigned int)val;
		} else if (!strncmp(argv[1], "--max-mem", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Maximum memory value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);;
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
		} else {
			printf("Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	for (i = 1; i <= repeats; i++) {
#if defined(JENT_TEST_BINARY_OUTPUT)
		snprintf(pathname, sizeof(pathname), "%s-%.4lu-u64.bin", file, i);
#else
		snprintf(pathname, sizeof(pathname), "%s-%.4lu.data", file, i);
#endif

		ret = jent_one_test(pathname, rounds, flags, osr, jent_es,
				    loopcnt, REPORT_COUNTER_TICKS);

		if (ret)
			return ret;
	}

	return 0;
}
