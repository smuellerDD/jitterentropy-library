/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
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

#include "jitterentropy-base.h"
#include "jitterentropy-gcd.h"
#include "jitterentropy-health.h"
#include "jitterentropy-internal.h"
#include "jitterentropy-noise.h"
#include "jitterentropy-timer.h"
#include "jitterentropy-sha3.h"

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

/*
 * ensure_osr_is_at_least_minimal ensures that the over sampling rate is, at
 * minimum, JENT_MIN_OSR.
 *
 * @return returns the argument current_osr if equal to or larger than
 * JENT_MIN_OSR otherwise, returns JENT_MIN_OSR.
 */
static unsigned int ensure_osr_is_at_least_minimal(unsigned int current_osr)
{
	if (current_osr < JENT_MIN_OSR)
		return (unsigned int) JENT_MIN_OSR;
	else
		return current_osr;
}

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
	return JENT_VERSION;
}

/***************************************************************************
 * Helper
 ***************************************************************************/

/* Calculate log2 of given value assuming that the value is a power of 2 */
static inline unsigned int jent_log2_simple(uint64_t val)
{
	unsigned int idx = 0;

	while (val >>= 1)
		idx++;
	return idx;
}

/*
 * The memory size field derived from a cache of @cache_size bytes (0: none
 * known), @inc reallocation steps up.
 */
static inline unsigned int jent_derive_memsize(uint64_t cache_size,
					       int cache_all, unsigned int inc)
{
	/*
	 * The safe starting value is the cache size increased by the
	 * multiplicator.
	 */
	unsigned int max = jent_log2_simple(cache_size);

	if (max) {
		max += JENT_CACHE_SHIFT_BITS;

		if (!cache_all) {
			/*
			 * We increase the memory size 4-fold. This is
			 * due to ensure that the memory access
			 * operation mostly is caused by L1 misses and
			 * L2 hits.
			 */
			max += 2;
		}
	}

	/* No usable cache size: start from the default, so @inc steps up. */
	if (max <= JENT_MAX_MEMSIZE_OFFSET)
		max = JENT_DEFAULT_MEMORY_BITS;

	/* Adjust offset */
	max = (max > JENT_MAX_MEMSIZE_OFFSET) ?
		max - JENT_MAX_MEMSIZE_OFFSET : 0;

	max += inc;

	/* Bound the derived size, see JENT_MAX_AUTO_MEMSIZE. */
	if (max > JENT_MAX_AUTO_MEMSIZE)
		max = JENT_MAX_AUTO_MEMSIZE;

	return max;
}

/*
 * Obtain memory size to allocate for memory access variations.
 *
 * The maximum variations we can get from the memory access is when we allocate
 * a bit more memory than we have as data cache. But allocating as much
 * memory as we have as data cache might strain the resources on the system
 * more than necessary.
 *
 * On a lot of systems it is not necessary to need so much memory as the
 * variations coming from the general Jitter RNG execution commonly provide
 * large amount of variations.
 *
 * Thus, the default is:
 * * size provided by the caller, or
 * * cache information * (1 << JENT_CACHE_SHIFT_BITS) where by default
 *   only L1 cache size is uzed or with JENT_CACHE_ALL all caches are used
 *   to determine the memory size, or
 * * 1 << JENT_DEFAULT_MEMORY_BITS
 *
 * A derived size grows by @inc steps, a size the caller provided does not.
 * All is capped by JENT_MAX_MEMSIZE_MAX
 */
static inline unsigned int jent_update_memsize(unsigned int flags,
					       unsigned int inc)
{
	unsigned int global_max = JENT_FLAGS_TO_MAX_MEMSIZE(
							JENT_MAX_MEMSIZE_MAX);
	unsigned int max;

	max = JENT_FLAGS_TO_MAX_MEMSIZE(flags);

	if (!max) {
		int cache_all = !!(flags & JENT_CACHE_ALL);

		max = jent_derive_memsize(jent_cache_size_roundup(cache_all),
					  cache_all, inc);
	}

	max = (max > global_max) ? global_max : max;

	/* Clear out the max size */
	flags &= ~JENT_MAX_MEMSIZE_MASK;
	/* Set the freshly calculated max size */
	flags |= JENT_MAX_MEMSIZE_TO_FLAGS(max);

	return flags;
}

static inline unsigned int jent_update_hashloop(unsigned int flags,
						unsigned int inc)
{
	unsigned int global_max = JENT_FLAGS_TO_HASHLOOP(JENT_MAX_HASHLOOP);
	unsigned int max;

	max = JENT_FLAGS_TO_HASHLOOP(flags);

	/*
	 * Field n is 2^(n - 1) loops, field 0 JENT_HASH_LOOP_DEFAULT: step up
	 * from the field covering the default, never below it.
	 */
	if (!max && inc) {
		if (JENT_HASH_LOOP_DEFAULT > (UINT32_C(1) << (global_max - 1)))
			return flags;
		max = 1;
		while ((UINT32_C(1) << (max - 1)) < JENT_HASH_LOOP_DEFAULT)
			max++;
	}

	max += inc;
	max = (max > global_max) ? global_max : max;

	/* Clear out the max size */
	flags &= ~(unsigned int)JENT_MAX_HASHLOOP_MASK;
	/* Set the freshly calculated max size */
	flags |= JENT_HASHLOOP_TO_FLAGS(max);

	return flags;
}

/*
 * The compliance modes require secure memory, and a collector runs in FIPS
 * mode on a system in FIPS mode as it does with JENT_FORCE_FIPS. Caller flags
 * only: the startup test instance sets JENT_FORCE_FIPS for the health tests
 * alone.
 */
static inline unsigned int jent_update_secure_mem(unsigned int flags)
{
	if ((flags & (JENT_NTG1 | JENT_FORCE_FIPS)) || jent_fips_enabled())
		flags |= JENT_FORCE_SECURE_MEM;

	return flags;
}

