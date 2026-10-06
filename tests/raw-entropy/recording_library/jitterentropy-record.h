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
 * The API of libjitterentropy-record, which the build offers when asked to
 * (ENABLE_RECORDING in CMake, ENABLE_RECORDING=1 with make), and installs
 * beside jitterentropy.h then: the recording of the raw noise data for the
 * SP800-90B and NTG.1 assessments, for a process no recording tool can run in
 * - an Android or iOS app, see tests/raw-entropy/recording_library/README.md.
 * jitterentropy-hashtime records through the same code, and so does the
 * debugfs test interface of the kernel module, through jent_record_alloc().
 *
 * The library carries a copy of libjitterentropy of its own and exports
 * nothing but the functions below, so it is linked beside libjitterentropy,
 * shared or static, without either seeing the other: a collector of one is no
 * collector of the other. It does not include jitterentropy.h either: the
 * flags a recording takes are those of jitterentropy.h, which a caller naming
 * them includes itself.
 *
 * Not for production: the recording bypasses the startup tests and runs the
 * noise source without conditioning its output.
 */

#ifndef _JITTERENTROPY_RECORD_H
#define _JITTERENTROPY_RECORD_H

#ifdef LINUX_KERNEL
# include <linux/types.h>
#else
# include <stddef.h>
# include <stdint.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * As the library's own export macro: dllexport while the DLL is built,
 * dllimport for its consumers, nothing where the code is compiled in
 * (jitterentropy-hashtime) or archived.
 */
#if defined(_WIN32)
# if defined(JENT_RECORD_STATIC_LIB)
#  define JENT_RECORD_API
# elif defined(JENT_RECORD_BUILDING_DLL)
#  define JENT_RECORD_API __declspec(dllexport)
# else
#  define JENT_RECORD_API __declspec(dllimport)
# endif
#else
# define JENT_RECORD_API __attribute__((visibility("default")))
#endif

/*
 * The noise source a recording measures: the whole of it, as
 * jitterentropy-hashtime by default, or the hash loop or the memory access
 * loop alone, as with its --hashloop and --memaccess.
 */
#define JENT_RECORD_COMMON	0
#define JENT_RECORD_HASHLOOP	1
#define JENT_RECORD_MEMACCESS	2

/* The return codes of jent_record_raw() and jent_record_alloc(). */
#define JENT_RECORD_OK		0
#define JENT_RECORD_EINVAL	1 /* no such noise source, no rounds, or the
				     memory access loop with memory access
				     disabled */
#define JENT_RECORD_ENOMEM	2 /* no memory for the samples */
#define JENT_RECORD_ECOLLECTOR	3 /* no collector: see jent_record_raw() */
#define JENT_RECORD_EIO		4 /* the file could not be written: errno */
#define JENT_RECORD_ESELFTEST	5 /* jent_record_alloc() only: the self tests
				     of the conditioning failed */

/* What a recording ran with, and what its health tests said. */
struct jent_record_result {
	unsigned int health_failure;	/* jent_health_failure() at the end */
	unsigned int memsize;		/* bytes of the memory access region */
	unsigned int hashloopcnt;	/* hash loop count of the collector */
	unsigned int internal_timer;	/* 1: time stamps of the timer thread */
};

/*
 * Record @rounds raw time deltas of the noise source @source into @pathname,
 * one decimal value per line - one repeat of jitterentropy-hashtime, run with
 * the same @osr, @flags and @loopcnt (0: the collector's own), and the file
 * format the validation scripts in tests/raw-entropy read.
 *
 * The collector is allocated without the startup, so that the recording
 * measures exactly the requested configuration, and after the self tests of
 * the conditioning. JENT_RECORD_ECOLLECTOR stands for those failing, for flags
 * the library refuses (NTG.1 with the timer thread), or for memory it cannot
 * get or, in a compliance mode, lock. With JENT_RECORD_EIO, errno says why
 * the file could not be written; nothing is left of it.
 *
 * @result, where not NULL, receives the configuration the collector ran with
 * once it was allocated, and the health test failures of the recording, which
 * do not end it. @status, where not NULL, receives the jent_status() document
 * of that collector where @statuslen holds it - JENT_RECORD_STATUS_LEN does -
 * and an empty string otherwise.
 *
 * Prints nothing. Not safe to call concurrently with any other call into the
 * library.
 */
#define JENT_RECORD_STATUS_LEN	4096

