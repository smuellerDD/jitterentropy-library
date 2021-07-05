/*
 * Copyright (C) 2021, Joshua E. Hill <josh@keypair.us>
 * Copyright (C) 2021, Stephan Mueller <smueller@chronox.de>
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

#include "jitterentropy.h"
#include "jitterentropy-gcd.h"

/* The common divisor for all timestamp deltas */
static uint64_t jent_common_timer_gcd = 0;

static uint64_t *delta_history = NULL;

static inline int jent_gcd_tested(void)
{
	return (jent_common_timer_gcd != 0);
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

		r = a % b;

		a = b;
		b = r;
	}

	return a;
}

void jent_gcd_add_value(uint64_t delta, uint64_t idx)
{
	/* Watch for common adjacent GCD values */
	if (delta_history)
		delta_history[idx] = delta;
}

int jent_gcd_analyze(size_t nelem)
{
	uint64_t running_gcd = 0, delta_sum = 0;
	size_t i, count_mod;
	int ret = 0;

	if (!delta_history)
		return 0;

	/* First initialize the analysis state. */
	if (delta_history[0] % 100 == 0)
		count_mod = 1;
	else
		count_mod = 0;

	/* How many distinct gcd values were found? */
	for (i = 0; i < nelem; i++) {
		if (delta_history[i] % 100 == 0)
			count_mod++;

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

	/*
	 * Variations of deltas of time must on average be larger than 1 to
	 * ensure the entropy estimation implied with 1 is preserved.
	 */
	if (delta_sum <= nelem - 1) {
		ret = EMINVARVAR;
		goto out;
	}

	/*
	 * Ensure that we have variations in the time stamp below 100 for at
	 * least 10% of all checks -- on some platforms, the counter increments
	 * in multiples of 100, but not always
	 */
	if ((JENT_STUCK_INIT_THRES(nelem) < count_mod) ||
	    (running_gcd >= 100)) {
		ret = ECOARSETIME;
		goto out;
	}

	/*  Adjust all deltas by the observed (small) common factor. */
	jent_common_timer_gcd = running_gcd;

out:
	return ret;
}

int jent_gcd_init(size_t nelem)
{
	/* If the GCD was initialized once, we do not do it again */
	if (jent_gcd_tested())
		return 0;

	if (delta_history)
		return 1;

	delta_history = jent_zalloc(nelem * sizeof(uint64_t));
	if (!delta_history)
		return 1;

	return 0;
}

void jent_gcd_fini(size_t nelem)
{
	if (delta_history)
		jent_zfree(delta_history,
			   (unsigned int)(nelem * sizeof(uint64_t)));
	delta_history = NULL;
}

int jent_gcd_get(uint64_t *value)
{
	if (!jent_gcd_tested())
		return 1;

	*value = jent_common_timer_gcd;
	return 0;
}
