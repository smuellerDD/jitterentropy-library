/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2022
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

#ifndef _JITTERENTROPY_H
#define _JITTERENTROPY_H

#ifdef __cplusplus
extern "C" {
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

/***************************************************************************
 * Jitter RNG State Definition Section
 ***************************************************************************/

#if defined(_MSC_VER)
#include "arch/jitterentropy-base-windows.h"
#else
#include "jitterentropy-base-user.h"
#endif

#define SHA3_256_SIZE_DIGEST_BITS	256
#define SHA3_256_SIZE_DIGEST		(SHA3_256_SIZE_DIGEST_BITS >> 3)

/*
 * The output 256 bits can receive more than 256 bits of min entropy,
 * of course, but the 256-bit output of SHA3-256(M) can only asymptotically
 * approach 256 bits of min entropy, not attain that bound. Random maps will
 * tend to have output collisions, which reduces the creditable output entropy
 * (that is what SP 800-90B Section 3.1.5.1.2 attempts to bound).
 *
 * The value "64" is justified in Appendix A.4 of the current 90C draft,
 * and aligns with NIST's in "epsilon" definition in this document, which is
 * that a string can be considered "full entropy" if you can bound the min
 * entropy in each bit of output to at least 1-epsilon, where epsilon is
 * required to be <= 2^(-32).
 */
#define ENTROPY_SAFETY_FACTOR		64

/**
 * Function pointer data structure to register an external thread handler
 * used for the timer-less mode of the Jitter RNG.
 *
 * The external caller provides these function pointers to handle the
 * management of the timer thread that is spawned by the Jitter RNG.
 *
 * @var jent_notime_init This function is intended to initialze the threading
 *	support. All data that is required by the threading code must be
 *	held in the data structure @param ctx. The Jitter RNG maintains the
 *	data structure and uses it for every invocation of the following calls.
 *
 * @var jent_notime_fini This function shall terminate the threading support.
 *	The function must dispose of all memory and resources used for the
 *	threading operation. It must also dispose of the @param ctx memory.
 *
 * @var jent_notime_start This function is called when the Jitter RNG wants
 *	to start a thread. Besides providing a pointer to the @param ctx
 *	allocated during initialization time, the Jitter RNG provides a
 *	pointer to the function the thread shall execute and the argument
 *	the function shall be invoked with. These two parameters have the
 *	same purpose as the trailing two parameters of pthread_create(3).
 *
 * @var jent_notime_stop This function is invoked by the Jitter RNG when the
 *	thread should be stopped. Note, the Jitter RNG intends to start/stop
 *	the thread frequently.
 *
 * An example implementation is found in the Jitter RNG itself with its
 * default thread handler of jent_notime_thread_builtin.
 *
 * If the caller wants to register its own thread handler, it must be done
 * with the API call jent_entropy_switch_notime_impl as the first
 * call to interact with the Jitter RNG, even before jent_entropy_init.
 * After jent_entropy_init is called, changing of the threading implementation
 * is not allowed.
 */
struct jent_notime_thread {
	int (*jent_notime_init)(void **ctx);
	void (*jent_notime_fini)(void *ctx);
	int (*jent_notime_start)(void *ctx,
				 void *(*start_routine) (void *), void *arg);
	void (*jent_notime_stop)(void *ctx);
};

#ifndef JENT_DISTRIBUTION_MIN
#define JENT_DISTRIBUTION_MIN 0U
#endif

#ifndef JENT_DISTRIBUTION_MAX
#define JENT_DISTRIBUTION_MAX UINT64_C(0xFFFFFFFFFFFFFFFF)
#endif

/* The entropy pool */
struct rand_data
{
	/* all data values that are vital to maintain the security
	 * of the RNG are marked as SENSITIVE. A user must not
	 * access that information while the RNG executes its loops to
	 * calculate the next random value. */
	void *hash_state;		/* SENSITIVE hash state entropy pool */
	uint64_t prev_time;		/* SENSITIVE Previous time stamp */
#define DATA_SIZE_BITS (SHA3_256_SIZE_DIGEST_BITS)

#ifndef JENT_HEALTH_LAG_PREDICTOR
	uint64_t last_delta;		/* SENSITIVE stuck test */
	uint64_t last_delta2;		/* SENSITIVE stuck test */
#endif /* JENT_HEALTH_LAG_PREDICTOR */

	uint32_t flags;			/* Flags used to initialize */
	unsigned int osr;		/* Oversampling rate */

#ifndef JENT_MEMORY_SIZE_EXP
# define JENT_MEMORY_SIZE_EXP 0
#endif

#ifndef JENT_MEMORY_DEPTH_EXP
#define JENT_MEMORY_DEPTH_EXP 0
#endif

#define JENT_HASHLOOP_EXP 0
#define JENT_MEMACCESSLOOP_EXP 0
	volatile unsigned char *mem;	/* Memory access location */
	uint32_t memsize_exp;		/* mem is size 2^memsize_exp */
        union {
                uint64_t u[4];
                uint8_t b[sizeof(uint64_t) * 4];
        } prngState;
	unsigned int hash_loop_exp;	/* Number of hash invocations per random
					 * bit generation */
	unsigned int memaccess_loop_exp;/* Number of hash invocations per random
					 * bit generation */

	uint64_t distribution_min;	/* The smallest value considered to be in the targeted sub-distribution. */
	uint64_t distribution_max;	/* The largest value considered to be in the targeted sub-distribution. */

	#define JENT_DIST_WINDOW 10000U
	uint64_t current_data_count;	/* The total number of timing values that have been observed. */
	uint64_t current_in_dist_count;	/*The total number of timing values within the expected distribution.*/
	uint64_t data_count_history;	/* The total number of timing values that have been observed. */
	uint64_t in_dist_count_history;	/*The total number of timing values within the expected distribution.*/

	/* Repetition Count Test */
	int rct_count;			/* Number of stuck values */

	/* Adaptive Proportion Test for a significance level of 2^-30 */
	unsigned int apt_cutoff;	/* Calculated using a corrected version
					 * of the SP800-90B sec 4.4.2 formula */
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

/* Flags that can be used to initialize the RNG */
#define JENT_DISABLE_STIR (1<<0) 	/* UNUSED */
#define JENT_DISABLE_UNBIAS (1<<1) 	/* UNUSED */
#define JENT_DISABLE_MEMORY_ACCESS (1<<2) /* UNUSED */
#define JENT_FORCE_INTERNAL_TIMER (1<<3)  /* Force the use of the internal
					     timer */
#define JENT_DISABLE_INTERNAL_TIMER (1<<4)  /* Disable the potential use of
					       the internal timer. */
#define JENT_FORCE_FIPS (1<<5)		  /* Force FIPS compliant mode
					     including full SP800-90B
					     compliance. */

/* Flags field limiting the amount of memory to be used for memory access */
/*These are stored in the high order nibble of the flags.*/
/* We start at 64kB, and 1>>(1+15) offset is 2^16 */
/* We end at 1GB, and 1>>(15+15) offset is 2^30 */
#define JENT_MEMSIZE_OFFSET		15

#define JENT_MEMSIZE_EXP_TO_SIZE(val)	(UINT32_C(1)<<val)
#define JENT_MEMSIZE_EXP_TO_MASK(val)	((UINT32_C(1)<<val)-1)

#define JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT	28
#define JENT_MAX_MEMSIZE_64kB		(UINT32_C( 1) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_128kB		(UINT32_C( 2) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_256kB		(UINT32_C( 3) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_512kB		(UINT32_C( 4) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_1MB		(UINT32_C( 5) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_2MB		(UINT32_C( 6) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_4MB		(UINT32_C( 7) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_8MB		(UINT32_C( 8) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_16MB		(UINT32_C( 9) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_32MB		(UINT32_C(10) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_64MB		(UINT32_C(11) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_128MB		(UINT32_C(12) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_256MB		(UINT32_C(13) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_512MB		(UINT32_C(14) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_1024MB		(UINT32_C(15) << JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT)

#define JENT_MAX_MEMSIZE_DEFAULT	JENT_MAX_MEMSIZE_1024MB
#define JENT_MAX_MEMSIZE_MASK		JENT_MAX_MEMSIZE_1024MB
#define JENT_FLAGS_TO_MAX_MEMSIZE_EXP(val)	(((val&JENT_MAX_MEMSIZE_MASK)>>JENT_FLAGS_TO_MAX_MEMSIZE_SHIFT) + JENT_MEMSIZE_OFFSET)

#define JENT_FLAGS_TO_MEMSIZE_SHIFT	24
#define JENT_MEMSIZE_64kB		(UINT32_C( 1) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_128kB		(UINT32_C( 2) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_256kB		(UINT32_C( 3) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_512kB		(UINT32_C( 4) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_1MB		(UINT32_C( 5) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_2MB		(UINT32_C( 6) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_4MB		(UINT32_C( 7) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_8MB		(UINT32_C( 8) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_16MB		(UINT32_C( 9) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_32MB		(UINT32_C(10) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_64MB		(UINT32_C(11) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_128MB		(UINT32_C(12) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_256MB		(UINT32_C(13) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_512MB		(UINT32_C(14) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MEMSIZE_1024MB		(UINT32_C(15) << JENT_FLAGS_TO_MEMSIZE_SHIFT)

#define JENT_MEMSIZE_DEFAULT		JENT_MEMSIZE_1MB
#define JENT_MEMSIZE_MASK		JENT_MEMSIZE_1024MB
#define JENT_FLAGS_TO_MEMSIZE_EXP(val)	(((val&JENT_MEMSIZE_MASK)>>JENT_FLAGS_TO_MEMSIZE_SHIFT) + JENT_MEMSIZE_OFFSET)
#define JENT_MEMSIZE_EXP_TO_FLAGS(val)	((val - JENT_MEMSIZE_OFFSET) << JENT_FLAGS_TO_MEMSIZE_SHIFT)

# define JENT_MIN_OSR	1

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* -- BEGIN Main interface functions -- */

#ifndef JENT_STUCK_INIT_THRES
/*
 * Per default, not more than 90% of all measurements during initialization
 * are allowed to be stuck.
 *
 * It is allowed to change this value as required for the intended environment.
 */
#define JENT_STUCK_INIT_THRES(x) ((x*9) / 10)
#endif

#ifndef JENT_DIST_RUNNING_THRES
/*
 * By default, at least 10% of all measurements
 * are expected to be in the expected distribution.
 * This is structured to round down (until there are at least 10000
 * observations, the cutoff is rounded down to 0).
 * Under a binomial assumption, InverseCDF[Binomial[10000, 0.10], 2^-40] = 795,
 * so we use that as our cutoff.
 *
 * It is allowed to change this value as required for the intended environment.
 */
#define JENT_DIST_RUNNING_THRES(x) (((x) / 10000)*795)
#endif

#ifdef JENT_PRIVATE_COMPILE
# define JENT_PRIVATE_STATIC static
#else /* JENT_PRIVATE_COMPILE */
#if defined(_MSC_VER)
#define JENT_PRIVATE_STATIC __declspec(dllexport)
#else
#define JENT_PRIVATE_STATIC __attribute__((visibility("default")))
#endif
#endif

/* Number of low bits of the time value that we want to consider */
/* get raw entropy */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy(struct rand_data *ec, char *data, size_t len);
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy_safe(struct rand_data **ec, char *data, size_t len);
/* initialize an instance of the entropy collector */
JENT_PRIVATE_STATIC
struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
	       				       unsigned int flags);
/* clearing of entropy collector */
JENT_PRIVATE_STATIC
void jent_entropy_collector_free(struct rand_data *entropy_collector);

/* initialization of entropy collector */
JENT_PRIVATE_STATIC
int jent_entropy_init(void);
JENT_PRIVATE_STATIC
int jent_entropy_init_ex(unsigned int osr, unsigned int flags);

/*
 * Set a callback to run on health failure in FIPS mode.
 * This function will take an action determined by the caller.
 */
typedef void (*jent_fips_failure_cb)(struct rand_data *ec,
				     unsigned int health_failure);
JENT_PRIVATE_STATIC
int jent_set_fips_failure_callback(jent_fips_failure_cb cb);

/* return version number of core library */
JENT_PRIVATE_STATIC
unsigned int jent_version(void);

/* Set a different thread handling logic for the notimer support */
JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread);

