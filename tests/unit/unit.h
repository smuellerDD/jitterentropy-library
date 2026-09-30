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
#include <stdlib.h>
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
 * JENT_UT_STRICT set in the environment, to anything but "0": this machine
 * is known to have a clock the startup accepts and memory it can lock, so no
 * verdict is the machine's and every one of them fails the case it would
 * have skipped. The error codes a clock the startup rejects produces -
 * ESTUCK, EHEALTH, ERCT among them - are the same ones a defect in the
 * health tests produces, and on a machine whose clock is not in doubt only
 * the second remains.
 */
static inline int jent_ut_strict(void)
{
	const char *s = getenv("JENT_UT_STRICT");

	return s && *s && strcmp(s, "0");
}

/*
 * Whether @ret, a startup self test that did not pass, is this machine's
 * verdict rather than a defect: a timer the startup test rejects, or no
 * memory that can be locked where secure memory is forced. On every backend
 * only FIPS mode forces it - the system's, or JENT_FORCE_FIPS or JENT_NTG1 in
 * the flags (see jent_update_secure_mem()) - or JENT_FORCE_SECURE_MEM in the
 * flags itself. Without it the libgcrypt and OpenSSL backends fall back to
 * ordinary memory when their arena has none, as the others do when the lock
 * fails; AWS-LC locks nothing in either case. Everything else fails: EHASH
 * and EGCD are the library's own self tests, and EMEM without a lock is a
 * failed malloc(). The same verdicts as tests/fuzz/fuzz-api.c.
 * JENT_UT_MACHINE_VERDICT() judges the startups the tests run with no flags,
 * jent_entropy_init() and jent_entropy_init_ex(0, 0);
 * JENT_UT_MACHINE_VERDICT_FLAGS() those run with @flags. For the programs that
 * absorb the library sources, which provide jent_fips_enabled(). Never a
 * verdict under JENT_UT_STRICT.
 */
#if defined(AWSLC)
# define JENT_UT_SECURE_MEM_FORCED(flags) 0
#else
# define JENT_UT_SECURE_MEM_FORCED(flags)				       \
	(((flags) & (JENT_FORCE_FIPS | JENT_NTG1 | JENT_FORCE_SECURE_MEM)) ||  \
	 jent_fips_enabled())
#endif

#define JENT_UT_MACHINE_VERDICT_FLAGS(ret, flags)			       \
	(!jent_ut_strict() &&						       \
	 ((ret) == ENOTIME || (ret) == ECOARSETIME ||			       \
	  (ret) == ENOMONOTONIC || (ret) == EMINVARVAR ||		       \
	  (ret) == ESTUCK || (ret) == EHEALTH || (ret) == ERCT ||	       \
	  ((ret) == EMEM && JENT_UT_SECURE_MEM_FORCED(flags))))

#define JENT_UT_MACHINE_VERDICT(ret) JENT_UT_MACHINE_VERDICT_FLAGS(ret, 0)

/*
 * A collector allocated with @flags that did not come, and whether that is the
 * machine or a defect. The machine's reasons, each asked of something other
 * than the allocation under test:
 *
 * - Secure memory forced - a compliance mode, or JENT_FORCE_SECURE_MEM - on a
 *   machine that lets the process lock none (jent_ut_memlock_available()).
 *
 * - With JENT_FORCE_INTERNAL_TIMER, an internal timer whose startup gives a
 *   verdict of the machine, asked without the compliance modes: under them a
 *   counting thread that does not start comes back as EMEM, the same code a
 *   lock that failed gives, and that would excuse a thread that never starts.
 *
 * - A startup over the same flags that gives a verdict of the machine: the
 *   allocation runs one, and a clock it rejects fails it.
 *
 * - A startup that passes but an allocation that does come on a later
 *   attempt: its own startup is longer in the compliance modes, and a coarse
 *   clock fails it now and then (see unit-mock.c on the macOS arm64 runners).
 *
 * Anything else - a startup that fails for another reason, or passes while
 * the allocation never comes - is a defect. Under JENT_UT_STRICT only the
 * last two are asked, and neither excuses the case. Asked under whatever time
 * source the caller has installed, as the allocation was. For the programs
 * that absorb the library sources, which provide jent_fips_enabled(),
 * jent_entropy_init_ex() and jent_entropy_collector_alloc().
 */
