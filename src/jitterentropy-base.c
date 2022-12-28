/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2022
 *
 * Design
 * ======
 *
 * See documentation in doc/ folder.
 *
 * Interface
 * =========
 *
 * See documentation in jitterentropy(3) man page.
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

#include "jitterentropy-base.h"
#include "jitterentropy-gcd.h"
#include "jitterentropy-health.h"
#include "jitterentropy-noise.h"
#include "jitterentropy-timer.h"
#include "jitterentropy-sha3.h"

#define MAJVERSION 3 /* API / ABI incompatible changes, functional changes that
		      * require consumer to be updated (as long as this number
		      * is zero, the API is not considered stable and can
		      * change without a bump of the major version) */
#define MINVERSION 4 /* API compatible, ABI may change, functional
		      * enhancements only, consumer can be left unchanged if
		      * enhancements are not considered */
#define PATCHLEVEL 1 /* API / ABI compatible, no functional changes, no
		      * enhancements, bug fixes only */

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

/***************************************************************************
 * Jitter RNG Static Definitions
 *
 * None of the following should be altered
 ***************************************************************************/

#ifdef __OPTIMIZE__
 #error "The CPU Jitter random number generator must not be compiled with optimizations. See documentation. Use the compiler switch -O0 for compiling jitterentropy.c."
#endif

/*
 * JENT_POWERUP_TESTLOOPCOUNT needs some loops to identify edge
 * systems. 100 is definitely too little.
 *
 * SP800-90B requires at least 1024 initial test cycles.
 */
#define JENT_POWERUP_TESTLOOPCOUNT 1024

/**
 * jent_version() - Return machine-usable version number of jent library
 *
 * The function returns a version number that is monotonic increasing
 * for newer versions. The version numbers are multiples of 100. For example,
 * version 1.2.3 is converted to 1020300 -- the last two digits are reserved
 * for future use.
 *
 * The result of this function can be used in comparing the version number
 * in a calling program if version-specific calls need to be make.
 *
 * @return Version number of jitterentropy library
 */
JENT_PRIVATE_STATIC
unsigned int jent_version(void)
{
	unsigned int version = 0;

	version =  MAJVERSION * 1000000;
	version += MINVERSION * 10000;
	version += PATCHLEVEL * 100;

	return version;
}

/* Increase the memory size by one step */
static uint32_t jent_update_memsize_exp(uint32_t flags, uint32_t memsize_exp)
{
	uint32_t requested_max_memsize_exp=0, max_memsize_exp=0;

        /* Is this memory size too large? */
        /* If there is a requested max memory size, use that.
         * If not, default to the compiled in size.
         */
        requested_max_memsize_exp = JENT_FLAGS_TO_MAX_MEMSIZE_EXP(flags);
        max_memsize_exp = (requested_max_memsize_exp==JENT_MEMSIZE_OFFSET)?JENT_FLAGS_TO_MAX_MEMSIZE_EXP(JENT_MAX_MEMSIZE_DEFAULT):requested_max_memsize_exp;

	if (memsize_exp >= max_memsize_exp) {
		memsize_exp = max_memsize_exp;
	} else {
		memsize_exp++;
	}

	/* Clear out the size */
	flags &= ~JENT_MEMSIZE_MASK;
	/* Set the freshly calculated size */
	flags |= JENT_MEMSIZE_EXP_TO_FLAGS(memsize_exp);

	return flags;
}

/***************************************************************************
 * Random Number Generation
 ***************************************************************************/

