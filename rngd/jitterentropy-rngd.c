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

#define _GNU_SOURCE

#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <asm/types.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <getopt.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <linux/random.h>
#include <linux/version.h>
#include <signal.h>

#include "jitterentropy.h"

#define MAJVERSION 1 /* API / ABI incompatible changes, functional changes that
		      * require consumer to be updated (as long as this number
		      * is zero, the API is not considered stable and can
		      * change without a bump of the major version) */
#define MINVERSION 3 /* API compatible, ABI may change, functional
		      * enhancements only, consumer can be left unchanged if
		      * enhancements are not considered */
#define PATCHLEVEL 3 /* API / ABI compatible, no functional changes, no
		      * enhancements, bug fixes only */

static int Verbosity = 0;
static int force_sp80090b = 0;
static int status = 0;

/*
 * When set, log messages are handed to syslog(3) instead of being printed to
 * stdout. This is what makes logging useful for a backgrounded daemon at all:
 * daemonize() redirects stdout and stderr to /dev/null, so without syslog
 * every message is discarded once the daemon detaches.
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
 * * Errors reported by IOCTLs of RNDADDENTROPY and RNDRESEEDCRNG to /dev/random
 * * Errors triggered by system calls including select(2), open(2), truncate(2),
 *   write(2), fork(2), as well as errors from library functions including
 *   lockf(3).
 */
static int exit_on_error = 0;

struct kernel_rng {
	int fd;
	struct rand_data *ec;
	struct rand_pool_info *rpi;
	const char *dev;
};

static struct kernel_rng Random = {
	/*.fd = */ -1,
	/*.ec = */ NULL,
	/*.rpi = */ NULL,
	/*.dev = */ "/dev/random"
};

/*
 * handler for /dev/urandom not needed as used IOCTL alters input_pool
static struct kernel_rng Urandom = {
	.fd = 0,
	.ec = NULL,
	.rpi = NULL,
	.dev = "/dev/urandom"
};
*/

static int Pidfile_fd = -1;
/* "/var/run/jitterentropy-rngd.pid" */
static char *Pidfile = NULL;

static int Entropy_avail_fd = -1;
static int Entropy_thresh_fd = -1;
static unsigned int jent_flags = 0;
static unsigned int jent_osr = 1;

#define ENTROPYBYTES 32
#define OVERSAMPLINGFACTOR 2
/*
 * Amount of data handed to the kernel in one RNDADDENTROPY operation, and thus
 * the size of the payload buffer trailing struct rand_pool_info.
 *
 * This is the single definition of that quantity: the buffer that is allocated,
 * the block of entropy that is gathered and the bound that is enforced before
 * copying into the buffer all have to agree, so they all derive from here.
 */
#define RNDADDENTROPY_BUFSIZE	(ENTROPYBYTES * OVERSAMPLINGFACTOR)
/* Total allocation size of struct rand_pool_info including its payload */
#define RNDADDENTROPY_ALLOCSIZE	(sizeof(struct rand_pool_info) + \
				 RNDADDENTROPY_BUFSIZE)
/*
 * After (force reseed wakeups), the installed alarm handler will unconditionally
 * trigger a reseed irrespective of the seed level in two phases. This ensures
 * that new seed is added after every (force reseed wakeups) * (alarm period).
 * PHASE1: 120(force reseed wakeups) * 5(alarm period) == 600s
 * PHASE2: 12(force reseed wakeups) * 50(alarm period) == 600s
 */
#define FORCE_RESEED_WAKEUPS_PHASE1	120
#define ALARM_PERIOD_PHASE1	5
#define FORCE_RESEED_WAKEUPS_PHASE2	12
#define ALARM_PERIOD_PHASE2	50
#define ENTROPYAVAIL "/proc/sys/kernel/random/entropy_avail"
#define ENTROPYTHRESH "/proc/sys/kernel/random/write_wakeup_threshold"
#define LRNG_FILE "/proc/lrng_type"

#define JENT_LOG_DEBUG	3
#define JENT_LOG_VERBOSE	2
#define JENT_LOG_WARN	1
#define JENT_LOG_ERR		0

