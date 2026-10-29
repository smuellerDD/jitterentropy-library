/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
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

/* The daemon on every system; jitterentropy-rngd.h has the kernel's part */

#define _GNU_SOURCE

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <signal.h>

#include "jitterentropy-rngd.h"

#define ARRAY_SIZE(a)	(sizeof(a) / sizeof((a)[0]))

static int Verbosity = 0;
static int force_sp80090b = 0;
/* ORed in after parsing, so --flags cannot drop them whatever the order */
static unsigned int jent_flags_add = 0;
static int status = 0;

/*
 * When set, log messages go to syslog(3) instead of log_stream(). A detached
 * daemon's stdout and stderr are /dev/null, which would discard them.
 */
static int use_syslog = 0;

/*
 * When set, the daemon does not detach from the invoking terminal. Whether the
 * daemon forks is decided by this flag alone - it is deliberately independent
 * of the log verbosity.
 */
static int foreground = 0;

/*
 * When set, the daemon will exit on any error it encounters. The goal is that
 * a wrapping monitor will pick up the errors and handle it as it sees fit, such
 * as creating audit logs and possibly restart the daemon.
 *
 * The following errors are returned:
 *
 * * EOPNOTSUPP - the Jitter RNG triggered a fatal health test error
 * * Errors the kernel reports for an injection: of the IOCTLs RNDADDENTROPY
 *   and RNDRESEEDCRNG to /dev/random on Linux, of write(2) to it elsewhere
 * * Errors of pselect(2) waiting for the next occasion
 *
 * Every other failure - open(2), the PID file, fork(2), the startup of the
 * Jitter RNG - ends the daemon with 1, with or without this option. A
 * permanent health test failure ends it with RNGD_EXIT_PERMANENT, with or
 * without this option as well, see read_jent().
 */
int exit_on_error = 0;

/* --oneshot: seed the kernel once in the foreground and exit */
static int oneshot = 0;

/* Set once the kernel accepted a full injection */
static int seeded = 0;

/* --generate: write this many bytes of the Jitter RNG out and exit */
static unsigned int generate = 0;
/* --output: the file --generate writes to, stdout without it */
static const char *output = NULL;
/* --hex: --generate writes two hex digits per byte */
static int hex = 0;

struct kernel_rng Random = {
	/*.fd = */ -1,
	/*.ec = */ NULL,
	/*.dev = */ "/dev/random"
};

static int Pidfile_fd = -1;
/* "/var/run/jitterentropy-rngd.pid" */
static char *Pidfile = NULL;

unsigned int jent_flags = 0;
static unsigned int jent_osr = 1;

static void jentrng_versionstring(char *buf, size_t buflen)
{
	/* Released with the library, so its version is the daemon's. */
	snprintf(buf, buflen, "jitterentropy-rngd %d.%d.%d",
		 JENT_MAJVERSION, JENT_MINVERSION, JENT_PATCHLEVEL);
}

void usage(void)
{
	unsigned int ver = jent_version();
	char version[30];

	memset(version, 0, sizeof(version));
	jentrng_versionstring(version, sizeof(version));

	fprintf(stderr, "\n%s\n", rngd_os_description);
	fprintf(stderr, "Version %s\n\n", version);
	fprintf(stderr, "Reported numeric version number of jent library %u\n\n", ver);
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t-h --help\tThis help information\n");
	fprintf(stderr, "\t   --version\tPrint version\n");
	fprintf(stderr, "\t-v --verbose\tVerbose logging, multiple options increase verbosity\n");
	fprintf(stderr, "\t-F --foreground\tDo not detach, keep running in the foreground\n");
	fprintf(stderr, "\t-l --syslog\tLog to syslog instead of stdout - required to see\n");
	fprintf(stderr, "\t\t\tany log output when the daemon detaches\n");
	fprintf(stderr, "\t-p --pid\tWrite daemon PID to file\n");
	fprintf(stderr, "\t-s --sp800-90b\tForce SP800-90B compliance\n");
	fprintf(stderr, "\t   --fips\tForce FIPS mode, adds JENT_FORCE_FIPS to --flags\n");
	fprintf(stderr, "\t-n --ntg1\tForce AIS 20/31 NTG.1 compliance, adds JENT_NTG1 to --flags\n");
	fprintf(stderr, "\t-i --force-internal-timer\tAdds JENT_FORCE_INTERNAL_TIMER to --flags\n");
	fprintf(stderr, "\t-I --disable-internal-timer\tAdds JENT_DISABLE_INTERNAL_TIMER to --flags\n");
	fprintf(stderr, "\t-f --flags\tInteger with flags used to allocate Jitter RNG\n");
	fprintf(stderr, "\t-o --osr\tInteger with OSR used to allocate Jitter RNG\n");
	fprintf(stderr, "\t   --status\tStatus information of the Jitter RNG - invoke with\n");
	fprintf(stderr, "\t           \tsame flags as used for runtime\n");
	fprintf(stderr, "\t   --exit-on-error\tCause the daemon to exit on errors\n");
	fprintf(stderr, "\t   --oneshot\tSeed the kernel once and exit\n");
	fprintf(stderr, "\t-g --generate\tWrite this many random bytes and exit,\n");
	fprintf(stderr, "\t\t\tseeding nothing\n");
	fprintf(stderr, "\t   --output\tFile --generate writes to (default stdout)\n");
	fprintf(stderr, "\t   --hex\t--generate writes hex digits\n");
	rngd_os_usage();
	exit(1);
}