/* -- END of Main interface functions -- */

/* -- BEGIN timer-less threading support functions to prevent code dupes -- */

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

struct jent_notime_ctx {
	pthread_attr_t notime_pthread_attr;	/* pthreads library */
	pthread_t notime_thread_id;		/* pthreads thread ID */
};

JENT_PRIVATE_STATIC
int jent_notime_init(void **ctx);

JENT_PRIVATE_STATIC
void jent_notime_fini(void *ctx);

#else

static inline int jent_notime_init(void **ctx) { (void)ctx; return 0; }
static inline void jent_notime_fini(void *ctx) { (void)ctx; }

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

/* -- END timer-less threading support functions to prevent code dupes -- */

/* -- BEGIN error codes for init function -- */
#define ENOTIME  	1 /* Timer service not available */
#define ECOARSETIME	2 /* Timer too coarse for RNG */
#define ENOMONOTONIC	3 /* Timer is not monotonic increasing */
#define EMINVARIATION	4 /* Timer variations too small for RNG */
#define EVARVAR		5 /* Timer does not produce variations of variations
			     (2nd derivation of time is zero) */
#define EMINVARVAR	6 /* Timer variations of variations is too small */
#define EPROGERR	7 /* Programming error */
#define ESTUCK		8 /* Too many stuck results during init. */
#define EHEALTH		9 /* Health test failed during initialization */
#define ERCT		10 /* RCT failed during initialization */
#define EHASH		11 /* Hash self test failed */
#define EMEM		12 /* Can't allocate memory for initialization */
#define EGCD		13 /* GCD self-test failed */
#define ETHREAD		14 /* Can't create thread for timer. */
#define EAPT		14 /* APT error */
#define ELAG		15 /* LAG error */
#define EDIST		16 /* DIST error */
/* -- END error codes for init function -- */

/* -- BEGIN error masks for health tests -- */
#define JENT_RCT_FAILURE	1 /* Failure in RCT health test. */
#define JENT_APT_FAILURE	2 /* Failure in APT health test. */
#define JENT_LAG_FAILURE	4 /* Failure in Lag predictor health test. */
#define JENT_DIST_FAILURE	8 /* Failure in distribution proportion health test. */
/* -- END error masks for health tests -- */

/* -- BEGIN statistical test functions only complied with CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT -- */

#ifdef CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT
JENT_PRIVATE_STATIC
uint64_t jent_lfsr_var_stat(struct rand_data *ec, unsigned int min);
#endif /* CONFIG_CRYPTO_CPU_JITTERENTROPY_STAT */

/* -- END of statistical test function -- */

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_H */
