/*
 * Copyright (C) 2019 - 2021, Stephan Mueller <smueller@chronox.de>
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

/***************************************************************************
 * Statistical test logic not compiled for regular operation
 ***************************************************************************/
static int jent_one_test(const char *pathname, unsigned long rounds,
			 unsigned int flags, int report_counter_ticks)
{
	unsigned long size = 0;
	struct rand_data *ec = NULL, *ec_min = NULL;
	FILE *out = NULL;
	uint64_t *duration, *duration_min;
	int ret = 0;
	unsigned int health_test_result;

	duration = calloc(rounds, sizeof(uint64_t));
	if (!duration)
		return 1;

	duration_min = calloc(rounds, sizeof(uint64_t));
	if (!duration_min) {
		free(duration);
		return 1;
	}

	printf("Processing %s\n", pathname);

	out = fopen(pathname, "w");
	if (!out) {
		ret = 1;
		goto out;
	}

	ret = jent_entropy_init();
	if (ret) {
		printf("The initialization failed with error code %d\n", ret);
		goto out;
	}
	ec = jent_entropy_collector_alloc(0, flags);
	if (!ec) {
		ret = 1;
		goto out;
	}

	ec_min = jent_entropy_collector_alloc(0, flags);
	if (!ec_min) {
		ret = 1;
		goto out;
	}

	if (!report_counter_ticks) {
		/*
		 * For this analysis, we want the raw values, not values that
		 * have had common factors removed.
		 */
		ec->jent_common_timer_gcd = 1;
		ec_min->jent_common_timer_gcd = 1;
	}

	if (ec->enable_notime) {
		jent_notime_settick(ec);
		jent_notime_settick(ec_min);
	}

	/* Enable full SP800-90B health test handling */
	ec->fips_enabled = 1;
	ec_min->fips_enabled = 1;

	/* Prime the test */
	jent_measure_jitter(ec, 0, NULL);
	for (size = 0; size < rounds; size++) {
		/* Disregard stuck indicator */
		jent_measure_jitter(ec, 0, &duration[size]);
	}

	jent_measure_jitter(ec_min, 0, NULL);
	for (size = 0; size < rounds; size++) {
		/* Disregard stuck indicator */
		jent_measure_jitter(ec_min, 1, &duration_min[size]);
	}

	for (size = 0; size < rounds; size++)
		fprintf(out, "%" PRIu64 " %" PRIu64 "\n", duration[size], duration_min[size]);

	if ((health_test_result = jent_health_failure(ec))) {
		printf("The main context encountered the following health testing failure(s):");
		if (health_test_result & JENT_RCT_FAILURE) printf(" RCT");
		if (health_test_result & JENT_APT_FAILURE) printf(" APT");
		if (health_test_result & JENT_LAG_FAILURE) printf(" Lag");
		printf("\n");
	}

	if ((health_test_result = jent_health_failure(ec_min))) {
		printf("The minimum context encountered the following health testing failure(s):");
		if (health_test_result & JENT_RCT_FAILURE) printf(" RCT");
		if (health_test_result & JENT_APT_FAILURE) printf(" APT");
		if (health_test_result & JENT_LAG_FAILURE) printf(" Lag");
		printf("\n");
	}

out:
	free(duration);
	free(duration_min);
	if (flags & JENT_FORCE_INTERNAL_TIMER) {
		if (ec)
			jent_notime_unsettick(ec);
		if (ec_min)
			jent_notime_unsettick(ec_min);
	}
	if (out)
		fclose(out);

	if (ec)
		jent_entropy_collector_free(ec);
	if (ec_min)
		jent_entropy_collector_free(ec_min);

	return ret;
}

/*
 * Invoke the application with
 *	argv[1]: number of raw entropy measurements to be obtained for one
 *		 entropy collector instance.
 *	argv[2]: number of test repetitions with a new entropy estimator
 *		 allocated for each round - this satisfies the restart tests
 *		 defined in SP800-90B section 3.1.4.3 and FIPS IG 7.18.
 *	argv[3]: File name of the output data
 */
int main(int argc, char * argv[])
{
	unsigned long i, rounds, repeats;
	unsigned int flags = 0;
	int ret;
	char pathname[4096];

	if (argc != 4 && argc != 5 && argc != 6) {
		printf("%s <rounds per repeat> <number of repeats> <filename> <max mem>\n", argv[0]);
		return 1;
	}

	rounds = strtoul(argv[1], NULL, 10);
	if (rounds >= UINT_MAX)
		return 1;

	repeats = strtoul(argv[2], NULL, 10);
	if (repeats >= UINT_MAX)
		return 1;

	if (argc >= 5) {
		unsigned long val = strtoul(argv[4], NULL, 10);

		switch (val) {
		case 0:
			// Allow to set no option
			break;
		case 1:
			flags |= JENT_MAX_MEMSIZE_32kB;
			break;
		case 2:
			flags |= JENT_MAX_MEMSIZE_64kB;
			break;
		case 3:
			flags |= JENT_MAX_MEMSIZE_128kB;
			break;
		case 4:
			flags |= JENT_MAX_MEMSIZE_256kB;
			break;
		case 5:
			flags |= JENT_MAX_MEMSIZE_512kB;
			break;
		case 6:
			flags |= JENT_MAX_MEMSIZE_1MB;
			break;
		case 7:
			flags |= JENT_MAX_MEMSIZE_2MB;
			break;
		case 8:
			flags |= JENT_MAX_MEMSIZE_4MB;
			break;
		case 9:
			flags |= JENT_MAX_MEMSIZE_8MB;
			break;
		case 10:
			flags |= JENT_MAX_MEMSIZE_16MB;
			break;
		case 11:
			flags |= JENT_MAX_MEMSIZE_32MB;
			break;
		case 12:
			flags |= JENT_MAX_MEMSIZE_64MB;
			break;
		case 13:
			flags |= JENT_MAX_MEMSIZE_128MB;
			break;
		case 14:
			flags |= JENT_MAX_MEMSIZE_256MB;
			break;
		case 15:
			flags |= JENT_MAX_MEMSIZE_512MB;
			break;
		default:
			printf("Unknown maximum memory value\n");
			return 1;
		}
	}

	if (argc == 6)
		flags |= JENT_FORCE_INTERNAL_TIMER;

	for (i = 1; i <= repeats; i++) {
		snprintf(pathname, sizeof(pathname), "%s-%.4lu.data", argv[3],
			 i);

		ret = jent_one_test(pathname, rounds, flags,
				    REPORT_COUNTER_TICKS);

		if (ret)
			return ret;
	}

	return 0;
}