/* The FIPS / NTG.1 startup needs the memory access noise source. */
static inline int jent_memaccess_contradicts(unsigned int flags)
{
	return (flags & JENT_DISABLE_MEMORY_ACCESS) &&
	       ((flags & (JENT_NTG1 | JENT_FORCE_FIPS)) || jent_fips_enabled());
}

/***************************************************************************
 * Stack Scrubbing
 ***************************************************************************/

/*
 * Clear the stack the noise source left behind, at the end of every entry
 * point that runs it. 4 kB covers the deepest path. Hosted only.
 */
#define JENT_STACK_SCRUB_LEN	4096

#if !defined(LINUX_KERNEL) && !defined(__KERNEL__) && !defined(JENT_BAREMETAL) && \
    !(defined(_KERNEL) && defined(__FreeBSD__))

/* Not instrumented: ASan would move the array away from the stack below. */
static JENT_NO_SANITIZE_ADDRESS void jent_stack_scrub_array(void)
{
	unsigned char scrub[JENT_STACK_SCRUB_LEN];

	jent_memset_secure(scrub, sizeof(scrub));
}

/* The padding between that array and the stack canary. */
static void jent_stack_scrub_frame(void)
{
	volatile unsigned long z0 = 0, z1 = 0, z2 = 0, z3 = 0;
	volatile unsigned long z4 = 0, z5 = 0, z6 = 0, z7 = 0;

	(void)z0; (void)z1; (void)z2; (void)z3;
	(void)z4; (void)z5; (void)z6; (void)z7;
}

#define jent_stack_scrub()						       \
	do {								       \
		jent_stack_scrub_array();				       \
		jent_stack_scrub_frame();				       \
	} while (0)

#else /* freestanding */

#define jent_stack_scrub()	do { } while (0)

#endif

/***************************************************************************
 * Random Number Generation
 ***************************************************************************/

/* The health failure bits of the two kinds. */
#define JENT_INTERMITTENT_FAILURES					       \
	(JENT_RCT_FAILURE | JENT_APT_FAILURE | JENT_LAG_FAILURE |	       \
	 JENT_RCT_MEM_FAILURE)
#define JENT_PERMANENT_FAILURES						       \
	JENT_PERMANENT_FAILURE(JENT_INTERMITTENT_FAILURES)

/* The JENT_ERR_* code of a non-zero set of health failure bits. */
static int jent_health_err(unsigned int health_test_result)
{
	if (health_test_result & JENT_RCT_FAILURE_PERMANENT)
		return JENT_ERR_RCT_PERMANENT;
	if (health_test_result & JENT_APT_FAILURE_PERMANENT)
		return JENT_ERR_APT_PERMANENT;
	if (health_test_result & JENT_LAG_FAILURE_PERMANENT)
		return JENT_ERR_LAG_PERMANENT;
	if (health_test_result & JENT_RCT_MEM_FAILURE_PERMANENT)
		return JENT_ERR_RCT_MEM_PERMANENT;
	if (health_test_result & JENT_RCT_FAILURE)
		return JENT_ERR_RCT;
	if (health_test_result & JENT_APT_FAILURE)
		return JENT_ERR_APT;
	if (health_test_result & JENT_RCT_MEM_FAILURE)
		return JENT_ERR_RCT_MEM;

	return JENT_ERR_LAG;
}

/* The permanent failure bit reported as @err, or 0. */
static unsigned int jent_health_err_permanent(int err)
{
	switch (err) {
	case JENT_ERR_RCT_PERMANENT:
		return JENT_RCT_FAILURE_PERMANENT;
	case JENT_ERR_APT_PERMANENT:
		return JENT_APT_FAILURE_PERMANENT;
	case JENT_ERR_LAG_PERMANENT:
		return JENT_LAG_FAILURE_PERMANENT;
	case JENT_ERR_RCT_MEM_PERMANENT:
		return JENT_RCT_MEM_FAILURE_PERMANENT;
	default:
		return 0;
	}
}

