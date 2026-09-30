/* Jitter RNG: Health Tests
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

#include "jitterentropy-health.h"
#include "jitterentropy-noise.h"

/* As in jitterentropy-base.c: the entropy core must not be optimized. */
#ifdef __OPTIMIZE__
 #error "The CPU Jitter random number generator must not be compiled with optimizations. See documentation. Use the compiler switch -O0 for compiling the entropy core."
#endif

/* Every oversampling rate the library accepts needs a cutoff table entry. */
#define JENT_CUTOFF_TABLE_CHECK(t)					       \
	JENT_BUILD_BUG_ON(JENT_ARRAY_SIZE(t) != JENT_MAX_OSR)

static jent_fnptr fips_cb = NULL;
static int jent_health_cb_switch_blocked = 0;

void jent_health_cb_block_switch(void)
{
	jent_atomic_store_int(&jent_health_cb_switch_blocked, 1);
}

int jent_set_fips_failure_callback_internal(jent_fips_failure_cb cb)
{
	if (jent_atomic_load_int(&jent_health_cb_switch_blocked))
		return -EAGAIN;
	jent_atomic_store_fnptr(&fips_cb, (jent_fnptr)cb);
	return 0;
}

/***************************************************************************
 * Lag Predictor Test
 *
 * This test is a vendor-defined conditional test that is designed to detect
 * a known failure mode where the result becomes mostly deterministic
 * Note that (lag_observations & JENT_LAG_MASK) is the index where the next
 * value provided will be stored.
 ***************************************************************************/

#ifdef JENT_HEALTH_LAG_PREDICTOR

/*
 * These cutoffs are configured using an entropy estimate of 1/osr under an
 * alpha=2^(-22) for a window size of 131072. The other health tests use
 * alpha=2^-30, but operate on much smaller window sizes. This larger selection
 * of alpha makes the behavior per-lag-window similar to the APT test.
 *
 * The permanent cutoffs use alpha=2^(-44), the square of the intermittent one.
 *
 * The global cutoffs are calculated using the
 * 1 + InverseBinomialCDF(n=(JENT_LAG_WINDOW_SIZE-JENT_LAG_HISTORY_SIZE), p=2^(-1/osr); 1-alpha)
 * The local cutoffs are somewhat more complicated. For background, see Feller's
 * _Introduction to Probability Theory and Its Applications_ Vol. 1,
 * Chapter 13, section 7 (in particular see equation 7.11, where x is a root
 * of the denominator of equation 7.6).
 *
 * We'll proceed using the notation of SP 800-90B Section 6.3.8 (which is
 * developed in Kelsey-McKay-Turan paper "Predictive Models for Min-entropy
 * Estimation".)
 *
 * Here, we set p=2^(-1/osr), seeking a run of successful guesses (r) with
 * probability of less than (1-alpha). That is, it is very very likely
 * (probability 1-alpha) that there is _no_ run of length r in a block of size
 * JENT_LAG_WINDOW_SIZE-JENT_LAG_HISTORY_SIZE.
 *
 * We have to iteratively look for an appropriate value for the cutoff r.
 *
 * tests/health/cutoffs.py computes both tables, and --check compares them
 * against the ones below.
 */
static const unsigned int jent_lag_global_cutoff_lookup[20] =
	{ 66444,  93505, 104762, 110876, 114708, 117331, 119238, 120687, 121824,
	 122740, 123494, 124125, 124661, 125121, 125521, 125872, 126182, 126458,
	 126705, 126927 };
static const unsigned int jent_lag_global_cutoff_permanent_lookup[20] =
	{ 66877,  93897, 105109, 111189, 114994, 117597, 119487, 120921, 122046,
	 122952, 123697, 124319, 124848, 125302, 125696, 126042, 126347, 126618,
	 126861, 127080 };
static const unsigned int jent_lag_local_cutoff_lookup[20] =
	{  38,  75, 111, 146, 181, 215, 250, 284, 318, 351,
	  385, 419, 452, 485, 518, 551, 584, 617, 650, 683 };
static const unsigned int jent_lag_local_cutoff_permanent_lookup[20] =
	{  60, 119, 177, 234, 291,  347,  404,  460,  516,  571,
	  627, 683, 738, 793, 848,  903,  958, 1013, 1068, 1123 };

