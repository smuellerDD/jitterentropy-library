/*
 * Raw noise recording of the Jitter RNG
 *
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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

/*
 * Compiled in two places, with the library's flags each time, as struct
 * rand_data depends on them: into libjitterentropy-record beside the library
 * sources, and into jitterentropy-hashtime, which #includes it after them and
 * is its command line - the recording itself, jent_record_run(), is here.
 * Under tests/, not src/, so that libjitterentropy never carries it.
 *
 * Both define JENT_RAW_COLLECTOR for the library sources beside it, the one
 * build of them with jent_entropy_collector_alloc_raw() in userspace.
 */
#ifndef JENT_RAW_COLLECTOR
# define JENT_RAW_COLLECTOR
#endif

/* sched_setaffinity() for the CPU pinning; also set by jitterentropy-hashtime */
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <sched.h>	/* sched_setaffinity() for --cpu */
#endif

#ifdef __APPLE__
#include <pthread.h>
#include <sys/qos.h>		/* QoS classes for --e-cores and --p-cores */
#include <sys/resource.h>	/* PRIO_DARWIN_PROCESS for --p-cores */
#endif

#include "jitterentropy-internal.h"
#include "jitterentropy-health.h"
#include "jitterentropy-noise.h"
#include "jitterentropy-timer.h"
#include "arch/jitterentropy-arch-thread.h"	/* jent_thread_pin_to_cpu() */

#include "jitterentropy-record.h"

/* The recording without a file, which the kernel module compiles alone. */
#include "jitterentropy-record-session.c"

JENT_RECORD_API
int jent_record_raw(const char *pathname, unsigned int rounds,
		    unsigned int osr, unsigned int flags, unsigned int source,
		    unsigned int loopcnt, struct jent_record_result *result,
		    char *status, size_t statuslen)
{
	jent_record_measure_t measure_jitter =
		jent_record_measure(source, flags);
	struct rand_data *ec = NULL;
	uint64_t *duration;
	unsigned int i;
	FILE *out;
	int ret = JENT_RECORD_OK, err = 0;

	if (status && statuslen)
		status[0] = '\0';

	if (!measure_jitter || !rounds)
		return JENT_RECORD_EINVAL;

	duration = calloc(rounds, sizeof(*duration));
	if (!duration)
		return JENT_RECORD_ENOMEM;

	ec = jent_entropy_collector_alloc_raw(osr, flags);
	if (!ec) {
		ret = JENT_RECORD_ECOLLECTOR;
		goto out;
	}

	if (result) {
		result->health_failure = 0;
		result->memsize = ec->mem ? ec->memmask + 1 : 0;
		result->hashloopcnt = ec->hashloopcnt;
		result->internal_timer = ec->enable_notime;
	}

	/* Before the measurement, which it would otherwise add to. */
	if (status && statuslen && jent_status(ec, status, statuslen))
		status[0] = '\0';

	/*
	 * The raw counter ticks. A process that ran jent_entropy_init*() has
	 * established the common divisor of the clock, and its collector would
	 * record every delta divided by it - a different set of low bits for
	 * the validation to extract than a recording tool, which never ran
	 * them, gets.
	 */
	ec->jent_common_timer_gcd = 1;

	/* A timer thread that did not start would record a counter at rest. */
	if (ec->enable_notime && jent_notime_settick(ec)) {
		ret = JENT_RECORD_ECOLLECTOR;
		goto out;
	}

	/*
	 * Opened before the measurement, so that a path that cannot be written
	 * fails before a recording - a million rounds at the SP800-90B sizes -
	 * is spent on it, and after the collector, so that a configuration the
	 * library refuses leaves an earlier file of that name alone.
	 *
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
		err = errno;
		ret = JENT_RECORD_EIO;
		goto out;
	}

	/* Enable full SP800-90B health test handling */
	ec->is_fips_enabled = 1;

	/*
	 * Prime the common measurement (ec->prev_time), as the kernel test
	 * interface does: at the recording's loop count, so the first recorded
	 * delta spans a measurement at that count like every other, and out of
	 * the health tests, as its delta is not one of the recording. The NTG.1
	 * hash-loop and memory-access variants prime themselves.
	 */
	if (source == JENT_RECORD_COMMON)
		jent_measure_jitter(ec, loopcnt, NULL, 0);
	for (i = 0; i < rounds; i++) {
		/* Disregard stuck indicator */
		measure_jitter(ec, loopcnt, &duration[i], 1);
	}

	if (result)
		result->health_failure = jent_health_failure(ec);

	/*
	 * Written after the measurement, not during it, so that the file
	 * system does not add to the timing being measured.
	 *
	 * Output errors are fatal: a silently truncated sample file would be
	 * analyzed by the SP800-90B validation as if it were complete.
	 */
#ifdef JENT_TEST_BINARY_OUTPUT
	if (fwrite(duration, sizeof(*duration), rounds, out) != rounds) {
		err = errno;
		ret = JENT_RECORD_EIO;
	}
#else
	for (i = 0; i < rounds; i++) {
		if (fprintf(out, "%" PRIu64 "\n", duration[i]) < 0) {
			err = errno;
			ret = JENT_RECORD_EIO;
			break;
		}
	}
#endif
	/* An fclose() error means buffered sample data was lost. */
	if (fclose(out) && ret == JENT_RECORD_OK) {
		err = errno;
		ret = JENT_RECORD_EIO;
	}

	/* Neither a short file nor a stale one for the validation to read. */
	if (ret != JENT_RECORD_OK)
		remove(pathname);

out:
	if (ec) {
		/* checks internally if timer was used, maybe NOOP */
		jent_notime_unsettick(ec);
		jent_entropy_collector_free(ec);
	}
	free(duration);

	/* Why the file could not be written, past the cleanup above. */
	if (err)
		errno = err;

	return ret;
}