/**
 * Entry function: Obtain entropy for the caller.
 *
 * This function invokes the entropy gathering logic as often to generate
 * as many bytes as requested by the caller. The entropy gathering logic
 * creates 256 bit per invocation.
 *
 * This function truncates the last 256 bit entropy value output to the exact
 * size specified by the caller.
 *
 * @param[in] ec Reference to entropy collector
 * @param[out] data pointer to buffer for storing random data -- buffer must
 *	       already exist
 * @param[in] len size of the buffer, specifying also the requested number of random
 *	     in bytes
 *
 * @return number of bytes returned when request is fulfilled or an error
 *
 * The following error codes can occur:
 *	JENT_ERR_EINVAL			(-1)  entropy_collector is NULL
 *	JENT_ERR_RCT			(-2)  RCT failed
 *	JENT_ERR_APT			(-3)  APT failed
 *	JENT_ERR_NOTIME			(-4)  The timer cannot be initialized
 *	JENT_ERR_LAG			(-5)  LAG failure
 *	JENT_ERR_RCT_PERMANENT		(-6)  RCT permanent failure
 *	JENT_ERR_APT_PERMANENT		(-7)  APT permanent failure
 *	JENT_ERR_LAG_PERMANENT		(-8)  LAG permanent failure
 *	JENT_ERR_RCT_MEM		(-9)  RCT with memory failed
 *	JENT_ERR_RCT_MEM_PERMANENT	(-10) RCT with memory permanent failure
 *	JENT_ERR_SELFTEST		(-11) A bound jent_selftest run, or the
 *					      self test of a recovery of
 *					      jent_read_entropy_safe, failed,
 *					      permanently
 */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy(struct rand_data *ec, char *data, size_t len)
{
	/*
	 * Maximum value representable by ssize_t. Use a portable definition
	 * in case SSIZE_MAX is not available under strict C standard modes.
	 * It is nevertheless available on POSIX systems.
	 *
	 * Clearing the sign bit of SIZE_MAX relies on ssize_t being the
	 * signed counterpart of size_t, which the build assertion below
	 * enforces. Deriving the shift count from sizeof(ssize_t) instead
	 * would be undefined behavior as soon as ssize_t is the wider type.
	 */
	static const size_t ssize_max = (size_t)-1 >> 1;
	char *p = data;
	size_t orig_len;
	int ret = 0;

	JENT_BUILD_BUG_ON(sizeof(ssize_t) != sizeof(size_t));

	/* check obvious misuse of API */
	if (!ec || (data == NULL && len > 0))
		return JENT_ERR_EINVAL;

	if (!len)
		return 0;

	/*
	 * (hypothetical) edge case: clamp to ssize_t range to prevent
	 * negative return on cast
	 */
	if (len > ssize_max)
		len = ssize_max;
	orig_len = len;

	if (jent_notime_settick(ec)) {
		jent_stack_scrub();
		return JENT_ERR_NOTIME;
	}

	while (len > 0) {
		size_t tocopy;
		unsigned int health_test_result;

		/* A self test bound to this instance failed. */
		if (jent_atomic_load_int(&ec->selftest_failed)) {
			ret = JENT_ERR_SELFTEST;
			goto err;
		}

		jent_random_data(ec);

		if ((health_test_result = jent_health_failure(ec))) {
			ret = jent_health_err(health_test_result);
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
	 * Enhanced backtracking resistance needs nothing done here: it is a
	 * property of the XDRBG-256 generate operation itself. Every
	 * jent_read_random_block() call above consumed the state V' into one
	 * XOF invocation whose output is the successor state V followed by
	 * the returned bits, and only V is retained - the sponge is reset and
	 * the temporary buffer is zeroized. Recovering returned data from V
	 * would require inverting the XOF or knowing the consumed V', so an
	 * attacker who obtains the memory after this point cannot deduce
	 * output the instance produced before it. The explicit ratchet the
	 * pre-XDRBG design performed here is gone with the design: only its
	 * retained pool state allowed the last output to be recomputed.
	 */

err:
	jent_notime_unsettick(ec);

	/* A failure outputs nothing: wipe what earlier blocks copied. */
	if (ret && p != data)
		jent_memset_secure(data, (size_t)(p - data));

	if (!ret) {
		ec->read_invocations++;
		ec->bytes_output += orig_len;
	}

	jent_stack_scrub();

	return ret ? ret : (ssize_t)orig_len;
}

/*
 * JENT_ALLOC_MEASURE_CLOCK: an instance measuring the clock (startup, raw
 * recording).
 */
#define JENT_ALLOC_MEASURE_CLOCK	1

/*
 * Failures of a reset short of a permanent health failure: the noise source
 * failed up to JENT_MAX_OSR, or the platform lacked memory / a thread.
 */
#define JENT_RESET_FAILED	1
#define JENT_RESET_NORESOURCE	2

static int jent_entropy_init_internal(unsigned int osr, unsigned int flags,
				      unsigned int reinits);
static struct rand_data
*jent_entropy_collector_alloc_internal(unsigned int osr, unsigned int flags,
				       unsigned int reinits, int mode);
static int jent_entropy_collector_startup(struct rand_data **ec);
static inline int jent_entropy_init_common_pre(unsigned int flags);

/*
 * Replace @ec with a collector at the next oversampling rate, running its
 * startup if @startup. Returns 0, JENT_RESET_*, JENT_ERR_SELFTEST if a self
 * test of the conditioning failed, or the JENT_ERR_* code of a permanent
 * failure of that startup; on failure @ec is left untouched.
 */
static int jent_health_failure_reset(struct rand_data **ec, int startup)
{
	struct rand_data *new_ec;
	unsigned int osr, flags, reinits;
	int ret;

	/* Increment OSR */
	osr = (*ec)->osr + 1;

	/* Remember flags value */
	flags = (*ec)->flags;

	/* A compliance mode stays on the clock it was validated on. */
	if ((*ec)->is_fips_enabled) {
		if ((*ec)->enable_notime)
			flags |= JENT_FORCE_INTERNAL_TIMER;
		else
			flags |= JENT_DISABLE_INTERNAL_TIMER;
	}

	/* generic arbitrary cutoff to prevent running "forever" */
	if (osr > JENT_MAX_OSR)
		return JENT_RESET_FAILED;

	reinits = (*ec)->reinit_count + 1;

	/*
	 * Perform new health test with updated OSR. Only a failure of the
	 * measurement itself can pass at a higher one: a failed self test of
	 * the conditioning, a timer that reads zero or refused arguments
	 * fail the same at every OSR.
	 */
	while ((ret = jent_entropy_init_internal(osr, flags, reinits))) {
		switch (ret) {
		case EMEM:
			return JENT_RESET_NORESOURCE;
		case EHASH:
		case EGCD:
			return JENT_ERR_SELFTEST;
		case ENOTIME:
		case EPROGERR:
			return JENT_RESET_FAILED;
		default:
			break;
		}
		osr++;
		if (osr > JENT_MAX_OSR)
			return JENT_RESET_FAILED;
	}

	new_ec = jent_entropy_collector_alloc_internal(osr, flags, reinits, 0);

	/*
	 * In case of an error, leave the existing ec state untouched as a
	 * safety measure. But it is in error state and is of not much use.
	 */
	if (!new_ec)
		return JENT_RESET_NORESOURCE;

	/*
	 * Duplicate the state of the health tests, the identity and the
	 * caller's flags before the startup, to ensure the newly allocated
	 * state will continue from the current health state.
	 */
	jent_health_duplicate(new_ec, *ec);
	memcpy(new_ec->uuid, (*ec)->uuid, sizeof(new_ec->uuid));
	new_ec->flags = (*ec)->flags;
	new_ec->read_invocations = (*ec)->read_invocations;
	new_ec->bytes_output = (*ec)->bytes_output;

	/*
	 * Seed the replacement unless the caller is itself a startup loop,
	 * which continues on the new collector instead of nesting a second one.
	 */
	if (startup) {
		ret = jent_entropy_collector_startup(&new_ec);
		if (ret)
			return ret;
	}

	/* A failed self test carries over. */
	if (jent_atomic_load_int(&(*ec)->selftest_failed))
		jent_atomic_store_int(&new_ec->selftest_failed, 1);

	jent_entropy_collector_free(*ec);
	*ec = new_ec;

	return 0;
}

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
 * Note the function returns with a permanent health test error if the OSR is
 * getting too large. If an error is returned by this function, the Jitter RNG
 * is not safe to be used on the current system.
 *
 * @param[in] ec Reference to entropy collector - this is a double pointer as
 *	    	 The entropy collector may be freed and reallocated.
 * @param[out] data pointer to buffer for storing random data -- buffer must
 *	      	    already exist
 * @param[in] len size of the buffer, specifying also the requested number of
 *		  random in bytes
 *
 * @return see jent_read_entropy()
 */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy_safe(struct rand_data **ec, char *data, size_t len)
{
	/*
	 * Maximum value representable by ssize_t. Use a portable definition
	 * in case SSIZE_MAX is not available under strict C standard modes.
	 * It is nevertheless available on POSIX systems.
	 *
	 * Clearing the sign bit of SIZE_MAX relies on ssize_t being the
	 * signed counterpart of size_t, which the build assertion below
	 * enforces. Deriving the shift count from sizeof(ssize_t) instead
	 * would be undefined behavior as soon as ssize_t is the wider type.
	 */
	static const size_t ssize_max = (size_t)-1 >> 1;
	char *p = data;
	size_t orig_len;
	ssize_t ret = 0;
	int reset;

	JENT_BUILD_BUG_ON(sizeof(ssize_t) != sizeof(size_t));

	/* check obvious misuse of API */
	if (!ec || !*ec || (data == NULL && len > 0))
		return JENT_ERR_EINVAL;

	/*
	 * (hypothetical) edge case: clamp to ssize_t range to prevent
	 * negative return on cast
	 */
	if (len > ssize_max)
		len = ssize_max;
	orig_len = len;

	while (len > 0) {
		ret = jent_read_entropy(*ec, p, len);

		switch (ret) {
			/* Generic errors are returned immediately */
		case JENT_ERR_EINVAL:
		case JENT_ERR_NOTIME:

			/* Permanent health errors are returned immediately */
		case JENT_ERR_RCT_PERMANENT:
		case JENT_ERR_APT_PERMANENT:
		case JENT_ERR_LAG_PERMANENT:
		case JENT_ERR_RCT_MEM_PERMANENT:

			/*
			 * A failed conditioning self test as well: it judges
			 * the implementation, which a reallocation at a higher
			 * oversampling rate cannot mend.
			 */
		case JENT_ERR_SELFTEST:
			return ret;

			/* Intermittent health errors */
		case JENT_ERR_RCT:
		case JENT_ERR_APT:
		case JENT_ERR_LAG:
		case JENT_ERR_RCT_MEM:
			/*
			 * Re-allocate the entropy collector with updated
			 * OSR, hash loop count and memory size and run
			 * the startup sequence for NTG.1 again.
			 *
			 * If we fail here, the Jitter RNG returns the error.
			 */
			reset = jent_health_failure_reset(ec, 1);

			/* Out of resources: retry on the next call. */
			if (reset == JENT_RESET_NORESOURCE) {
				jent_stack_scrub();
				return ret;
			}

			/*
			 * The conditioning failed its self test: the
			 * instance stops for good, as after a failed bound
			 * jent_selftest().
			 */
			if (reset == JENT_ERR_SELFTEST) {
				jent_atomic_store_int(&(*ec)->selftest_failed,
						      1);
				jent_stack_scrub();
				return JENT_ERR_SELFTEST;
			}

			if (reset) {
				unsigned int permanent =
					jent_health_err_permanent(reset);

				/*
				 * No usable replacement: escalate to a
				 * permanent failure on the collector, reported
				 * to the FIPS callback once.
				 */
				if (!permanent)
					permanent = JENT_PERMANENT_FAILURE(
						(*ec)->health_failure &
						JENT_INTERMITTENT_FAILURES);
				else
					(*ec)->health_failure_reported |=
						permanent;
				(*ec)->health_failure |= permanent;
				jent_health_failure(*ec);
				jent_stack_scrub();
				return jent_health_err((*ec)->health_failure);
			}

			/*
			 * We are not returning the intermittent errors here.
			 * If a caller wants them, he should register a callback
			 * with jent_set_fips_failure_callback.
			 */

			break;

		default:
			/* defensive check for uncaught errors */
			if (ret >= 0) {
				len -= (size_t)ret;
				p += (size_t)ret;
			} else {
				return JENT_ERR_EINVAL;
			}
		}
	}

	return (ssize_t)orig_len;
}

/***************************************************************************
 * Initialization logic
 ***************************************************************************/

uint32_t jent_memsize(unsigned int flags)
{
	uint32_t memsize = JENT_FLAGS_TO_MAX_MEMSIZE(flags);
	static const uint32_t max_field =
		JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX);

	/*
	 * Flags of collectors instantiated with JENT_DISABLE_MEMORY_ACCESS are
	 * never normalized by jent_update_memsize(), so an out-of-range
	 * caller-provided size field can reach this point (e.g. via
	 * jent_status()). Clamp it: the shift below would otherwise exceed the
	 * uint32_t width, which is undefined behavior.
	 */
	if (memsize > max_field)
		memsize = max_field;

	if (memsize == 0) {
		memsize = JENT_DEFAULT_MEMORY_BITS;
	} else {
		memsize = memsize + JENT_MAX_MEMSIZE_OFFSET;
	}

	memsize = UINT32_C(1) << memsize;

	return memsize;
}

unsigned int jent_hashloop_cnt(unsigned int flags)
{
	unsigned int cnt = JENT_FLAGS_TO_HASHLOOP(flags);

	/* Clamp an unchecked field to the maximum. */
	if (cnt > JENT_FLAGS_TO_HASHLOOP(JENT_MAX_HASHLOOP))
		cnt = JENT_FLAGS_TO_HASHLOOP(JENT_MAX_HASHLOOP);

	if (cnt == 0)
		cnt = JENT_HASH_LOOP_DEFAULT;
	else
		cnt = UINT32_C(1) << (cnt - 1);

	return cnt;
}

/*
 * Whether the startup self tests have passed, per clock. Atomic: the store
 * releases the state the tests established, the load acquires it.
 */
#define JENT_CLOCK_PLATFORM	0	/* jent_get_nstime() */
#define JENT_CLOCK_NOTIME	1	/* the counting thread */
static int jent_selftest_run[2] = { 0, 0 };

/* Whether a startup has passed on @clock. */
static inline int jent_startup_passed(unsigned int clock)
{
	return jent_atomic_load_int(&jent_selftest_run[clock]);
}

/*
 * Whether a startup has passed on the clock @ec reads, run here if none has.
 * A failed one is run again by the next allocation: the fallback to the
 * internal timer is decided per collector, not remembered for the process,
 * so a platform clock that passes later is used again.
 */
static int jent_clock_usable(struct rand_data *ec, unsigned int osr,
			     unsigned int flags, unsigned int reinits)
{
	unsigned int clock = ec->enable_notime ? JENT_CLOCK_NOTIME :
						 JENT_CLOCK_PLATFORM;

	flags &= ~(unsigned int)(JENT_FORCE_INTERNAL_TIMER |
				 JENT_DISABLE_INTERNAL_TIMER);
	flags |= ec->enable_notime ? JENT_FORCE_INTERNAL_TIMER :
				     JENT_DISABLE_INTERNAL_TIMER;

	return jent_startup_passed(clock) ||
	       !jent_entropy_init_internal(osr, flags, reinits);
}

/* @mode: JENT_ALLOC_* */
static struct rand_data
*jent_entropy_collector_alloc_internal(unsigned int osr, unsigned int flags,
				       unsigned int reinits, int mode)
{
	int measure_clock = mode & JENT_ALLOC_MEASURE_CLOCK;
	struct rand_data *entropy_collector;
	uint32_t memsize = 0;

	/*
	 * Enforce the invariants of the compile-time tunable OSR bounds: the
	 * health-test lookup tables are indexed with osr - 1, and an empty
	 * [JENT_MIN_OSR, JENT_MAX_OSR] range would make every allocation fail.
	 */
	JENT_BUILD_BUG_ON(JENT_MIN_OSR < 1);
	JENT_BUILD_BUG_ON(JENT_MIN_OSR > JENT_MAX_OSR);

	/*
	 * Requesting disabling and forcing of internal timer
	 * makes no sense. NTG.1 disables it below.
	 */
	if ((flags & (JENT_DISABLE_INTERNAL_TIMER | JENT_NTG1)) &&
	    (flags & JENT_FORCE_INTERNAL_TIMER))
		return NULL;

	/*
	 * Ensure over sampling rate is not too low.
	 */
	osr = ensure_osr_is_at_least_minimal(osr);

	/*
	 * Reject too high OSR
	 */
	if (osr > JENT_MAX_OSR)
		return NULL;

	/* Force the self test to be run */
	if (!measure_clock &&
	    !jent_startup_passed(JENT_CLOCK_PLATFORM) &&
	    !jent_startup_passed(JENT_CLOCK_NOTIME) &&
	    jent_entropy_init_internal(osr, flags, reinits))
		return NULL;

	/*
	 * NTG.1 requires to disable the internal timer.
	 */
	if (flags & JENT_NTG1)
		flags |= JENT_DISABLE_INTERNAL_TIMER;

	entropy_collector = jent_zalloc(sizeof(struct rand_data), flags);
	if (NULL == entropy_collector)
		return NULL;

	entropy_collector->reinit_count = reinits;

	if (!(flags & JENT_DISABLE_MEMORY_ACCESS)) {
		memsize = jent_memsize(jent_update_memsize(flags, reinits));

		/* Never locked: the region is only timed, never output. */
		entropy_collector->mem =
			(unsigned char *)jent_zalloc_unlocked(memsize);

		if (entropy_collector->mem == NULL)
			goto err;

		/*
		 * Transform the size into a mask - it is assumed that size is
		 * a power of 2.
		 */
		entropy_collector->memmask = memsize - 1;
		entropy_collector->memaccessloops = JENT_MEM_ACC_LOOP_DEFAULT;
	}

	/* Set the hash loop count */
	entropy_collector->hashloopcnt =
		jent_hashloop_cnt(jent_update_hashloop(flags, reinits));

	/*
	 * Initialize the hash state for the XDRBG
	 */
	jent_shake256_init(&entropy_collector->hash_state);

	if ((flags & JENT_FORCE_FIPS) || jent_fips_enabled()) {
		/*
		 * NIST explicitly suggested to use an identical approach
		 * to the initialization of the conditioner as specified
		 * for the NTG.1 compliance.
		 */
		entropy_collector->startup_state = jent_startup_memory;
		entropy_collector->is_fips_enabled = 1;
	}

	/* Set the oversampling rate */
	entropy_collector->osr = osr;
	entropy_collector->flags = flags;
	entropy_collector->measure_clock = !!measure_clock;

	/*
	 * BSI AIS 20/31 NTG.1 requires that during startup 2 noise sources
	 * are sampled where each independently delivers 240 bits of entropy.
	 * This is ensured by setting the startup state such that the memory
	 * access is treated independently from the SHA3 operation and both
	 * must separately deliver the requested amount of entropy.
	 *
	 * NTG.1 implies the enabling of the FIPS mode to apply noise source
	 * oversampling and the enabling of the health tests.
	 */
	if (flags & JENT_NTG1) {
		entropy_collector->startup_state = jent_startup_memory;
		entropy_collector->is_fips_enabled = 1;
	}

	/* Initialize the health tests */
	if (jent_health_init(entropy_collector, flags & JENT_NTG1 ?
					        jent_health_init_type_ntg1 :
					        jent_health_init_type_common))
		goto err;

	/*
	 * Use timer-less noise source - note, OSR must be set in
	 * entropy_collector!
	 */
	if (!(flags & JENT_DISABLE_INTERNAL_TIMER)) {
		if (jent_notime_enable(entropy_collector, flags))
			goto err;
	}

	/* Fall back to the internal timer unless the caller pinned the clock. */
	if (!measure_clock &&
	    !jent_clock_usable(entropy_collector, osr, flags, reinits) &&
	    (entropy_collector->enable_notime ||
	     (flags & JENT_DISABLE_INTERNAL_TIMER) ||
	     jent_notime_enable(entropy_collector,
				flags | JENT_FORCE_INTERNAL_TIMER) ||
	     !jent_clock_usable(entropy_collector, osr, flags, reinits)))
		goto err;

	/* Was jent_entropy_init run on this clock (establishing the GCD)? */
	if (jent_gcd_get(&entropy_collector->jent_common_timer_gcd,
			 entropy_collector->enable_notime)) {
		if (!measure_clock)
			goto err;

		entropy_collector->jent_common_timer_gcd = 1;
	}

	return entropy_collector;

err:
	jent_entropy_collector_free(entropy_collector);
	return NULL;
}

/*
 * Run the startup entropy collection on *@ec. Returns 0, JENT_RESET_* or the
 * JENT_ERR_* code of a permanent failure, in which case *@ec is freed.
 */
static int jent_entropy_collector_startup(struct rand_data **ec_p)
{
	struct rand_data *ec = *ec_p;
	unsigned int health_test_result;
	int ret;

	/* fill the data pad with non-zero values */
	if (jent_notime_settick(ec)) {
		ret = JENT_RESET_NORESOURCE;
		goto err;
	}

	/*
	 * Assure, that we always have 512 bits (NTG.1 / FIPS compliance due to
	 * startup_state is set to 2) or 256 bits (other cases) entropy in
	 * our hash state before outputting a block by adding at least 256 bits
	 * before first usage. 512 bits are always transferred to the next state
	 * before the actual generation of random numbers to be returned to the
	 * caller. The size is due to the XDRBG state variable.
	 *
	 * For NTG.1: already perform the startup stages guaranteeing the
	 * invocation of 2 noise sources each delivering 240 bits of entropy
	 * at least at this point.
	 */
	do {
		jent_random_data(ec);

		health_test_result = jent_health_failure(ec);
		if (!health_test_result)
			continue;

		/* A permanent failure ends the startup as it ends generation. */
		if (health_test_result & JENT_PERMANENT_FAILURES) {
			ret = jent_health_err(health_test_result);
			goto err;
		}

		/* The reset runs a startup with a counting thread of its own. */
		jent_notime_unsettick(ec);

		/*
		 * Re-allocate the entropy collector with updated
		 * OSR, hash loop count and memory size.
		 */
		ret = jent_health_failure_reset(&ec, 0);
		if (ret)
			goto err;

		/*
		 * The reset replaced ec with a freshly allocated
		 * collector without a running timer thread. Restart it.
		 */
		if (jent_notime_settick(ec)) {
			ret = JENT_RESET_NORESOURCE;
			goto err;
		}
	} while (ec->startup_state != jent_startup_completed);

	jent_notime_unsettick(ec);
	*ec_p = ec;

	return 0;

err:
	jent_entropy_collector_free(ec);
	*ec_p = NULL;

	return ret;
}

static struct rand_data *_jent_entropy_collector_alloc(unsigned int osr,
						       unsigned int flags)
{
	struct rand_data *ec;

	if (jent_flags_invalid(flags))
		return NULL;

	if (jent_memaccess_contradicts(flags))
		return NULL;

	flags = jent_update_secure_mem(flags);

	ec = jent_entropy_collector_alloc_internal(osr, flags, 0, 0);
	if (!ec)
		return NULL;

	/* The per-instance identifier, carried over to every replacement. */
	jent_uuid_generate(ec->uuid);

	if (jent_entropy_collector_startup(&ec))
		return NULL;

	return ec;
}

JENT_PRIVATE_STATIC
struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
					       unsigned int flags)
{
	struct rand_data *ec = _jent_entropy_collector_alloc(osr, flags);

	jent_stack_scrub();

	return ec;
}

