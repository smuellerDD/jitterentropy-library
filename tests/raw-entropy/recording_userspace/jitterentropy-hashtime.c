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

#include "jitterentropy-memlock.h"

#ifndef REPORT_COUNTER_TICKS
#define REPORT_COUNTER_TICKS 1
#endif

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

enum jent_es {
	jent_common,		/* Common entropy source */
	jent_hashloop,		/* SHA3 loop exclusively */
	jent_memaccess_loop,	/* Memory access loop exclusively */
};

/*
 * Pin the measuring thread to the given CPU. On a hybrid CPU the timing of the
 * noise sources depends on the core, so a recording is only meaningful for one
 * core type at a time - jitterentropy-cpuinfo says which core is which.
 *
 * The memory block is unaffected - the library sizes it from the largest cache
 * in the system, not from the core it runs on - so --max-mem is what matches
 * it to an efficiency core.
 *
 * The library's portable pinning primitive is compiled with the internal timer
 * only; without it the native affinity call covers Linux alone, and elsewhere
 * the request is rejected rather than measuring an arbitrary core.
 */
static int jent_pin_cpu(unsigned long cpu)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	return jent_thread_pin_to_cpu(cpu);
#elif defined(__linux__)
	cpu_set_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;
	CPU_ZERO(&set);
	CPU_SET((size_t)cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -errno;
	return 0;
#else
	(void)cpu;
	return -ENOSYS;
#endif
}

/*
 * Whether the function above can place the measurement, and so whether --cpu is
 * offered: the library's primitive covers the systems named here and no other,
 * and without it only the Linux fallback is left. The option is parsed where it
 * is not offered, so passing it yields the reason rather than "unknown option".
 */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
# if defined(_MSC_VER) || defined(__MINGW32__) || defined(__linux__) || \
     defined(__FreeBSD__) || defined(__NetBSD__)
#  define JENT_HAVE_CPU_PINNING
# endif
#elif defined(__linux__)
# define JENT_HAVE_CPU_PINNING
#endif

#ifdef JENT_HAVE_CPU_PINNING
# define JENT_USAGE_CPU		"|--cpu <NUM>"
#else
# define JENT_USAGE_CPU		""
#endif

/* Set when --cpu confined the measurement to a single CPU. */
static int jent_cpu_pinned = 0;

/*
 * Ask for the core type the measurement is to run on. macOS has no CPU pinning
 * (see jent_pin_cpu()) but schedules the core types by quality-of-service
 * class: QOS_CLASS_BACKGROUND runs on the E-cores alone and so confines a
 * recording to them, while QOS_CLASS_USER_INTERACTIVE is only a preference,
 * and the class a shell command carries anyway.
 *
 * The class does not lift a background task policy ("taskpolicy -b"), under
 * which the same workload stays at 815 ms against 240 ms in the foreground, so
 * --p-cores clears that first - 244 ms. Other ways of holding a process there
 * remain, hence a request rather than a guarantee.
 *
 * Later threads inherit the class, so the counting thread follows, and as with
 * jent_pin_cpu() --max-mem is what matches the memory block to the E-caches.
 * --e-cores records those cores at the lower clock of the background class; no
 * interface exposes them at their own maximum.
 */
static int jent_select_cores(int performance)
{
#ifdef __APPLE__
	int ret;

	/* Priority zero is what takes the process out of the background. */
	if (performance && setpriority(PRIO_DARWIN_PROCESS, 0, 0))
		return -errno;

	ret = pthread_set_qos_class_self_np(performance ?
					    QOS_CLASS_USER_INTERACTIVE :
					    QOS_CLASS_BACKGROUND, 0);

	/* The call reports the error directly rather than through errno. */
	return ret ? -ret : 0;
#else
	(void)performance;
	return -ENOSYS;
#endif
}

/* Offered where they can be honored, parsed everywhere, as --cpu above. */
#ifdef __APPLE__
# define JENT_USAGE_CORES	"|--e-cores|--p-cores"
#else
# define JENT_USAGE_CORES	""
#endif

/***************************************************************************
 * Statistical test logic not compiled for regular operation
 ***************************************************************************/
static int jent_one_test(const char *pathname, unsigned long rounds,
			 unsigned int flags, unsigned int osr,
			 enum jent_es jent_es, unsigned int loopcnt,
			 int report_counter_ticks,
			 unsigned int status)
{
	unsigned long size = 0;
	struct rand_data *ec = NULL;
	uint64_t *duration;
#ifdef JENT_TEST_BINARY_OUTPUT
	size_t recordsWritten;
#endif
	unsigned int (*measure_jitter)(struct rand_data *ec,
			               uint64_t loop_cnt,
				       uint64_t *ret_current_delta);

	FILE *out = NULL;
	int ret = 0;
	unsigned int health_test_result;

	duration = calloc(rounds, sizeof(uint64_t));
	if (!duration)
		return 1;

	/*
	 * Do not perform the common startup check as the health test may
	 * disable the Jitter RNG. However, as we are in test mode, we
	 * *want* to also know about insufficient entropy.
	 * Thus, only perform the cryptographic self tests and go on.
	 */
#if 0
	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		printf("The initialization failed with error code %d\n", ret);
		goto out;
	}