static void jent_lag_init(struct rand_data *ec, unsigned int osr)
{
	JENT_CUTOFF_TABLE_CHECK(jent_lag_global_cutoff_lookup);
	JENT_CUTOFF_TABLE_CHECK(jent_lag_global_cutoff_permanent_lookup);
	JENT_CUTOFF_TABLE_CHECK(jent_lag_local_cutoff_lookup);
	JENT_CUTOFF_TABLE_CHECK(jent_lag_local_cutoff_permanent_lookup);

	/*
	 * Establish the lag global and local cutoffs based on the presumed
	 * entropy rate of 1/osr.
	 */
	ec->lag_global_cutoff = jent_lag_global_cutoff_lookup[osr - 1];
	ec->lag_global_cutoff_permanent =
		jent_lag_global_cutoff_permanent_lookup[osr - 1];

	ec->lag_local_cutoff = jent_lag_local_cutoff_lookup[osr - 1];
	ec->lag_local_cutoff_permanent =
		jent_lag_local_cutoff_permanent_lookup[osr - 1];
}

/* Reverse lag_delta_history[from..to], a step of the rotation below. */
static void jent_lag_history_reverse(struct rand_data *ec, unsigned int from,
				     unsigned int to)
{
	while (from < to) {
		uint64_t tmp = ec->lag_delta_history[from];

		ec->lag_delta_history[from] = ec->lag_delta_history[to];
		ec->lag_delta_history[to] = tmp;
		from++;
		to--;
	}
}

/**
 * Reset the lag counters
 *
 * The delta history is kept, as the stuck test derives from it, and rotated
 * to where JENT_LAG_HISTORY() looks once lag_observations restarts at zero.
 *
 * @param[in] ec Reference to entropy collector
 */
static void jent_lag_reset(struct rand_data *ec)
{
	unsigned int i, rot = ec->lag_observations & JENT_LAG_MASK;

	/* Rotate left by rot, in place: three reversals. */
	if (rot) {
		jent_lag_history_reverse(ec, 0, rot - 1);
		jent_lag_history_reverse(ec, rot, JENT_LAG_HISTORY_SIZE - 1);
		jent_lag_history_reverse(ec, 0, JENT_LAG_HISTORY_SIZE - 1);
	}

	/* Reset Lag counters */
	ec->lag_prediction_success_count = 0;
	ec->lag_prediction_success_run = 0;
	ec->lag_best_predictor = 0; /* The first guess is basically arbitrary. */
	ec->lag_observations = 0;

	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
		ec->lag_scoreboard[i] = 0;
	}
}

/*
 * A macro for accessing the history. Index 0 is the last observed symbol
 * index 1 is the symbol observed two inputs ago, etc.
 */
#define JENT_LAG_HISTORY(EC,LOC)					       \
	((EC)->lag_delta_history[((EC)->lag_observations - (LOC) - 1) &	       \
	 JENT_LAG_MASK])

/**
 * Insert a new entropy event into the lag predictor test
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] current_delta Current time delta
 */
static void jent_lag_insert(struct rand_data *ec, uint64_t current_delta)
{
	uint64_t prediction;
	unsigned int i;

	/* Initialize the delta_history */
	if (ec->lag_observations < JENT_LAG_HISTORY_SIZE) {
		ec->lag_delta_history[ec->lag_observations] = current_delta;
		ec->lag_observations++;
		return;
	}

	/*
	 * The history is initialized. First make a guess and examine the
	 * results.
	 */
	prediction = JENT_LAG_HISTORY(ec, ec->lag_best_predictor);

	if (prediction == current_delta) {
		/* The prediction was correct. */
		ec->lag_prediction_success_count++;
		ec->lag_prediction_success_run++;

		if ((ec->lag_prediction_success_run >=
		     ec->lag_local_cutoff_permanent) ||
		    (ec->lag_prediction_success_count >=
		     ec->lag_global_cutoff_permanent))
			ec->health_failure |= JENT_LAG_FAILURE_PERMANENT;
		else if ((ec->lag_prediction_success_run >=
			  ec->lag_local_cutoff) ||
			 (ec->lag_prediction_success_count >=
			  ec->lag_global_cutoff))
			ec->health_failure |= JENT_LAG_FAILURE;
	} else {
		/* The prediction wasn't correct. End any run of successes.*/
		ec->lag_prediction_success_run = 0;
	}

	/* Now update the predictors using the current data. */
	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
		if (JENT_LAG_HISTORY(ec, i) == current_delta) {
			/*
			 * The ith predictor (which guesses i + 1 symbols in
			 * the past) successfully guessed.
			 */
			ec->lag_scoreboard[i] ++;

			/*
			 * Keep track of the best predictor (tie goes to the
			 * shortest lag)
			 */
			if (ec->lag_scoreboard[i] >
			    ec->lag_scoreboard[ec->lag_best_predictor])
				ec->lag_best_predictor = i;
		}
	}

	/*
	 * Finally, update the lag_delta_history array with the newly input
	 * value.
	 */
	ec->lag_delta_history[(ec->lag_observations) & JENT_LAG_MASK] =
								current_delta;
	ec->lag_observations++;

	/*
	 * lag_best_predictor now is the index of the predictor with the largest
	 * number of correct guesses.
	 * This establishes our next guess.
	 */

	/* Do we now need a new window? */
	if (ec->lag_observations >= JENT_LAG_WINDOW_SIZE)
		jent_lag_reset(ec);
}