#if defined(LINUX_KERNEL) || defined(JENT_RAW_COLLECTOR)
/*
 * The self tests of the conditioning, which jent_entropy_collector_alloc_raw()
 * runs: EHASH or EGCD for a failed one, for a recording to name.
 */
int jent_raw_selftest(unsigned int flags)
{
	return jent_entropy_init_common_pre(flags);
}

/*
 * Test interface only, for linux_kernel/jitterentropy_testing.c and the
 * recording library of tests/raw-entropy/recording_library, and built only
 * for them - JENT_RAW_COLLECTOR is defined for the recording library's own
 * copy of the sources, never for libjitterentropy: allocate a
 * collector without the startup, so the raw noise recording measures exactly
 * the requested OSR and flags. The self tests of the conditioning run first,
 * as they do before jitterentropy-hashtime records: a recording over a broken
 * SHA-3 or GCD is no assessment of anything.
 */
struct rand_data *jent_entropy_collector_alloc_raw(unsigned int osr,
						   unsigned int flags)
{
	if (jent_raw_selftest(flags))
		return NULL;

	/* The memory the same flags get from jent_entropy_collector_alloc(). */
	return jent_entropy_collector_alloc_internal(osr,
						     jent_update_secure_mem(flags),
						     0, JENT_ALLOC_MEASURE_CLOCK);
}
#endif /* LINUX_KERNEL || JENT_RAW_COLLECTOR */