#define JENT_UT_NO_COLLECTOR(what, flags)				       \
	do {								       \
		unsigned int _f = (flags), _i;				       \
		unsigned int _t = _f & ~(unsigned int)(JENT_FORCE_FIPS |       \
						       JENT_NTG1 |	       \
						       JENT_FORCE_SECURE_MEM); \
		struct rand_data *_ec = NULL;				       \
		int _r = 0;						       \
									       \
		if (!jent_ut_strict() && JENT_UT_SECURE_MEM_FORCED(_f) &&      \
		    !jent_ut_memlock_available()) {			       \
			JENT_UT_SKIP(what, "no memory can be locked on this "  \
				     "machine");			       \
			break;						       \
		}							       \
		if ((_f & JENT_FORCE_INTERNAL_TIMER) &&			       \
		    (_r = jent_entropy_init_ex(0, _t)) != 0) {		       \
			if (JENT_UT_MACHINE_VERDICT_FLAGS(_r, _t))	       \
				JENT_UT_SKIP(what, "the internal timer does "  \
					     "not pass its startup on this "   \
					     "machine");		       \
			else						       \
				JENT_UT_FAIL("%s: no collector was allocated, "\
					     "the internal timer's startup "   \
					     "fails with %d", what, _r);       \
			break;						       \
		}							       \
		if ((_r = jent_entropy_init_ex(0, _f)) != 0) {		       \
			if (JENT_UT_MACHINE_VERDICT_FLAGS(_r, _f))	       \
				JENT_UT_SKIP(what, "the startup does not "     \
					     "pass on this machine");	       \
			else						       \
				JENT_UT_FAIL("%s: no collector was allocated, "\
					     "the startup fails with %d",      \
					     what, _r);			       \
			break;						       \
		}							       \
		for (_i = 0; _i < 3 && !_ec && !jent_ut_strict(); _i++)	       \
			_ec = jent_entropy_collector_alloc(0, _f);	       \
		if (_ec) {						       \
			jent_entropy_collector_free(_ec);		       \
			JENT_UT_SKIP(what, "the allocation fails now and "     \
				     "then on this machine");		       \
		} else {						       \
			JENT_UT_FAIL("%s: no collector was allocated", what);  \
		}							       \
	} while (0)

/*
 * Whether @ret, returned by jent_read_entropy(), is an intermittent health
 * test failure: documented behaviour in FIPS mode on any machine, which
 * jent_read_entropy_safe() recovers from by reallocating.
 */
#define JENT_UT_INTERMITTENT(ret)					       \
	((ret) == JENT_ERR_RCT || (ret) == JENT_ERR_APT ||		       \
	 (ret) == JENT_ERR_LAG || (ret) == JENT_ERR_RCT_MEM)

/*
 * A startup that returned @ret, non-zero, where a case needs it to pass: a
 * skip if that is the machine's verdict, a failure otherwise.
 */
#define JENT_UT_NO_STARTUP(what, ret)					       \
	do {								       \
		int _r = (ret);						       \
									       \
		if (JENT_UT_MACHINE_VERDICT(_r))			       \
			JENT_UT_SKIP(what,				       \
				     "the startup does not pass on this "     \
				     "machine");			       \
		else							       \
			JENT_UT_FAIL("%s: the startup fails with %d", what,   \
				     _r);				       \
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
 * JENT_FORCE_SECURE_MEM, so the collector's state has to be locked into RAM
 * or the allocation fails. That is one page per collector, but a suite that
 * allocates collectors in numbers can exceed a tight RLIMIT_MEMLOCK or the
 * Windows minimum working set, and the libgcrypt and OpenSSL builds need a
 * secure arena only the process owner can create.
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

/*
 * Whether this machine lets the process lock the few pages a compliance-mode
 * collector needs (RLIMIT_MEMLOCK 0 does not; below 16 KiB the checks fail).
 * Asked of the platform, not of the allocator under test: a broken secure
 * allocator must fail its checks, not skip them. Solaris and Cygwin have no
 * RLIMIT_MEMLOCK.
 */
static inline int jent_ut_memlock_available(void)
{
#ifdef RLIMIT_MEMLOCK
	struct rlimit rl;

	if (getrlimit(RLIMIT_MEMLOCK, &rl))
		return 1;
	return rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= 16384;
#else
	return 1;
#endif
}

/*
 * The exit status: 1 on a failure, 77 - CTest's SKIP_RETURN_CODE - where
 * every case was skipped and nothing was checked, so that a program that
 * tested nothing is reported as skipped rather than passed.
 */
static inline int jent_ut_report(const char *suite)
{
	printf("%s: %u checks, %u failed, %u skipped\n", suite, jent_ut_checks,
	       jent_ut_failures, jent_ut_skipped);

	if (jent_ut_failures)
		return 1;
	return (!jent_ut_checks && jent_ut_skipped) ? 77 : 0;
}

#endif /* JITTERENTROPY_UNIT_H */
