/*
 * Jitter RNG: unit tests for src/jitterentropy-gcd.c
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

#include "unit.h"

#include "jitterentropy-gcd.c"
#include "jitterentropy-arch-memory.c"

#define ELEM 1000

/* The Euclidean GCD itself, including the cases where one side is zero. */
static void test_gcd64(void)
{
	jent_ut_group("jent_gcd64");

	JENT_UT_EQ(jent_gcd64(12, 18), 6, "gcd(12, 18)");
	JENT_UT_EQ(jent_gcd64(18, 12), 6, "gcd(18, 12) is symmetric");
	JENT_UT_EQ(jent_gcd64(17, 13), 1, "gcd of two coprime values");
	JENT_UT_EQ(jent_gcd64(50, 50), 50, "gcd of a value with itself");
	JENT_UT_EQ(jent_gcd64(0, 7), 7, "gcd(0, x) is x");
	JENT_UT_EQ(jent_gcd64(7, 0), 7, "gcd(x, 0) is x");
	JENT_UT_EQ(jent_gcd64(0, 0), 0, "gcd(0, 0)");
	JENT_UT_EQ(jent_gcd64(UINT64_C(1) << 40, UINT64_C(1) << 36),
		   UINT64_C(1) << 36, "gcd of two large powers of two");
}

/*
 * jent_gcd_analyze() on crafted delta histories. Each case isolates one of the
 * conditions it reports.
 */
static void test_analyze(void)
{
	uint64_t *deltas = jent_gcd_init(ELEM, 0);
	unsigned int i;

	jent_ut_group("jent_gcd_analyze");

	if (!deltas) {
		JENT_UT_SKIP("jent_gcd_analyze", "allocation failed");
		return;
	}

	/*
	 * A history whose adjacent deltas differ by 1 every step: the variation
	 * of variations is ample and the GCD is 1, so nothing is reported.
	 */
	for (i = 0; i < ELEM; i++)
		deltas[i] = (i & 1) ? 101 : 100;
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, JENT_MIN_OSR), 0,
		   "a varying history with a GCD of 1 passes");

	/*
	 * A constant history has no variation at all, so the assumed 1/osr bits
	 * of entropy per sample cannot hold.
	 */
	for (i = 0; i < ELEM; i++)
		deltas[i] = 12345;
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, JENT_MIN_OSR), EMINVARVAR,
		   "a constant history is rejected");

	/*
	 * Enough variation, but every delta is a multiple of a counter
	 * granularity so large that the timer is too coarse to be usable.
	 */
	for (i = 0; i < ELEM; i++)
		deltas[i] = (uint64_t)(i + 1) * (UINT32_MAX / 2);
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, JENT_MIN_OSR), ECOARSETIME,
		   "a coarse timer granularity is rejected");

	/* No history at all is not an error, it is nothing to analyze. */
	JENT_UT_EQ(jent_gcd_analyze(NULL, ELEM, JENT_MIN_OSR), 0,
		   "a NULL history is tolerated");
	JENT_UT_EQ(jent_gcd_analyze(deltas, 0, JENT_MIN_OSR), 0,
		   "an empty history is tolerated");

	/*
	 * The oversampling rate is the divisor of the variation requirement, so
	 * a history that is too flat for a low osr passes at a high one. One
	 * step of 1 across ELEM samples needs osr >= ELEM.
	 */
	for (i = 0; i < ELEM; i++)
		deltas[i] = (i == ELEM - 1) ? 1001 : 1000;
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, 1), EMINVARVAR,
		   "a barely varying history is rejected at osr 1");
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, ELEM), 0,
		   "the same history passes at an osr as large as the sample count");

	jent_gcd_fini(deltas, ELEM);
	/* Must tolerate being handed nothing. */
	jent_gcd_fini(NULL, ELEM);
}

/*
 * The common GCD is module-global and established once, so these three run in
 * order and before test_analyze(), whose passing cases would establish one as a
 * side effect.
 */