static void install_alarm(unsigned int secs);
static void dealloc(void);
static int alloc(void);
static void dealloc_rng(struct kernel_rng *rng);
static void dolog(int severity, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static unsigned long kern_maj = ULONG_MAX, kern_minor, kern_patchlevel;

static void jentrng_versionstring(char *buf, size_t buflen)
{
	snprintf(buf, buflen, "jitterentropy-rngd %d.%d.%d",
		 MAJVERSION, MINVERSION, PATCHLEVEL);
}

/* Is the LRNG present instead of the legacy /dev/random? */
static int lrng_present(void)
{
	struct stat buf;
	static int lrng_present = -1;

	if (lrng_present < 0) {
		int ret = stat(LRNG_FILE, &buf);

		if (ret == -1 && errno == ENOENT)
			lrng_present = 0;
		else
			lrng_present = 1;
	}

	return lrng_present;
}

static int get_kernver(void)
{
	struct utsname kernel;
	char *saveptr = NULL;
	char *res = NULL;
	unsigned long maj, minor, patchlevel;

	if (kern_maj != ULONG_MAX)
		return 0;

	if (uname(&kernel))
		return -errno;

	/*
	 * Parse into local variables and only publish them once all three
	 * components were obtained. Otherwise a partial parse would leave
	 * kern_maj set, which makes all subsequent calls report success while
	 * kern_minor / kern_patchlevel hold bogus values.
	 */

	/* 5.11.2 */
	res = strtok_r(kernel.release, ".", &saveptr);
	if (!res)
		goto err;
	maj = strtoul(res, NULL, 10);

	res = strtok_r(NULL, ".", &saveptr);
	if (!res)
		goto err;
	minor = strtoul(res, NULL, 10);

	res = strtok_r(NULL, ".", &saveptr);
	if (!res)
		goto err;
	patchlevel = strtoul(res, NULL, 10);

	if (maj == ULONG_MAX)
		goto err;

	kern_maj = maj;
	kern_minor = minor;
	kern_patchlevel = patchlevel;

	return 0;

err:
	dolog(JENT_LOG_WARN, "Could not parse kernel version \"%s\"", kernel.release);
	return -EFAULT;
}

/* return true if kernel is greater or equal to given values, otherwise false */
static int kernver_ge(unsigned int maj, unsigned int minor,
		      unsigned int patchlevel)
{
	if (get_kernver())
		return 0;

	if (maj < kern_maj)
		return 1;
	if (maj == kern_maj) {
		if (minor < kern_minor)
			return 1;
		if (minor == kern_minor) {
			if (patchlevel <= kern_patchlevel)
				return 1;
		}
	}
	return 0;
}

static void usage(void)
{
	unsigned int ver = jent_version();
	char version[30];

	memset(version, 0, sizeof(version));
	jentrng_versionstring(version, sizeof(version));

	fprintf(stderr, "\njitterentropy rngd feeding entropy to input_pool of Linux RNG\n");
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
	fprintf(stderr, "\t-f --flags\tInteger with flags used to allocate Jitter RNG\n");
	fprintf(stderr, "\t-o --osr\tInteger with OSR used to allocate Jitter RNG\n");
	fprintf(stderr, "\t   --status\tStatus information of the Jitter RNG - invoke with\n");
	fprintf(stderr, "\t           \tsame flags as used for runtime\n");
	fprintf(stderr, "\t   --exit-on-error\tCause the daemon to exit on errors\n");
	fprintf(stderr, "\nLRNG presence %sdetected\n",
		lrng_present() ? "" : "not ");
	exit(1);
}

/* Convert a command line argument into an unsigned int or bail out */
static unsigned int parse_uint(const char *str)
{
	char *endptr = NULL;
	unsigned long val;

	errno = 0;
	val = strtoul(str, &endptr, 10);

	/* Reject empty strings, trailing garbage and out-of-range values */
	if (errno || endptr == str || *endptr != '\0')
		usage();
#if ULONG_MAX > UINT_MAX
	if (val > UINT_MAX)
		usage();
#endif

	return (unsigned int)val;
}

static void parse_opts(int argc, char *argv[])
{
	int c = 0;
	char version[30];

	while (1) {
		int opt_index = 0;
		static struct option opts[] = {
			{"verbose", 0, 0, 0},
			{"pid", 1, 0, 0},
			{"help", 0, 0, 0},
			{"version", 0, 0, 0},
			{"sp800-90b", 0, 0, 0},
			{"flags", 1, 0, 0},
			{"osr", 1, 0, 0},
			{"status", 0, 0, 0},
			{"exit-on-error", 0, 0, 0},
			{"syslog", 0, 0, 0},
			{"foreground", 0, 0, 0},
			{0, 0, 0, 0}
		};
		c = getopt_long(argc, argv, "svp:hf:o:lF", opts, &opt_index);
		if (-1 == c)
			break;
		switch (c) {
		case 0:
			switch (opt_index) {

			/* verbose */
			case 0:
				Verbosity++;
				break;

			/* pid */
			case 1:
				Pidfile = optarg;
				break;

			/* help */
			case 2:
				usage();
				break;

			/* version */
			case 3:
				jentrng_versionstring(version, sizeof(version));
				fprintf(stderr, "Version %s\n", version);
				fprintf(stderr, "Version Jitterentropy Core %u\n", jent_version());
				exit(0);
				break;

			/* sp800-90b */
			case 4:
				force_sp80090b = 1;
				break;

			/* flags */
			case 5:
				jent_flags = parse_uint(optarg);
				break;

			/* osr */
			case 6:
				jent_osr = parse_uint(optarg);
				break;

			/* status */
			case 7:
				status = 1;
				break;

			/* exit-on-error */
			case 8:
				exit_on_error = 1;
				break;

			/* syslog */
			case 9:
				use_syslog = 1;
				break;

			/* foreground */
			case 10:
				foreground = 1;
				break;

			default:
				usage();
			}
			break;
		case 'v':
			Verbosity++;
			break;
		case 'p':
			Pidfile = optarg;
			break;
		case 'h':
			usage();
			break;
		case 's':
			force_sp80090b = 1;
			break;
		case 'l':
			use_syslog = 1;
			break;
		case 'F':
			foreground = 1;
			break;
		case 'f':
			jent_flags = parse_uint(optarg);
			break;
		case 'o':
			jent_osr = parse_uint(optarg);
			break;
		default:
			usage();
		}
	}
}

/* ANSI SGR sequences used to colorize the log on a capable terminal */
#define COL_RESET	"\033[0m"
#define COL_DIM		"\033[2m"
#define COL_CYAN	"\033[36m"
#define COL_YELLOW	"\033[33m"
#define COL_RED		"\033[1;31m"

static int use_color = 0;

/*
 * Determine whether the log may carry ANSI escape sequences.
 *
 * Colors are only emitted when stdout is a terminal that is expected to
 * interpret them. Writing escape sequences into a file or a pipe would corrupt
 * the log for anything that later reads it, which is why this is detected
 * rather than enabled unconditionally.
 *
 * This is re-evaluated after daemonize() redirects stdout to /dev/null.
 */
static void detect_color(void)
{
	const char *term = getenv("TERM");

	use_color = 0;

	/* Escape sequences are meaningless unless a terminal reads them */
	if (!isatty(STDOUT_FILENO))
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

static void dolog(int severity, const char *fmt, ...)
{
	va_list args;
	char msg[1024];
	const char *sev, *col;
	char now[32];

	if (severity <= Verbosity) {
		int prio;

		va_start(args, fmt);
		vsnprintf(msg, sizeof(msg), fmt, args);
		va_end(args);

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
				printf("[%s%s%s - jitterentropy-rngd - %s%s%s] %s\n",
				       COL_DIM, now, COL_RESET,
				       col, sev, COL_RESET, msg);
			} else {
				printf("[%s - jitterentropy-rngd - %s] %s\n",
				       now, sev, msg);
			}
		}
	}

	if (JENT_LOG_ERR == severity) {
		dealloc();
		exit(1);
	}
}