/**
 * Entry function: Obtain entropy for the caller.
 *
 * This function invokes the entropy gathering logic as often to generate
 * as many bytes as requested by the caller. The entropy gathering logic
 * creates 64 bit per invocation.
 *
 * This function truncates the last 64 bit entropy value output to the exact
 * size specified by the caller.
 *
 * @ec [in] Reference to entropy collector
 * @data [out] pointer to buffer for storing random data -- buffer must
 *	       already exist
 * @len [in] size of the buffer, specifying also the requested number of random
 *	     in bytes
 *
 * @return number of bytes returned when request is fulfilled or an error
 *
 * The following error codes can occur:
 *	-1	entropy_collector is NULL
 *	-2	RCT failed
 *	-3	APT test failed
 *	-4	The timer cannot be initialized
 *	-5	LAG failure
 *	-6	Distribution proportion failure
 */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy(struct rand_data *ec, char *data, size_t len)
{
	char *p = data;
	size_t orig_len = len;
	int ret = 0;

	if (NULL == ec)
		return -1;

	if (jent_notime_settick(ec))
		return -4;

	while (len > 0) {
		size_t tocopy;
		unsigned int health_test_result;

		jent_random_data(ec);

		if ((health_test_result = jent_health_failure(ec))) {
			if (health_test_result & JENT_RCT_FAILURE) {
				ret = -2;
			} else if (health_test_result & JENT_APT_FAILURE) {
				ret = -3;
			} else if (health_test_result & JENT_LAG_FAILURE) {
				ret = -5;
			} else if (health_test_result & JENT_DIST_FAILURE) {
				ret = -6;
			} else {
				ret = -1;
			}

			goto err;
		}

		if ((DATA_SIZE_BITS / 8) < len)
			tocopy = (DATA_SIZE_BITS / 8);
		else
			tocopy = len;

		jent_read_random_block(ec, p, tocopy);

		len -= tocopy;
		p += tocopy;
	}

	/*
	 * Enhanced backtracking support: At this point, the hash state
	 * contains the digest of the previous Jitter RNG collection round
	 * which is inserted there by jent_read_random_block with the SHA
	 * update operation. At the current code location we completed
	 * one request for a caller and we do not know how long it will
	 * take until a new request is sent to us. To guarantee enhanced
	 * backtracking resistance at this point (i.e. ensure that an attacker
	 * cannot obtain information about prior random numbers we generated),
	 * but still stirring the hash state with old data the Jitter RNG
	 * obtains a new message digest from its state and re-inserts it.
	 * After this operation, the Jitter RNG state is still stirred with
	 * the old data, but an attacker who gets access to the memory after
	 * this point cannot deduce the random numbers produced by the
	 * Jitter RNG prior to this point.
	 */
	/*
	 * If we use secured memory, where backtracking support may not be
	 * needed because the state is protected in a different method,
	 * it is permissible to drop this support. But strongly weigh the
	 * pros and cons considering that the SHA3 operation is not that
	 * expensive.
	 */
#ifndef CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	jent_read_random_block(ec, NULL, 0);
#endif

err:
	jent_notime_unsettick(ec);
	return ret ? ret : (ssize_t)orig_len;
}

static struct rand_data *_jent_entropy_collector_alloc(unsigned int osr,
						       unsigned int flags);

/**
 * Entry function: Obtain entropy for the caller.
 *
 * This is a service function to jent_read_entropy() with the difference
 * that it automatically re-allocates the entropy collector if a health
 * test failure is observed. Before reallocation, a new power-on health test
 * is performed. The allocation of the new entropy collector automatically
 * increases the OSR by one. This is done based on the idea that a health
 * test failure indicates that the assumed entropy rate is too high.
 *
 * Note the function returns with an health test error if the OSR is
 * getting too large. If an error is returned by this function, the Jitter RNG
 * is not safe to be used on the current system.
 *
 * @ec [in] Reference to entropy collector - this is a double pointer as
 *	    The entropy collector may be freed and reallocated.
 * @data [out] pointer to buffer for storing random data -- buffer must
 *	       already exist
 * @len [in] size of the buffer, specifying also the requested number of random
 *	     in bytes
 *
 * @return see jent_read_entropy()
 */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy_safe(struct rand_data **ec, char *data, size_t len)
{
	char *p = data;
	size_t orig_len = len;
	ssize_t ret = 0;

	if (!ec)
		return -1;

	while (len > 0) {
		uint32_t osr, flags;

		ret = jent_read_entropy(*ec, p, len);

		switch (ret) {
		case -1:
		case -4:
			return ret;
		case -2:
		case -3:
		case -5:
		case -6:
			/* The APT and RCT cutoffs are dependent on the osr setting.*/
			osr = (*ec)->osr + 1;
			flags = (*ec)->flags;

			/* The APT and RCT tests only reduce to a fixed OSR. Higher OSR
			 * settings may be required, but wouldn't help the rate of
			 * health test failures. In this instance, other parameters need
			 * to be adjusted to use this library. */
			if (osr > 20)
				return ret;

			/*
			 * Let the Jitter RNG increase the maximum memory by
			 * one step so long as it is under the compiled maximum.
			 */
			flags = jent_update_memsize_exp(flags, (*ec)->memsize_exp);

			/*
			 * re-allocate entropy collector with higher OSR and
			 * memory size
			 */
			jent_entropy_collector_free(*ec);

			/* Perform new health test with updated OSR */
			if (jent_entropy_init_ex(osr, flags))
				return -1;

			*ec = _jent_entropy_collector_alloc(osr, flags);
			if (!*ec)
				return -1;

			break;
		default:
			if(ret < 0) return ret;

			len -= (size_t)ret;
			p += (size_t)ret;
		}
	}

	return (ssize_t)orig_len;
}