static inline uint64_t jent_delta2(struct rand_data *ec, uint64_t current_delta)
{
	/* Note that delta2_n = delta_n - delta_{n-1} */
	return jent_delta(JENT_LAG_HISTORY(ec, 0), current_delta);
}

static inline uint64_t jent_delta3(struct rand_data *ec, uint64_t delta2)
{
	/*
	 * Note that delta3_n = delta2_n - delta2_{n-1}
	 *		      = delta2_n - (delta_{n-1} - delta_{n-2})
	 */
	return jent_delta(jent_delta(JENT_LAG_HISTORY(ec, 1),
				     JENT_LAG_HISTORY(ec, 0)), delta2);
}

void jent_lag_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	unsigned int i;

	new_ec->lag_prediction_success_run = old_ec->lag_prediction_success_run;
	new_ec->lag_prediction_success_count =
		old_ec->lag_prediction_success_count;
	new_ec->lag_best_predictor = old_ec->lag_best_predictor;
	new_ec->lag_observations = old_ec->lag_observations;

	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
		new_ec->lag_scoreboard[i] = old_ec->lag_scoreboard[i];
		new_ec->lag_delta_history[i] = old_ec->lag_delta_history[i];
	}
}


#else /* JENT_HEALTH_LAG_PREDICTOR */

static inline void jent_lag_insert(struct rand_data *ec, uint64_t current_delta)
{
	(void)ec;
	(void)current_delta;
}

static inline uint64_t jent_delta2(struct rand_data *ec, uint64_t current_delta)
{
	uint64_t delta2 = jent_delta(ec->last_delta, current_delta);

	ec->last_delta = current_delta;
	return delta2;
}

static inline uint64_t jent_delta3(struct rand_data *ec, uint64_t delta2)
{
	uint64_t delta3 = jent_delta(ec->last_delta2, delta2);

	ec->last_delta2 = delta2;
	return delta3;
}

static inline void jent_lag_init(struct rand_data *ec, unsigned int osr)
{
	(void)ec;
	(void)osr;
}

static inline void jent_lag_reset(struct rand_data *ec)
{
	(void)ec;
}

void jent_lag_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	new_ec->last_delta = old_ec->last_delta;
	new_ec->last_delta2 = old_ec->last_delta2;
}

#endif /* JENT_HEALTH_LAG_PREDICTOR */

/***************************************************************************
 * Adaptive Proportion Test
 *
 * This test complies with SP800-90B section 4.4.2.
 ***************************************************************************/

/*
 * See the SP 800-90B comment #10b for the corrected cutoff for the SP 800-90B
 * APT.
 * http://www.untruth.org/~josh/sp80090b/UL%20SP800-90B-final%20comments%20v1.9%2020191212.pdf
 * In in the syntax of R, this is C = 2 + qbinom(1 - 2^(-30), 511, 2^(-1/osr)).
 * (The original formula wasn't correct because the first symbol must
 * necessarily have been observed, so there is no chance of observing 0 of these
 * symbols.)
 *
 * For the alpha < 2^-53, R cannot be used as it uses a float data type without
 * arbitrary precision. tests/health/cutoffs.py computes these tables with
 * mpmath, and --check compares them against the ones below.
 *
 * For any value above 14, this yields the maximal allowable value of 512
 * (by FIPS 140-2 IG 7.19 Resolution # 16, we cannot choose a cutoff value that
 * renders the test unable to fail).
 */