/*
 * Convert a command line argument up to the character term into an unsigned
 * int or bail out. Where rest is given, it points behind term.
 */
unsigned int parse_uint_until(const char *str, char term, const char **rest)
{
	char *endptr = NULL;
	unsigned long val;

	errno = 0;
	val = strtoul(str, &endptr, 10);

	/* Reject empty strings, trailing garbage and out-of-range values */
	if (errno || endptr == str || *endptr != term)
		usage();
	if (rest)
		*rest = endptr + 1;
#if ULONG_MAX > UINT_MAX
	if (val > UINT_MAX)
		usage();
#endif

	return (unsigned int)val;
}

unsigned int parse_uint(const char *str)
{
	return parse_uint_until(str, '\0', NULL);
}

/* The long options without a short one, below those of RNGD_OS_OPT */
enum {
	OPT_VERSION = 256,
	OPT_STATUS,
	OPT_EXIT_ON_ERROR,
	OPT_FIPS,
	OPT_ONESHOT,
	OPT_OUTPUT,
	OPT_HEX,
};

static const struct option rngd_options[] = {
	{"verbose", 0, 0, 'v'},
	{"pid", 1, 0, 'p'},
	{"help", 0, 0, 'h'},
	{"version", 0, 0, OPT_VERSION},
	{"sp800-90b", 0, 0, 's'},
	{"flags", 1, 0, 'f'},
	{"osr", 1, 0, 'o'},
	{"status", 0, 0, OPT_STATUS},
	{"exit-on-error", 0, 0, OPT_EXIT_ON_ERROR},
	{"syslog", 0, 0, 'l'},
	{"foreground", 0, 0, 'F'},
	{"ntg1", 0, 0, 'n'},
	{"force-internal-timer", 0, 0, 'i'},
	{"disable-internal-timer", 0, 0, 'I'},
	{"fips", 0, 0, OPT_FIPS},
	{"oneshot", 0, 0, OPT_ONESHOT},
	{"generate", 1, 0, 'g'},
	{"output", 1, 0, OPT_OUTPUT},
	{"hex", 0, 0, OPT_HEX},
};

/* Room for the daemon's options and the operating system's */
#define MAX_OPTIONS	32