static inline void memset_secure(void *s, int c, size_t n)
{
	memset(s, c, n);
	__asm__ __volatile__("" : : "r" (s) : "memory");
}

/*******************************************************************
 * service manager notification
 *******************************************************************/

/*
 * Send a status notification to the service manager following the protocol
 * documented in sd_notify(3): a single datagram written to the AF_UNIX socket
 * named by the NOTIFY_SOCKET environment variable.
 *
 * The protocol is implemented directly rather than by linking against
 * libsystemd. The daemon is started very early during boot, deliberately
 * depends on almost nothing, and must keep building on systems without
 * systemd - all of which a library dependency would work against.
 *
 * If NOTIFY_SOCKET is not set, no service manager is waiting for us and the
 * call does nothing. Failures are logged but never fatal: not being able to
 * talk to the service manager is no reason to stop delivering entropy.
 */
static void notify_service_manager(const char *state)
{
	const char *socket_path = getenv("NOTIFY_SOCKET");
	struct sockaddr_un addr;
	size_t path_len;
	int fd;

	if (!socket_path)
		return;

	path_len = strlen(socket_path);
	if (0 == path_len || path_len > sizeof(addr.sun_path)) {
		dolog(JENT_LOG_WARN,
		      "NOTIFY_SOCKET holds an unusable path of %zu bytes",
		      path_len);
		return;
	}

	fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (0 > fd) {
		dolog(JENT_LOG_WARN, "Cannot create notification socket: %s",
		      strerror(errno));
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, socket_path, path_len);

	/*
	 * A leading '@' denotes a socket in the abstract namespace, whose name
	 * begins with a NULL byte instead. The length passed to sendto(2) must
	 * then cover exactly the name, without a terminating NULL byte.
	 */
	if ('@' == addr.sun_path[0])
		addr.sun_path[0] = '\0';

	if (0 > sendto(fd, state, strlen(state), MSG_NOSIGNAL,
		       (struct sockaddr *)&addr,
		       (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
				   path_len))) {
		dolog(JENT_LOG_WARN, "Cannot notify service manager: %s",
		      strerror(errno));
	} else {
		dolog(JENT_LOG_DEBUG, "Notified service manager: %s", state);
	}

	close(fd);
}

