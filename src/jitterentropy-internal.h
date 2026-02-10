/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2025
 *
 * License
 * =======
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
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

#ifndef _JITTERENTROPY_INTERNAL_H
#define _JITTERENTROPY_INTERNAL_H

#include "jitterentropy.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#ifndef JENT_STUCK_INIT_THRES
/*
 * Per default, not more than 90% of all measurements during initialization
 * are allowed to be stuck.
 *
 * It is allowed to change this value as required for the intended environment.
 */
#define JENT_STUCK_INIT_THRES(x) ((x*9) / 10)
#endif

/***************************************************************************
 * Jitter RNG Configuration Section
 *
 * You may alter the following options
 ***************************************************************************/

/*
 * Enable timer-less timer support with JENT_CONF_ENABLE_INTERNAL_TIMER
 *
 * In case the hardware is identified to not provide a high-resolution time
 * stamp, this option enables a built-in high-resolution time stamp mechanism.
 *
 * The timer-less noise source is based on threads. This noise source requires
 * the linking with the POSIX threads library. I.e. the executing environment
 * must offer POSIX threads. If this option is disabled, no linking
 * with the POSIX threads library is needed.
 */

/*
 * Shall the LAG predictor health test be enabled?
 */
#define JENT_HEALTH_LAG_PREDICTOR

/*
 * Shall the jent_memaccess use a (statistically) random selection for the
 * memory to update?
 */
#ifndef JENT_TEST_MEASURE_RAW_MEMORY_ACCESS
#define JENT_RANDOM_MEMACCESS
#else
#undef JENT_RANDOM_MEMACCESS
#endif

/*
 * Mask specifying the number of bits of the raw entropy data of the time delta
 * value used for the APT.
 *
 * This value implies that for the APT, only the bits specified by
 * JENT_APT_MASK are taken. This was suggested in a draft IG D.K resolution 22
 * provided by NIST, but further analysis
 * (https://www.untruth.org/~josh/sp80090b/CMUF%20EWG%20Draft%20IG%20D.K%20Comments%20D10.pdf)
 * suggests that this truncation / translation generally results in a health
 * test with both a higher false positive rate (because multiple raw symbols
 * map to the same symbol within the health test) and a lower statistical power
 * when the APT cutoff is selected based on the apparent truncated entropy
 * (i.e., truncation generally makes the test worse). NIST has since withdrawn
 * this draft and stated that they will not propose truncation prior to
 * health testing.
 * Because the general tendency of such truncation to make the health test
 * worse the default value is set such that no data is masked out and this
 * should only be changed if a hardware-specific analysis suggests that some
 * other mask setting is beneficial.
 * The mask is applied to a time stamp where the GCD is already divided out, and thus no
 * "non-moving" low-order bits are present.
 */
#define JENT_APT_MASK		(UINT64_C(0xffffffffffffffff))

/* This parameter establishes the multiplicative factor that the desired
 * memory region size should be larger than the observed cache size; the
 * multiplicative factor is 2^JENT_CACHE_SHIFT_BITS.
 * Set this to 0 if the desired memory region should be at least as large as
 * the cache. If one wants most of the memory updates to result in a memory
 * update, then this value should be at least 1.
 * If the memory updates should dominantly result in a memory update, then
 * the value should be set to at least 3.
 * The actual size of the memory region is never larger than requested by
 * the passed in JENT_MAX_MEMSIZE_* flag (if provided) or JENT_MEMORY_SIZE
 * (if no JENT_MAX_MEMSIZE_* flag is provided).
 */
#ifndef JENT_CACHE_SHIFT_BITS
#define JENT_CACHE_SHIFT_BITS 0
#endif

/*
 * Meory access loop count: This value defines the default memory access loop
 * count. The memory access loop is one of the hearts of the Jitter RNG. The
 * number of loop counts has a direct impact on the entropy rate.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source. Make sure you fully understand what you do. If you want to
 * individually measure the memory access loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --memaccess.
 */
