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
 * Jitter RNG: unit tests for the CPU count backend.
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

static void test_ncpu(void)
{
	long ncpu = jent_ncpu();

	jent_ut_group("jent_ncpu");

	/*
	 * A negative value is the documented "cannot tell" answer; anything
	 * else must be a count that makes sense for a machine this code is
	 * running on.
	 */
	jent_ut_checks++;
	if (ncpu < 0)
		printf("  note: the CPU count is not discoverable here\n");
	else if (ncpu < 1)
		JENT_UT_FAIL("jent_ncpu returned %ld", ncpu);
	else
		printf("  note: %ld CPUs\n", ncpu);
}

#if defined(JENT_ARCH_NCPU_LINUX_SYSFS)
static void test_ncpu_parse(void)
{
	static const struct {
		const char *list;
		long want;
		const char *what;
	} cases[] = {
		{ "0\n",		1,	"a single CPU" },
		{ "0-3\n",		4,	"one contiguous range" },
		{ "0,2-5,8\n",		6,	"ranges with holes" },
		{ "0-0\n",		1,	"a range of one" },
		{ "\n",		-EINVAL, "an empty list" },
		{ "",			-EINVAL, "no list at all" },
		{ "x\n",		-EINVAL, "a non-numeric list" },
		{ "-1\n",		-EINVAL, "a negative CPU number" },
		{ "3-1\n",		-EINVAL, "a range that runs backwards" },
		{ "0-\n",		-EINVAL, "a range with no end" },
		/*
		 * Parses, but names more CPUs than a signed long can count -
		 * rejected before the width is added rather than overflowed
		 * into it.
		 */
		{ "0-9223372036854775807\n",
					-EINVAL, "a range wider than a CPU set" },
	};
	size_t i;
	char path[] = "/tmp/jent-ncpu-XXXXXX";
	int fd;

	jent_ut_group("the online CPU list");

	for (i = 0; i < JENT_ARRAY_SIZE(cases); i++)
		JENT_UT_EQ(jent_ncpu_parse_online(cases[i].list), cases[i].want,
			   cases[i].what);

	/* And the reading of it. */
	JENT_UT_TRUE(jent_ncpu_sysfs_file("/nonexistent/jent/online") < 0,
		     "an unreadable list is an error");

	fd = mkstemp(path);
	if (fd < 0) {
		JENT_UT_SKIP("an empty online list", "no temporary file");
		return;
	}
	close(fd);
	JENT_UT_TRUE(jent_ncpu_sysfs_file(path) < 0,
		     "an empty list file is an error");

	{
		FILE *f = fopen(path, "w");

		if (f) {
			fputs("0-7\n", f);
			fclose(f);
			JENT_UT_EQ(jent_ncpu_sysfs_file(path), 8,
				   "a list of eight CPUs is counted");
		}
	}
	remove(path);
}
#else
static void test_ncpu_parse(void)
{
	JENT_UT_SKIP("the online CPU list", "not the sysfs CPU backend");
}
#endif

/* The CSPRNG read behind the UUID, against files with known behaviour. */

int main(void)
{
	test_ncpu();
	test_ncpu_parse();

	return jent_ut_report("unit-arch-ncpu");
}