static const unsigned int jent_apt_cutoff_lookup[20]=
	{ 325, 422, 459, 477, 488, 494, 499, 502, 505, 507,
	  508, 509, 510, 511, 512, 512, 512, 512, 512, 512 };
static const unsigned int jent_apt_cutoff_permanent_lookup[20]=
	{ 355, 447, 479, 494, 502, 507, 510, 512, 512, 512,
	  512, 512, 512, 512, 512, 512, 512, 512, 512, 512 };

static void jent_apt_init(struct rand_data *ec)
{
	JENT_CUTOFF_TABLE_CHECK(jent_apt_cutoff_lookup);
	JENT_CUTOFF_TABLE_CHECK(jent_apt_cutoff_permanent_lookup);

	/*
	 * Establish the apt_cutoff based on the presumed entropy rate of
	 * 1/osr.
	 */
	ec->apt_cutoff = jent_apt_cutoff_lookup[ec->osr - 1];
	ec->apt_cutoff_permanent =
			jent_apt_cutoff_permanent_lookup[ec->osr - 1];
}

/*
 * For NTG.1: 8-fold security margin
 * alpha for intermdiate error: 2^-30
 * alpha for permanent error: 2^-60
 *
 * Example formula for R for intermediate cutoffs:
 * C = 2 + qbinom(1 - 2^(-30), 511, 2^(-8/osr))
 */
static const unsigned int jent_apt_cutoff_lookup_ntg1[20]=
	{  17,  71, 136, 191, 236, 272, 301, 325, 345, 361,
	  375, 388, 398, 407, 415, 422, 429, 434, 439, 444 };
static const unsigned int jent_apt_cutoff_permanent_lookup_ntg1[20]=
	{  26,  92, 162, 221, 267, 303, 332, 355, 375, 390,
	  404, 415, 425, 433, 440, 447, 453, 458, 462, 466 };

static void jent_apt_init_ntg1(struct rand_data *ec)
{
	JENT_CUTOFF_TABLE_CHECK(jent_apt_cutoff_lookup_ntg1);
	JENT_CUTOFF_TABLE_CHECK(jent_apt_cutoff_permanent_lookup_ntg1);

	ec->apt_cutoff = jent_apt_cutoff_lookup_ntg1[ec->osr - 1];
	ec->apt_cutoff_permanent =
		jent_apt_cutoff_permanent_lookup_ntg1[ec->osr - 1];
}

static void jent_apt_reinit(struct rand_data *ec,
			    uint64_t current_delta,
			    unsigned int apt_count,
			    unsigned int apt_observations)
{
	ec->apt_base = current_delta;	/* APT Step 1 */
	ec->apt_base_set = 1;		/* APT Step 2 */

	/*
	 * Reset APT counter
	 * Note that we've taken in the first symbol in the window.
	 *
	 * Thus, if apt_count is zero, set it to the intermittent error.
	 */
	if (apt_count)
		ec->apt_count = apt_count;
	else
		ec->apt_count = ec->apt_cutoff;
	ec->apt_observations = apt_observations;
}

void jent_apt_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	/* Continue a window in progress with the repetitions it holds. */
	if (old_ec->apt_observations && old_ec->apt_base_set) {
		jent_apt_reinit(new_ec, old_ec->apt_base, old_ec->apt_count,
				old_ec->apt_observations);
	}
}

/**
 * Reset the APT counter
 *
 * @param[in] ec Reference to entropy collector
 */
static void jent_apt_reset(struct rand_data *ec)
{
	/* When reset, accept the _next_ value input as the new base. */
	ec->apt_base_set = 0;
}

/**
 * Insert a new entropy event into APT
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] current_delta  Current time delta
 */
static void jent_apt_insert(struct rand_data *ec, uint64_t current_delta)
{
	current_delta &= JENT_APT_MASK;

	/* Initialize the base reference */
	if (!ec->apt_base_set) {
		jent_apt_reinit(ec, current_delta, 1, 1);
		return;
	}

	if (current_delta == ec->apt_base) {
		ec->apt_count++;		/* B = B + 1 */

		/* Note, ec->apt_count starts with one. */
		if (ec->apt_count >= ec->apt_cutoff_permanent)
			ec->health_failure |= JENT_APT_FAILURE_PERMANENT;
		else if (ec->apt_count == ec->apt_cutoff)
			ec->health_failure |= JENT_APT_FAILURE;
	}

	ec->apt_observations++;

	/* Completed one window, the next symbol input will be new apt_base. */
	if (ec->apt_observations >= JENT_APT_WINDOW_SIZE)
		jent_apt_reset(ec);		/* APT Step 4 */
}