/* Nothing has analyzed a timer yet, so there is no GCD to hand out. */
static void test_gcd_get_unset(void)
{
	uint64_t value = 0;

	jent_ut_group("jent_gcd_get before any analysis");

	JENT_UT_EQ(jent_gcd_get(&value), 1,
		   "jent_gcd_get reports that no GCD is established");
	JENT_UT_EQ(value, 0, "and leaves the caller's value alone");
}

/* Establishing one through the documented path, and reading it back. */
static void test_gcd_establish(void)
{
	uint64_t *deltas = jent_gcd_init(ELEM, 0);
	uint64_t value = 0;
	unsigned int i;

	jent_ut_group("jent_gcd_analyze establishes the common GCD");

	if (!deltas) {
		JENT_UT_SKIP("jent_gcd_establish", "allocation failed");
		return;
	}

	for (i = 0; i < ELEM; i++)
		deltas[i] = (uint64_t)i * 50;
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, JENT_MIN_OSR), 0,
		   "a history with a GCD of 50 passes");
	JENT_UT_EQ(jent_gcd_get(&value), 0, "jent_gcd_get now reports a GCD");
	JENT_UT_EQ(value, 50, "and it is the one the history has");

	jent_gcd_fini(deltas, ELEM);
}

/*
 * Established once and then left alone: the deltas the noise source collects
 * are divided by this value, so a later analysis replacing it would change the
 * meaning of every measurement taken so far.
 */
static void test_gcd_sticky(void)
{
	uint64_t *deltas = jent_gcd_init(ELEM, 0);
	uint64_t value = 0;
	unsigned int i;

	jent_ut_group("the established GCD is not replaced by a later analysis");

	if (!deltas) {
		JENT_UT_SKIP("jent_gcd_sticky", "allocation failed");
		return;
	}

	for (i = 0; i < ELEM; i++)
		deltas[i] = (i & 1) ? 101 : 100;
	JENT_UT_EQ(jent_gcd_analyze(deltas, ELEM, JENT_MIN_OSR), 0,
		   "a second history with a GCD of 1 passes");
	JENT_UT_EQ(jent_gcd_get(&value), 0, "a GCD is still reported");
	JENT_UT_EQ(value, 50, "and it is still the first one");

	jent_gcd_fini(deltas, ELEM);
}

/* The self test the library runs at startup. */
static void test_selftest(void)
{
	uint64_t running_gcd = 0, delta_sum = 0;

	jent_ut_group("jent_gcd_selftest");

	JENT_UT_EQ(jent_gcd_selftest(0), 0, "jent_gcd_selftest");

	/*
	 * What the self test checks, reached directly: the internal analysis
	 * declining a history it cannot use, and a history whose GCD is not
	 * the expected one. Both are how the self test detects that the
	 * analysis itself is broken, so neither is reachable through it while
	 * the analysis works.
	 */
	JENT_UT_NE(jent_gcd_analyze_internal(NULL, JENT_GCD_SELFTEST_ELEM,
					     &running_gcd, &delta_sum), 0,
		   "the analysis declines a history it was not given");
	JENT_UT_NE(jent_gcd_analyze_internal((uint64_t *)&delta_sum, 0,
					     &running_gcd, &delta_sum), 0,
		   "and one with no elements");

	{
		uint64_t history[JENT_GCD_SELFTEST_ELEM];
		unsigned int i;

		/* A GCD other than the one the self test expects. */
		for (i = 0; i < JENT_GCD_SELFTEST_ELEM; i++)
			history[i] = (uint64_t)i * 7;
		JENT_UT_EQ(jent_gcd_analyze_internal(history,
						     JENT_GCD_SELFTEST_ELEM,
						     &running_gcd, &delta_sum),
			   0, "a well-formed history is analyzed");
		JENT_UT_EQ(running_gcd, 7,
			   "and its GCD is the one it was built with");
		JENT_UT_NE(running_gcd, JENT_GCD_SELFTEST_EXP,
			   "which is not what the self test expects");
	}
}

int main(void)
{
	test_gcd64();
	test_gcd_get_unset();
	test_gcd_establish();
	test_gcd_sticky();
	test_analyze();
	test_selftest();

	return jent_ut_report("unit-gcd");
}
