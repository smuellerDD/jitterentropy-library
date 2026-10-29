/*
 * jitterentropy-rngd on Linux: feeding the input_pool through RNDADDENTROPY
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
#include <sys/types.h>
#include <asm/types.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/random.h>
#include <linux/version.h>
#include <signal.h>

#include "jitterentropy-rngd.h"

/* The payload buffer trailing struct rand_pool_info */
#define RNDADDENTROPY_BUFSIZE	INJECT_BYTES
/* Total allocation size of struct rand_pool_info including its payload */
#define RNDADDENTROPY_ALLOCSIZE	(sizeof(struct rand_pool_info) + \
				 RNDADDENTROPY_BUFSIZE)
/*
 * After (force reseed wakeups), the installed alarm handler will unconditionally
 * trigger a reseed irrespective of the seed level in two phases. This ensures
 * that new seed is added after every (force reseed wakeups) * (alarm period).
 * PHASE1: 120(force reseed wakeups) * 5(alarm period) == 600s
 * PHASE2: 12(force reseed wakeups) * 50(alarm period) == 600s
 * These are the defaults of --phase1 and --phase2.
 */
#define FORCE_RESEED_WAKEUPS_PHASE1	120
#define ALARM_PERIOD_PHASE1	5
#define FORCE_RESEED_WAKEUPS_PHASE2	12
#define ALARM_PERIOD_PHASE2	50
static unsigned int alarm_period[2] = {
	ALARM_PERIOD_PHASE1, ALARM_PERIOD_PHASE2
};
static unsigned int force_reseed_wakeups[2] = {
	FORCE_RESEED_WAKEUPS_PHASE1, FORCE_RESEED_WAKEUPS_PHASE2
};
#define ENTROPYAVAIL "/proc/sys/kernel/random/entropy_avail"
#define ENTROPYTHRESH "/proc/sys/kernel/random/write_wakeup_threshold"
#define LRNG_FILE "/proc/lrng_type"

/* The RNDADDENTROPY argument, for Random */
static struct rand_pool_info *rpi = NULL;

static int Entropy_avail_fd = -1;
static int Entropy_thresh_fd = -1;

static unsigned long kern_maj = ULONG_MAX, kern_minor, kern_patchlevel;

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

/*******************************************************************
 * options
 *******************************************************************/

const char rngd_os_description[] =
	"jitterentropy rngd feeding entropy to input_pool of Linux RNG";

const char rngd_os_optstring[] = "t:";

enum {
	OPT_PHASE1 = RNGD_OS_OPT,
	OPT_PHASE2,
};

const struct option rngd_os_options[] = {
	{"interval", 1, 0, 't'},
	{"phase1", 1, 0, OPT_PHASE1},
	{"phase2", 1, 0, OPT_PHASE2},
	{0, 0, 0, 0}
};

/*
 * --interval and --phase1 / --phase2 set the same schedule: not both. Which
 * of them set it, for rngd_os_start(): the LRNG has no schedule to set.
 */
enum { BY_INTERVAL = 1, BY_PHASES };
static int schedule_set_by = 0;

static void schedule_by(int by_option)
{
	if (schedule_set_by && schedule_set_by != by_option)
		usage();
	schedule_set_by = by_option;
}

/* PERIOD:WAKEUPS of --phase1 / --phase2, zero would stop the alarm */
static void parse_phase(const char *str, unsigned int phase)
{
	unsigned int period = parse_uint_until(str, ':', &str);
	unsigned int wakeups = parse_uint(str);

	if (!period || !wakeups)
		usage();
	alarm_period[phase] = period;
	force_reseed_wakeups[phase] = wakeups;
}

/* --interval: a seed every @secs in either phase, whatever the estimate */
static void parse_interval(const char *str)
{
	unsigned int secs = parse_uint(str), phase;

	if (!secs)
		usage();
	for (phase = 0; phase < 2; phase++) {
		alarm_period[phase] = secs;
		force_reseed_wakeups[phase] = 1;
	}
}

void rngd_os_parse(int c, const char *arg)
{
	switch (c) {
	case 't':
		schedule_by(BY_INTERVAL);
		parse_interval(arg);
		break;
	case OPT_PHASE1:
		schedule_by(BY_PHASES);
		parse_phase(arg, 0);
		break;
	case OPT_PHASE2:
		schedule_by(BY_PHASES);
		parse_phase(arg, 1);
		break;
	default:
		usage();
	}
}