/***************************************************************************
 * Stuck Test and its use as Repetition Count with Memory Test
 *
 * The Jitter RNG applies the stuck test to the repetition count test with
 * memory as defined in a paper "Überlegungen zum Jitter-RNG" by Jonas Fiege,
 * Johannes Mittmann, Werner Schindler, 6. Februar 2026. This document
 * outlines a health test which applies the standard normal distribution. Using
 * the distribution, the cutoffs are calculated by applying the heuristic
 * entropy value (potentially adjusted by a safety factor).
 *
 * The test is defined to cover the window required non-rejected time deltas
 * to be generated for one output block of 256 bits of data. In FIPS mode
 * this window is 321 * OSR as implemented by jent_random_data_one, of which
 * tau = 3 leaves n = 107 * OSR observations. Outside FIPS mode the window
 * has no safety factor (256 * OSR rounded up to a multiple of three, see
 * jent_rct_mem_window()); its observations stay below the intermittent
 * cutoff, so the test never fires there.
 *
 * With p = 2^(1 - safety_factor/OSR) and p' = min(p, 1/2), the cutoffs are
 * floor(n*p + tau * sqrt(n * p' * (1 - p'))) with tau 4 for the intermittent
 * and 5 for the permanent cutoff, where safety_factor is 1 for common case,
 * 8 for the NTG.1. The intermittent cutoff is capped at n, the permanent one
 * at n + 1.
 *
 * tests/health/cutoffs.py computes all four tables from that formula, and
 * --check compares them against the ones below.
 ***************************************************************************/

/*
 * Recovery loop count defining the number of successful generation of
 * random blocks after a RCT with mem health alarm to recover from that
 * alarm.
 */
#define JENT_RCT_MEM_RECOVERY_LOOP_CNT 10

/* RCT with memory using tau = 4, capped at n */
static const unsigned short jent_rct_mem_cutoff_lookup[] =
	{ 107,  214,  321,  428,  535,  642,  749,  856,  963, 1070,
	  1177, 1284, 1391, 1498, 1605, 1712, 1819, 1926, 2033, 2140 };
/* RCT with memory using tau = 5, capped at n + 1 */
static const unsigned short jent_rct_mem_cutoff_permanent_lookup[] =
	{ 108,  215,  322,  429,  536,  643,  750,  857,  964, 1071,
	  1178, 1285, 1392, 1499, 1606, 1713, 1820, 1927, 2034, 2141 };

static void jent_rct_mem_init(struct rand_data *ec)
{
	JENT_CUTOFF_TABLE_CHECK(jent_rct_mem_cutoff_lookup);
	JENT_CUTOFF_TABLE_CHECK(jent_rct_mem_cutoff_permanent_lookup);

	ec->rct_mem_cutoff = jent_rct_mem_cutoff_lookup[ec->osr - 1];
	ec->rct_mem_cutoff_permanent =
		jent_rct_mem_cutoff_permanent_lookup[ec->osr - 1];
}

/*
 * For NTG.1: 8-fold security margin using tau 4 with a significance level
 * pnorm(-4) yielding 3.17e-05 (roughly 2^-15) for first-order errors. Due to
 * the recovery loop we can afford such higher value.
 */
static const unsigned short jent_rct_mem_cutoff_lookup_ntg1[] =
	{ 4,    46,   134,  255,  399,  560,  733,  856,  963,  1070,
	  1177, 1284, 1391, 1498, 1605, 1712, 1819, 1926, 2033, 2140 };
/*
 * For NTG.1: 8-fold security margin using tau 5 with a significance level of
 * pnorm(-5) yielding about 2^-20.
 */
static const unsigned short jent_rct_mem_cutoff_permanent_lookup_ntg1[] =
	{ 5,    50,   142,  265,  410,  572,  746,  857,  964,  1071,
	  1178, 1285, 1392, 1499, 1606, 1713, 1820, 1927, 2034, 2141 };
