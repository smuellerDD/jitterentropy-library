/* Jitter RNG: GCD health test
 *
 * Copyright (C) 2021 - 2026, Joshua E. Hill <josh@keypair.us>
 * Copyright (C) 2021 - 2026, Stephan Mueller <smueller@chronox.de>
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

#include "jitterentropy-gcd.h"
#include "jitterentropy-internal.h"

/*
 * The common divisor for all timestamp deltas - one per clock, not one per
 * process. The granularities are unrelated (a platform clock may step by 1000,
 * the counting thread counts by one), and dividing by the other clock's is a
 * lost measurement: too large truncates the jitter away, too small leaves the
 * deltas un-normalized for the minimum-variation check below.
 *
 * 64 bits are not atomically accessible everywhere, so each divisor is
 * published through two 32-bit flags (see arch/jitterentropy-arch-atomic.h):
 * claimed is exchanged, so of several threads analyzing one clock exactly one
 * writes - they need not agree on the value - and set is stored with release
 * after that write and loaded with acquire before every read.
 */
static uint64_t jent_common_timer_gcd[JENT_GCD_CLOCKS] = { 0 };
static int jent_common_timer_gcd_claimed[JENT_GCD_CLOCKS] = { 0 };
static int jent_common_timer_gcd_set[JENT_GCD_CLOCKS] = { 0 };

/* Takes enable_notime as it stands, so that no call site has to translate. */
static inline unsigned int jent_gcd_clock(unsigned int notime)
{
	return notime ? JENT_GCD_CLOCK_NOTIME : JENT_GCD_CLOCK_PLATFORM;
}

static inline int jent_gcd_tested(unsigned int clock)
{
	return jent_atomic_load_int(&jent_common_timer_gcd_set[clock]);
}

/* A straight forward implementation of the Euclidean algorithm for GCD. */
static inline uint64_t jent_gcd64(uint64_t a, uint64_t b)
{
	/* Make a greater a than or equal b. */
	if (a < b) {
		uint64_t c = a;

		a = b;
		b = c;
	}

	/* Now perform the standard inner-loop for this algorithm.*/
	while (b != 0) {
		uint64_t r;

		r = jent_umod64(a, b);

		a = b;
		b = r;
	}

	return a;
}

static int jent_gcd_analyze_internal(uint64_t *delta_history, size_t nelem,
				     uint64_t *running_gcd_out,
				     uint64_t *delta_sum_out)
{
	uint64_t running_gcd, delta_sum = 0;
	size_t i;

	if (!delta_history || !nelem)
		return -EAGAIN;

	running_gcd = delta_history[0];

	/* Now perform the analysis on the accumulated delta data. */
	for (i = 1; i < nelem; i++) {
		/*
		 * ensure that we have a varying delta timer which is necessary
		 * for the calculation of entropy -- perform this check
		 * only after the first loop is executed as we need to prime
		 * the old_data value
		 */
		if (delta_history[i] >= delta_history[i - 1])
			delta_sum +=  delta_history[i] - delta_history[i - 1];
		else
			delta_sum +=  delta_history[i - 1] - delta_history[i];

		/*
		 * This calculates the gcd of all the delta values. that is
		 * gcd(delta_1, delta_2, ..., delta_nelem)

		 * Some timers increment by a fixed (non-1) amount each step.
		 * This code checks for such increments, and allows the library
		 * to output the number of such changes have occurred.
		 */
		running_gcd = jent_gcd64(delta_history[i], running_gcd);
	}

	*running_gcd_out = running_gcd;
	*delta_sum_out = delta_sum;

	return 0;
}

int jent_gcd_analyze(uint64_t *delta_history, size_t nelem, size_t osr,
		     unsigned int notime)
{
	unsigned int clock = jent_gcd_clock(notime);
	uint64_t running_gcd, delta_sum;
	int ret = jent_gcd_analyze_internal(delta_history, nelem, &running_gcd,
					    &delta_sum);

	if (ret == -EAGAIN)
		return 0;

	/*
	 * We assume 1/osr bits of entropy per sample. On average, variations
	 * of deltas must be larger than 1 over osr cases; we do not capture
	 * fractions. Hence delta_sum < (nelem / osr) means we cannot satisfy the
	 * 1/osr bits of entropy per sample assumption.
	 */
	if ((delta_sum * osr) < nelem) {
		ret = EMINVARVAR;
		goto out;
	}

	/* Set a sensible maximum value. */
	if (running_gcd >= UINT32_MAX / 2) {
		ret = ECOARSETIME;
		goto out;
	}

	/*
	 * Adjust all deltas by the observed (small) common factor.
	 *
	 * A zero divisor is not established, as it was not while the flag was
	 * the value itself: every caller of jent_gcd_get() divides by what it
	 * is given, and the "not established yet" answer is what makes it use
	 * a divisor of one instead. It takes an all-zero delta history to
	 * arrive here with one, which the variation check above rejects before
	 * this point - the guard states the invariant rather than covering a
	 * reachable case.
	 */
	if (running_gcd && !jent_gcd_tested(clock) &&
	    !jent_atomic_exchange_int(&jent_common_timer_gcd_claimed[clock],
				      1)) {
		jent_common_timer_gcd[clock] = running_gcd;
		jent_atomic_store_int(&jent_common_timer_gcd_set[clock], 1);
	}

out:
	return ret;
}

uint64_t *jent_gcd_init(size_t nelem, unsigned int flags)
{
	uint64_t *delta_history;

	delta_history = jent_zalloc(nelem * sizeof(uint64_t), flags);
	if (!delta_history)
		return NULL;

	return delta_history;
}

void jent_gcd_fini(uint64_t *delta_history, size_t nelem)
{
	if (delta_history)
		jent_zfree(delta_history, nelem * sizeof(uint64_t));
}

int jent_gcd_get(uint64_t *value, unsigned int notime)
{
	unsigned int clock = jent_gcd_clock(notime);

	if (!jent_gcd_tested(clock))
		return 1;

	*value = jent_common_timer_gcd[clock];
	return 0;
}

int jent_gcd_selftest(unsigned int flags)
{
#define JENT_GCD_SELFTEST_ELEM 10
#define JENT_GCD_SELFTEST_EXP 3ULL
	uint64_t *gcd = jent_gcd_init(JENT_GCD_SELFTEST_ELEM, flags);
	uint64_t running_gcd, delta_sum;
	unsigned int i;
	int ret = EGCD;

	if (!gcd)
		return EMEM;

	for (i = 0; i < JENT_GCD_SELFTEST_ELEM; i++)
		jent_gcd_add_value(gcd, i * JENT_GCD_SELFTEST_EXP, i);

	if (jent_gcd_analyze_internal(gcd, JENT_GCD_SELFTEST_ELEM,
				      &running_gcd, &delta_sum))
		goto out;

	if (running_gcd != JENT_GCD_SELFTEST_EXP)
		goto out;

	ret = 0;

out:
	jent_gcd_fini(gcd, JENT_GCD_SELFTEST_ELEM);
	return ret;
}