JENT_PRIVATE_STATIC
void jent_entropy_collector_free(struct rand_data *entropy_collector)
{
	if (entropy_collector != NULL) {
		/* Safety measure */
		jent_notime_unsettick(entropy_collector);

		jent_notime_disable(entropy_collector);

		if (entropy_collector->mem != NULL) {
			/*
			 * Use memmask (== memsize - 1, set whenever mem was
			 * allocated) rather than recomputing the size from
			 * ->flags. On the allocation-failure cleanup path ->flags
			 * may not have been assigned yet, in which case
			 * jent_memsize(->flags) would return the default size and
			 * mis-size the free (heap overflow or partial zeroization).
			 */
			jent_zfree(entropy_collector->mem,
				   (size_t)entropy_collector->memmask + 1);
			entropy_collector->mem = NULL;
		}
		jent_zfree(entropy_collector, sizeof(struct rand_data));
	}
}

/*
 * The startup measurement on one clock, at the memory size and hash loop
 * count of the @reinits-th reinitialization. Returns 0 or an E* error.
 */
static int jent_time_entropy_init_internal(unsigned int osr,
					   unsigned int flags,
					   unsigned int reinits)
{
	struct rand_data *ec = NULL;
	uint64_t *delta_history, gcd;
	int i, time_backwards = 0, count_stuck = 0, ret = 0, ticking = 0;
	size_t nelem = 0;
	unsigned int health_test_result;

	delta_history = jent_gcd_init(JENT_POWERUP_TESTLOOPCOUNT, flags);
	if (!delta_history)
		return EMEM;

	if (!(flags & JENT_FORCE_INTERNAL_TIMER))
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
	ec = jent_entropy_collector_alloc_internal(osr, flags, reinits,
						   JENT_ALLOC_MEASURE_CLOCK);
	if (!ec) {
		/*
		 * A counting thread that cannot run on this system is no
		 * timer, not missing memory: no retry and no recovery will
		 * find one.
		 */
		ret = ((flags & JENT_FORCE_INTERNAL_TIMER) &&
		       jent_notime_cpus_missing()) ? ENOTIME : EMEM;
		goto out;
	}

	/* The RCT-with-memory recovery must sample the source under test. */
	ec->startup_state = jent_startup_completed;

	if (jent_notime_settick(ec)) {
		ret = EMEM;
		goto out;
	}
	ticking = 1;

	/* To initialize the prior time. */
	jent_measure_jitter_one(ec, 0, NULL, 0);

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

		/*
		 * The reading the previous measurement ended on, taken before
		 * this one overwrites it, so the monotonicity check below
		 * compares two readings the timer actually produced.
		 *
		 * Reconstructing it as prev_time - delta could not: delta is
		 * unsigned and jent_measure_jitter() has already divided it by
		 * the common timer divisor, so the check reduced to delta > 0
		 * - which the coarseness check above had established anyway,
		 * letting a timer running backwards pass.
		 */
		start_time = ec->prev_time;

		/* Invoke core entropy collection logic */
		stuck = jent_measure_jitter(ec, 0, &delta);
		end_time = ec->prev_time;

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
		if (!delta) {
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
		if (!(end_time > start_time)) {
			time_backwards++;
			/* A wrapped delta would collapse the GCD analysis. */
			continue;
		}

		/* Watch for common adjacent GCD values */
		jent_gcd_add_value(delta_history, delta, nelem++);
	}

	/*
	 * we allow up to three times the time running backwards.
	 * All timer backends are monotonic by construction, but a counter read
	 * can still appear to go backwards once in a while, e.g. when the
	 * thread migrates between CPUs whose counters are not perfectly in
	 * sync. Such an event must not fail the test outright; the value of 3
	 * covers the occasional occurrence during our test run.
	 */
	if (time_backwards > 3) {
		ret = ENOMONOTONIC;
		goto out;
	}

	/* First, did we encounter a health test failure? */
	if ((health_test_result = jent_health_failure(ec))) {
		/*
		 * A permanent RCT failure only sets
		 * JENT_RCT_FAILURE_PERMANENT, not the intermittent bit, so both
		 * must be tested to report ERCT instead of the generic EHEALTH.
		 */
		ret = (health_test_result &
		       (JENT_RCT_FAILURE | JENT_RCT_FAILURE_PERMANENT)) ?
		      ERCT : EHEALTH;
		goto out;
	}

	ret = jent_gcd_verdict(delta_history, nelem, ec->osr, &gcd);
	if (ret)
		goto out;

	/*
	 * If we have more than 90% stuck results, then this Jitter RNG is
	 * likely to not work well.
	 */
	if (JENT_STUCK_INIT_THRES(JENT_POWERUP_TESTLOOPCOUNT) < count_stuck) {
		ret = ESTUCK;
		goto out;
	}

	/* Only a passed startup establishes the divisor of its clock. */
	jent_gcd_store(gcd, ec->enable_notime);

out:
	jent_gcd_fini(delta_history, JENT_POWERUP_TESTLOOPCOUNT);

	/* NOOP if notime disabled. Can be done unconditionally */
	if (ticking)
		jent_notime_unsettick(ec);

	jent_entropy_collector_free(ec);

	return ret;
}