static void jent_rct_mem_init_ntg1(struct rand_data *ec)
{
	JENT_CUTOFF_TABLE_CHECK(jent_rct_mem_cutoff_lookup_ntg1);
	JENT_CUTOFF_TABLE_CHECK(jent_rct_mem_cutoff_permanent_lookup_ntg1);

	ec->rct_mem_cutoff =
		jent_rct_mem_cutoff_lookup_ntg1[ec->osr - 1];
	ec->rct_mem_cutoff_permanent =
		jent_rct_mem_cutoff_permanent_lookup_ntg1[ec->osr - 1];
}

static void jent_rct_mem_insert(struct rand_data *ec, unsigned int stuck)
{
	/* Start of a new window, unless jent_rct_mem_duplicate() primed it */
	if (ec->rct_mem_ctr == 0) {
		if (ec->rct_mem_primed)
			ec->rct_mem_primed = 0;
		else
			ec->rct_mem_count = 0;
	}

	/*
	 * If we are outside of a window, do not bother any more: the health
	 * test will not be applied any more.
	 *
	 * We could simply return at this point if we leave the window, but then
	 * we have a multi-modal behavior of the noise source, because if the
	 * window is completed, a significant part of the code is not executed.
	 * Therefore we apply a different strategy: always apply the health
	 * test, but only set a health error if we are within the window.
	 */
#define JENT_RCT_MEM_IN_WINDOW	(ec->rct_mem_ctr < ec->rct_mem_nosr)

	/*
	 * According to the specification of this health test, we only consider
	 * every third iteration count.
	 *
	 * Again, we could simply return here, but that would again imply a
	 * multi-modal behavior. Therefore, always perform the health test
	 * and turn it into a noop.
	 */
#define JENT_RCT_MEM_SKIP_STUCK (ec->rct_mem_ctr % 3)

	/*
	 * We have a stuck value, count it
	 */
	if (stuck && JENT_RCT_MEM_IN_WINDOW && !JENT_RCT_MEM_SKIP_STUCK)
		ec->rct_mem_count++;

	/*
	 * Apply the cut off value.
	 */
	if (ec->rct_mem_count >= ec->rct_mem_cutoff_permanent) {
		if (JENT_RCT_MEM_IN_WINDOW)
			ec->health_failure |= JENT_RCT_MEM_FAILURE_PERMANENT;
	} else if (ec->rct_mem_count == ec->rct_mem_cutoff &&
		   !ec->in_recovery && JENT_RCT_MEM_IN_WINDOW) {
		/*
		 * This is a "recovery loop" to recover from expected false
		 * positives of the health test. Note, the health test cutoffs
		 * have a significantly higher false positive rate than the
		 * APT and RCT. Therefore, this loop generates additional
		 * random values to see whether no health test is detected
		 * during this loop. If no health test error is seen, then
		 * we incurred an expected spurious false positive that we
		 * can ignore. The random data generated by that recovery
		 * loop is simply added to the internal state and thus is not
		 * wasted.
		 *
		 * The outer window is closed while the recovery blocks run
		 * windows of their own, and restored with a fresh count.
		 */
		unsigned short saved_ctr = ec->rct_mem_ctr;

		ec->rct_mem_ctr = ec->rct_mem_nosr;
		ec->rct_mem_count = 0;

		ec->in_recovery = 1;
		jent_random_data_recovery(ec, JENT_RCT_MEM_RECOVERY_LOOP_CNT);
		ec->in_recovery = 0;

		ec->rct_mem_ctr = saved_ctr;
		ec->rct_mem_count = 0;
	}

	/*
	 * Count up the seen measurements if we are in the window - this
	 * prevents also a wrap-around.
	 */
	if (JENT_RCT_MEM_IN_WINDOW) {
		ec->rct_mem_ctr++;

		/*
		 * A recovery block is judged once its window closed, so that
		 * its count can still reach the permanent cutoff.
		 */
		if (ec->in_recovery && !JENT_RCT_MEM_IN_WINDOW &&
		    ec->rct_mem_count >= ec->rct_mem_cutoff &&
		    ec->rct_mem_count < ec->rct_mem_cutoff_permanent)
			ec->health_failure |= JENT_RCT_MEM_FAILURE;
	}
}

void jent_rct_mem_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	new_ec->rct_mem_count = old_ec->rct_mem_cutoff;
	/* Primes the first window, closed until jent_random_data_one() */
	new_ec->rct_mem_ctr = new_ec->rct_mem_nosr;
	new_ec->rct_mem_primed = 1;
}

