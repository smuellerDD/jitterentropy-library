/*
 * jitterentropy-rngd on FreeBSD, NetBSD, OpenBSD and macOS: seeding the kernel
 * RNG through writes to /dev/random
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
 * None of these kernels has the Linux RNDADDENTROPY or an entropy count to
 * poll: each takes a write to /dev/random straight into its generator. Only
 * root may open the node for writing on FreeBSD and OpenBSD, on macOS it is
 * 0666. NetBSD 10 and later check every write against the writer's credential,
 * not the opener's, and refuse all but root's, crediting that one bit per bit.
 *
 * So the daemon writes one seed at the start with the privileges it has, and
 * one each interval after with none, except on NetBSD, where it stays root.
 *
 * No signalfd here: SIGUSR1 and SIGUSR2 stay blocked but while the daemon
 * waits, so that their handler, which notes them, runs in pselect(2) alone.
 */

#include <sys/types.h>
#if defined(__FreeBSD__)
# include <sys/capsicum.h>
#endif
#include <sys/select.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "jitterentropy-rngd.h"

/* The Linux daemon forces a reseed this often, in either phase. */
#define DEFAULT_INTERVAL	600
/* And retries a first seed that failed this often, as its first alarm. */
#define UNSEEDED_INTERVAL	5

/* The user after the first seed: NetBSD refuses the writes of all but root */
#if defined(__NetBSD__)
#define DEFAULT_USER	""
#else
#define DEFAULT_USER	"nobody"
#endif

static unsigned int interval = DEFAULT_INTERVAL;
static const char *user = DEFAULT_USER;

/* Set once drop_privileges() switched to user, for good */
static int dropped = 0;

/*
 * SIGUSR1 and SIGUSR2, noted by sig_user(): unblocked only while rngd_os_run()
 * waits, so that one arriving before does not end the daemon, as by default.
 */
static volatile sig_atomic_t Status_pending = 0;
static volatile sig_atomic_t Reseed_pending = 0;

/*******************************************************************
 * options
 *******************************************************************/

const char rngd_os_description[] =
	"jitterentropy rngd seeding the kernel RNG through /dev/random";

const char rngd_os_optstring[] = "t:u:d:";

const struct option rngd_os_options[] = {
	{"interval", 1, 0, 't'},
	{"user", 1, 0, 'u'},
	{"device", 1, 0, 'd'},
	{0, 0, 0, 0}
};

void rngd_os_parse(int c, const char *arg)
{
	switch (c) {
	case 't':
		interval = parse_uint(arg);
		if (!interval)
			usage();
		break;
	case 'u':
		user = arg;
		break;
	case 'd':
		Random.dev = arg;
		break;
	default:
		usage();
	}
}

void rngd_os_usage(void)
{
	fprintf(stderr, "\t-t --interval\tSeconds between seeds (default %u)\n",
		DEFAULT_INTERVAL);
	fprintf(stderr, "\t-u --user\tUser to switch to after the first seed when\n");
#if defined(__NetBSD__)
	fprintf(stderr, "\t\t\tstarted as root (default \"\", staying root)\n");
#else
	fprintf(stderr, "\t\t\tstarted as root (default nobody, \"\" stays root)\n");
#endif
	fprintf(stderr, "\t-d --device\tNode to write to (default /dev/random)\n");
}

/*******************************************************************
 * entropy handler functions
 *******************************************************************/