/*
 * Report readiness once. The daemon is only truly operational after it has
 * seeded the kernel for the first time, so this is deliberately not called at
 * startup - a service ordered after us can then rely on the kernel having been
 * seeded by the time it runs.
 */
static void notify_ready(void)
{
	static int notified = 0;

	if (notified)
		return;

	notified = 1;
	notify_service_manager("READY=1");
}

/*******************************************************************
 * entropy handler functions
 *******************************************************************/

static ssize_t write_random(struct kernel_rng *rng, char *buf, size_t len,
			    size_t entropy_bytes, int force_reseed)
{
	ssize_t written = 0;
	int ret;

	if (len > SSIZE_MAX)
		return -EOVERFLOW;

	/*
	 * rpi->buf is allocated with exactly RNDADDENTROPY_BUFSIZE bytes -
	 * guard the memcpy below against a caller asking for more.
	 */
	if (len > RNDADDENTROPY_BUFSIZE) {
		dolog(JENT_LOG_WARN, "Injection of %zu bytes requested, buffer holds only %u bytes",
		      len, (unsigned int)RNDADDENTROPY_BUFSIZE);
		return -EOVERFLOW;
	}

	 /* value is in bits */
	rng->rpi->entropy_count = (entropy_bytes * 8);
	rng->rpi->buf_size = len;
	memcpy(rng->rpi->buf, buf, len);

	ret = ioctl(rng->fd, RNDADDENTROPY, rng->rpi);
	if (0 > ret) {
		int errsv = errno;

		dolog(JENT_LOG_WARN, "Error injecting entropy: %s", strerror(errsv));
		return -errsv;
	} else {
		dolog(JENT_LOG_DEBUG, "Injected %zu bytes with an entropy count of %zu bytes of entropy",
		      len, entropy_bytes);
		written = len;
	}

	rng->rpi->entropy_count = 0;
	rng->rpi->buf_size = 0;
	memset(rng->rpi->buf, 0, len);

	/*
	 * The LRNG does not require this IOCTL as the reseed is automatically
	 * triggered.
	 */
	if (force_reseed && !lrng_present()) {
		if (ioctl(rng->fd, RNDRESEEDCRNG) < 0) {
			static int logged = 0;

			written = -errno;
			if (errno == EINVAL)
				goto out;

			if (!logged) {
				dolog(JENT_LOG_WARN,
				      "Error triggering a reseed of the kernel DRNG: %s",
				      strerror(errno));
				logged = 1;
			}
		} else {
			dolog(JENT_LOG_DEBUG, "Reseeding of kernel DRNG triggered");
		}
	}

out:
	return written;
}