/***************************************************************************
 * Stuck Test and its use as Repetition Count Test
 *
 * The Jitter RNG uses an enhanced version of the Repetition Count Test
 * (RCT) specified in SP800-90B section 4.4.1. Instead of counting identical
 * back-to-back values, the input to the RCT is the counting of the stuck
 * values during the generation of one Jitter RNG output block.
 *
 * The RCT is applied with an alpha of 2^{-30} compliant to SP800-90B section
 * 4.2 for the intermittent failure and 2^{-60} for permanent failures.
 *
 * During the counting operation, the Jitter RNG always calculates the RCT
 * cut-off value of C. If that value exceeds the allowed cut-off value,
 * the Jitter RNG output block will be calculated completely but discarded at
 * the end. The caller of the Jitter RNG is informed with an error code.
 ***************************************************************************/
static void jent_rct_init(struct rand_data *ec, unsigned short safety)
{
	unsigned short osr = (unsigned short)ec->osr;

	JENT_BUILD_BUG_ON(JENT_HEALTH_RCT_PERMANENT_CUTOFF(JENT_MAX_OSR) >
			  USHRT_MAX);

	ec->rct_cutoff = JENT_HEALTH_RCT_INTERMITTENT_CUTOFF(osr);
	ec->rct_cutoff_permanent = JENT_HEALTH_RCT_PERMANENT_CUTOFF(osr);

	if (safety) {
		ec->rct_cutoff = (unsigned short)
			((ec->rct_cutoff + safety - 1) / safety);
		ec->rct_cutoff_permanent = (unsigned short)
			((ec->rct_cutoff_permanent + safety - 1) / safety);
	}
}

void jent_rct_duplicate(struct rand_data *new_ec)
{
	/*
	 * RCT re-initialization to intermittent error: As during a reset,
	 * the OSR is updated and the OSR is at the same time the cutoff for
	 * the RCT, we re-initialize the new RCT to the intermittent error
	 * value based on the new OSR (and the NTG.1 safety divisor).
	 */
	new_ec->rct_count = (unsigned int)new_ec->rct_cutoff;
}

/**
 * Carry the health test state of a collector over to its replacement
 *
 * The RCTs are primed and the stuck test keeps its reference deltas. The APT
 * and the lag predictor describe the old source, so they are only carried
 * when the replacement samples the same clock in the same startup stage -
 * which the recoveries of today never meet: a replacement is made for a FIPS
 * or NTG.1 collector only, starts in the memory stage, and replaces one that
 * has completed its startup, so both tests start afresh on it. The carry-over
 * is kept for a replacement that would continue where the old one stood.
 *
 * @param[in] new_ec The replacement, already allocated and health-initialized
 * @param[in] old_ec The collector it replaces
 */
void jent_health_duplicate(struct rand_data *new_ec, struct rand_data *old_ec)
{
	jent_rct_duplicate(new_ec);
	jent_rct_mem_duplicate(new_ec, old_ec);

	/*
	 * Across a change of clock or startup stage, the replacement primes
	 * the stuck test's reference from its own source.
	 */
	if (new_ec->enable_notime != old_ec->enable_notime ||
	    new_ec->startup_state != old_ec->startup_state) {
		new_ec->stuck_prime = JENT_STUCK_PRIME;
		return;
	}

	jent_apt_duplicate(new_ec, old_ec);
	/* With the stuck test's reference, which derives from the lag state. */
	jent_lag_duplicate(new_ec, old_ec);
	new_ec->stuck_prime = old_ec->stuck_prime;
}

/**
 * Repetition Count Test as defined in SP800-90B section 4.4.1
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] stuck Indicator whether the value is stuck
 */
static void jent_rct_insert(struct rand_data *ec, unsigned int stuck)
{
	if (stuck) {
		ec->rct_count++;

		if (ec->rct_count >= ec->rct_cutoff_permanent) {
			ec->health_failure |= JENT_RCT_FAILURE_PERMANENT;
		} else if (ec->rct_count == ec->rct_cutoff) {
			ec->health_failure |= JENT_RCT_FAILURE;
		}
	} else {
		/* Must start at zero to reach the correct cutoff value */
		ec->rct_count = 0;
	}
}

/**
 * Stuck test by checking the:
 * 	1st derivative of the jitter measurement (time delta)
 * 	2nd derivative of the jitter measurement (delta of time deltas)
 * 	3rd derivative of the jitter measurement (delta of delta of time deltas)
 *
 * All values must always be non-zero.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] current_delta  Jitter time delta
 *
 * @return
 * 	0 jitter measurement not stuck (good bit)
 * 	1 jitter measurement stuck (reject bit)
 */