static void parse_opts(int argc, char *argv[])
{
	struct option opts[MAX_OPTIONS + 1];
	char optstring[64];
	char version[30];
	size_t n = 0, i;
	int c = 0;

	for (i = 0; i < ARRAY_SIZE(rngd_options); i++)
		opts[n++] = rngd_options[i];
	for (i = 0; rngd_os_options[i].name; i++) {
		if (n == MAX_OPTIONS) {
			fprintf(stderr, "More options than MAX_OPTIONS\n");
			exit(1);
		}
		opts[n++] = rngd_os_options[i];
	}
	memset(&opts[n], 0, sizeof(opts[n]));
	snprintf(optstring, sizeof(optstring), "svp:hf:o:lFniIg:%s",
		 rngd_os_optstring);

	while (1) {
		c = getopt_long(argc, argv, optstring, opts, NULL);
		if (-1 == c)
			break;
		switch (c) {
		case 'v':
			Verbosity++;
			break;
		case 'p':
			Pidfile = optarg;
			break;
		case 'h':
			usage();
			break;
		case OPT_VERSION:
			jentrng_versionstring(version, sizeof(version));
			fprintf(stderr, "Version %s\n", version);
			fprintf(stderr, "Version Jitterentropy Core %u\n", jent_version());
			exit(0);
			break;
		case 's':
			force_sp80090b = 1;
			break;
		case 'f':
			jent_flags = parse_uint(optarg);
			break;
		case 'o':
			jent_osr = parse_uint(optarg);
			break;
		case OPT_STATUS:
			status = 1;
			break;
		case OPT_EXIT_ON_ERROR:
			exit_on_error = 1;
			break;
		case 'l':
			use_syslog = 1;
			break;
		case 'F':
			foreground = 1;
			break;
		case 'n':
			jent_flags_add |= JENT_NTG1;
			break;
		case 'i':
			jent_flags_add |= JENT_FORCE_INTERNAL_TIMER;
			break;
		case 'I':
			jent_flags_add |= JENT_DISABLE_INTERNAL_TIMER;
			break;
		case OPT_FIPS:
			jent_flags_add |= JENT_FORCE_FIPS;
			break;
		case OPT_ONESHOT:
			oneshot = 1;
			break;
		case 'g':
			generate = parse_uint(optarg);
			if (!generate)
				usage();
			break;
		case OPT_OUTPUT:
			output = optarg;
			break;
		case OPT_HEX:
			hex = 1;
			break;
		case '?':
			usage();
			break;
		default:
			rngd_os_parse(c, optarg);
		}
	}

	/* --generate is a run of its own, which --output and --hex shape */
	if ((output || hex) && !generate)
		usage();
	if (generate && (oneshot || status))
		usage();

	jent_flags |= jent_flags_add;
}

/* ANSI SGR sequences used to colorize the log on a capable terminal */
#define COL_RESET	"\033[0m"
#define COL_DIM		"\033[2m"
#define COL_CYAN	"\033[36m"
#define COL_YELLOW	"\033[33m"
#define COL_RED		"\033[1;31m"

static int use_color = 0;

/* The log unless --syslog: stdout, or stderr if --generate's bytes take it */
static FILE *log_stream(void)
{
	return generate ? stderr : stdout;
}

/*
 * Colors only on a terminal expected to interpret them: escape sequences would
 * corrupt a file or a pipe for anything that later reads it.
 */
static void detect_color(void)
{
	const char *term = getenv("TERM");

	use_color = 0;

	/* Escape sequences are meaningless unless a terminal reads them */
	if (!isatty(fileno(log_stream())))
		return;

	/* Honour the NO_COLOR convention, see https://no-color.org/ */
	if (getenv("NO_COLOR"))
		return;

	/* A terminal that announces itself as incapable is taken at its word */
	if (!term || !strcmp(term, "dumb"))
		return;

	use_color = 1;
}

/*
 * Render the current wall clock time into buf.
 *
 * CLOCK_REALTIME is deliberate: the log is read alongside other system logs,
 * so the entries have to carry the actual time of day rather than an offset
 * from some arbitrary start point. Note this means the printed times follow
 * adjustments of the system clock, which is the right trade-off for a log but
 * makes them unsuitable for measuring intervals.
 *
 * Only the messages printed to stdout are stamped here - syslog records its
 * own timestamp, so adding one there would just duplicate it.
 */
static void logtime(char *buf, size_t buflen)
{
	struct timespec ts;
	struct tm tm;
	size_t len;

	if (clock_gettime(CLOCK_REALTIME, &ts) || !localtime_r(&ts.tv_sec, &tm))
		goto unknown;

	len = strftime(buf, buflen, "%Y-%m-%d %H:%M:%S", &tm);
	if (0 == len)
		goto unknown;

	/* Millisecond resolution keeps closely spaced events distinguishable */
	snprintf(buf + len, buflen - len, ".%03ld", ts.tv_nsec / 1000000);

	return;

unknown:
	snprintf(buf, buflen, "<no timestamp>");
}