/***************************************************************************
 * Initialization logic
 ***************************************************************************/

/* Return the number of leading zeros. */
static uint32_t nlz(uint32_t x) {
	uint32_t n=1;
	/* If this is exactly 0, then all the bits are 0.*/
	if (x == 0) return(32);

	/*Check binary sizes (16, 8, 4, 2) */
	if ((x >> 16) == 0) {n = n + 16; x = x << 16;}
	if ((x >> 24) == 0) {n = n +  8; x = x <<  8;}
	if ((x >> 28) == 0) {n = n +  4; x = x <<  4;}
	if ((x >> 30) == 0) {n = n +  2; x = x <<  2;}

        /*Now correct for the high order bit.*/
	return n - (x >> 31);
}

/* return the integer log2; this is floor(log2(x)) */
static inline uint32_t ilog2(uint32_t x) {
	return 31 - nlz(x);
}

/*
 * Obtain memory size to allocate for memory access variations.
 *
 * The maximum variations we can get from the memory access is when we allocate
 * approximately 8 times more memory than we have as data cache. Allocating this much
 * memory might strain the resources on the system more than necessary.
 *
 * On a lot of systems it is not necessary to need so much memory as the
 * variations coming from the general Jitter RNG execution commonly provide
 * large amount of variations.
 *
 * Thus, the result is (in order of priority):
 * 1) The requested memory level (in flags)
 * 2) JENT_MEMORY_SIZE_EXP
 * 3) 8 times the data cache size
 * 4) The default memory size
 *
 */
static uint32_t jent_memsize_exp(unsigned int flags)
{
	uint32_t memsize_exp=0, requested_memsize_exp=0;
	uint32_t max_memsize_exp=0, requested_max_memsize_exp=0;

	/* First, check the flags to see if a specific memory size was requested. */
	requested_memsize_exp = JENT_FLAGS_TO_MEMSIZE_EXP(flags);

	/* If there is a requested memory size, use that.
	 * If not, default to the compiled in size.
	 */
	memsize_exp = (requested_memsize_exp==JENT_MEMSIZE_OFFSET)?JENT_MEMORY_SIZE_EXP:requested_memsize_exp;

	if (memsize_exp == 0) {
		uint32_t cache_memsize=0;
		/*
		 * If we have reached here, then there was neither a runtime
		 * memory request using flags, nor was there a compiled in
		 * requested memory size.
		 */
		cache_memsize = jent_cache_size();

		if(cache_memsize > 0) {
			/*
			 * Try to allocate an amount of memory based on the apparent
			 * cache size. We want roughly 8 times the found cache size.
			 *
			 * note: ilog2(cache_memsize) + 3 is the same as
			 * ilog2(jent_cache_size() * 8).
			 */
			memsize_exp = ilog2(cache_memsize) + 3;
		} else {
			/* Set a default value if none was found.
			 * This is essentially arbitrary, and may be too small
			 * for some systems.
			 */
			memsize_exp = JENT_FLAGS_TO_MEMSIZE_EXP(JENT_MEMSIZE_DEFAULT);
		}
	}

	/* Is this memory size too large? */
	/* If there is a requested max memory size, use that.
	 * If not, default to the compiled in size.
	 */
	requested_max_memsize_exp = JENT_FLAGS_TO_MAX_MEMSIZE_EXP(flags);
	max_memsize_exp = (requested_max_memsize_exp==JENT_MEMSIZE_OFFSET)?JENT_FLAGS_TO_MAX_MEMSIZE_EXP(JENT_MAX_MEMSIZE_DEFAULT):requested_max_memsize_exp;


	if(memsize_exp > max_memsize_exp) {
		memsize_exp = max_memsize_exp;
	}

	return memsize_exp;
}