void rngd_os_usage(void)
{
	fprintf(stderr, "\t-t --interval\tSeconds between wakeups that each inject, in place\n");
	fprintf(stderr, "\t\t\tof --phase1 and --phase2\n");
	fprintf(stderr, "\t   --phase1 PERIOD:WAKEUPS\tAlarm period in seconds and wakeups until\n");
	fprintf(stderr, "\t           \tthe forced reseed that ends phase 1 (default %u:%u)\n",
		ALARM_PERIOD_PHASE1, FORCE_RESEED_WAKEUPS_PHASE1);
	fprintf(stderr, "\t   --phase2 PERIOD:WAKEUPS\tThe same for every later forced reseed\n");
	fprintf(stderr, "\t           \t(default %u:%u)\n",
		ALARM_PERIOD_PHASE2, FORCE_RESEED_WAKEUPS_PHASE2);
	fprintf(stderr, "\nLRNG presence %sdetected\n",
		lrng_present() ? "" : "not ");
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
void rngd_os_seeded(void)
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
	rpi->entropy_count = (int)(entropy_bytes * 8);
	rpi->buf_size = (int)len;
	memcpy(rpi->buf, buf, len);

	ret = ioctl(rng->fd, RNDADDENTROPY, rpi);
	if (0 > ret) {
		int errsv = errno;

		dolog(JENT_LOG_WARN, "Error injecting entropy: %s", strerror(errsv));
		return -errsv;
	} else {
		dolog(JENT_LOG_DEBUG, "Injected %zu bytes with an entropy count of %zu bytes of entropy",
		      len, entropy_bytes);
		written = (ssize_t)len;
	}

	rpi->entropy_count = 0;
	rpi->buf_size = 0;
	memset(rpi->buf, 0, len);

	/*
	 * The LRNG does not require this IOCTL as the reseed is automatically
	 * triggered.
	 */
	if (force_reseed && !lrng_present()) {
		if (ioctl(rng->fd, RNDRESEEDCRNG) < 0) {
			static int logged = 0;
			int errsv = errno;

			/*
			 * A kernel before 4.17 has no such ioctl: the data is
			 * injected, only the reseed is left to the kernel.
			 */
			if (errsv == EINVAL)
				goto out;

			written = -errsv;
			if (!logged) {
				dolog(JENT_LOG_WARN,
				      "Error triggering a reseed of the kernel DRNG: %s",
				      strerror(errsv));
				logged = 1;
			}
		} else {
			dolog(JENT_LOG_DEBUG, "Reseeding of kernel DRNG triggered");
		}
	}

out:
	return written;
}

ssize_t rngd_os_inject(struct kernel_rng *rng, char *buf, size_t len,
		       size_t entropy_bytes)
{
	if (lrng_present()) {
		/*
		 * The LRNG operates fully 90B compliant, no special handling
		 * is necessary.
		 */
		/*
		 * Not random.c's BLAKE2s: the LRNG has its own hash and credits
		 * no more than its digest size.
		 */
		dolog(JENT_LOG_DEBUG, "LRNG: Inject %zu bits of data with %zu bits of entropy into the auxiliary pool, which credits at most its digest size",
		      len << 3, entropy_bytes << 3);

		/* Write the entropy, LRNG seeds automatically. */
		return write_random(rng, buf, len, entropy_bytes, 0);
	}

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
	dolog(JENT_LOG_DEBUG, "Linux kernel >= 5.18: Inject %zu bits of data with %zu bits of entropy into BLAKE2s state",
	      len << 3, entropy_bytes << 3);

	/* Write the entropy and trigger reseed. */
	return write_random(rng, buf, len, entropy_bytes, 1);
}

/* The value of an entropy file, or -1 where it cannot be read. */
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
		return -1;
	}
	if (0 == data) {
		dolog(JENT_LOG_WARN, "Could not read data from entropy fd");
		return -1;
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
		return -1;
	}

	if (0 > entropy || 4096 < entropy) {
		dolog(JENT_LOG_WARN, "Entropy read from entropy fd (%ld) is outside of range",
		      entropy);
		return -1;
	}

	return (int)entropy;
}

/*******************************************************************
 * the alarm and the wait
 *******************************************************************/

/*
 * The alarm: a timerfd that select_fd() waits for along with /dev/random,
 * rather than alarm(2) and SIGALRM - no signal handler, and the expiries the
 * daemon was too busy to see are counted rather than lost. -1 with the LRNG,
 * which reseeds by itself.
 */
static int Alarm_fd = -1;

/*
 * SIGUSR1 and SIGUSR2, blocked from rngd_os_start() on: one arriving before
 * this exists stays pending rather than ending the daemon, as by default.
 */
static int Signal_fd = -1;

/* Let the alarm go off every @secs seconds from now on. */
static void set_alarm(unsigned int secs)
{
	struct itimerspec its;

	if (-1 == Alarm_fd)
		return;

	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = (time_t)secs;
	its.it_interval.tv_sec = (time_t)secs;
	if (timerfd_settime(Alarm_fd, 0, &its, NULL))
		dolog(JENT_LOG_ERR, "Cannot set the alarm: %s", strerror(errno));
}