/* No count: NetBSD credits a write by the writer, the others credit none */
ssize_t rngd_os_inject(struct kernel_rng *rng, char *buf, size_t len,
		       size_t entropy_bytes)
{
	int ret;

	(void)entropy_bytes;

	ret = write_all(rng->fd, buf, len);
	if (ret) {
		int errsv = -ret;

		/*
		 * Refused for good, on NetBSD with a --user: the retries would
		 * only warn while the daemon seeded nothing any more.
		 */
		if (dropped && (EPERM == errsv || EACCES == errsv)) {
			/* dolog() exits, past the caller's wipe */
			memset_secure(buf, 0, len);
			dolog(JENT_LOG_ERR, "%s refuses the writes of user %s: %s",
			      rng->dev, user, strerror(errsv));
		}
		if (ENOSPC == errsv) {
			dolog(JENT_LOG_WARN, "%s took no data", rng->dev);
			return -EIO;
		}
		dolog(JENT_LOG_WARN, "Error writing to %s: %s",
		      rng->dev, strerror(errsv));
		return ret;
	}

	dolog(JENT_LOG_DEBUG, "Wrote %zu bytes to %s", len, rng->dev);

	return (ssize_t)len;
}

/* Root only: on macOS the daemon is started unprivileged in the first place. */
static void drop_privileges(void)
{
	struct passwd *pw;
	gid_t gid;

	if (geteuid() != 0 || !*user)
		return;

	pw = getpwnam(user);
	if (!pw)
		dolog_exit(RNGD_EXIT_ERROR, "Unknown user %s", user);
	gid = pw->pw_gid;

	if (setgroups(1, &gid) || setgid(gid) || setuid(pw->pw_uid))
		dolog(JENT_LOG_ERR, "Cannot switch to user %s: %s", user,
		      strerror(errno));
	if (setuid(0) != -1)
		dolog(JENT_LOG_ERR, "Could switch back to root after dropping it");

	dropped = 1;
	dolog(JENT_LOG_VERBOSE, "Running as user %s", user);
}

/*
 * Give up root, then confine the daemon to writing Random and computing - and
 * to re-initializing the Jitter RNG, by the library's own health test reset or
 * by gather_entropy_retry().
 *
 * Capability mode permits all a re-initialization calls: mmap(), mlock(),
 * cpuset_getaffinity(), hw.ncpu, thr_new(), getrandom(). No pledge promise
 * grants mlock(); "error" has it fail with ENOSYS instead of killing the
 * daemon, which the library tolerates as any refused lock - unless FIPS or
 * NTG.1 mode or JENT_FORCE_SECURE_MEM demands it, so those stay unpledged. The
 * unlink() of the PID file at exit fails the same way in both, leaving a stale
 * file the next start takes over. NetBSD and macOS have no sandbox a program
 * enters by itself.
 */
void rngd_os_sandbox(struct kernel_rng *rng)
{
#if defined(__FreeBSD__)
	cap_rights_t rights;
#endif

	drop_privileges();

#if defined(__OpenBSD__)
	(void)rng;
	if (jent_flags &
	    (JENT_FORCE_FIPS | JENT_NTG1 | JENT_FORCE_SECURE_MEM)) {
		dolog(JENT_LOG_VERBOSE,
		      "No pledge: with these flags a re-initialization must lock its memory");
		return;
	}
	if (pledge("stdio error", NULL))
		dolog(JENT_LOG_ERR, "pledge: %s", strerror(errno));
#elif defined(__FreeBSD__)
	cap_rights_init(&rights, CAP_WRITE);
	if (cap_rights_limit(rng->fd, &rights) || cap_enter())
		dolog(JENT_LOG_ERR, "Capsicum: %s", strerror(errno));
#else
	(void)rng;
#endif
}

/*******************************************************************
 * the signals and the wait
 *******************************************************************/

static void sig_user(int sig)
{
	if (SIGUSR1 == sig)
		Status_pending = 1;
	else
		Reseed_pending = 1;
}

/*
 * SIGUSR2: a seed now, as each interval writes one - which leaves the
 * interval as it runs. The outcome is logged whatever the verbosity, as
 * asked for.
 */
static void process_reseed(struct kernel_rng *rng)
{
	ssize_t written = gather_entropy_retry(rng);

	if (written > 0)
		dolog(JENT_LOG_NOTICE, "Seeded %s with %zd bytes, as SIGUSR2 asked",
		      rng->dev, written);
	else
		dolog(JENT_LOG_NOTICE, "The seed SIGUSR2 asked for failed: %zd",
		      written);
}