static int jent_selftest_run = 0;

static struct rand_data
*jent_entropy_collector_alloc_internal(unsigned int osr, unsigned int flags)
{
	struct rand_data *entropy_collector;
	uint32_t memsize_exp = 0;

	/*
	 * Requesting disabling and forcing of internal timer
	 * makes no sense.
	 */
	if ((flags & JENT_DISABLE_INTERNAL_TIMER) &&
	    (flags & JENT_FORCE_INTERNAL_TIMER))
		return NULL;

	/* Force the self test to be run */
	if (!jent_selftest_run && jent_entropy_init_ex(osr, flags))
		return NULL;

	/*
	 * If the initial test code concludes to force the internal timer
	 * and the user requests it not to be used, do not allocate
	 * the Jitter RNG instance.
	 */
	if (jent_notime_forced() && (flags & JENT_DISABLE_INTERNAL_TIMER))
		return NULL;

	/*Allocate the necessary memory regions.*/
	entropy_collector = jent_zalloc(sizeof(struct rand_data));
	if (NULL == entropy_collector)
		return NULL;

	memsize_exp = jent_memsize_exp(flags);
	entropy_collector->mem = (volatile unsigned char *)jent_zalloc(JENT_MEMSIZE_EXP_TO_SIZE(memsize_exp));
	if (entropy_collector->mem == NULL)
		goto err;

	entropy_collector->memsize_exp = memsize_exp;

	/* Make sure the PRNG has an initial seed before anything tries
	 * to use it. This initial seed was randomly generated, and can
	 * be replaced with any random value that doesn't have too small
	 * a hamming weight.
	 * A random value should work (with high probability).
	 */
	entropy_collector->prngState.u[0] = UINT64_C(0x8e93eec0697aaba7);
	entropy_collector->prngState.u[1] = UINT64_C(0xce65608a31b35a5e);
	entropy_collector->prngState.u[2] = UINT64_C(0xa8d46b46cb642eee);
	entropy_collector->prngState.u[3] = UINT64_C(0xe83cef69c548c744);

	if (sha3_alloc(&entropy_collector->hash_state))
		goto err;

	/* Initialize the desired number of hashloops. */
	entropy_collector->hash_loop_exp = JENT_HASHLOOP_EXP;
	entropy_collector->memaccess_loop_exp = JENT_MEMACCESSLOOP_EXP;

	/* Initialize the expected sub-distribution. */
	entropy_collector->distribution_min = JENT_DISTRIBUTION_MIN;
	entropy_collector->distribution_max = JENT_DISTRIBUTION_MAX;

	/* Initialize the hash state */
	sha3_256_init(entropy_collector->hash_state);

	/* verify and set the oversampling rate */
	if (osr < JENT_MIN_OSR)
		osr = JENT_MIN_OSR;

	entropy_collector->osr = osr;

	/* Set the internal flags. */
	entropy_collector->flags = flags;

	if ((flags & JENT_FORCE_FIPS) || jent_fips_enabled())
		entropy_collector->fips_enabled = 1;

	/* Initialize the distribution data counts. */
	jent_dist_init(entropy_collector);

	/* Initialize the APT */
	jent_apt_init(entropy_collector, osr);

	/* Initialize the Lag Predictor Test */
	jent_lag_init(entropy_collector, osr);

	/* Was jent_entropy_init run (establishing the common GCD)? */
	if (jent_gcd_get(&entropy_collector->jent_common_timer_gcd)) {
		/*
		 * It was not. This should probably be an error, but this
		 * behavior breaks the test code. Set the gcd to a value that
		 * won't hurt anything.
		 */
		entropy_collector->jent_common_timer_gcd = 1;
		/* If the GCD isn't set reasonably, then the identified sub-distribution bound values aren't meaningful. */
		entropy_collector->distribution_min = UINT64_C(0);
		entropy_collector->distribution_max = UINT64_C(0xFFFFFFFFFFFFFFFF);
	}

	/*
	 * Use timer-less noise source - note, OSR must be set in
	 * entropy_collector!
	 */
	if (!(flags & JENT_DISABLE_INTERNAL_TIMER)) {
		if (jent_notime_enable(entropy_collector, flags))
			goto err;
	}

	/* Initialize the PRNG seed. */
	jent_random_data(entropy_collector);

	if (jent_health_failure(entropy_collector)>0)
		goto err;

	BUILD_BUG_ON((DATA_SIZE_BITS / 8) < sizeof(entropy_collector->prngState.b));
	jent_read_random_block(entropy_collector, (char *)(entropy_collector->prngState.b), sizeof(entropy_collector->prngState.b));

	return entropy_collector;

err:
	if (entropy_collector->mem != NULL)
		jent_zfree((void *)entropy_collector->mem, JENT_MEMSIZE_EXP_TO_SIZE(memsize_exp));
	jent_zfree(entropy_collector, sizeof(struct rand_data));
	return NULL;
}