/*
 * Wakeup and check entropy_avail -- this covers the drain of entropy
 * from the nonblocking_pool via get_random_bytes. @expiries is the number of
 * periods since the last wakeup: they all count toward the forced reseed.
 */
static void process_alarm(uint64_t expiries)
{
	int entropy = 0, thresh = 0;
	ssize_t written = 0;
	static unsigned int phase = 0;
	static unsigned int wakeups = 0;
	unsigned int due = force_reseed_wakeups[phase] - wakeups;

	dolog(JENT_LOG_VERBOSE, "Wakeup call for alarm on %s", ENTROPYAVAIL);

	if (expiries >= due) {
		wakeups = 0;
		/* Phase 2 from the first forced reseed on, at its own period */
		if (!phase) {
			phase = 1;
			set_alarm(alarm_period[phase]);
		}
		dolog(JENT_LOG_DEBUG, "Force reseed");
		written = gather_entropy_retry(&Random);
		dolog(JENT_LOG_VERBOSE, "%zd bytes written to /dev/random", written);
		return;
	}
	wakeups += (unsigned int)expiries;

	entropy = read_entropy_value(Entropy_avail_fd);
	thresh = read_entropy_value(Entropy_thresh_fd);

	/* An empty pool is the case to act on, not an error. */
	if (0 > entropy || 0 >= thresh)
		return;
	if (entropy >= thresh) {
		dolog(JENT_LOG_DEBUG, "Sufficient entropy %d available", entropy);
		return;
	}
	dolog(JENT_LOG_DEBUG, "Insufficient entropy %d available (threshold %d)",
	      entropy, thresh);
	written = gather_entropy_retry(&Random);
	dolog(JENT_LOG_VERBOSE, "%zd bytes written to /dev/random", written);
}

/*
 * SIGUSR2: an injection now and a reseed of the kernel's RNG after it, as the
 * alarm forces one. The LRNG gets the injection alone, as on every occasion:
 * it reseeds by itself. The outcome is logged whatever the verbosity, as
 * asked for.
 */
static void process_reseed(void)
{
	ssize_t written = gather_entropy_retry(&Random);

	if (written <= 0)
		dolog(JENT_LOG_NOTICE, "The %s SIGUSR2 asked for failed: %zd",
		      lrng_present() ? "injection" : "reseed", written);
	else if (lrng_present())
		dolog(JENT_LOG_NOTICE, "Injected %zd bytes into the LRNG, as SIGUSR2 asked",
		      written);
	else
		dolog(JENT_LOG_NOTICE, "Reseeded the kernel with %zd bytes, as SIGUSR2 asked",
		      written);
}

/*
 * Wait for the alarm, for /dev/random to want more entropy - only that
 * implements polling, and current kernels never report it writable - and for
 * a termination signal.
 */
static void select_fd(void)
{
	fd_set rfds, wfds;
	int ret = 0, nfds;
	ssize_t written = 0;
	sigset_t blocked, unblocked;

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGINT);
	sigaddset(&blocked, SIGQUIT);
	sigaddset(&blocked, SIGTERM);

	while (1) {
		/*
		 * pselect(2) unblocks the termination signals only while it
		 * waits, so the flag test and going to sleep are atomic: a
		 * signal in between would wait for the next wakeup.
		 */
		sigprocmask(SIG_BLOCK, &blocked, &unblocked);

		if (Term_pending) {
			sigprocmask(SIG_SETMASK, &unblocked, NULL);
			process_term();
			/* NOTREACHED */
		}

		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		FD_SET(Random.fd, &wfds);
		nfds = Random.fd + 1;
		if (-1 != Alarm_fd) {
			FD_SET(Alarm_fd, &rfds);
			if (Alarm_fd >= nfds)
				nfds = Alarm_fd + 1;
		}
		FD_SET(Signal_fd, &rfds);
		if (Signal_fd >= nfds)
			nfds = Signal_fd + 1;
		dolog(JENT_LOG_DEBUG, "Polling /dev/random");
		ret = pselect(nfds, &rfds, &wfds, NULL, NULL, &unblocked);

		sigprocmask(SIG_SETMASK, &unblocked, NULL);

		if (-1 == ret) {
			if (EINTR == errno)
				continue;
			if (exit_on_error) {
				int errsv = errno;

				dealloc();
				exit(errsv);
			}
			dolog(JENT_LOG_ERR, "Select returned with error %s", strerror(errno));
		}

		if (-1 != Alarm_fd && FD_ISSET(Alarm_fd, &rfds)) {
			uint64_t expiries = 0;

			/* Non-blocking: a wakeup without an expiry reads none */
			if (sizeof(expiries) ==
			    read(Alarm_fd, &expiries, sizeof(expiries)))
				process_alarm(expiries);
		}
		if (FD_ISSET(Signal_fd, &rfds)) {
			struct signalfd_siginfo si;
			int status = 0, reseed = 0;

			/* Signals sent meanwhile asked for the same */
			while (sizeof(si) == read(Signal_fd, &si, sizeof(si))) {
				if (SIGUSR1 == si.ssi_signo)
					status = 1;
				else if (SIGUSR2 == si.ssi_signo)
					reseed = 1;
			}
			if (status)
				process_status();
			if (reseed)
				process_reseed();
		}
		if (FD_ISSET(Random.fd, &wfds)) {
			dolog(JENT_LOG_VERBOSE, "Wakeup call for select on /dev/random");
			written = gather_entropy_retry(&Random);
			dolog(JENT_LOG_VERBOSE, "%zd bytes written to /dev/random",
			      written);
		}
	}
}

