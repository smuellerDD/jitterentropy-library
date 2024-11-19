/*
 * Copyright (C) 2019 - 2024, Stephan Mueller <smueller@chronox.de>
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

#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <assert.h>

#include "jitterentropy.h"

/* Returns the number of nanoseconds per output for the selected flags and osr. */
uint64_t jent_output_time(unsigned int rounds, unsigned int osr, unsigned int flags) 
{
	struct rand_data *ec_nostir;
	struct timespec start, finish;
	uint64_t runtime;
	int ret;

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		fprintf(stderr, "The initialization failed with error code %d\n", ret);
		return ret;
	}

	ec_nostir = jent_entropy_collector_alloc(osr, flags);

	clock_gettime(CLOCK_REALTIME, &start);
	
	if (!ec_nostir) {
		fprintf(stderr, "Jitter RNG handle cannot be allocated\n");
		return 1;
	}

	for (unsigned long size = 0; size < rounds; size++) {
		char tmp[32];

		if (0 > jent_read_entropy_safe(&ec_nostir, tmp, sizeof(tmp))) {
			fprintf(stderr, "FIPS 140-2 continuous test failed\n");
			return 1;
		}
	}

	clock_gettime(CLOCK_REALTIME, &finish);
	runtime = ((uint64_t)finish.tv_sec * UINT64_C(1000000000) + (uint64_t)finish.tv_nsec) - ((uint64_t)start.tv_sec * UINT64_C(1000000000) + (uint64_t)start.tv_nsec);

	jent_entropy_collector_free(ec_nostir);

	return runtime / (uint64_t)rounds;
}

int main(int argc, char * argv[])
{
	unsigned long rounds;
	unsigned int flags = 0;
	char *endtimeparam;
	double timeBoundIn;
	uint64_t timeBound;
	unsigned int maxBound, minBound;

	if (argc < 2) {
		fprintf(stderr, "%s <number of measurements> <target time> [--force-fips|--disable-memory-access|--disable-internal-timer|--force-internal-timer|--max-mem <NUM>]\n", argv[0]);
		return 1;
	}

	rounds = strtoul(argv[1], NULL, 10);
	if ((rounds >= UINT_MAX) || (rounds == 0))
		return 1;
	argc--;
	argv++;

	timeBoundIn = strtod(argv[1], &endtimeparam);
	if ((timeBoundIn <= 0.0) || (*endtimeparam != '\0'))
		return 1;

	//The time bound, expressed as an integer number of nanoseconds.
	timeBound = (uint64_t)floor(timeBoundIn * 1000000000.0);
	argc--;
	argv++;

	while (argc > 1) {
		if (!strncmp(argv[1], "--force-fips", 12))
			flags |= JENT_FORCE_FIPS;
		else if (!strncmp(argv[1], "--disable-memory-access", 23))
			flags |= JENT_DISABLE_MEMORY_ACCESS;
		else if (!strncmp(argv[1], "--disable-internal-timer", 24))
			flags |= JENT_DISABLE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--force-internal-timer", 22))
			flags |= JENT_FORCE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--max-mem", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				fprintf(stderr, "Maximum memory value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			switch (val) {
			case 0:
				/* Allow to set no option */
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
				fprintf(stderr, "Unknown maximum memory value\n");
				return 1;
			}
		} else {
			fprintf(stderr, "Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	/* Verify the first invariant: generation using minBound occurs in less than or equal time than the targeted time. */
	minBound = 1;
	if(jent_output_time(rounds, minBound, flags) > timeBound) {
		fprintf(stderr, "Minimum osr %u exceeds the bound.\n", minBound);
		return 1;
	} else
		printf("Minimum osr=%u invariant verified.\n", minBound);

	/* Locate the maxBound */
	maxBound = 2;
	while(jent_output_time(rounds, maxBound, flags) <= timeBound) {
		minBound = maxBound;
		maxBound = maxBound * 2;
		assert(maxBound > minBound);
	}
	printf("Maximum osr bound %u found.\n", maxBound);

	/* The second invariant is now verified: generation using maxBound occurs in greater time than the targeted time. */

	while(maxBound - minBound > 1) {
		unsigned int curosr;
		assert(maxBound > minBound);
		/* Calculate (minBound + maxBound)/2 without risk of overflow. */
		curosr = minBound + (maxBound - minBound) / 2;
		assert(curosr > minBound);
		assert(curosr < maxBound);

		if(jent_output_time(rounds, curosr, flags) <= timeBound) {
			minBound = curosr;
		} else {
			maxBound = curosr;
		}

		printf("Desired osr bound is in [%u, %u]\n", minBound, maxBound);
	}

	printf("osr bound is %u\n", minBound);
			
	return 0;
}