JENT_RECORD_API
int jent_record_raw(const char *pathname, unsigned int rounds,
		    unsigned int osr, unsigned int flags, unsigned int source,
		    unsigned int loopcnt, struct jent_record_result *result,
		    char *status, size_t statuslen);

/*
 * A recording driven by its caller one sample at a time, for a recorder that
 * writes no file of its own - the debugfs test interface of the kernel
 * module. Allocated as jent_record_raw() allocates its collector, with
 * JENT_RECORD_EINVAL also for an @osr above JENT_MAX_OSR, and
 * JENT_RECORD_ESELFTEST where
 * jent_record_raw() says JENT_RECORD_ECOLLECTOR for the self tests. Unlike
 * jent_record_raw() the deltas keep the division by the common timer GCD.
 *
 * jent_record_prime() takes the time stamp the first delta is measured from,
 * at the loop count of the samples to follow; it is needed again after any
 * pause - a reschedule, a copy - that is not to be recorded. A no-op for the
 * hash loop and memory access loop alone, which prime themselves.
 * jent_record_sample() measures one delta, the health tests judging it.
 * @loopcnt 0 is the collector's own.
 *
 * One recording is not safe to use from two threads at once.
 */
struct jent_record;

JENT_RECORD_API
int jent_record_alloc(struct jent_record **rec, unsigned int osr,
		      unsigned int flags, unsigned int source);

JENT_RECORD_API
void jent_record_free(struct jent_record *rec);

JENT_RECORD_API
void jent_record_prime(struct jent_record *rec, unsigned int loopcnt);

JENT_RECORD_API
uint64_t jent_record_sample(struct jent_record *rec, unsigned int loopcnt);

#ifdef LINUX_KERNEL
/*
 * The collector of @rec, for jent_status() and the jent_entropy_collector_*()
 * getters: in the kernel module the recording and the library are one copy.
 * Not in userspace, where it belongs to the copy inside
 * libjitterentropy-record that no function of libjitterentropy may be given.
 */
struct rand_data;
struct rand_data *jent_record_collector(struct jent_record *rec);
#endif

/* Where jent_record_run() measures, beyond a CPU of its own */
#define JENT_RECORD_CORES_ANY		0
#define JENT_RECORD_CORES_EFFICIENCY	1 /* confined to them, Apple only */
#define JENT_RECORD_CORES_PERFORMANCE	2 /* a preference, Apple only */

/*
 * A recording as the command line of jitterentropy-hashtime describes it:
 * jitterentropy-hashtime <rounds> <repeats> <file> [options].
 */
struct jent_record_config {
	const char *file;	/* <file>-0001.data up to <file>-<repeats> */
	unsigned long rounds;	/* time deltas per file */
	unsigned long repeats;	/* files, each from a new collector */
	unsigned int flags;	/* JENT_* flags of jitterentropy.h */
	unsigned int osr;	/* --osr, 0: the default */
	unsigned int loopcnt;	/* --loopcnt, 0: the collector's own */
	unsigned int source;	/* JENT_RECORD_*: --hashloop, --memaccess */
	int cpu;		/* --cpu, -1: not pinned */
	unsigned int cores;	/* JENT_RECORD_CORES_*: --e-cores, --p-cores */
	int status;		/* --status: print the status, record nothing */
};

/*
 * Run jitterentropy-hashtime as @config describes: pin the calling thread as
 * asked, then record @config->repeats files through jent_record_raw(). What
 * the tool reports - the file at hand, the memory size and hash loop count,
 * the health test failures, the status document of --status, the reason a
 * recording failed - is written to @report, as far as @reportlen allows;
 * jent_record_report_len() is enough for all of it. Prints nothing. Returns
 * the tool's exit status, 0 or 1.
 *
 * The memory lock limit and the secure memory of the compliance modes are the
 * caller's to set up, as the tool does before calling this.
 */
JENT_RECORD_API
int jent_record_run(const struct jent_record_config *config, char *report,
		    size_t reportlen);

/* A @reportlen for jent_record_run() that holds everything it reports. */
JENT_RECORD_API
size_t jent_record_report_len(const struct jent_record_config *config);

/*
 * The names of the health tests in @health_failure, as jitterentropy-hashtime
 * lists them ("RCT APT-permanent"), in @buf. Returns the length the whole list
 * needs, as snprintf() does.
 */
JENT_RECORD_API
size_t jent_record_health_names(unsigned int health_failure, char *buf,
				size_t len);

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_RECORD_H */