/*******************************************************************
 * the daemon's hooks
 *******************************************************************/

/* SIGUSR1 and SIGUSR2, for Signal_fd */
static void user_signals(sigset_t *set)
{
	sigemptyset(set);
	sigaddset(set, SIGUSR1);
	sigaddset(set, SIGUSR2);
}

void rngd_os_start(void)
{
	sigset_t usr;

	if (geteuid())
		dolog(JENT_LOG_ERR, "Program must start as root!");

	/*
	 * The LRNG reseeds by itself and takes data when /dev/random reports
	 * it wants some: the daemon sets no alarm there, which would leave a
	 * schedule asked for unkept without a word.
	 */
	if (schedule_set_by && lrng_present())
		dolog(JENT_LOG_ERR, "%s has no effect on a kernel with the LRNG, which reseeds by itself",
		      BY_INTERVAL == schedule_set_by ?
		      "--interval" : "--phase1 / --phase2");

	/* Before the startup of the Jitter RNG, which takes its time */
	user_signals(&usr);
	sigprocmask(SIG_BLOCK, &usr, NULL);
}

int rngd_os_alloc(struct kernel_rng *rng)
{
	(void)rng;

	rpi = malloc(RNDADDENTROPY_ALLOCSIZE);
	if (!rpi) {
		dolog(JENT_LOG_ERR, "Cannot allocate memory for random bytes");
		return -ENOMEM;
	}

	Entropy_avail_fd = open(ENTROPYAVAIL, O_RDONLY);
	if (-1 == Entropy_avail_fd) {
		int errsv = errno;

		dolog(JENT_LOG_ERR, "Open of %s failed: %s", ENTROPYAVAIL, strerror(errno));
		return -errsv;
	}

	Entropy_thresh_fd = open(ENTROPYTHRESH, O_RDONLY);
	if (-1 == Entropy_thresh_fd) {
		int errsv = errno;

		dolog(JENT_LOG_ERR, "Open of %s failed: %s", ENTROPYTHRESH, strerror(errno));
		return -errsv;
	}

	return 0;
}

void rngd_os_run(struct kernel_rng *rng)
{
	sigset_t usr;

	(void)rng;

	/* After daemonize(), as the alarm below */
	user_signals(&usr);
	Signal_fd = signalfd(-1, &usr, SFD_CLOEXEC | SFD_NONBLOCK);
	if (-1 == Signal_fd)
		dolog(JENT_LOG_ERR, "Cannot create the signalfd for SIGUSR1 and SIGUSR2: %s",
		      strerror(errno));

	/* After daemonize(), so that the timer is the detached daemon's own */
	if (!lrng_present()) {
		Alarm_fd = timerfd_create(CLOCK_MONOTONIC,
					  TFD_CLOEXEC | TFD_NONBLOCK);
		if (-1 == Alarm_fd)
			dolog(JENT_LOG_ERR, "Cannot create the alarm: %s",
			      strerror(errno));
		dolog(JENT_LOG_DEBUG, "Alarm every %u s, forced reseed after %u wakeups; after the first every %u s and %u wakeups",
		      alarm_period[0], force_reseed_wakeups[0],
		      alarm_period[1], force_reseed_wakeups[1]);
		set_alarm(alarm_period[0]);
	}
	select_fd();
	/* NOTREACHED */
	exit(1);
}

void rngd_os_dealloc(struct kernel_rng *rng)
{
	(void)rng;

	if (NULL != rpi) {
		memset(rpi, 0, RNDADDENTROPY_ALLOCSIZE);
		free(rpi);
		rpi = NULL;
	}
	if (-1 != Entropy_avail_fd) {
		close(Entropy_avail_fd);
		Entropy_avail_fd = -1;
	}
	if (-1 != Entropy_thresh_fd) {
		close(Entropy_thresh_fd);
		Entropy_thresh_fd = -1;
	}
	if (-1 != Alarm_fd) {
		close(Alarm_fd);
		Alarm_fd = -1;
	}
	if (-1 != Signal_fd) {
		close(Signal_fd);
		Signal_fd = -1;
	}
}