static void vdolog(int severity, const char *fmt, va_list args)
{
	char msg[1024];
	const char *sev, *col;
	char now[32];

	if (severity <= Verbosity) {
		int prio;

		vsnprintf(msg, sizeof(msg), fmt, args);

		switch (severity) {
		case JENT_LOG_DEBUG:
			sev = "Debug";
			col = COL_DIM;
			prio = LOG_DEBUG;
			break;
		case JENT_LOG_VERBOSE:
			sev = "Verbose";
			col = COL_CYAN;
			prio = LOG_INFO;
			break;
		case JENT_LOG_WARN:
			sev = "Warning";
			col = COL_YELLOW;
			prio = LOG_WARNING;
			break;
		case JENT_LOG_ERR:
			sev = "Error";
			col = COL_RED;
			prio = LOG_ERR;
			break;
		default:
			sev = "Unknown";
			col = COL_RESET;
			prio = LOG_NOTICE;
		}

		if (use_syslog) {
			/*
			 * The identity and the priority already convey the
			 * program name and the severity, so only the bare
			 * message is handed over. The message is passed as an
			 * argument rather than as the format string so that a
			 * percent sign in it cannot be interpreted.
			 */
			syslog(prio, "%s", msg);
		} else {
			logtime(now, sizeof(now));

			if (use_color) {
				fprintf(log_stream(),
					"[%s%s%s - jitterentropy-rngd - %s%s%s] %s\n",
					COL_DIM, now, COL_RESET,
					col, sev, COL_RESET, msg);
			} else {
				fprintf(log_stream(),
					"[%s - jitterentropy-rngd - %s] %s\n",
					now, sev, msg);
			}
		}
	}
}

void dolog(int severity, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vdolog(severity, fmt, args);
	va_end(args);

	if (JENT_LOG_ERR == severity) {
		dealloc();
		exit(1);
	}
}

/* An error that ends the daemon with a status other than dolog()'s 1 */
void dolog_exit(int exit_status, const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	vdolog(JENT_LOG_ERR, fmt, args);
	va_end(args);

	dealloc();
	exit(exit_status);
}

static inline void memset_secure(void *s, int c, size_t n)
{
	memset(s, c, n);
	__asm__ __volatile__("" : : "r" (s) : "memory");
}

/*******************************************************************
 * entropy handler functions
 *******************************************************************/

/*
 * The status document of the Jitter RNG @ec at verbose level, @when it was
 * initialized: a line of the log per line of the document, which a single
 * message would not hold.
 */
static void log_status(struct rand_data *ec, const char *when)
{
	char buf[4096], *line, *saveptr = NULL;

	if (Verbosity < JENT_LOG_VERBOSE)
		return;

	if (jent_status(ec, buf, sizeof(buf))) {
		dolog(JENT_LOG_WARN, "Cannot obtain the status of the Jitter RNG");
		return;
	}

	dolog(JENT_LOG_VERBOSE, "Status of the Jitter RNG %s:", when);
	for (line = strtok_r(buf, "\n", &saveptr); line;
	     line = strtok_r(NULL, "\n", &saveptr))
		dolog(JENT_LOG_VERBOSE, "%s", line);
}

static ssize_t read_jent(struct kernel_rng *rng, char *buf, size_t buflen)
{
	unsigned int reinits = rng->ec ?
		jent_entropy_collector_reinitializations(rng->ec) : 0;
	ssize_t ret;

	/*
	 *jent_read_entropy_safe implies a changing H_submitter which is not
	 * allowed in SP800-90B.
	 */
	if (force_sp80090b)
		ret = jent_read_entropy(rng->ec, buf, buflen);
	else
		ret = jent_read_entropy_safe(&rng->ec, buf, buflen);

	/* jent_read_entropy_safe() replaces the collector to recover */
	if (rng->ec &&
	    jent_entropy_collector_reinitializations(rng->ec) != reinits)
		log_status(rng->ec, "after it re-initialized itself");

	if (ret >= 0)
		return ret;

	switch (ret) {
	/*
	 * A permanent health test failure: the noise source must not be used
	 * any more. gather_entropy_retry() would re-initialize the Jitter RNG
	 * as for any other failure, and the new instance, its health tests
	 * starting afresh, would carry on past the failure. So the daemon
	 * ends here, with or without --exit-on-error.
	 */
	case JENT_ERR_RCT_PERMANENT:
	case JENT_ERR_APT_PERMANENT:
	case JENT_ERR_LAG_PERMANENT:
	case JENT_ERR_RCT_MEM_PERMANENT:
		dolog_exit(RNGD_EXIT_PERMANENT,
			   "Permanent health test failure of the Jitter RNG (error code %zd): it must not be used any more",
			   ret);
	default:
		break;
	}

	dolog(JENT_LOG_WARN, "Cannot read entropy");

	return -EOPNOTSUPP;
}