static struct rand_data *_jent_entropy_collector_alloc(unsigned int osr,
						       unsigned int flags)
{
	struct rand_data *ec = jent_entropy_collector_alloc_internal(osr,
								     flags);

	if (!ec)
		return ec;

	/* fill the data pad with non-zero values */
	if (jent_notime_settick(ec)) {
		jent_entropy_collector_free(ec);
		return NULL;
	}
	jent_random_data(ec);
	jent_notime_unsettick(ec);

	return ec;
}

JENT_PRIVATE_STATIC
struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
					       unsigned int flags)
{
	struct rand_data *ec = _jent_entropy_collector_alloc(osr, flags);

	return ec;
}

JENT_PRIVATE_STATIC
void jent_entropy_collector_free(struct rand_data *entropy_collector)
{
	if (entropy_collector != NULL) {
		sha3_dealloc(entropy_collector->hash_state);
		jent_notime_disable(entropy_collector);
		if (entropy_collector->mem != NULL) {
			jent_zfree((void *)entropy_collector->mem,
				   JENT_MEMSIZE_EXP_TO_SIZE(entropy_collector->memsize_exp));
			entropy_collector->mem = NULL;
		}
		jent_zfree(entropy_collector, sizeof(struct rand_data));
	}
}

int jent_time_entropy_init(unsigned int osr, unsigned int flags)
{
	struct rand_data *ec;
	uint64_t *delta_history;
	int i, time_backwards = 0, count_stuck = 0, ret = 0;
	unsigned int health_test_result;

	delta_history = jent_gcd_init(JENT_POWERUP_TESTLOOPCOUNT);
	if (!delta_history)
		return EMEM;

	if (flags & JENT_FORCE_INTERNAL_TIMER)
		jent_notime_force();
	else
		flags |= JENT_DISABLE_INTERNAL_TIMER;

	/*
	 * If the start-up health tests (including the APT and RCT) are not
	 * run, then the entropy source is not 90B compliant. We could test if
	 * fips_enabled should be set using the jent_fips_enabled() function,
	 * but this can be overridden using the JENT_FORCE_FIPS flag, which
	 * isn't passed in yet. It is better to run the tests on the small
	 * amount of data that we have, which should not fail unless things
	 * are really bad.
	 */
	flags |= JENT_FORCE_FIPS;
	ec = jent_entropy_collector_alloc_internal(osr, flags);
	if (!ec) {
		ret = EMEM;
		goto out;
	}

	if (jent_notime_settick(ec)) {
		ret = ETHREAD;
		goto out;
	}

	/* To initialize the prior time. */
	jent_measure_jitter(ec, NULL);

	/* We could perform statistical tests here, but the problem is
	 * that we only have a few loop counts to do testing. These
	 * loop counts may show some slight skew leading to false positives.
	 */

	/*
	 * We could add a check for system capabilities such as clock_getres or
	 * check for CONFIG_X86_TSC, but it does not make much sense as the
	 * following sanity checks verify that we have a high-resolution
	 * timer.
	 */
#define CLEARCACHE 100
	for (i = -CLEARCACHE; i < JENT_POWERUP_TESTLOOPCOUNT; i++) {
		uint64_t start_time = 0, end_time = 0, delta = 0;
		unsigned int stuck;

		/* Invoke core entropy collection logic */
		stuck = jent_measure_jitter(ec, &delta);
		end_time = ec->prev_time;
		start_time = ec->prev_time - delta;

		/* test whether timer works */
		if (!start_time || !end_time) {
			ret = ENOTIME;
			goto out;
		}

		/*
		 * test whether timer is fine grained enough to provide
		 * delta even when called shortly after each other -- this
		 * implies that we also have a high resolution timer
		 */
		if (!delta || (end_time == start_time)) {
			ret = ECOARSETIME;
			goto out;
		}

		/*
		 * up to here we did not modify any variable that will be
		 * evaluated later, but we already performed some work. Thus we
		 * already have had an impact on the caches, branch prediction,
		 * etc. with the goal to clear it to get the worst case
		 * measurements.
		 */
		if (i < 0)
			continue;

		if (stuck)
			count_stuck++;

		/* test whether we have an increasing timer */
		if (!(end_time > start_time))
			time_backwards++;

		/* Watch for common adjacent GCD values */
		jent_gcd_add_value(delta_history, delta, i);
	}

	/*
	 * we allow up to three times the time running backwards.
	 * CLOCK_REALTIME is affected by adjtime and NTP operations. Thus,
	 * if such an operation just happens to interfere with our test, it
	 * should not fail. The value of 3 should cover the NTP case being
	 * performed during our test run.
	 */
	if (time_backwards > 3) {
		ret = ENOMONOTONIC;
		goto out;
	}

	/* First, did we encounter a health test failure? */
	if ((health_test_result = jent_health_failure(ec))) {
		if(health_test_result & JENT_RCT_FAILURE) ret = ERCT;
		else if(health_test_result & JENT_APT_FAILURE) ret = EAPT;
		else if(health_test_result & JENT_LAG_FAILURE) ret = ELAG;
		else if(health_test_result & JENT_DIST_FAILURE) ret = EDIST;
		else {
			ret = EMEM;
		}
		goto out;
	}

	ret = jent_gcd_analyze(delta_history, JENT_POWERUP_TESTLOOPCOUNT);
	if (ret)
		goto out;

	/*
	 * If we have more than 90% stuck results, then this Jitter RNG is
	 * likely to not work well.
	 */
	if (JENT_STUCK_INIT_THRES(JENT_POWERUP_TESTLOOPCOUNT) < count_stuck)
		ret = ESTUCK;

out:
	jent_gcd_fini(delta_history, JENT_POWERUP_TESTLOOPCOUNT);

	if ((flags & JENT_FORCE_INTERNAL_TIMER) && ec)
		jent_notime_unsettick(ec);

	jent_entropy_collector_free(ec);

	return ret;
}

