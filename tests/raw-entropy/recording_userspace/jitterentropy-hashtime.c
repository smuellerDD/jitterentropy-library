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

#include "jitterentropy-base.c"

/***************************************************************************
 * Statistical test logic not compiled for regular operation
 ***************************************************************************/
static int jent_one_test(const char *pathname, unsigned long rounds,
			 int notime)
{
	unsigned long size = 0;
	struct rand_data *ec = NULL, *ec_min = NULL;
	FILE *out = NULL;
	uint64_t *duration, *duration_min;
	int ret = 0;

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
	if(ret) {
		printf("The initialization failed with error code %d\n", ret);
		goto out;
	}
	ec = jent_entropy_collector_alloc(0, notime ?
					     JENT_FORCE_INTERNAL_TIMER : 0);
	if (!ec) {
		ret = 1;
		goto out;
	}

	ec_min = jent_entropy_collector_alloc(0, notime ?
						 JENT_FORCE_INTERNAL_TIMER : 0);
	if (!ec_min) {
		ret = 1;
		goto out;
	}

	if (notime) {
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

out:
	free(duration);
	free(duration_min);
	if (notime) {
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
	int ret, notime = 0;
	char pathname[4096];

	if (argc != 4 && argc != 5) {
		printf("%s <rounds per repeat> <number of repeats> <filename>\n", argv[0]);
		return 1;
	}

	rounds = strtoul(argv[1], NULL, 10);
	if (rounds >= UINT_MAX)
		return 1;

	repeats = strtoul(argv[2], NULL, 10);
	if (repeats >= UINT_MAX)
		return 1;

	if (argc == 5)
		notime = 1;

	for (i = 1; i <= repeats; i++) {
		snprintf(pathname, sizeof(pathname), "%s-%.4lu.data", argv[3],
			 i);

		ret = jent_one_test(pathname, rounds, notime);

		if (ret)
			return ret;
	}

	return 0;
}