static ssize_t gather_entropy(struct kernel_rng *rng)
{
	char buf[INJECT_BYTES];
	const size_t buflen = sizeof(buf);
	ssize_t ret = 0;

	ret = read_jent(rng, buf, buflen);
	if (ret < 0)
		goto out;

	/* Full entropy, where the kernel's interface takes a count at all */
	ret = rngd_os_inject(rng, buf, buflen, (size_t)ret);

	if (ret >= 0 && buflen != (size_t)ret) {
		dolog(JENT_LOG_WARN, "Injected %zd bytes into %s, expected %zu",
		      ret, rng->dev, buflen);
		ret = 0;
	}

out:
	memset_secure(buf, 0, sizeof(buf));

	if (exit_on_error && ret < 0) {
		/* We now exit as requested by caller */
		dealloc();
		exit((int)-ret);
	}

	/*
	 * A full injection means the kernel has been seeded. Announcing
	 * readiness from here rather than from the startup path covers every
	 * caller: should the very first seeding attempt fail, the daemon stays
	 * alive and retries, and the service manager is told about the success
	 * whenever it eventually happens.
	 */
	if (ret > 0) {
		seeded = 1;
		rngd_os_seeded();
	}

	return ret;
}

/*
 * Number of times the Jitter RNG is torn down and re-initialized when the
 * gathering of entropy reports an error.
 *
 * The retry has to be bounded: an error that does not heal (say, the
 * RNDADDENTROPY IOCTL being rejected) would otherwise make the caller spin in
 * a tight loop, burning a CPU indefinitely while re-allocating the entropy
 * collector on every iteration.
 */
#define GATHER_RETRIES	5

/*
 * A fresh Jitter RNG for gather_entropy_retry(). A failure only warns: the
 * caller retries, and the missing collector is a failed read until then.
 */
static int realloc_jent(struct kernel_rng *rng)
{
	int ret;

	if (NULL != rng->ec) {
		jent_entropy_collector_free(rng->ec);
		rng->ec = NULL;
	}

	ret = jent_entropy_init_ex(jent_osr, jent_flags);
	if (ret) {
		dolog(JENT_LOG_WARN, "The initialization of CPU Jitter RNG failed with error code %d",
		      ret);
		return -EAGAIN;
	}

	rng->ec = jent_entropy_collector_alloc(jent_osr, jent_flags);
	if (!rng->ec) {
		dolog(JENT_LOG_WARN, "Allocation of entropy collector failed");
		return -EAGAIN;
	}
	log_status(rng->ec, "after its re-initialization");

	return 0;
}

ssize_t gather_entropy_retry(struct kernel_rng *rng)
{
	unsigned int i;
	ssize_t written = gather_entropy(rng);

	for (i = 0; written < 0 && i < GATHER_RETRIES; i++) {
		dolog(JENT_LOG_DEBUG, "Re-initializing the Jitter RNG");
		if (realloc_jent(rng))
			continue;

		written = gather_entropy(rng);
	}

	if (written < 0) {
		struct timespec delay;

		dolog(JENT_LOG_WARN,
		      "Gathering of entropy still failing after %u retries",
		      GATHER_RETRIES);

		/*
		 * Back off before returning to the caller so that a permanent
		 * error does not translate into a hot loop.
		 */
		delay.tv_sec = 1;
		delay.tv_nsec = 0;
		nanosleep(&delay, NULL);
	}

	return written;
}

/*******************************************************************
 * Signal handling functions
 *******************************************************************/

/*
 * Signal handlers may only call async-signal-safe functions, see
 * signal-safety(7), so they do nothing but set a volatile sig_atomic_t flag.
 * The process_*() counterparts do the work, from the loop of rngd_os_run().
 */
volatile sig_atomic_t Term_pending = 0;

/* terminate the daemon cleanly */
static void sig_term(int sig)
{
	(void)sig;
	Term_pending = 1;
}

/* Deferred clean shutdown - does not return */
void process_term(void)
{
	dolog(JENT_LOG_DEBUG, "Shutting down cleanly");

	dealloc();

	if (use_syslog)
		closelog();

	exit(0);
}

/*
 * Install a signal handler via sigaction(2) - unlike signal(2) its semantics
 * are unambiguous across systems. SA_RESTART resumes blocking calls rather than
 * failing them with EINTR, but per signal(7) not pselect(2), which is how the
 * wait in rngd_os_run() notices a pending flag.
 */
int install_handler(int sig, void (*handler)(int), int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = flags | SA_RESTART;

	return sigaction(sig, &sa, NULL);
}