/* The interval from now on: when the next seed is due */
static void set_due(struct timespec *due)
{
	unsigned int secs = interval;

	if (!seeded && secs > UNSEEDED_INTERVAL)
		secs = UNSEEDED_INTERVAL;

	clock_gettime(CLOCK_MONOTONIC, due);
	due->tv_sec += (time_t)secs;
}

/*
 * What is left until @due, none once it passed: a full interval after each
 * signal would put the seed off for as long as the signals keep coming.
 */
static void time_left(const struct timespec *due, struct timespec *left)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	left->tv_sec = due->tv_sec - now.tv_sec;
	left->tv_nsec = due->tv_nsec - now.tv_nsec;
	if (left->tv_nsec < 0) {
		left->tv_sec--;
		left->tv_nsec += 1000000000L;
	}
	if (left->tv_sec < 0) {
		left->tv_sec = 0;
		left->tv_nsec = 0;
	}
}

/*******************************************************************
 * the daemon's hooks
 *******************************************************************/

void rngd_os_harden(void)
{
}

void rngd_os_drop_privileges(int seeding)
{
	(void)seeding;
}

void rngd_os_start(void)
{
	sigset_t usr;

	/* Before the startup of the Jitter RNG, which takes its time */
	user_signals(&usr);
	sigprocmask(SIG_BLOCK, &usr, NULL);
}

void rngd_os_alloc(struct kernel_rng *rng)
{
	/* Before the first seed, so that it is seen if that fails the start */
	if (!oneshot)
		dolog(JENT_LOG_NOTICE, "Seeding %s every %u s", rng->dev,
		      interval);
}

void rngd_os_seeded(void)
{
}

void rngd_os_run(struct kernel_rng *rng)
{
	sigset_t blocked, unblocked, waiting;
	struct timespec due;

	if (install_handler(SIGUSR1, sig_user, 0) ||
	    install_handler(SIGUSR2, sig_user, 0))
		dolog(JENT_LOG_ERR, "Cannot install the handler of SIGUSR1 and SIGUSR2: %s",
		      strerror(errno));

	term_signals(&blocked);

	set_due(&due);
	daemon_ready();

	while (1) {
		struct timespec ts;
		ssize_t written;
		int ret;

		/* As in the Linux select_fd(): test and wait are one. */
		sigprocmask(SIG_BLOCK, &blocked, &unblocked);

		if (Term_pending) {
			sigprocmask(SIG_SETMASK, &unblocked, NULL);
			process_term();
			/* NOTREACHED */
		}

		/* The user signals as well, blocked everywhere else */
		waiting = unblocked;
		sigdelset(&waiting, SIGUSR1);
		sigdelset(&waiting, SIGUSR2);

		time_left(&due, &ts);
		ret = pselect(0, NULL, NULL, NULL, &ts, &waiting);

		sigprocmask(SIG_SETMASK, &unblocked, NULL);

		if (-1 == ret && EINTR != errno) {
			if (exit_on_error) {
				int errsv = errno;

				dealloc();
				exit(rngd_exit_status(errsv));
			}
			dolog(JENT_LOG_ERR, "pselect returned with error %s",
			      strerror(errno));
		}

		/* Signals sent meanwhile asked for the same */
		if (Status_pending) {
			Status_pending = 0;
			process_status();
		}
		if (Reseed_pending) {
			Reseed_pending = 0;
			process_reseed(rng);
		}
		if (0 == ret) {
			written = gather_entropy_retry(rng);
			dolog(JENT_LOG_VERBOSE, "%zd bytes written to %s",
			      written, rng->dev);
			set_due(&due);
		}
	}
}

void rngd_os_dealloc(struct kernel_rng *rng)
{
	(void)rng;
}