#ifndef JENT_MEM_ACC_LOOP_DEFAULT
#define JENT_MEM_ACC_LOOP_DEFAULT 1
#endif

/*
 * Hash loop count: This value defines the default hash loop count. The hash
 * loop is one of the hearts of the Jitter RNG. The number of loop counts has a
 * direct impact on the entropy rate.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source. Make sure you fully understand what you do. If you want to
 * individually measure the hash loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --hashloop.
 */
#ifndef JENT_HASH_LOOP_DEFAULT
#define JENT_HASH_LOOP_DEFAULT 1
#endif

/*
 * Hash loop initialization count: This value defines the multiplier of the
 * hash loop count during initialization phase, when the SHA-3-based loop is the
 * sole entropy provider. Typically a higher iteration count is necessary to
 * take enough time.
 *
 * It is permissible to configure this value differently at compile time if the
 * observed entropy rate is too small.
 *
 * NOTE: When you modify this value, you are directly altering the behavior of
 * the noise source during NTG.1 initialization.
 * Make sure you fully understand what you do. If you want to
 * individually measure the hash loop entropy rate, use the
 * jitterentropy-hashtime tool with the command line option of --hashloop.
 */
#ifndef JENT_HASH_LOOP_INIT
#define JENT_HASH_LOOP_INIT 3
#endif

/***************************************************************************
 * Jitter RNG State Definition Section
 ***************************************************************************/

#define JENT_SHA3_256_SIZE_DIGEST_BITS	256
#define JENT_SHA3_256_SIZE_DIGEST	(JENT_SHA3_256_SIZE_DIGEST_BITS >> 3)

/*
 * The output 256 bits can receive more than 256 bits of min entropy,
 * of course, but the 256-bit output of XDRBG-256(M) can only
 * asymptotically approach 256 bits of min entropy, not attain that bound.
 * Random maps will tend to have output collisions, which reduces the creditable
 * output entropy (that is what SP 800-90B Section 3.1.5.1.2 attempts to bound).
 *
 * The value "64" is justified in Appendix A.4 of the current 90C draft,
 * and aligns with NIST's in "epsilon" definition in this document, which is
 * that a string can be considered "full entropy" if you can bound the min
 * entropy in each bit of output to at least 1-epsilon, where epsilon is
 * required to be <= 2^(-32).
 *
 * The additional bit for the safety factor comes from the fact that the
 * we want the internal state variable to have 256 + 64 bit of entropy. As this
 * state variable is generated by one SHAKE256 operation of the input data, we
 * loose one bit of entropy due to the SHAKE. For more details, see the NTG.1
 * analysis provided with the big documentation.
 */
#define ENTROPY_SAFETY_FACTOR		(64 + 1)

enum jent_startup_state {
	jent_startup_completed,
	jent_startup_sha3,
	jent_startup_memory
};

/* The entropy pool */
struct rand_data
{
	/* all data values that are vital to maintain the security
	 * of the RNG are marked as SENSITIVE. A user must not
	 * access that information while the RNG executes its loops to
	 * calculate the next random value. */
	void *hash_state;		/* SENSITIVE hash state entropy pool */
	uint64_t prev_time;		/* SENSITIVE Previous time stamp */
#define DATA_SIZE_BITS (JENT_SHA3_256_SIZE_DIGEST_BITS)

#ifndef JENT_HEALTH_LAG_PREDICTOR
	uint64_t last_delta;		/* SENSITIVE stuck test */
	uint64_t last_delta2;		/* SENSITIVE stuck test */
#endif /* JENT_HEALTH_LAG_PREDICTOR */

	unsigned int flags;		/* Flags used to initialize */
	unsigned int osr;		/* Oversampling rate */