JENT_RECORD_API
size_t jent_record_health_names(unsigned int health_failure, char *buf,
				size_t len)
{
	static const struct {
		unsigned int bit;
		const char *name;
	} tests[] = {
		{ JENT_RCT_FAILURE, "RCT" },
		{ JENT_APT_FAILURE, "APT" },
		{ JENT_LAG_FAILURE, "Lag" },
		{ JENT_RCT_MEM_FAILURE, "RCT-mem" },
		{ JENT_RCT_FAILURE_PERMANENT, "RCT-permanent" },
		{ JENT_APT_FAILURE_PERMANENT, "APT-permanent" },
		{ JENT_LAG_FAILURE_PERMANENT, "Lag-permanent" },
		{ JENT_RCT_MEM_FAILURE_PERMANENT, "RCT-mem-permanent" },
	};
	unsigned int known = 0;
	size_t used = 0, i;
	int n;

	if (len)
		buf[0] = '\0';

	for (i = 0; i < JENT_ARRAY_SIZE(tests); i++) {
		known |= tests[i].bit;
		if (!(health_failure & tests[i].bit))
			continue;
		n = snprintf(used < len ? buf + used : NULL,
			     used < len ? len - used : 0, "%s%s",
			     used ? " " : "", tests[i].name);
		if (n > 0)
			used += (size_t)n;
	}

	/* A bit this list does not know yet, not an empty list */
	if (health_failure & ~known) {
		n = snprintf(used < len ? buf + used : NULL,
			     used < len ? len - used : 0, "%s(unknown 0x%x)",
			     used ? " " : "", health_failure & ~known);
		if (n > 0)
			used += (size_t)n;
	}

	return used;
}

/***************************************************************************
 * jitterentropy-hashtime, driven by a struct jent_record_config
 ***************************************************************************/

/* The report of jent_record_run(), for its caller to print. */
struct jent_record_report {
	char *buf;
	size_t len;
	size_t used;
};

/* Append to @r what fits, always terminated; the rest is dropped. */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 2, 3)))
#endif
static void jent_report(struct jent_record_report *r, const char *fmt, ...)
{
	va_list ap;
	int n;

	if (!r->buf || r->used + 1 >= r->len)
		return;

	va_start(ap, fmt);
	n = vsnprintf(r->buf + r->used, r->len - r->used, fmt, ap);
	va_end(ap);

	if (n < 0)
		r->buf[r->used] = '\0';
	else if ((size_t)n >= r->len - r->used)
		r->used = r->len - 1;
	else
		r->used += (size_t)n;
}

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
 * Whether the function above can place the measurement, and so whether
 * jitterentropy-hashtime offers --cpu: the library's primitive covers the
 * systems named here and no other, and without it only the Linux fallback is
 * left. The option is parsed where it is not offered, so passing it yields the
 * reason rather than "unknown option".
 */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
