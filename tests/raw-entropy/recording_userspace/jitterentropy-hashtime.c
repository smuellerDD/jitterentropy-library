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

#ifdef __linux__
#define _GNU_SOURCE
#endif

/*
 * jent_entropy_collector_alloc_raw() of the absorbed jitterentropy-base.c,
 * for the recording of jitterentropy-record.c below.
 */
#define JENT_RAW_COLLECTOR

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _MSC_VER
#include <io.h>
#define open  _open
#define close _close
#else
#include <unistd.h>
#endif

#ifdef __linux__
#include <sched.h>	/* sched_setaffinity() for --cpu */
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>		/* QoS classes for --e-cores and --p-cores */
#include <sys/resource.h>	/* PRIO_DARWIN_PROCESS for --p-cores */
#endif

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-sha3.c"
#include "jitterentropy-gcd.c"
#include "jitterentropy-health.c"
#include "jitterentropy-noise.c"
#include "jitterentropy-timer.c"
#include "jitterentropy-base.c"
#include "jitterentropy-uuid.c"
#include "jitterentropy-status.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/* The recording itself, shared with libjitterentropy-record. */
#define JENT_RECORD_STATIC_LIB
#include "../recording_library/jitterentropy-record.c"

#include "jitterentropy-memlock.h"
#include "jitterentropy-options.h"

/*
 * --cpu where jent_record_run() can pin (JENT_HAVE_CPU_PINNING of
 * jitterentropy-record.c), --e-cores and --p-cores where it can select a core
 * type. Each is parsed everywhere, so that passing it yields the reason it is
 * not honored rather than "unknown option".
 */
#ifdef JENT_HAVE_CPU_PINNING
# define JENT_USAGE_CPU		"|--cpu <NUM>"
#else
# define JENT_USAGE_CPU		""
#endif

#ifdef __APPLE__
# define JENT_USAGE_CORES	"|--e-cores|--p-cores"
#else
# define JENT_USAGE_CORES	""
#endif


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
 * --disable-internal-timer Enable flag JENT_DISABLE_INTERNAL_TIMER
 * --force-internal-timer Enable flag JENT_FORCE_INTERNAL_TIMER
 * --osr Apply the given OSR value
 * --loopcnt Apply the given loop count value for the operation (i.e. apply it
 *	     to the respecive used noise source(s))
 * --max-mem Set the memory size of the memory block used for the memory access
 *	     loop
 * --hashloop Perform the measurement of the hash loop only
 * --memaccess Perform the measurement of the memory access loop only
 * --hloopcnt Number of hashloop operations at runtime
 * --cpu Pin the measurement to the given CPU - use this on hybrid CPUs to
 *	 record one core type at a time (see jitterentropy-cpuinfo). Note that
 *	 the internal timer cannot be used together with this option as its
 *	 counting thread requires a CPU of its own.
 * --e-cores Confine the measurement to the efficiency cores. macOS only, where
 *	 there is no CPU pinning and the quality-of-service class of the thread
 *	 is what selects a core type instead.
 * --p-cores Ask for the performance cores - a preference, not a confinement.
 *	 macOS only, and of use where the tool is started with a lower class
 *	 than a shell command carries, which would otherwise take the recording
 *	 onto the efficiency cores unnoticed.
 *
 * --cpu, --e-cores and --p-cores are mutually exclusive.
 */