static inline int jent_entropy_init_common_pre(void)
{
	int ret;

	jent_notime_block_switch();
	jent_health_cb_block_switch();

	if (sha3_tester())
		return EHASH;

	ret = jent_gcd_selftest();

	jent_selftest_run = 1;

	return ret;
}

static inline int jent_entropy_init_common_post(int ret)
{
	/* Unmark the execution of the self tests if they failed. */
	if (ret)
		jent_selftest_run = 0;

	return ret;
}

JENT_PRIVATE_STATIC
int jent_entropy_init(void)
{
	int ret = jent_entropy_init_common_pre();

	if (ret)
		return ret;

	ret = jent_time_entropy_init(0, JENT_DISABLE_INTERNAL_TIMER);

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	if (ret)
		ret = jent_time_entropy_init(0, JENT_FORCE_INTERNAL_TIMER);
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	return jent_entropy_init_common_post(ret);
}

JENT_PRIVATE_STATIC
int jent_entropy_init_ex(unsigned int osr, unsigned int flags)
{
	int ret = jent_entropy_init_common_pre();

	if (ret)
		return ret;

	ret = ENOTIME;

	/* Test without internal timer unless caller does not want it */
	if (!(flags & JENT_FORCE_INTERNAL_TIMER))
		ret = jent_time_entropy_init(osr,
					flags | JENT_DISABLE_INTERNAL_TIMER);

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	/* Test with internal timer unless caller does not want it */
	if (ret && !(flags & JENT_DISABLE_INTERNAL_TIMER))
		ret = jent_time_entropy_init(osr,
					     flags | JENT_FORCE_INTERNAL_TIMER);
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	return jent_entropy_init_common_post(ret);
}

JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread)
{
	return jent_notime_switch(new_thread);
}

JENT_PRIVATE_STATIC
int jent_set_fips_failure_callback(jent_fips_failure_cb cb)
{
	return jent_set_fips_failure_callback_internal(cb);
}
