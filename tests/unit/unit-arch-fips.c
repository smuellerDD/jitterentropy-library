/*
 * Jitter RNG: unit tests for the platform backends in arch/
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
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
 * Jitter RNG: unit tests for the FIPS mode query backend.
 *
 * Every assertion has to hold on every platform: what is checked is the
 * contract the backend header in arch/ states, not the behaviour of one
 * implementation.
 */

/*
 * As in the AMALGAMATED programs under tests/raw-entropy: several arch sources
 * are absorbed here, and the ones needing _GNU_SOURCE define it themselves
 * before their own includes - which is too late once an earlier source in this
 * translation unit has already pulled the headers in. Stated once up front.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

/*
 * setitimer() and its header are POSIX and MSVC has neither. Only the FIPS
 * indicator EINTR test uses them, and that test is Linux-only, as is the
 * backend it exercises - so this is keyed on __linux__ like the rest.
 */
#ifdef __linux__
# include <sys/time.h>
#endif

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

static void test_fips(void)
{
	int ret = jent_fips_enabled();

	jent_ut_group("jent_fips_enabled");

	JENT_UT_TRUE(ret == 0 || ret == 1, "the answer is 0 or 1");
	printf("  note: FIPS mode is %s\n", ret ? "on" : "off");
}

/* The allocator behind the collector state and the memory access block. */

/*
 * The kernel FIPS indicator, read from a file this test writes rather than
 * from /proc: the real one exists only on a kernel built with
 * CONFIG_CRYPTO_FIPS, so on every other machine none of these three outcomes
 * would be exercised.
 */
#ifdef JENT_ARCH_FIPS_PROC
static void test_fips_file(void)
{
	static const struct {
		const char *content;
		int want;
		const char *what;
	} cases[] = {
		{ "1\n",	1,	"an indicator of 1 means enabled" },
		{ "0\n",	0,	"an indicator of 0 means disabled" },
		{ "",		0,	"an empty file means disabled" },
		{ "2\n",	0,	"an unexpected value means disabled" },
	};
	size_t i;
	char path[] = "/tmp/jent-fips-test-XXXXXX";
	int fd = mkstemp(path);

	jent_ut_group("the kernel FIPS indicator");

	if (fd < 0) {
		JENT_UT_SKIP("the kernel FIPS indicator",
			     "no temporary file could be created");
		return;
	}
	close(fd);

	for (i = 0; i < JENT_ARRAY_SIZE(cases); i++) {
		FILE *f = fopen(path, "w");

		if (!f) {
			JENT_UT_SKIP(cases[i].what, "the file is not writable");
			continue;
		}
		fputs(cases[i].content, f);
		fclose(f);

		JENT_UT_EQ(jent_fips_enabled_file(path), cases[i].want,
			   cases[i].what);
	}

	remove(path);
	JENT_UT_EQ(jent_fips_enabled_file(path), 0,
		   "an absent indicator means disabled");
	JENT_UT_EQ(jent_fips_enabled_file("/nonexistent/jent/fips_enabled"), 0,
		   "and so does an unreachable path");
}

/*
 * The EINTR retry. Only a signal arriving while the read blocks reaches it and
 * a regular file never blocks, so the indicator is a pipe, named through
 * /proc/self/fd. sa_flags of 0 is the point: no SA_RESTART means EINTR. The
 * handler stays silent on its first firing, so the read has to go round again.
 */
static volatile sig_atomic_t jent_test_alarms;
static int jent_test_pipe_w = -1;

static void jent_test_alarm(int sig)
{
	(void)sig;

	jent_test_alarms++;
	if (jent_test_alarms >= 2) {
		/* write() is async-signal-safe; the result is of no interest. */
		ssize_t unused = write(jent_test_pipe_w, "1\n", 2);

		(void)unused;
	}
}

static void test_fips_file_eintr(void)
{
	struct sigaction sa, saved_sa;
	struct itimerval it, saved_it;
	char path[64];
	int fds[2];

	jent_ut_group("the FIPS indicator read interrupted by a signal");

	if (pipe(fds)) {
		JENT_UT_SKIP("an interrupted read is retried", "no pipe");
		return;
	}
	jent_test_pipe_w = fds[1];
	jent_test_alarms = 0;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = jent_test_alarm;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, &saved_sa)) {
		JENT_UT_SKIP("an interrupted read is retried",
			     "SIGALRM is not available");
		close(fds[0]);
		close(fds[1]);
		return;
	}

	/* Repeating, in case the first firing lands before the read. */
	memset(&it, 0, sizeof(it));
	it.it_value.tv_usec = 50000;
	it.it_interval.tv_usec = 50000;
	setitimer(ITIMER_REAL, &it, &saved_it);

	snprintf(path, sizeof(path), "/proc/self/fd/%d", fds[0]);
	JENT_UT_EQ(jent_fips_enabled_file(path), 1,
		   "the indicator is read on the attempt after the interruption");

	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
	sigaction(SIGALRM, &saved_sa, NULL);

	JENT_UT_TRUE(jent_test_alarms >= 2,
		     "the first signal arrived with nothing to read");
	printf("  note: the read was signalled %d time(s)\n",
	       (int)jent_test_alarms);

	close(fds[0]);
	close(fds[1]);
}
#else
static void test_fips_file(void)
{
	JENT_UT_SKIP("the kernel FIPS indicator", "not the /proc backend");
}
static void test_fips_file_eintr(void)
{
	JENT_UT_SKIP("the FIPS indicator read interrupted by a signal",
		     "not the /proc backend");
}
#endif

int main(void)
{
	test_fips();
	test_fips_file();
	test_fips_file_eintr();

	return jent_ut_report("unit-arch-fips");
}
