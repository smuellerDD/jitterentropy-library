/*
 * Copyright (C) 2019 - 2022, Stephan Mueller <smueller@chronox.de>
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
#include <time.h>
#include <sysexits.h>

#include "jitterentropy.h"

static uint64_t current_nanoseconds() {
        uint64_t tmp = 0;
        struct timespec time;
        if (clock_gettime(CLOCK_REALTIME, &time) == 0)
        {
                tmp = ((uint64_t)time.tv_sec & 0xFFFFFFFF) * 1000000000UL;
                tmp = tmp + (uint64_t)time.tv_nsec;
        }
        return tmp;
}

int main(int argc, char * argv[])
{
	unsigned long size, rounds;
	int ret = 0;
	unsigned int flags = 0, osr = 0;
	struct rand_data *ec;
	ssize_t ent_return;

	if (argc < 2) {
		fprintf(stderr, "%s <number of measurements> [--force-fips|--disable-internal-timer|--force-internal-timer|--osr <OSR>|--max-mem <NUM>]\n", argv[0]);
		return 1;
	}

	rounds = strtoul(argv[1], NULL, 10);
	if (rounds >= UINT_MAX)
		return 1;
	argc--;
	argv++;

	while (argc > 1) {
		if (!strncmp(argv[1], "--force-fips", 12))
			flags |= JENT_FORCE_FIPS;
		else if (!strncmp(argv[1], "--disable-internal-timer", 24))
			flags |= JENT_DISABLE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--force-internal-timer", 22))
			flags |= JENT_FORCE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--osr", 5)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				fprintf(stderr, "OSR value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			if (val >= UINT_MAX)
				return 1;
			osr = (unsigned int)val;
		} else if (!strncmp(argv[1], "--max-mem", 9)) {
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
				flags |= JENT_MEMSIZE_64kB;
				break;
			case 2:
				flags |= JENT_MEMSIZE_128kB;
				break;
			case 3:
				flags |= JENT_MEMSIZE_256kB;
				break;
			case 4:
				flags |= JENT_MEMSIZE_512kB;
				break;
			case 5:
				flags |= JENT_MEMSIZE_1MB;
				break;
			case 6:
				flags |= JENT_MEMSIZE_2MB;
				break;
			case 7:
				flags |= JENT_MEMSIZE_4MB;
				break;
			case 8:
				flags |= JENT_MEMSIZE_8MB;
				break;
			case 9:
				flags |= JENT_MEMSIZE_16MB;
				break;
			case 10:
				flags |= JENT_MEMSIZE_32MB;
				break;
			case 11:
				flags |= JENT_MEMSIZE_64MB;
				break;
			case 12:
				flags |= JENT_MEMSIZE_128MB;
				break;
			case 13:
				flags |= JENT_MEMSIZE_256MB;
				break;
			case 14:
				flags |= JENT_MEMSIZE_512MB;
				break;
			case 15:
				flags |= JENT_MEMSIZE_1024MB;
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

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		fprintf(stderr, "jent_entropy_init_ex() failed with error code %d\n", ret);
		return ret;
	}

	ec = jent_entropy_collector_alloc(osr, flags);
	if (!ec) {
		fprintf(stderr, "Jitter RNG handle cannot be allocated\n");
		return 1;
	}

        fprintf(stderr, "Bytes of memory: 2^%u\n", ec->memsize_exp);
        fprintf(stderr, "Memory depth: 2^%u\n", JENT_MEMORY_DEPTH_EXP);
	fprintf(stderr, "gcd: %zu\n", ec->jent_common_timer_gcd);
        fprintf(stderr, "osr: %u\n", ec->osr);


	volatile char *tmp = calloc(rounds, 32);
	if(tmp == NULL) {
		fprintf(stderr, "Can't allocate output buffer.\n");
		exit(EX_OSERR);
	}

	volatile uint64_t startTime = current_nanoseconds();

	for (size = 0; size < rounds; size++) {
		if (0 > (ent_return = jent_read_entropy(ec, (char *)(tmp+32*size), 32))) {
			switch(ent_return) {
				case -1:
					fprintf(stderr, "Invalid entropy collector context\n");
					break;
				case -2:
					fprintf(stderr, "RCT Failure\n");
					break;
				case -3:
					fprintf(stderr, "APT Failure\n");
					break;
				case -4:
					fprintf(stderr, "Set tick Failure\n");
					break;
				case -5:
					fprintf(stderr, "LAG Failure\n");
					break;
				case -6:
					fprintf(stderr, "DIST Failure\n");
					break;
				default:
					fprintf(stderr, "Not really sure what just happened.\n");
			}
			return 1;
		}
	}

	volatile uint64_t endTime = current_nanoseconds();

	if(fwrite((void *)tmp, 32, rounds, stdout) != rounds) {
		fprintf(stderr, "Can't write data\n");
		exit(EX_OSERR);
	}

	uint64_t dist_count = ec->in_dist_count_history + ec->current_in_dist_count;
	uint64_t total_count = ec->data_count_history + ec->current_data_count;
	if(total_count > 0) {
		fprintf(stderr, "%zu / %zu (%g %%) samples in reference distribution\n", dist_count, total_count, 100.0 * (double)dist_count/((double)total_count));
	}

	fprintf(stderr, "Produced %zu outputs in %zu ns (%g outputs / s)\n", rounds, endTime-startTime, (double)rounds*1.0E9/((double)(endTime-startTime)));

	jent_entropy_collector_free(ec);

	return 0;
}
