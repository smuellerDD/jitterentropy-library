/*
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
#include "qsort.h"

/* The common divisor for all timestamp deltas */
static uint64_t jent_common_timer_gcd = 0;

static uint64_t *delta_gcd = NULL;

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

static inline void jent_qsort(uint64_t *array, size_t nelem)
{
	uint64_t tmp;

#define LESS(i, j)	array[i] < array[j]
#define SWAP(i, j)	tmp = array[i], array[i] = array[j], array[j] = tmp
	QSORT(nelem, LESS, SWAP);
#undef LESS
#undef SWAP
}

void jent_gcd_add_value(uint64_t delta, uint64_t old_delta, uint64_t idx)
{
	/* Watch for common adjacent GCD values */
	if (delta_gcd)
		delta_gcd[idx] = jent_gcd64(delta, old_delta);
}

int jent_gcd_analyze(size_t nelem)
{
	uint64_t i, j , cur_gcd = 0, cur_gcd_count = 0, most_common_gcd = 0;
	uint64_t *gcd_table;
	unsigned int distinct_gcd_count = 0, distinct_gcd_count2 = 0;
	int ret = 0;

	if (!delta_gcd)
		return 0;

	/*
	 * Ensure that delta predominately change in integer multiples.
	 * Some timers increment by a fixed (non-1) amount each step.
	 * This code checks for such increments, and allows the library
	 * to output the number of such changes have occurred.
         * A candidate divisor must divide at least 90% of the test values.
	 * The largest such value is used.
	 */
	/* First, sort the delta gcd values. */
	jent_qsort(delta_gcd, nelem);

	/* How many distinct gcd values were found? */
	for (i = 0; nelem > i; i++) {
		if (delta_gcd[i] != cur_gcd) {
			cur_gcd = delta_gcd[i];
			distinct_gcd_count++;
		}
	}

	/* Need to count the last one as well. */
	if (cur_gcd != 0)
		distinct_gcd_count++;

	/* Guarantee to always have one entry */
	if (!distinct_gcd_count)
		distinct_gcd_count++;

	/*
	 * Now we'll determine the number of times that each distinct gcd
	 * occurs.
	 *
	 * We could have done this in the above pass at the cost of more
	 * memory.
	 */
	gcd_table = jent_zalloc(distinct_gcd_count * 2 * sizeof(uint64_t));
	if (!gcd_table)
		return EMEM;

	cur_gcd = 0;

	/* Populate the gcd table with the gcd values and their counts. */
	for (i = 0; nelem > i; i++) {
		if (delta_gcd[i] == cur_gcd) {
			cur_gcd_count++;
		} else {
			if (cur_gcd_count > 0) {
				/* Save the gcd. */
				gcd_table[2 * distinct_gcd_count2] = cur_gcd;
				gcd_table[2 * distinct_gcd_count2 + 1] =
								cur_gcd_count;
				distinct_gcd_count2++;
			}
			cur_gcd_count = 1;
			cur_gcd = delta_gcd[i];
		}
	}

	/*
	 * Save the last gcd.
	 */
	if (cur_gcd_count > 0) {
		gcd_table[2 * distinct_gcd_count2] = cur_gcd;
		gcd_table[2 * distinct_gcd_count2 + 1] = cur_gcd_count;
		distinct_gcd_count2++;
	}

	/*
	 * The number of times a specific GCD "works" isn't the number of times
	 * it directly appeared. We also need to include each GCD value that
	 * some integer multiple of that value occurs as a GCD. For example,
	 * if the GCD 2 occurred 10 times, the GCD 3 occurred 5 times and the
	 * GCD 6 occurred 5 times, then a GCD of 2 occurred should be counted
	 * as 15 times (10 for the value '2', and 5 for the value '6'),  the
	 * GCD 3 should be counted as 10 times, and the value 6 should be
	 * counted 5 times.
	 */
	for (i = 0; distinct_gcd_count2 > i; i++) {
		for (j = 0; i > j; j++) {
			/* Safety check */
			if (gcd_table[2 * j] == 0)
				continue;

			/*
			 * Account for all lower gcds that are divisors of the
			 * current gcd.
			 */
			if ((gcd_table[2 * i] % gcd_table[2 * j]) == 0)
				gcd_table[2 * j + 1] += gcd_table[2 * i + 1];
		}
	}

	/*
	 * We can now establish the largest GCD such that over 90% of the
	 * tested values are divisible by this value.
	 */
	for (i = 0; distinct_gcd_count2 > i; i++) {
		if (gcd_table[2 * i + 1] >
			JENT_STUCK_INIT_THRES(nelem)) {
			most_common_gcd = gcd_table[2 * i];
		}
	}

	if (most_common_gcd > 0) {
		if (most_common_gcd >= 100) {
			/* We found some divisor, and it is 100 or greater. */
			ret = ECOARSETIME;
			goto out;
		} else {
			/*
			 * We found a divisor, but it is small. Adjust all
			 * deltas by removing this factor.
			 */
			jent_common_timer_gcd = most_common_gcd;
		}
	}

out:
	if (gcd_table != NULL) {
		jent_zfree(gcd_table, distinct_gcd_count * 2 *
				      (unsigned int)sizeof(uint64_t));
	}

	return ret;
}

int jent_gcd_init(size_t nelem)
{
	/* If the GCD was initialized once, we do not do it again */
	if (jent_gcd_tested())
		return 0;

	if (delta_gcd)
		return 1;

	delta_gcd = jent_zalloc(nelem * sizeof(uint64_t));
	if (!delta_gcd)
		return 1;

	return 0;
}

void jent_gcd_fini(size_t nelem)
{
	if (delta_gcd)
		jent_zfree(delta_gcd,
			   (unsigned int)(nelem * sizeof(uint64_t)));
	delta_gcd = NULL;
}

int jent_gcd_get(uint64_t *value)
{
	if (!jent_gcd_tested())
		return 1;

	*value = jent_common_timer_gcd;
	return 0;
}
