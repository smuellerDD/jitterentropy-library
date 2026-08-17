/*
 * Jitter RNG: minimal unit test scaffolding
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
 * Just enough to run assertions and report a count. Deliberately not a
 * framework dependency: the library has none, is built on platforms whose
 * package availability cannot be assumed (see the CI matrix), and what these
 * tests need is an assertion that keeps going and a process exit status.
 */

#ifndef JITTERENTROPY_UNIT_H
#define JITTERENTROPY_UNIT_H

/*
 * First, ahead of every other header: it states the Windows API level it
 * needs, and that has to precede <windows.h>. Same reasoning as the sources
 * under arch/, and the same value.
 */
#include "jitterentropy-memlock.h"

#include <stdio.h>
#include <string.h>

static unsigned int jent_ut_checks = 0;
static unsigned int jent_ut_failures = 0;
static unsigned int jent_ut_skipped = 0;

/*
 * For the fault injection that is reached on some backends only. The
 * interposers are defined before the source that would call them is absorbed,
 * so which of them a given platform uses is not yet known where they are
 * written - and a backend that calls none of them must not turn the build
 * noisy.
 */
#if defined(__GNUC__) || defined(__clang__)
# define JENT_UT_MAYBE_UNUSED __attribute__((unused))
#else
# define JENT_UT_MAYBE_UNUSED
#endif

#define JENT_UT_FAIL(fmt, ...)						       \
	do {								       \
		jent_ut_failures++;					       \
		printf("  FAILED %s:%d: " fmt "\n", __func__, __LINE__,	       \
		       __VA_ARGS__);					       \
	} while (0)

/* Boolean assertion. */
#define JENT_UT_TRUE(cond, what)					       \
	do {								       \
		jent_ut_checks++;					       \
		if (!(cond))						       \
			JENT_UT_FAIL("%s (%s)", what, #cond);		       \
	} while (0)

/* Equality of two integers, printing both sides on failure. */
#define JENT_UT_EQ(got, want, what)					       \
	do {								       \
		long long _g = (long long)(got);			       \
		long long _w = (long long)(want);			       \
									       \
		jent_ut_checks++;					       \
		if (_g != _w)						       \
			JENT_UT_FAIL("%s: %s is %lld, expected %lld", what,    \
				     #got, _g, _w);			       \
	} while (0)

#define JENT_UT_NE(got, unwanted, what)					       \
	do {								       \
		long long _g = (long long)(got);			       \
		long long _u = (long long)(unwanted);			       \
									       \
		jent_ut_checks++;					       \
		if (_g == _u)						       \
			JENT_UT_FAIL("%s: %s is %lld, expected anything else", \
				     what, #got, _g);			       \
	} while (0)

#define JENT_UT_MEM_EQ(got, want, len, what)				       \
	do {								       \
		jent_ut_checks++;					       \
		if (memcmp((got), (want), (len)))			       \
			JENT_UT_FAIL("%s: %s does not match the expected "     \
				     "%zu bytes", what, #got, (size_t)(len));  \
	} while (0)

/*
 * For a property that cannot be established on this build or this machine.
 * Counted and reported rather than passed over silently: a suite that quietly
 * tests nothing looks exactly like one that passes.
 */
#define JENT_UT_SKIP(what, why)						       \
	do {								       \
		jent_ut_skipped++;					       \
		printf("  skipped %s: %s\n", what, why);		       \
	} while (0)

/*
 * Flushed rather than left to the buffer: the output of these programs is a
 * pipe under CTest, where it is block buffered, and a suite that stops in the
 * middle - a timer that never converges, a loop that does not terminate - then
 * reports nothing at all about where it got to.
 */
static inline void jent_ut_group(const char *name)
{
	printf("%s\n", name);
	fflush(stdout);
}

/*
 * What a unit test program that allocates compliance-mode collectors does
 * before its first check. Only those: the raise is process-wide, and a suite
 * that never asks for locked memory has no reason to claim any.
 *
 * The compliance modes - JENT_NTG1 and JENT_FORCE_FIPS - imply
 * JENT_FORCE_SECURE_MEM, so the collector's memory has to be locked into RAM
 * or the allocation fails, and the operating system's default bound is below
 * what a collector maps: RLIMIT_MEMLOCK on POSIX, on Windows the process
 * minimum working set, which starts at 200 kB and makes VirtualLock() refuse
 * even the 256 kB block of a default collector. Every compliance-mode
 * allocation then returns NULL and every check needing one is skipped - which
 * on Windows was most of unit-error and both hard failures of unit-mock.
 *
 * Raising those bounds is process-wide state and not the library's to touch
 * (see arch/jitterentropy-arch-memory.h) but the process owner's, here the
 * test program. tests/jitterentropy-memlock.h asks for its default upper bound
 * rather than a size derived from one collector's flags, as a single program
 * allocates collectors with several.
 *
 * Best effort, and deliberately not reported: a machine that refuses the raise
 * leaves the suite exactly where it was, skipping the checks it cannot make.
 */
static inline void jent_ut_setup(void)
{
	jent_raise_memlock_limit(JENT_FORCE_SECURE_MEM);
	jent_init_secure_memory(JENT_FORCE_SECURE_MEM);
}

static inline int jent_ut_report(const char *suite)
{
	printf("%s: %u checks, %u failed, %u skipped\n", suite, jent_ut_checks,
	       jent_ut_failures, jent_ut_skipped);

	return jent_ut_failures ? 1 : 0;
}

#endif /* JITTERENTROPY_UNIT_H */