int main(int argc, char * argv[])
{
	struct jent_record_config config = {
		.cpu = -1,
		.cores = JENT_RECORD_CORES_ANY,
		.source = JENT_RECORD_COMMON,
	};
	unsigned long val;
	int ret;

	/*
	 * Absorbed with the library sources, and used by it in one
	 * configuration of JENT_RANDOM_MEMACCESS or the other only.
	 */
	(void)jent_memaccess_deterministic;
	(void)jent_memaccess_pseudorandom;

	if (argc < 4) {
		printf("%s <rounds per repeat> <number of repeats> <filename> [" JENT_OPTIONS_USAGE "|" JENT_OPTIONS_USAGE_OSR "|--loopcnt <NUM>|--hashloop|--memaccess" JENT_USAGE_CPU JENT_USAGE_CORES "|--status]\n", argv[0]);
		return 1;
	}

	{
		char *endp;

		/*
		 * Reject non-numeric input and zero: rounds feeds
		 * calloc(rounds, ...), and calloc(0, ...) may legally return
		 * NULL, which would be reported as an allocation failure (or
		 * silently record empty data files).
		 */
		config.rounds = strtoul(argv[1], &endp, 10);
		if (endp == argv[1] || *endp != '\0' ||
		    config.rounds == 0 || config.rounds >= UINT_MAX) {
			fprintf(stderr, "Invalid rounds value %s\n", argv[1]);
			return 1;
		}
		argc--;
		argv++;

		config.repeats = strtoul(argv[1], &endp, 10);
		if (endp == argv[1] || *endp != '\0' ||
		    config.repeats == 0 || config.repeats >= UINT_MAX) {
			fprintf(stderr, "Invalid repeats value %s\n", argv[1]);
			return 1;
		}
		argc--;
		argv++;
	}

	config.file = argv[1];
	argc--;
	argv++;

	while (argc > 1) {
		ret = jent_parse_option(&argc, &argv, &config.flags,
					&config.osr);
		if (ret < 0)
			return 1;
		if (ret > 0) {
			/* One of the options all tools share. */
		} else if (!strncmp(argv[1], "--hashloop", 10)) {
			config.source = JENT_RECORD_HASHLOOP;
		} else if (!strncmp(argv[1], "--memaccess", 11)) {
			config.source = JENT_RECORD_MEMACCESS;
		} else if (!strncmp(argv[1], "--loopcnt", 9)) {
			if (jent_option_value(&argc, &argv, "Loop count", &val) ||
			    val >= UINT_MAX)
				return 1;
			config.loopcnt = (unsigned int)val;
		} else if (!strncmp(argv[1], "--cpu", 5)) {
			if (jent_option_value(&argc, &argv, "CPU", &val))
				return 1;
			if (val > INT_MAX) {
				fprintf(stderr, "Invalid CPU value %lu\n", val);
				return 1;
			}
			config.cpu = (int)val;
		} else if (!strncmp(argv[1], "--e-cores", 9)) {
			if (config.cores == JENT_RECORD_CORES_PERFORMANCE) {
				printf("--cpu, --e-cores and --p-cores are mutually exclusive\n");
				return 1;
			}
			config.cores = JENT_RECORD_CORES_EFFICIENCY;
		} else if (!strncmp(argv[1], "--p-cores", 9)) {
			if (config.cores == JENT_RECORD_CORES_EFFICIENCY) {
				printf("--cpu, --e-cores and --p-cores are mutually exclusive\n");
				return 1;
			}
			config.cores = JENT_RECORD_CORES_PERFORMANCE;
		} else if (!strncmp(argv[1], "--status", 8)) {
			config.status = 1;
		} else {
			fprintf(stderr, "Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	/*
	 * The compliance modes require the collector memory to be locked into
	 * RAM, which the operating system permits only within a per-process
	 * limit. Raised once here rather than per repeat, as the limit is
	 * process-wide state. See jitterentropy-memlock.h.
	 */
	if (jent_raise_memlock_limit(config.flags))
		fprintf(stderr,
			"Cannot raise the memory lock limit, allocating the entropy collector may fail\n");

	/*
	 * Likewise the secure memory arena of the external crypto backends,
	 * which is created once for the process and is what the library
	 * allocates the collector from. See jitterentropy-memlock.h.
	 */
	if (jent_init_secure_memory(config.flags))
		fprintf(stderr,
			"Cannot create the secure memory arena, allocating the entropy collector will fail\n");

	/* What the tool prints, the recording reports. */
	{
		size_t len = jent_record_report_len(&config);
		char *report = malloc(len);

		if (!report) {
			fprintf(stderr, "Cannot allocate the report\n");
			return 1;
		}
		ret = jent_record_run(&config, report, len);
		fputs(report, stdout);
		free(report);
	}

	return ret;
}