# if defined(_MSC_VER) || defined(__MINGW32__) || defined(__linux__) || \
     defined(__FreeBSD__) || defined(__NetBSD__)
#  define JENT_HAVE_CPU_PINNING
# endif
#elif defined(__linux__)
# define JENT_HAVE_CPU_PINNING
#endif

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

/*
 * One repeat: the time deltas of @config into @pathname, through
 * jent_record_raw(), or the status of the collector the recording would use.
 */
static int jent_one_test(const struct jent_record_config *config,
			 const char *pathname, struct jent_record_report *r)
{
	struct jent_record_result result;
	char names[160];
	unsigned int hashloops;
	int ret, err;

	/*
	 * Do not perform the common startup check as the health test may
	 * disable the Jitter RNG. However, as we are in test mode, we
	 * *want* to also know about insufficient entropy.
	 * Thus, only perform the cryptographic self tests and go on.
	 *
	 * jent_record_raw() runs them as well, and refuses to record over a
	 * failed one; they run here first only to say which one failed. Unlike
	 * a health test verdict this one is not about the noise source and
	 * there is nothing to learn from carrying on.
	 */
	ret = jent_raw_selftest(config->flags);
	if (ret) {
		jent_report(r, "The conditioning self test failed with error code %d (%s)\n",
		       ret,
		       (ret == EHASH) ? "SHA-3 known answer test" :
		       (ret == EGCD) ? "GCD known answer test" :
				       "unexpected startup failure");
		return 1;
	}

	/*
	 * early exit, when status is requested. Can be used to
	 * compare config of measurements with runtime
	 */
	if (config->status) {
		struct rand_data *ec =
			jent_entropy_collector_alloc_raw(config->osr,
							 config->flags);
		char status_str[JENT_RECORD_STATUS_LEN];

		if (!ec) {
			jent_report(r, "Allocation of the entropy collector failed\n");
			return 1;
		}
		ret = jent_status(ec, status_str, sizeof(status_str));
		if (ret)
			jent_report(r, "Fetching jent status failed with code: %d\n", ret);
		else
			jent_report(r, "%s", status_str);
		jent_entropy_collector_free(ec);
		return ret ? 1 : 0;
	}

	jent_report(r, "Processing %s\n", pathname);

	ret = jent_record_raw(pathname, (unsigned int)config->rounds,
			      config->osr, config->flags, config->source,
			      config->loopcnt, &result, NULL, 0);
	/* Why the file could not be written, before the reports below. */
	err = errno;
	switch (ret) {
	case JENT_RECORD_OK:
	case JENT_RECORD_EIO:
		break;
	case JENT_RECORD_ECOLLECTOR:
		jent_report(r, "Allocation of the entropy collector failed\n");
		/*
		 * The counting thread needs a CPU of its own, and
		 * jent_notime_init() refuses to start with a single-CPU
		 * affinity mask - exactly what --cpu establishes.
		 */
		if (config->cpu >= 0)
			jent_report(r, "Note: --cpu leaves one CPU in the affinity mask, which rules out the internal timer\n");
		return 1;
	case JENT_RECORD_EINVAL:
		if (config->source == JENT_RECORD_MEMACCESS &&
		    (config->flags & JENT_DISABLE_MEMORY_ACCESS))
			jent_report(r, "The memory access loop cannot be recorded with memory access disabled\n");
		else
			jent_report(r, "Invalid recording arguments\n");
		return 1;
	case JENT_RECORD_ENOMEM:
		jent_report(r, "No memory for the %lu samples to be recorded\n",
			    config->rounds);
		return 1;
	default:
		jent_report(r, "Recording failed with code %d\n", ret);
		return 1;
	}

	/*
	 * Print the size of the memory region and the hash loop iterations
	 * each measurement ran: --loopcnt where given, else the collector's
	 * count - tripled for the NTG.1 hash loop alone, as
	 * jent_measure_jitter_ntg1_sha3() does - and none in the memory access
	 * loop alone.
	 */
	if (config->source == JENT_RECORD_MEMACCESS)
		hashloops = 0;
	else if (config->loopcnt)
		hashloops = config->loopcnt;
	else if (config->source == JENT_RECORD_HASHLOOP)
		hashloops = result.hashloopcnt * JENT_HASH_LOOP_INIT;
	else
		hashloops = result.hashloopcnt;

#ifdef JENT_RANDOM_MEMACCESS
	jent_report(r, "Random memory access - Memory size: %u - Hashloop count: %u\n",
	       result.memsize, hashloops);
#else
	jent_report(r, "Deterministic memory access - Memory size: %u - Hashloop count: %u\n",
	       result.memsize, hashloops);
#endif

	if (result.health_failure) {
		jent_record_health_names(result.health_failure, names,
					 sizeof(names));
		jent_report(r, "The main context encountered the following health testing failure(s): %s\n",
		       names);
	}

	/*
	 * Output errors are fatal for the tool: jent_record_raw() removed the
	 * file rather than leave a short one for the validation.
	 */
	if (ret == JENT_RECORD_EIO) {
		jent_report(r, "Can't output data: %s\n", strerror(err));
		return 1;
	}

	return 0;
}