int jent_time_entropy_init(unsigned int osr, unsigned int flags)
{
	return jent_time_entropy_init_internal(osr, flags, 0);
}

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
/*
 * The startup on the internal timer after the platform timer failed with
 * @platform_ret, which is returned if the internal timer cannot be set up.
 * That includes an internal timer this system cannot run (ENOTIME, too few
 * CPUs). With JENT_FORCE_INTERNAL_TIMER no platform timer was tested and
 * @platform_ret is no verdict: the EMEM or ENOTIME is returned as it is, so
 * that a recovery takes the missing memory or thread for what it is.
 */
static int jent_entropy_init_notime(unsigned int osr, unsigned int flags,
				    unsigned int reinits, int platform_ret,
				    unsigned int *clock)
{
	int ret = jent_time_entropy_init_internal(osr,
					flags | JENT_FORCE_INTERNAL_TIMER,
					reinits);

	if ((ret == EMEM || (ret == ENOTIME && jent_notime_cpus_missing())) &&
	    !(flags & JENT_FORCE_INTERNAL_TIMER))
		return platform_ret;

	*clock = JENT_CLOCK_NOTIME;
	return ret;
}
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

/**
 * jent_selftest() - Run the known answer tests of the conditioning component
 *
 * @param[in] ec Entropy collector a failure permanently disables, or NULL.
 *
 * @return 0 on success, EHASH if a known answer test failed or @ec is already
 *	   out of service after a failed self test.
 */