static ssize_t read_jent(struct kernel_rng *rng, char *buf, size_t buflen)
{
	ssize_t ret;

	/*
	 *jent_read_entropy_safe implies a changing H_submitter which is not
	 * allowed in SP800-90B.
	 */
	if (force_sp80090b)
		ret = jent_read_entropy(rng->ec, buf, buflen);
	else
		ret = jent_read_entropy_safe(&rng->ec, buf, buflen);

	if (ret >= 0)
		return ret;

	dolog(JENT_LOG_WARN, "Cannot read entropy");

	return -EOPNOTSUPP;
}

static ssize_t gather_entropy(struct kernel_rng *rng)
{
	sigset_t blocking_set, previous_set;
/*
 * Maximum numbers of blocks is determined by numbers of reseed IOCTLs: if
 * the reseed IOCTL is used, we call ceil(256 / 80) numbers of IOCTLs. As
 * each IOCTL may drain the entropy pool by 256 bits, we need to ensure that
 * after the numbers of IOCTLs, we finally inject more blocks than the numbers
 * of IOCTLs into the input_pool. Otherwise the entropy estimator will never
 * rise and we encounter an endless loop.
 */
#define ENTBLOCKS	(4 + 2 + 1)
	char buf[(RNDADDENTROPY_BUFSIZE * ENTBLOCKS)];
	ssize_t buflen = RNDADDENTROPY_BUFSIZE;
	ssize_t ret = 0;

	sigemptyset(&previous_set);
	sigemptyset(&blocking_set);
	sigaddset(&blocking_set, SIGALRM);

	sigprocmask(SIG_BLOCK, &blocking_set, &previous_set);

	if (lrng_present()) {
		/*
		 * The LRNG operates fully 90B compliant, no special handling
		 * is necessary.
		 */
		ret = read_jent(rng, buf, buflen);
		if (ret < 0)
			goto out;

		dolog(JENT_LOG_DEBUG, "LRNG: Inject %zd bits of data with %zd bits of entropy into BLAKE2s state",
		      buflen << 3, ret << 3);

		/*
		 * Write the entropy, LRNG seeds automatically - the Jitter RNG
		 * provides full entropy so, we tell the Linux RNG the amount of
		 * entropy.
		 */
		ret = write_random(rng, buf, buflen, ret, 0);
	} else {
		if (!kernver_ge(5, 18, 0)) {
			static int reported = 0;

			if (!reported) {
				dolog(JENT_LOG_WARN, "Kernel older than 5.18 detected - DRT.1 status unclear");
				reported = 1;
			}
		}

		/*
		 * AIS 20/31 DRT.1, no special handling is necessary.
		 */
		ret = read_jent(rng, buf, buflen);
		if (ret < 0)
			goto out;

		dolog(JENT_LOG_DEBUG, "Linux kernel >= 5.18: Inject %zd bits of data with %zd bits of entropy into BLAKE2s state",
		      buflen << 3, ret << 3);

		/*
		 * Write the entropy and trigger reseed - the Jitter RNG provides
		 * full entropy so, we tell the Linux RNG the amount of entropy.
		 */
		ret = write_random(rng, buf, buflen, ret, 1);
	}

	if (ret >= 0 && buflen != ret) {
		dolog(JENT_LOG_WARN, "Injected %zd bytes into %s, expected %zd",
		      ret, rng->dev, buflen);
		ret = 0;
	}

out:
	memset_secure(buf, 0, sizeof(buf));

	if (exit_on_error && ret < 0) {
		/* We now exit as requested by caller */
		dealloc();
		exit(-ret);
	}

	sigprocmask(SIG_SETMASK, &previous_set, NULL);

	/*
	 * A full injection means the kernel has been seeded. Announcing
	 * readiness from here rather than from the startup path covers every
	 * caller: should the very first seeding attempt fail, the daemon stays
	 * alive and retries, and the service manager is told about the success
	 * whenever it eventually happens.
	 */
	if (ret > 0)
		notify_ready();

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

static ssize_t gather_entropy_retry(struct kernel_rng *rng)
{
	unsigned int i;
	ssize_t written = gather_entropy(rng);

	for (i = 0; written < 0 && i < GATHER_RETRIES; i++) {
		int ret;

		dolog(JENT_LOG_DEBUG, "Re-initializing rngd");
		dealloc();

		ret = alloc();
		if (ret < 0) {
			dolog(JENT_LOG_WARN,
			      "Re-initialization of rngd failed with %d", ret);
			return ret;
		}

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

static int read_entropy_value(int fd)
{
	ssize_t data = 0;
	/* Room for the largest value ("4096") plus the trailing NULL byte */
	char buf[8];
	char *endptr = NULL;
	long entropy = 0;

	data = read(fd, buf, sizeof(buf) - 1);
	lseek(fd, 0, SEEK_SET);

	if (0 > data) {
		dolog(JENT_LOG_WARN, "Error reading data from entropy fd: %s",
		      strerror(errno));
		return 0;
	}
	if (0 == data) {
		dolog(JENT_LOG_WARN, "Could not read data from entropy fd");
		return 0;
	}

	/*
	 * read(2) does not NULL-terminate - do it here, as the conversion
	 * below would otherwise read beyond the buffer.
	 */
	buf[data] = '\0';

	errno = 0;
	entropy = strtol(buf, &endptr, 10);
	if (errno || endptr == buf) {
		dolog(JENT_LOG_WARN, "Cannot parse value read from entropy fd");
		return 0;
	}

	if (0 > entropy || 4096 < entropy) {
		dolog(JENT_LOG_WARN, "Entropy read from entropy fd (%ld) is outside of range",
		      entropy);
		return 0;
	}

	return (int)entropy;
}

/*******************************************************************
 * Signal handling functions
 *******************************************************************/

/*
 * Signal handlers must restrict themselves to the functions listed as
 * async-signal-safe in POSIX.1 signal-safety(7). Writing a flag of type
 * volatile sig_atomic_t is permitted, so the handlers below do nothing but
 * that - the actual work is carried out by the process_*() counterparts
 * invoked from the main loop in select_fd().
 *
 * The previous implementation performed the entire entropy gathering from
 * within the handler, which calls printf(3), malloc(3) and free(3). Should
 * such a signal arrive while the interrupted code holds the malloc arena lock
 * or is in the middle of a stdio operation, the daemon deadlocks or corrupts
 * its heap.
 */
static volatile sig_atomic_t Alarm_pending = 0;
static volatile sig_atomic_t Term_pending = 0;

static void sig_entropy_avail(int sig)
{
	(void)sig;
	Alarm_pending = 1;
}

/* terminate the daemon cleanly */
static void sig_term(int sig)
{
	(void)sig;
	Term_pending = 1;
}

/*
 * Wakeup and check entropy_avail -- this covers the drain of entropy
 * from the nonblocking_pool via get_random_bytes
 */
static void process_alarm(void)
{
	int entropy = 0, thresh = 0;
	ssize_t written = 0;
	static unsigned int force_reseed = FORCE_RESEED_WAKEUPS_PHASE1;
	static unsigned int alarm_period = ALARM_PERIOD_PHASE1;

	dolog(JENT_LOG_VERBOSE, "Wakeup call for alarm on %s", ENTROPYAVAIL);

	if (--force_reseed == 0) {
		force_reseed = FORCE_RESEED_WAKEUPS_PHASE2;
		alarm_period = ALARM_PERIOD_PHASE2;
		dolog(JENT_LOG_DEBUG, "Force reseed");
		written = gather_entropy_retry(&Random);
		dolog(JENT_LOG_VERBOSE, "%zd bytes written to /dev/random", written);
		goto out;
	}

	entropy = read_entropy_value(Entropy_avail_fd);
	thresh = read_entropy_value(Entropy_thresh_fd);

	if (0 == entropy || 0 == thresh)
		goto out;
	if (entropy >= thresh) {
		dolog(JENT_LOG_DEBUG, "Sufficient entropy %d available", entropy);
		goto out;
	}
	dolog(JENT_LOG_DEBUG, "Insufficient entropy %d available (threshold %d)",
	      entropy, thresh);
	written = gather_entropy_retry(&Random);
	dolog(JENT_LOG_VERBOSE, "%zd bytes written to /dev/random", written);
out:
	install_alarm(alarm_period);
	return;
}

/* Deferred clean shutdown - does not return */
static void process_term(void)
{
	dolog(JENT_LOG_DEBUG, "Shutting down cleanly");

	/* Prevent the alarm from interfering with the shutdown */
	alarm(0);
	signal(SIGALRM, SIG_IGN);

	dealloc();

	if (use_syslog)
		closelog();

	exit(0);
}

/*
 * Wakeup on insufficient entropy on /dev/random
 */
static void select_fd(void)
{
	fd_set fds;
	int ret = 0;
	ssize_t written = 0;
	sigset_t blocked, unblocked;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGALRM);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGQUIT);
	sigaddset(&blocked, SIGTERM);

	while (1) {
		/*
		 * Block the signals that drive the daemon for the duration of
		 * the flag test and the wait, and hand the previous - i.e.
		 * unblocked - mask to pselect(2). pselect restores that mask
		 * for as long as it waits and re-blocks before returning, so
		 * the test below and going to sleep are one atomic operation.
		 *
		 * Without this, a signal arriving between the test and the
		 * wait would set its flag, be gone by the time we sleep, and
		 * leave the daemon blocked until the next unrelated wakeup -
		 * on current kernels /dev/random never becomes writable, so a
		 * missed alarm would stall the daemon indefinitely.
		 *
		 * The window is kept narrow on purpose: the signals stay
		 * unblocked while entropy is gathered below, so a termination
		 * request is recorded the moment it arrives.
		 */
		sigprocmask(SIG_BLOCK, &blocked, &unblocked);

		if (Term_pending) {
			sigprocmask(SIG_SETMASK, &unblocked, NULL);
			process_term();
			/* NOTREACHED */
		}

		if (Alarm_pending) {
			Alarm_pending = 0;
			sigprocmask(SIG_SETMASK, &unblocked, NULL);
			process_alarm();
			continue;
		}

		FD_ZERO(&fds);
		dolog(JENT_LOG_DEBUG, "Polling /dev/random");
		FD_SET(Random.fd, &fds);
		/* only /dev/random implements polling */
		ret = pselect((Random.fd + 1), NULL, &fds, NULL, NULL,
			      &unblocked);

		sigprocmask(SIG_SETMASK, &unblocked, NULL);

		if (-1 == ret && EINTR != errno) {
			if (exit_on_error) {
				int errsv = errno;

				dealloc();
				exit(errsv);
			}
			dolog(JENT_LOG_ERR, "Select returned with error %s", strerror(errno));
		}
		if (0 <= ret) {
			dolog(JENT_LOG_VERBOSE, "Wakeup call for select on /dev/random");
			written = gather_entropy_retry(&Random);
			dolog(JENT_LOG_VERBOSE, "%zd bytes written to /dev/random",
			      written);
		}
	}
}

/*
 * Install a signal handler via sigaction(2) - unlike signal(2) its semantics
 * are unambiguous across systems.
 *
 * SA_RESTART is requested so that the blocking calls used elsewhere (read(2)
 * on the procfs files, the RNDADDENTROPY IOCTL) are resumed rather than
 * failing with EINTR. This does not affect the wait in select_fd(): per
 * signal(7) pselect(2) is never restarted and always reports EINTR, which is
 * exactly what the main loop needs to notice a pending flag.
 */
static int install_handler(int sig, void (*handler)(int), int flags)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = flags | SA_RESTART;

	return sigaction(sig, &sa, NULL);
}