static void install_term(void)
{
	static const int term_sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	size_t i;

	dolog(JENT_LOG_DEBUG, "Install termination signal handler");

	for (i = 0; i < ARRAY_SIZE(term_sigs); i++) {
		/*
		 * SA_RESETHAND restores the default disposition once the
		 * signal was delivered: if the clean shutdown cannot complete,
		 * a second termination signal kills the daemon outright. This
		 * replaces the signal(SIGxxx, SIG_DFL) calls that the former
		 * handler performed on itself.
		 */
		if (install_handler(term_sigs[i], sig_term, (int)SA_RESETHAND))
			dolog(JENT_LOG_ERR,
			      "Cannot install termination handler: %s",
			      strerror(errno));
	}
}

/*******************************************************************
 * allocation functions
 *******************************************************************/

void dealloc(void)
{
	if (NULL != Random.ec) {
		jent_entropy_collector_free(Random.ec);
		Random.ec = NULL;
	}
	rngd_os_dealloc(&Random);
	if (-1 != Random.fd) {
		close(Random.fd);
		Random.fd = -1;
	}
	if (-1 != Pidfile_fd) {
		close(Pidfile_fd);
		Pidfile_fd = -1;
		if (NULL != Pidfile)
			unlink(Pidfile);
	}
}

static int alloc_rng(struct kernel_rng *rng)
{
	rng->ec = jent_entropy_collector_alloc(jent_osr, jent_flags);
	if (!rng->ec) {
		dolog(JENT_LOG_ERR, "Allocation of entropy collector failed");
		return -EAGAIN;
	}

	if (status) {
		char buf[4096];
		int ret = jent_status(rng->ec, buf, sizeof(buf));

		if (ret)
			return ret;

		/* As it is: jent_status() ends the document with a newline */
		fputs(buf, stderr);
		return -EAGAIN;
	}

	log_status(rng->ec, "after its initialization");

	rng->fd = open(rng->dev, O_WRONLY);
	if (-1 == rng->fd) {
		int errsv = errno;

		dolog(JENT_LOG_ERR, "Open of %s failed: %s", rng->dev, strerror(errno));
		return -errsv;
	}

	return 0;
}

static int alloc(void)
{
	int ret = 0;
	ssize_t written = 0;

	ret = jent_entropy_init_ex(jent_osr, jent_flags);
	if (ret) {
		dolog(JENT_LOG_ERR, "The initialization of CPU Jitter RNG failed with error code %d\n", ret);
		return ret;
	}

	ret = alloc_rng(&Random);
	if (ret)
		return ret;

	ret = rngd_os_alloc(&Random);
	if (ret)
		return ret;

	written = gather_entropy(&Random);
	if (written >= 0) {
		dolog(JENT_LOG_VERBOSE, "%zd bytes to %s", written, Random.dev);
	} else {
		/*
		 * We consider this as no error at this point - note that
		 * dolog(JENT_LOG_ERR) terminates the daemon, which would defeat the
		 * re-initialization performed by gather_entropy_retry().
		 * gather_entropy() already honors --exit-on-error itself.
		 */
		dolog(JENT_LOG_WARN, "Cannot write to %s, failure: %zd",
		      Random.dev, written);
	}

	return 0;
}

/*******************************************************************
 * --generate
 *******************************************************************/

/* Bytes of the Jitter RNG --generate reads at a time */
#define GENERATE_CHUNK	1024

/* All @len bytes of @buf to @fd: 0 or a -errno */
static int write_all(int fd, const char *buf, size_t len)
{
	while (len) {
		ssize_t ret = write(fd, buf, len);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret < 0)
			return -errno;
		if (ret == 0)
			return -EIO;
		buf += ret;
		len -= (size_t)ret;
	}

	return 0;
}

/*
 * --generate: random bytes to stdout or --output, raw or as hex digits. A
 * failed read ends the run rather than re-initializing: what was written
 * stays, and the exit status tells.
 */