JENT_PRIVATE_STATIC
int jent_selftest(struct rand_data *ec)
{
	/*
	 * A passing run does not bring back an instance a self test stopped -
	 * this one bound earlier, or the GCD self test of a recovery - and
	 * must not report it as sound.
	 */
	if (ec && jent_atomic_load_int(&ec->selftest_failed))
		return EHASH;

	if (jent_sha3_tester()) {
		if (ec)
			jent_atomic_store_int(&ec->selftest_failed, 1);
		return EHASH;
	}

	return 0;
}

/*
 * The flags are only needed for the memory the GCD self test allocates:
 * JENT_FORCE_SECURE_MEM must reach it as well, or the initialization would
 * report success on memory the collector allocation is then going to reject.
 */
static inline int jent_entropy_init_common_pre(unsigned int flags)
{
	int ret;

	jent_notime_block_switch();
	jent_health_cb_block_switch();

	if (jent_sha3_tester())
		return EHASH;

	ret = jent_gcd_selftest(flags);

	return ret;
}

static inline int jent_entropy_init_common_post(int ret, unsigned int clock)
{
	/* Never unmark: a failure must not retract another thread's verdict. */
	if (!ret)
		jent_atomic_store_int(&jent_selftest_run[clock], 1);

	return ret;
}

/*
 * Note: the process-wide state this function and jent_entropy_init_ex()
 * establish - the self test verdict and the common timer GCD of each
 * clock - is written and read through
 * arch/jitterentropy-arch-atomic.h, so several threads may run them at once:
 * each establishes the same state, and a thread that is told the state is
 * established also sees what was established. Running them once before any
 * concurrent use, e.g. via pthread_once(), remains the cheaper way to get
 * there, as every call measures the timer afresh.
 *
 * What must still happen before any concurrent use is configuration.
 * jent_entropy_set_notime_cpu(), jent_entropy_switch_notime_impl() and
 * jent_set_fips_failure_callback() write state that the calls above and the
 * generation only read; the -EAGAIN they return once an initialization has run
 * reports that the window has closed, it does not synchronize with a thread
 * that is already in it.
 */