static void install_alarm(unsigned int secs)
{
	if (lrng_present())
		return;
	dolog(JENT_LOG_DEBUG, "Install alarm signal handler");
	if (install_handler(SIGALRM, sig_entropy_avail, 0))
		dolog(JENT_LOG_ERR, "Cannot install alarm handler: %s",
		      strerror(errno));
	alarm(secs);
}

static void install_term(void)
{
	static const int term_sigs[] = { SIGHUP, SIGINT, SIGQUIT, SIGTERM };
	size_t i;

	dolog(JENT_LOG_DEBUG, "Install termination signal handler");

	for (i = 0; i < (sizeof(term_sigs) / sizeof(term_sigs[0])); i++) {
		/*
		 * SA_RESETHAND restores the default disposition once the
		 * signal was delivered: if the clean shutdown cannot complete,
		 * a second termination signal kills the daemon outright. This
		 * replaces the signal(SIGxxx, SIG_DFL) calls that the former
		 * handler performed on itself.
		 */
		if (install_handler(term_sigs[i], sig_term, SA_RESETHAND))
			dolog(JENT_LOG_ERR,
			      "Cannot install termination handler: %s",
			      strerror(errno));
	}
}

/*******************************************************************
 * allocation functions
 *******************************************************************/

static void dealloc_rng(struct kernel_rng *rng)
{
	if (NULL != rng->ec) {
		jent_entropy_collector_free(rng->ec);
		rng->ec = NULL;
	}
	if (NULL != rng->rpi) {
		memset(rng->rpi, 0, RNDADDENTROPY_ALLOCSIZE);
		free(rng->rpi);
		rng->rpi = NULL;
	}
	if (-1 != rng->fd) {
		close(rng->fd);
		rng->fd = -1;
	}
}