	/* Initialization state supporting AIS 20/31 NTG.1 */
	enum jent_startup_state startup_state;

/* The step size should be larger than the cacheline size. */
#ifndef JENT_DEFAULT_MEMORY_BITS
# define JENT_DEFAULT_MEMORY_BITS 18
#endif
#ifndef JENT_MEMORY_BLOCKSIZE
# define JENT_MEMORY_BLOCKSIZE 128
#endif
#define JENT_MEMORY_BLOCKS(ec) ((ec->memmask + 1) / JENT_MEMORY_BLOCKSIZE)

#define JENT_MEMORY_ACCESSLOOPS 128
	unsigned char *mem;		/* Memory access location with size of
					 * memmask + 1 */

	uint32_t memmask;		/* Memory mask (size of memory - 1) */
	unsigned int memlocation; 	/* Pointer to byte in *mem */
	unsigned int memaccessloops;	/* Number of memory accesses per random
					 * bit generation */

	unsigned int hashloopcnt;	/* Hash loop count */

	/* Repetition Count Test */
	int rct_count;			/* Number of stuck values */

	/* Adaptive Proportion Test for a significance level of 2^-30 */
	unsigned int apt_cutoff;	/* Intermittent health test failure */
	unsigned int apt_cutoff_permanent; /* Permanent health test failure */
#define JENT_APT_WINDOW_SIZE	512	/* Data window size */
	unsigned int apt_observations;	/* Number of collected observations in
					 * current window. */
	unsigned int apt_count;		/* The number of times the reference
					 * symbol been encountered in the
					 * window. */
	uint64_t apt_base;		/* APT base reference */
	unsigned int health_failure;	/* Permanent health failure */

	unsigned int apt_base_set:1;	/* APT base reference set? */
	unsigned int fips_enabled:1;
	unsigned int enable_notime:1;	/* Use internal high-res timer */
	unsigned int max_mem_set:1;	/* Maximum memory configured by user */

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	volatile uint8_t notime_interrupt;	/* indicator to interrupt ctr */
	volatile uint64_t notime_timer;		/* high-res timer mock-up */
	uint64_t notime_prev_timer;		/* previous timer value */
	void *notime_thread_ctx;		/* register thread data */
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	uint64_t jent_common_timer_gcd;	/* Common divisor for all time deltas */

#ifdef JENT_HEALTH_LAG_PREDICTOR
	/* Lag predictor test to look for re-occurring patterns. */

	/* The lag global cutoff selected based on the selection of osr. */
	unsigned int lag_global_cutoff;

	/* The lag local cutoff selected based on the selection of osr. */
	unsigned int lag_local_cutoff;

	/*
	 * The number of times the lag predictor was correct. Compared to the
	 * global cutoff.
	 */
	unsigned int lag_prediction_success_count;

	/*
	 * The size of the current run of successes. Compared to the local
	 * cutoff.
	 */
	unsigned int lag_prediction_success_run;

	/*
	 * The total number of collected observations since the health test was
	 * last reset.
	 */
	unsigned int lag_best_predictor;

	/*
	 * The total number of collected observations since the health test was
	 * last reset.
	 */
	unsigned int lag_observations;

	/*
	 * This is the size of the window used by the predictor. The predictor
	 * is reset between windows.
	 */
#define JENT_LAG_WINDOW_SIZE (1U<<17)

	/*
	 * The amount of history to base predictions on. This must be a power
	 * of 2. Must be 4 or greater.
	 */
#define JENT_LAG_HISTORY_SIZE 8
#define JENT_LAG_MASK (JENT_LAG_HISTORY_SIZE - 1)

	/* The delta history for the lag predictor. */
	uint64_t lag_delta_history[JENT_LAG_HISTORY_SIZE];

	/* The scoreboard that tracks how successful each predictor lag is. */
	unsigned int lag_scoreboard[JENT_LAG_HISTORY_SIZE];
#endif /* JENT_HEALTH_LAG_PREDICTOR */
};

#define JENT_MIN_OSR	3

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_INTERNAL_H */