JENT_PRIVATE_STATIC
int jent_entropy_init(void)
{
	unsigned int clock = JENT_CLOCK_PLATFORM;
	/*
	 * As jent_entropy_init_ex(0, 0): a system in FIPS mode requires secure
	 * memory of the startup as it does of every collector.
	 */
	unsigned int flags = jent_update_secure_mem(0);
	int ret = jent_entropy_init_common_pre(flags);

	if (ret)
		return ret;

	ret = jent_time_entropy_init(0, flags | JENT_DISABLE_INTERNAL_TIMER);

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	if (ret)
		ret = jent_entropy_init_notime(0, flags, 0, ret, &clock);
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	ret = jent_entropy_init_common_post(ret, clock);

	jent_stack_scrub();

	return ret;
}

/*
 * jent_entropy_init_ex() for the @reinits-th reinitialization: the self
 * tests, then the startup on the platform timer and, failing that, on the
 * internal timer. Returns 0 or an E* error.
 */
static int jent_entropy_init_internal(unsigned int osr, unsigned int flags,
				    unsigned int reinits)
{
	unsigned int clock = JENT_CLOCK_PLATFORM;
	int ret;

	/*
	 * Apply the same requirement the collector allocation will apply, so
	 * that a caller asking for a compliance mode is not told the
	 * initialization succeeded on memory that the later allocation then
	 * refuses to work with.
	 */
	flags = jent_update_secure_mem(flags);

	/* The configuration window closes on the first attempt. */
	jent_notime_block_switch();
	jent_health_cb_block_switch();

	/* Arguments every allocation refuses. */
	if (osr > JENT_MAX_OSR || jent_flags_invalid(flags) ||
	    jent_memaccess_contradicts(flags))
		return EPROGERR;

	/* NTG.1 forbids the internal timer. */
	if ((flags & JENT_NTG1) && (flags & JENT_FORCE_INTERNAL_TIMER))
		return ENOTIME;

	ret = jent_entropy_init_common_pre(flags);

	if (ret)
		return ret;

	ret = ENOTIME;

	/* Test without internal timer unless caller does not want it */
	if (!(flags & JENT_FORCE_INTERNAL_TIMER))
		ret = jent_time_entropy_init_internal(osr,
					flags | JENT_DISABLE_INTERNAL_TIMER,
					reinits);

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	/*
	 * Test with internal timer unless the caller does not want it - and
	 * not for NTG.1, where the collector allocation disables the internal
	 * timer anyway and then refuses the combination.
	 */
	if (ret && !(flags & (JENT_DISABLE_INTERNAL_TIMER | JENT_NTG1)))
		ret = jent_entropy_init_notime(osr, flags, reinits, ret,
					       &clock);
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	ret = jent_entropy_init_common_post(ret, clock);

	jent_stack_scrub();

	return ret;
}

JENT_PRIVATE_STATIC
int jent_entropy_init_ex(unsigned int osr, unsigned int flags)
{
	return jent_entropy_init_internal(osr, flags, 0);
}

JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread)
{
	return jent_notime_switch(new_thread);
}

JENT_PRIVATE_STATIC
int jent_entropy_set_notime_cpu(unsigned long cpu)
{
	return jent_notime_set_cpu(cpu);
}

JENT_PRIVATE_STATIC
int jent_set_fips_failure_callback(jent_fips_failure_cb cb)
{
	return jent_set_fips_failure_callback_internal(cb);
}