static void generate_bytes(void)
{
	static const char digits[] = "0123456789abcdef";
	char buf[GENERATE_CHUNK], hexbuf[2 * GENERATE_CHUNK];
	const char *name = output ? output : "stdout";
	unsigned int left = generate;
	int fd = STDOUT_FILENO, ret;

	ret = jent_entropy_init_ex(jent_osr, jent_flags);
	if (ret)
		dolog(JENT_LOG_ERR, "The initialization of CPU Jitter RNG failed with error code %d",
		      ret);

	/* In Random, so that dealloc() frees it on every exit */
	Random.ec = jent_entropy_collector_alloc(jent_osr, jent_flags);
	if (!Random.ec)
		dolog(JENT_LOG_ERR, "Allocation of entropy collector failed");
	log_status(Random.ec, "after its initialization");

	if (output) {
		struct stat st;

		/*
		 * Nothing planted to read the bytes or to clobber a file:
		 * O_TRUNC waits for the check, and O_NONBLOCK keeps a FIFO
		 * without a reader from blocking the open.
		 */
		fd = open(output, O_WRONLY|O_CREAT|O_NOFOLLOW|O_NONBLOCK,
			  S_IRUSR|S_IWUSR);
		if (-1 == fd)
			dolog(JENT_LOG_ERR, "Cannot open %s: %s", output,
			      strerror(errno));
		if (fstat(fd, &st))
			dolog(JENT_LOG_ERR, "Cannot stat %s: %s", output,
			      strerror(errno));
		if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
		    st.st_nlink != 1)
			dolog(JENT_LOG_ERR, "%s is not a regular file of user %u with a single link",
			      output, (unsigned int)geteuid());
		if (fchmod(fd, S_IRUSR|S_IWUSR) || ftruncate(fd, 0) ||
		    -1 == fcntl(fd, F_SETFL, 0))
			dolog(JENT_LOG_ERR, "Cannot prepare %s: %s", output,
			      strerror(errno));
	}

	while (left) {
		size_t len = left < sizeof(buf) ? left : sizeof(buf), i;
		const char *data = buf;
		size_t datalen = len;
		ssize_t got = read_jent(&Random, buf, len);

		if (got < 0 || (size_t)got != len)
			break;

		if (hex) {
			for (i = 0; i < len; i++) {
				unsigned char c = (unsigned char)buf[i];

				hexbuf[2 * i] = digits[c >> 4];
				hexbuf[2 * i + 1] = digits[c & 0xf];
			}
			data = hexbuf;
			datalen = 2 * len;
		}

		ret = write_all(fd, data, datalen);
		if (ret)
			break;
		left -= (unsigned int)len;
	}

	memset_secure(buf, 0, sizeof(buf));
	memset_secure(hexbuf, 0, sizeof(hexbuf));

	if (left && !ret)
		dolog(JENT_LOG_ERR, "Reading the Jitter RNG failed after %u of %u bytes",
		      generate - left, generate);
	if (!ret && output && close(fd))
		ret = -errno;
	if (ret)
		dolog(JENT_LOG_ERR, "Cannot write to %s: %s", name,
		      strerror(-ret));

	dolog(JENT_LOG_VERBOSE, "%u bytes to %s", generate, name);
}

static void create_pid_file(const char *pid_file)
{
	char pid_str[12];	/* max. integer length + '\n' + null */
	int fd;

	/*
	 * Deliberately without O_EXCL: a PID file left behind by a daemon that
	 * was killed must not stop a new instance from starting. The advisory
	 * lock taken below - not the presence of the file - is what ensures
	 * that only one copy runs.
	 *
	 * O_NOFOLLOW retains the protection that O_EXCL used to provide
	 * against being redirected through a planted symlink.
	 */
	fd = open(pid_file, O_RDWR|O_CREAT|O_NOFOLLOW, S_IRUSR|S_IWUSR);
	if (fd == -1)
		dolog(JENT_LOG_ERR, "Cannot open pid file %s: %s", pid_file,
		      strerror(errno));

	if (lockf(fd, F_TLOCK, 0) == -1) {
		int errsv = errno;

		/*
		 * The file belongs to somebody else, so drop it without
		 * recording it in Pidfile_fd - otherwise dealloc() would
		 * unlink the PID file of the instance that is still running.
		 */
		close(fd);

		if (errsv == EAGAIN || errsv == EACCES)
			dolog(JENT_LOG_ERR,
			      "PID file already locked - another instance is running");
		else
			dolog(JENT_LOG_ERR, "Cannot lock pid file: %s",
			      strerror(errsv));
	}

	/* From here on we own the PID file and dealloc() may remove it. */
	Pidfile_fd = fd;

	if (ftruncate(Pidfile_fd, 0) == -1)
		dolog(JENT_LOG_ERR, "Cannot truncate pid file: %s", strerror(errno));

	/* write our pid to the pid file */
	snprintf(pid_str, sizeof(pid_str), "%d\n", getpid());
	if (write(Pidfile_fd, pid_str, strlen(pid_str)) !=
	    (ssize_t)strlen(pid_str))
		dolog(JENT_LOG_ERR, "Cannot write to pid file");
}