JENT_RECORD_API
size_t jent_record_report_len(const struct jent_record_config *config)
{
	/*
	 * Each line has a bound: the file name, the fixed texts, the health
	 * test names, strerror(), and the status document where asked for.
	 */
	size_t per_repeat = 1024 + (config->file ? strlen(config->file) : 0) +
			    (config->status ? JENT_RECORD_STATUS_LEN : 0);

	return 1024 + per_repeat * (config->repeats ? config->repeats : 1);
}

JENT_RECORD_API
int jent_record_run(const struct jent_record_config *config, char *report,
		    size_t reportlen)
{
	struct jent_record_report r = { report, reportlen, 0 };
	char pathname[4096];
	unsigned long i;
	int ret;

	if (report && reportlen)
		report[0] = '\0';

	/*
	 * Reject zero: it would record empty files, or none. The upper bound is
	 * what jent_record_raw() takes.
	 */
	if (!config->file || !config->rounds || config->rounds >= UINT_MAX ||
	    !config->repeats) {
		jent_report(&r, "Invalid recording configuration\n");
		return 1;
	}

	/* Each names the core to measure on, in ways that cannot be combined. */
	if (config->cpu >= 0 && config->cores != JENT_RECORD_CORES_ANY) {
		jent_report(&r, "--cpu, --e-cores and --p-cores are mutually exclusive\n");
		return 1;
	}

	/*
	 * Before the first initialization, so that the self tests, the memory
	 * allocation and the recording all run on the selected core.
	 */
	if (config->cpu >= 0) {
		ret = jent_pin_cpu((unsigned long)config->cpu);
		if (ret) {
			jent_report(&r, "Cannot pin the measurement to CPU %d: %s\n",
			       config->cpu, strerror(-ret));
			return 1;
		}
		jent_report(&r, "Measurement pinned to CPU %d\n", config->cpu);
	}

	if (config->cores != JENT_RECORD_CORES_ANY) {
		int performance =
			config->cores == JENT_RECORD_CORES_PERFORMANCE;

		ret = jent_select_cores(performance);
		if (ret) {
			jent_report(&r, "Cannot ask for the %s cores: %s\n",
			       performance ? "performance" : "efficiency",
			       strerror(-ret));
			return 1;
		}
		/*
		 * Only the efficiency cores are a confinement; the performance
		 * ones are a preference the scheduler is free to leave.
		 */
		if (performance)
			jent_report(&r, "Measurement asked for the performance cores\n");
		else
			jent_report(&r, "Measurement confined to the efficiency cores\n");
	}

	for (i = 1; i <= config->repeats; i++) {
		int len;

#if defined(JENT_TEST_BINARY_OUTPUT)
		len = snprintf(pathname, sizeof(pathname), "%s-%.4lu-u64.bin",
			       config->file, i);
#else
		len = snprintf(pathname, sizeof(pathname), "%s-%.4lu.data",
			       config->file, i);
#endif
		/*
		 * A truncated path would drop the repeat-number suffix and make
		 * every repeat overwrite the same file, silently collapsing the
		 * SP800-90B restart matrix to a single repeat.
		 */
		if (len < 0 || (size_t)len >= sizeof(pathname)) {
			jent_report(&r, "Output file path too long\n");
			return 1;
		}

		/*
		 * The status is the configuration's, which records nothing:
		 * once, not once per repeat.
		 */
		ret = jent_one_test(config, pathname, &r);
		if (ret || config->status)
			return ret;
	}

	return 0;
}