#else
	jent_entropy_init_common_pre(flags);
#endif

	/*
	 * Use the internal allocation to prevent checking and updating the
	 * OSR, memory size or hash loop count.
	 */
	ec = jent_entropy_collector_alloc_internal(osr, flags);
	if (!ec) {
		printf("Allocation of the entropy collector failed\n");
		/*
		 * The counting thread needs a CPU of its own, and
		 * jent_notime_init() refuses to start with a single-CPU
		 * affinity mask - exactly what --cpu establishes.
		 */
		if (jent_cpu_pinned)
			printf("Note: --cpu leaves one CPU in the affinity mask, which rules out the internal timer\n");
		ret = 1;
		goto out;
	}

	/*
	 * early exit, when status is requested. Can be used to
	 * compare config of measurements with runtime
	 */
	if (status) {
		char status_str[4096];

		ret = jent_status(ec, status_str, sizeof(status_str));
		if (ret) {
			printf("Fetching jent status failed with code: %d\n", ret);
			goto out;
		}
		printf("%s", status_str);
		ret = 0;
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
	ec->is_fips_enabled = 1;

	printf("Processing %s\n", pathname);

	/*
	 * "wb" for the binary variant: Windows opens streams in text mode
	 * otherwise and would expand every 0x0A byte of the recorded
	 * timestamps to 0x0D 0x0A, corrupting the sample file.
	 */
#ifdef JENT_TEST_BINARY_OUTPUT
	out = fopen(pathname, "wb");
#else
	out = fopen(pathname, "w");
#endif
	if (!out) {
		ret = 1;
		goto out;
	}

	/* Print the size of the memory region. */
#ifdef JENT_RANDOM_MEMACCESS
	(void)jent_memaccess_deterministic;
	printf("Random memory access - Memory size: %" PRIu32 " - Hashloop count: %" PRIu32 "\n",
	       ec->memmask + 1,
	       ec->hashloopcnt * (jent_es == jent_common ? 0 : JENT_HASH_LOOP_INIT));
#else
	(void)jent_memaccess_pseudorandom;
	printf("Deterministic memory access - Memory size: %" PRIu32 " - Hashloop count: %" PRIu32 "\n",
	       ec->memmask + 1, ec->hashloopcnt * JENT_HASH_LOOP_INIT);
#endif

	switch (jent_es) {
	case jent_hashloop:
		measure_jitter = jent_measure_jitter_ntg1_sha3;
		break;
	case jent_memaccess_loop:
		measure_jitter = jent_measure_jitter_ntg1_memaccess;
		break;
	case jent_common:
	default:
		measure_jitter = jent_measure_jitter;
		break;
	}

	/* Prime the test */
	if (jent_es == jent_common)
		jent_measure_jitter(ec, 0, NULL);
	for (size = 0; size < rounds; size++) {
		/* Disregard stuck indicator */
		measure_jitter(ec, loopcnt, &duration[size]);
	}

	/*
	 * Treat output errors as fatal for the tool: a silently truncated
	 * sample file would be analyzed by the SP800-90B validation pipeline
	 * as if it were complete.
	 */
#ifdef JENT_TEST_BINARY_OUTPUT
	recordsWritten = fwrite(duration, sizeof(uint64_t), rounds, out);
	if (recordsWritten != rounds) {
		fprintf(stderr, "Can't output data.\n");
		ret = 1;
	}
#else
	for (size = 0; size < rounds; size++) {
		if (fprintf(out, "%" PRIu64 "\n", duration[size]) < 0) {
			fprintf(stderr, "Can't output data.\n");
			ret = 1;
			break;
		}
	}
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

	/* An fclose() error means buffered sample data was lost. */
	if (out && fclose(out) != 0) {
		fprintf(stderr, "Can't close output file.\n");
		ret = 1;
	}

	if (ec) {
		/* checks internally if timer was used, maybe NOOP */
		jent_notime_unsettick(ec);
		jent_entropy_collector_free(ec);
	}

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
	const char *file;
	unsigned long i, rounds, repeats, cpu = 0;
	unsigned int flags = 0, osr = 0, loopcnt = 0;
	unsigned int status = 0;
	int e_cores = 0, p_cores = 0;
	enum jent_es jent_es = jent_common;
	int ret;
	char pathname[4096];

	if (argc < 4) {
		printf("%s <rounds per repeat> <number of repeats> <filename> [--ntg1|--force-fips|--disable-memory-access|--disable-internal-timer|--force-internal-timer|--osr <OSR>|--loopcnt <NUM>|--max-mem <NUM>|--hashloop|--memaccess|--all-caches|--hloopcnt <NUM>" JENT_USAGE_CPU JENT_USAGE_CORES "|--status]\n", argv[0]);
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
		rounds = strtoul(argv[1], &endp, 10);
		if (endp == argv[1] || *endp != '\0' ||
		    rounds == 0 || rounds >= UINT_MAX) {
			fprintf(stderr, "Invalid rounds value %s\n", argv[1]);
			return 1;
		}
		argc--;
		argv++;

		repeats = strtoul(argv[1], &endp, 10);
		if (endp == argv[1] || *endp != '\0' ||
		    repeats == 0 || repeats >= UINT_MAX) {
			fprintf(stderr, "Invalid repeats value %s\n", argv[1]);
			return 1;
		}
		argc--;
		argv++;
	}

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
			jent_es = jent_hashloop;
		else if (!strncmp(argv[1], "--memaccess", 11))
			jent_es = jent_memaccess_loop;
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
		} else if (!strncmp(argv[1], "--loopcnt", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Loop count value missing\n");
				return 1;
			}

			if (parse_ulong(argv[1], &val) || val >= UINT_MAX)
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
		} else if (!strncmp(argv[1], "--cpu", 5)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("CPU value missing\n");
				return 1;
			}

			if (parse_ulong(argv[1], &val))
				return 1;
			cpu = val;
			jent_cpu_pinned = 1;
		} else if (!strncmp(argv[1], "--e-cores", 9)) {
			e_cores = 1;
		} else if (!strncmp(argv[1], "--p-cores", 9)) {
			p_cores = 1;
		} else if (!strncmp(argv[1], "--status", 8)) {
			status = 1;
		} else {
			printf("Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	/* Each names the core to measure on, in ways that cannot be combined. */
	if (jent_cpu_pinned + e_cores + p_cores > 1) {
		printf("--cpu, --e-cores and --p-cores are mutually exclusive\n");
		return 1;
	}

	/*
	 * Before the first initialization, so that the self tests, the memory
	 * allocation and the recording all run on the selected core.
	 */
	if (jent_cpu_pinned) {
		ret = jent_pin_cpu(cpu);
		if (ret) {
			printf("Cannot pin the measurement to CPU %lu: %s\n",
			       cpu, strerror(-ret));
			return 1;
		}
		printf("Measurement pinned to CPU %lu\n", cpu);
	}

	if (e_cores || p_cores) {
		ret = jent_select_cores(p_cores);
		if (ret) {
			printf("Cannot ask for the %s cores: %s\n",
			       p_cores ? "performance" : "efficiency",
			       strerror(-ret));
			return 1;
		}
		/*
		 * Only the efficiency cores are a confinement; the performance
		 * ones are a preference the scheduler is free to leave.
		 */
		if (e_cores)
			printf("Measurement confined to the efficiency cores\n");
		else
			printf("Measurement asked for the performance cores\n");
	}

	/*
	 * The compliance modes require the collector memory to be locked into
	 * RAM, which the operating system permits only within a per-process
	 * limit. Raised once here rather than per repeat below, as the limit is
	 * process-wide state. See jitterentropy-memlock.h.
	 */
	if (jent_raise_memlock_limit(flags))
		fprintf(stderr,
			"Cannot raise the memory lock limit, allocating the entropy collector may fail\n");

	/*
	 * Likewise the secure memory arena of the external crypto backends,
	 * which is created once for the process and is what the library
	 * allocates the collector from. See jitterentropy-memlock.h.
	 */
	if (jent_init_secure_memory(flags))
		fprintf(stderr,
			"Cannot create the secure memory arena, allocating the entropy collector will fail\n");

	for (i = 1; i <= repeats; i++) {
		int len;

#if defined(JENT_TEST_BINARY_OUTPUT)
		len = snprintf(pathname, sizeof(pathname), "%s-%.4lu-u64.bin",
			       file, i);
#else
		len = snprintf(pathname, sizeof(pathname), "%s-%.4lu.data",
			       file, i);
#endif
		/*
		 * A truncated path would drop the repeat-number suffix and make
		 * every repeat overwrite the same file, silently collapsing the
		 * SP800-90B restart matrix to a single repeat.
		 */
		if (len < 0 || (size_t)len >= sizeof(pathname)) {
			fprintf(stderr, "Output file path too long\n");
			return 1;
		}

		ret = jent_one_test(pathname, rounds, flags, osr, jent_es,
				    loopcnt, REPORT_COUNTER_TICKS, status);

		if (ret)
			return ret;
	}

	return 0;
}