static void dealloc(void)
{
	dealloc_rng(&Random);
	if (-1 != Entropy_avail_fd) {
		close(Entropy_avail_fd);
		Entropy_avail_fd = -1;
	}
	if (-1 != Entropy_thresh_fd) {
		close(Entropy_thresh_fd);
		Entropy_thresh_fd = -1;
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
		char buf[2500];
		int ret = jent_status(rng->ec, buf, sizeof(buf));

		if (ret)
			return ret;

		fprintf(stderr, "%s\n", buf);
		return -EAGAIN;
	}

	rng->rpi = malloc(RNDADDENTROPY_ALLOCSIZE);
	if (!rng->rpi) {
		dolog(JENT_LOG_ERR, "Cannot allocate memory for random bytes");
		dealloc_rng(rng);
		return -ENOMEM;
	}

	rng->fd = open(rng->dev, O_WRONLY);
	if (-1 == rng->fd) {
		int errsv = errno;

		dolog(JENT_LOG_ERR, "Open of %s failed: %s", rng->dev, strerror(errno));
		dealloc_rng(rng);
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

	Entropy_avail_fd = open(ENTROPYAVAIL, O_RDONLY);
	if (-1 == Entropy_avail_fd) {
		int errsv = errno;

		dolog(JENT_LOG_ERR, "Open of %s failed: %s", ENTROPYAVAIL, strerror(errno));
		dealloc();
		return -errsv;
	}

	Entropy_thresh_fd = open(ENTROPYTHRESH, O_RDONLY);
	if (-1 == Entropy_thresh_fd) {
		int errsv = errno;

		dolog(JENT_LOG_ERR, "Open of %s failed: %s", ENTROPYTHRESH, strerror(errno));
		dealloc();
		return -errsv;
	}

	written = gather_entropy(&Random);
	if (written >= 0) {
		dolog(JENT_LOG_VERBOSE, "%zd bytes to /dev/random", written);
	} else {
		/*
		 * We consider this as no error at this point - note that
		 * dolog(JENT_LOG_ERR) terminates the daemon, which would defeat the
		 * re-initialization performed by gather_entropy_retry().
		 * gather_entropy() already honors --exit-on-error itself.
		 */
		dolog(JENT_LOG_WARN, "Cannot write to /dev/random, failure: %zd",
		      written);
	}

	return 0;
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
	pid_t pid;
	
	/* already a daemon */
	if (1 == getppid())
	       return;

	pid = fork();
	if (pid < 0)
		dolog(JENT_LOG_ERR, "Cannot fork to daemonize\n");

	/* the parent process exits -- nothing has been allocated, nothing
	 * needs to be freed */
	if (0 < pid)
		exit(0);

	/* we are the child now */

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

	/* stdout is no longer a terminal, so drop the colors */
	detect_color();
}


int main(int argc, char *argv[])
{
	int ret;

	detect_color();

	parse_opts(argc, argv);

	/*
	 * Establish the syslog connection before anything can log, and before
	 * daemonize() redirects the standard streams. LOG_NDELAY opens the
	 * socket right away rather than on the first message, so a failure to
	 * reach the logger surfaces here instead of much later.
	 */
	if (use_syslog)
		openlog("jitterentropy-rngd", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	if (geteuid())
		dolog(JENT_LOG_ERR, "Program must start as root!");

	ret = alloc();
	if (ret)
		goto out;

	if (!foreground)
		daemonize();
	install_term();
	install_alarm(ALARM_PERIOD_PHASE1);
	select_fd();
	/* NOTREACHED */

out:
	dealloc();
	return -ret;
}