unsigned int jent_stuck(struct rand_data *ec, uint64_t current_delta)
{
	uint64_t delta2 = jent_delta2(ec, current_delta);
	uint64_t delta3 = jent_delta3(ec, delta2);
	unsigned int stuck = !current_delta || !delta2 || !delta3;

	/*
	 * A noise source's first deltas only establish the reference of
	 * delta2 and delta3: unjudged, and reported stuck so that they are
	 * measured again rather than counted.
	 */
	if (ec->stuck_prime) {
		ec->stuck_prime--;
		jent_lag_insert(ec, current_delta);
		return 1;
	}

	/*
	 * Insert the result of the comparison of two back-to-back time
	 * deltas.
	 */
	jent_apt_insert(ec, current_delta);
	jent_lag_insert(ec, current_delta);

	/* RCT with stuck result */
	jent_rct_insert(ec, stuck);
	jent_rct_mem_insert(ec, stuck);

	return stuck;
}

/**
 * Insert an externally obtained time stamp into the health tests
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] timestamp Externally obtained time stamp
 *
 * @return Whether the resulting measurement is stuck
 */
unsigned int jent_health_insert_timestamp(struct rand_data *ec,
					  uint64_t timestamp)
{
	/* A collector assembled by a test may have no divisor. */
	uint64_t gcd = ec->jent_common_timer_gcd ?
		       ec->jent_common_timer_gcd : 1;
	uint64_t current_delta = jent_udiv64(jent_delta(ec->prev_time,
							timestamp), gcd);

	ec->prev_time = timestamp;

	return jent_stuck(ec, current_delta);
}

/**
 * Report any health test failures
 *
 * @param[in] ec Reference to entropy collector
 *
 * @return a bitmask indicating which tests failed
 *	0 No health test failure
 *	1 RCT failure
 *	2 APT failure
 *	4 Lag predictor test failure
 *	8 RCT with memory failure
 *	1<<JENT_PERMANENT_FAILURE_SHIFT RCT permanent failure
 *	2<<JENT_PERMANENT_FAILURE_SHIFT APT permanent failure
 *	4<<JENT_PERMANENT_FAILURE_SHIFT Lag predictor test permanent failure
 *	8<<JENT_PERMANENT_FAILURE_SHIFT RCT with memory permanent failure
 */
unsigned int jent_health_failure(struct rand_data *ec)
{
	jent_fips_failure_cb cb;

	/* Test is only enabled in FIPS mode */
	if (!ec->is_fips_enabled)
		return 0;

	cb = (jent_fips_failure_cb)jent_atomic_load_fnptr(&fips_cb);

	/* Once per failure, not once per check of it. */
	if (cb && !ec->measure_clock &&
	    (ec->health_failure & ~ec->health_failure_reported)) {
		ec->health_failure_reported = ec->health_failure;
		cb(ec, ec->health_failure);
	}

	return ec->health_failure;
}

/**
 * Initialize the health tests
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] inittype Startup type
 *
 * @return 0 on success, nonzero for an oversampling rate the tables lack
 */
int jent_health_init(struct rand_data *ec, enum jent_health_init_type inittype)
{
	/* Must start at zero to reach the correct cutoff value */
	ec->rct_count = 0;
	ec->rct_mem_nosr = jent_rct_mem_window(ec);

	/*
	 * The cutoff tables are indexed by osr - 1 and hold JENT_MAX_OSR
	 * entries: refuse rather than read past one. A window of 0 is
	 * jent_rct_mem_window() saying the RCT with memory cannot run at this
	 * rate (counters too narrow, or less than one output block).
	 */
	if (!ec->osr || ec->osr > JENT_MAX_OSR || !ec->rct_mem_nosr)
		return 1;

	/* Each startup stage samples another noise source. */
	jent_apt_reset(ec);
	jent_lag_reset(ec);

	jent_lag_init(ec, ec->osr);
	switch (inittype) {
	case jent_health_init_type_ntg1:
		jent_apt_init_ntg1(ec);
		jent_rct_init(ec, 8);
		jent_rct_mem_init_ntg1(ec);
		break;
	case jent_health_init_type_common:
	default:
		jent_apt_init(ec);
		jent_rct_init(ec, 0);
		jent_rct_mem_init(ec);
		break;
	}

	return 0;
}