static void daemonize(void)
{
	int ready[2];
	pid_t pid;

	/* already a daemon */
	if (1 == getppid())
	       return;

	/*
	 * The parent exits with the verdict of the child, which writes a byte
	 * once it owns the PID file: a second instance naming the same file
	 * fails its start, and the file exists when the start returns. A child
	 * that exits before - dolog(JENT_LOG_ERR) - leaves only EOF.
	 */
	if (pipe(ready))
		dolog(JENT_LOG_ERR, "Cannot create pipe to daemonize: %s",
		      strerror(errno));

	/* Neither process is to write out what the other buffered. */
	fflush(NULL);

	pid = fork();
	if (pid < 0)
		dolog(JENT_LOG_ERR, "Cannot fork to daemonize\n");

	/*
	 * The parent process exits -- its resources go with it, and the child
	 * owns them from here on, so nothing is freed.
	 */
	if (0 < pid) {
		ssize_t n;
		char c;

		close(ready[1]);
		do {
			n = read(ready[0], &c, 1);
		} while (n < 0 && errno == EINTR);
		_exit(n == 1 ? 0 : 1);
	}

	/* we are the child now */
	close(ready[0]);

	/* new SID for the child process */
	if (setsid() < 0)
		dolog(JENT_LOG_ERR, "Cannot obtain new SID for child\n");

	/* Change the current working directory.  This prevents the current
	 * directory from being locked; hence not being able to remove it. */
	if ((chdir("/")) < 0)
		dolog(JENT_LOG_ERR, "Cannot change directory\n");

	if (Pidfile && strlen(Pidfile))
		create_pid_file(Pidfile);

	/* Redirect standard files to /dev/null */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
	freopen( "/dev/null", "r", stdin);
	freopen( "/dev/null", "w", stdout);
	freopen( "/dev/null", "w", stderr);
#pragma GCC diagnostic pop

	/* Started: release the parent. */
	if (write(ready[1], "", 1) != 1)
		dolog(JENT_LOG_ERR, "Cannot signal the start to the parent");
	close(ready[1]);

	/* stdout is no longer a terminal, so drop the colors */
	detect_color();
}


/*
 * Open /dev/null on each closed standard descriptor: /dev/random opened as 1
 * would give way to the /dev/null of daemonize(), and the syslog socket as 1
 * would receive the bytes of --generate. Returns the mask of those closed.
 */
static unsigned int open_std_fds(void)
{
	unsigned int closed = 0;
	int fd;

	for (fd = STDIN_FILENO; fd <= STDERR_FILENO; fd++) {
		if (-1 != fcntl(fd, F_GETFD) || EBADF != errno)
			continue;
		/* The lowest free number, which is fd */
		if (open("/dev/null", O_RDWR) != fd)
			exit(1);
		closed |= 1U << fd;
	}

	return closed;
}

int main(int argc, char *argv[])
{
	unsigned int closed_fds;
	int ret;

	/* Before anything opens a file */
	closed_fds = open_std_fds();

	parse_opts(argc, argv);

	/* After the options: --generate moves the log to stderr */
	detect_color();

	/*
	 * Establish the syslog connection before anything can log, and before
	 * daemonize() redirects the standard streams. LOG_NDELAY opens the
	 * socket right away rather than on the first message, so a failure to
	 * reach the logger surfaces here instead of much later.
	 */
	if (use_syslog)
		openlog("jitterentropy-rngd", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	/* Before the operating system's part: the kernel takes nothing here */
	if (generate) {
		/* Not into the /dev/null standing in for a closed stdout */
		if (!output && (closed_fds & (1U << STDOUT_FILENO)))
			dolog(JENT_LOG_ERR, "Standard output is closed");
		generate_bytes();
		dealloc();
		return 0;
	}

	rngd_os_start();

	ret = alloc();
	if (ret)
		goto out;

	/* alloc() injected once; a failure gets the retries the alarm has */
	if (oneshot) {
		if (!seeded)
			gather_entropy_retry(&Random);
		dealloc();
		return seeded ? 0 : 1;
	}

	if (!foreground)
		daemonize();
	install_term();
	rngd_os_run(&Random);
	/* NOTREACHED */

out:
	dealloc();
	return -ret;
}
