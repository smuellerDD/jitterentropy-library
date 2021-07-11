/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2021
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
 * the GNU General Public License, in which case the provisions of the GPL2 are
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

#include "jitterentropy.h"

#include "jitterentropy-gcd.h"
#include "jitterentropy-health.h"
#include "jitterentropy-sha3.h"

#define MAJVERSION 3 /* API / ABI incompatible changes, functional changes that
		      * require consumer to be updated (as long as this number
		      * is zero, the API is not considered stable and can
		      * change without a bump of the major version) */
#define MINVERSION 0 /* API compatible, ABI may change, functional
		      * enhancements only, consumer can be left unchanged if
		      * enhancements are not considered */
#define PATCHLEVEL 3 /* API / ABI compatible, no functional changes, no
		      * enhancements, bug fixes only */

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

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/***************************************************************************
 * Thread handler
 ***************************************************************************/

JENT_PRIVATE_STATIC
int jent_notime_init(void **ctx)
{
	struct jent_notime_ctx *thread_ctx;
	long ncpu = jent_ncpu();

	if (ncpu < 0)
		return (int)ncpu;

	/* We need at least two CPUs to enable the timer thread */
	if (ncpu < 2)
		return -EOPNOTSUPP;

	thread_ctx = calloc(1, sizeof(struct jent_notime_ctx));
	if (!thread_ctx)
		return -errno;

	*ctx = thread_ctx;

	return 0;
}

JENT_PRIVATE_STATIC
void jent_notime_fini(void *ctx)
{
	struct jent_notime_ctx *thread_ctx = (struct jent_notime_ctx *)ctx;

	if (thread_ctx)
		free(thread_ctx);
}

static int jent_notime_start(void *ctx,
			     void *(*start_routine) (void *), void *arg)
{
	struct jent_notime_ctx *thread_ctx = (struct jent_notime_ctx *)ctx;
	int ret;

	if (!thread_ctx)
		return -EINVAL;

	ret = -pthread_attr_init(&thread_ctx->notime_pthread_attr);
	if (ret)
		return ret;

	return -pthread_create(&thread_ctx->notime_thread_id,
			       &thread_ctx->notime_pthread_attr,
			       start_routine, arg);
}

static void jent_notime_stop(void *ctx)
{
	struct jent_notime_ctx *thread_ctx = (struct jent_notime_ctx *)ctx;

	pthread_join(thread_ctx->notime_thread_id, NULL);
	pthread_attr_destroy(&thread_ctx->notime_pthread_attr);
}

static struct jent_notime_thread jent_notime_thread_builtin = {
	.jent_notime_init  = jent_notime_init,
	.jent_notime_fini  = jent_notime_fini,
	.jent_notime_start = jent_notime_start,
	.jent_notime_stop  = jent_notime_stop
};

/***************************************************************************
 * Timer-less timer replacement
 *
 * If there is no high-resolution hardware timer available, we create one
 * ourselves. This logic is only used when the initialization identifies
 * that no suitable time source is available.
 ***************************************************************************/

static int jent_force_internal_timer = 0;
static int jent_notime_switch_blocked = 0;

static struct jent_notime_thread *notime_thread = &jent_notime_thread_builtin;

/**
 * Timer-replacement loop
 *
 * @brief The measurement loop triggers the read of the value from the
 * counter function. It conceptually acts as the low resolution
 * samples timer from a ring oscillator.
 */
static void *jent_notime_sample_timer(void *arg)
{
	struct rand_data *ec = (struct rand_data *)arg;

	ec->notime_timer = 0;

	while (1) {
		if (ec->notime_interrupt)
			return NULL;

		ec->notime_timer++;
	}

	return NULL;
}

/*
 * Enable the clock: spawn a new thread that holds a counter.
 *
 * Note, although creating a thread is expensive, we do that every time a
 * caller wants entropy from us and terminate the thread afterwards. This
 * is to ensure an attacker cannot easily identify the ticking thread.
 */
static inline int jent_notime_settick(struct rand_data *ec)
{
	if (!ec->enable_notime || !notime_thread)
		return 0;

	ec->notime_interrupt = 0;
	ec->notime_prev_timer = 0;
	ec->notime_timer = 0;

	return notime_thread->jent_notime_start(ec->notime_thread_ctx,
					       jent_notime_sample_timer, ec);
}

static inline void jent_notime_unsettick(struct rand_data *ec)
{
	if (!ec->enable_notime || !notime_thread)
		return;

	ec->notime_interrupt = 1;
	notime_thread->jent_notime_stop(ec->notime_thread_ctx);
}

static inline void jent_get_nstime_internal(struct rand_data *ec, uint64_t *out)
{
	if (ec->enable_notime) {
		/*
		 * Allow the counting thread to be initialized and guarantee
		 * that it ticked since last time we looked.
		 *
		 * Note, we do not use an atomic operation here for reading
		 * jent_notime_timer since if this integer is garbled, it even
		 * adds to entropy. But on most architectures, read/write
		 * of an uint64_t should be atomic anyway.
		 */
		while (ec->notime_timer == ec->notime_prev_timer)
			;

		ec->notime_prev_timer = ec->notime_timer;
		*out = ec->notime_prev_timer;
	} else {
		jent_get_nstime(out);
	}
}

static inline int jent_notime_enable_thread(struct rand_data *ec)
{
	if (notime_thread)
		return notime_thread->jent_notime_init(&ec->notime_thread_ctx);
	return 0;
}

static inline void jent_notime_disable_thread(struct rand_data *ec)
{
	if (notime_thread)
		notime_thread->jent_notime_fini(ec->notime_thread_ctx);
}

static int jent_time_entropy_init(unsigned int enable_notime);
static int jent_notime_enable(struct rand_data *ec, unsigned int flags)
{
	/* Use internal timer */
	if (jent_force_internal_timer || (flags & JENT_FORCE_INTERNAL_TIMER)) {
		/* Self test not run yet */
		if (!jent_force_internal_timer && jent_time_entropy_init(1))
			return EHEALTH;

		ec->enable_notime = 1;
		return jent_notime_enable_thread(ec);
	}

	return 0;
}

static inline int jent_notime_switch(struct jent_notime_thread *new_thread)
{
	if (jent_notime_switch_blocked)
		return -EAGAIN;
	notime_thread = new_thread;
	return 0;
}

#else /* JENT_CONF_ENABLE_INTERNAL_TIMER */

static inline void jent_get_nstime_internal(struct rand_data *ec, uint64_t *out)
{
	(void)ec;
	jent_get_nstime(out);
}

static inline int jent_notime_enable_thread(struct rand_data *ec)
{
	(void)ec;
	(void)flags;
	return 0;
}

static inline void jent_notime_disable_thread(struct rand_data *ec)
{
	(void)ec;
	return 0;
}


static inline int jent_notime_enable(struct rand_data *ec, unsigned int flags)
{
	(void)ec;

	/* If we force the timer-less noise source, we return an error */
	if (flags & JENT_FORCE_INTERNAL_TIMER)
		return EHEALTH;

	return 0;
}

static inline int jent_notime_settick(struct rand_data *ec)
{
	(void)ec;
	return 0;
}

static inline void jent_notime_unsettick(struct rand_data *ec) { (void)ec; }

static inline int jent_notime_switch(struct jent_notime_thread *new_thread)
{
	(void)new_thread;
	return -EOPNOTSUPP;
}

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

/***************************************************************************
 * Noise sources
 ***************************************************************************/

/**
 * Update of the loop count used for the next round of
 * an entropy collection.
 *
 * @ec [in] entropy collector struct -- may be NULL
 * @bits [in] is the number of low bits of the timer to consider
 * @min [in] is the number of bits we shift the timer value to the right at
 *	     the end to make sure we have a guaranteed minimum value
 *
 * @return Newly calculated loop counter
 */
static uint64_t jent_loop_shuffle(struct rand_data *ec,
				  unsigned int bits, unsigned int min)
{
#ifdef JENT_CONF_DISABLE_LOOP_SHUFFLE

	(void)ec;
	(void)bits;

	return (1U<<min);

#else /* JENT_CONF_DISABLE_LOOP_SHUFFLE */

	uint64_t time = 0;
	uint64_t shuffle = 0;
	unsigned int i = 0;
	unsigned int mask = (1U<<bits) - 1;

	/*
	 * Mix the current state of the random number into the shuffle
	 * calculation to balance that shuffle a bit more.
	 */
	if (ec) {
		jent_get_nstime_internal(ec, &time);
		time ^= ec->data[0];
	}

	/*
	 * We fold the time value as much as possible to ensure that as many
	 * bits of the time stamp are included as possible.
	 */
	for (i = 0; ((DATA_SIZE_BITS + bits - 1) / bits) > i; i++) {
		shuffle ^= time & mask;
		time = time >> bits;
	}

	/*
	 * We add a lower boundary value to ensure we have a minimum
	 * RNG loop count.
	 */
	return (shuffle + (1U<<min));

#endif /* JENT_CONF_DISABLE_LOOP_SHUFFLE */
}

/**
 * CPU Jitter noise source -- this is the noise source based on the CPU
 * 			      execution time jitter
 *
 * This function injects the individual bits of the time value into the
 * entropy pool using a hash.
 *
 * @ec [in] entropy collector struct -- may be NULL
 * @time [in] time stamp to be injected
 * @loop_cnt [in] if a value not equal to 0 is set, use the given value as
 *		  number of loops to perform the hash operation
 * @stuck [in] Is the time stamp identified as stuck?
 *
 * Output:
 * updated hash context
 */
static void jent_hash_time(struct rand_data *ec, uint64_t time,
			   uint64_t loop_cnt, unsigned int stuck)
{
	HASH_CTX_ON_STACK(ctx);
	uint8_t itermediary[SHA3_256_SIZE_DIGEST];
	uint64_t j = 0;
#define MAX_HASH_LOOP 3
#define MIN_HASH_LOOP 0
	uint64_t hash_loop_cnt =
		jent_loop_shuffle(ec, MAX_HASH_LOOP, MIN_HASH_LOOP);

	sha3_256_init(ctx);

	/*
	 * testing purposes -- allow test app to set the counter, not
	 * needed during runtime
	 */
	if (loop_cnt)
		hash_loop_cnt = loop_cnt;

	/*
	 * This loop basically slows down the SHA-3 operation depending
	 * on the hash_loop_cnt. Each iteration of the loop generates the
	 * same result.
	 */
	for (j = 0; j < hash_loop_cnt; j++) {
		sha3_update(ctx, ec->data, SHA3_256_SIZE_DIGEST);
		sha3_update(ctx, (uint8_t *)&time, sizeof(uint64_t));
		sha3_update(ctx, (uint8_t *)&j, sizeof(uint64_t));

		/*
		 * If the time stamp is stuck, do not finally insert the value
		 * into the entropy pool. Although this operation should not do
		 * any harm even when the time stamp has no entropy, SP800-90B
		 * requires that any conditioning operation to have an identical
		 * amount of input data according to section 3.1.5.
		 */

		/*
		 * The sha3_final operations re-initialize the context for the
		 * next loop iteration.
		 */
		if (stuck || (j < hash_loop_cnt - 1))
			sha3_final(ctx, itermediary);
		else
			sha3_final(ctx, ec->data);
	}

	jent_memset_secure(ctx, SHA_MAX_CTX_SIZE);
	jent_memset_secure(itermediary, sizeof(itermediary));
}

/**
 * Memory Access noise source -- this is a noise source based on variations in
 * 				 memory access times
 *
 * This function performs memory accesses which will add to the timing
 * variations due to an unknown amount of CPU wait states that need to be
 * added when accessing memory. The memory size should be larger than the L1
 * caches as outlined in the documentation and the associated testing.
 *
 * The L1 cache has a very high bandwidth, albeit its access rate is  usually
 * slower than accessing CPU registers. Therefore, L1 accesses only add minimal
 * variations as the CPU has hardly to wait. Starting with L2, significant
 * variations are added because L2 typically does not belong to the CPU any more
 * and therefore a wider range of CPU wait states is necessary for accesses.
 * L3 and real memory accesses have even a wider range of wait states. However,
 * to reliably access either L3 or memory, the ec->mem memory must be quite
 * large which is usually not desirable.
 *
 * @ec [in] Reference to the entropy collector with the memory access data -- if
 *	    the reference to the memory block to be accessed is NULL, this noise
 *	    source is disabled
 * @loop_cnt [in] if a value not equal to 0 is set, use the given value as
 *		  number of loops to perform the hash operation
 */
static void jent_memaccess(struct rand_data *ec, uint64_t loop_cnt)
{
	unsigned int wrap = 0;
	uint64_t i = 0;
#define MAX_ACC_LOOP_BIT 7
#define MIN_ACC_LOOP_BIT 0
	uint64_t acc_loop_cnt =
		jent_loop_shuffle(ec, MAX_ACC_LOOP_BIT, MIN_ACC_LOOP_BIT);

	if (NULL == ec || NULL == ec->mem)
		return;
	wrap = ec->memblocksize * ec->memblocks;

	/*
	 * testing purposes -- allow test app to set the counter, not
	 * needed during runtime
	 */
	if (loop_cnt)
		acc_loop_cnt = loop_cnt;
	for (i = 0; i < (ec->memaccessloops + acc_loop_cnt); i++) {
		unsigned char *tmpval = ec->mem + ec->memlocation;
		/*
		 * memory access: just add 1 to one byte,
		 * wrap at 255 -- memory access implies read
		 * from and write to memory location
		 */
		*tmpval = (unsigned char)((*tmpval + 1) & 0xff);
		/*
		 * Addition of memblocksize - 1 to pointer
		 * with wrap around logic to ensure that every
		 * memory location is hit evenly
		 */
		ec->memlocation = ec->memlocation + ec->memblocksize - 1;
		ec->memlocation = ec->memlocation % wrap;
	}
}

/***************************************************************************
 * Start of entropy processing logic
 ***************************************************************************/

/**
 * This is the heart of the entropy generation: calculate time deltas and
 * use the CPU jitter in the time deltas. The jitter is injected into the
 * entropy pool.
 *
 * WARNING: ensure that ->prev_time is primed before using the output
 * 	    of this function! This can be done by calling this function
 * 	    and not using its result.
 *
 * @ec [in] Reference to entropy collector
 * @loop_cnt [in] see jent_hash_time
 * @ret_current_delta [out] Test interface: return time delta - may be NULL
 *
 * @return: result of stuck test
 */
static unsigned int jent_measure_jitter(struct rand_data *ec,
					uint64_t loop_cnt,
					uint64_t *ret_current_delta)
{
	uint64_t time = 0;
	uint64_t current_delta = 0;
	unsigned int stuck;

	/* Invoke one noise source before time measurement to add variations */
	jent_memaccess(ec, loop_cnt);

	/*
	 * Get time stamp and calculate time delta to previous
	 * invocation to measure the timing variations
	 */
	jent_get_nstime_internal(ec, &time);
	current_delta = jent_delta(ec->prev_time, time) /
						ec->jent_common_timer_gcd;
	ec->prev_time = time;

	/* Check whether we have a stuck measurement. */
	stuck = jent_stuck(ec, current_delta);

	/* Now call the next noise sources which also injects the data */
	jent_hash_time(ec, current_delta, loop_cnt, stuck);

	/* return the raw entropy value */
	if (ret_current_delta)
		*ret_current_delta = current_delta;

	return stuck;
}

/**
 * Generator of one 256 bit random number
 * Function fills rand_data->data
 *
 * @ec [in] Reference to entropy collector
 */
static void jent_random_data(struct rand_data *ec)
{
	unsigned int k = 0, safety_factor = ENTROPY_SAFETY_FACTOR;

	if (!ec->fips_enabled)
		safety_factor = 0;

	/* priming of the ->prev_time value */
	jent_measure_jitter(ec, 0, NULL);

	while (1) {
		/* If a stuck measurement is received, repeat measurement */
		if (jent_measure_jitter(ec, 0, NULL))
			continue;

		/*
		 * We multiply the loop value with ->osr to obtain the
		 * oversampling rate requested by the caller
		 */
		if (++k >= ((DATA_SIZE_BITS + safety_factor) * ec->osr))
			break;
	}
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
			if (health_test_result & JENT_RCT_FAILURE)
				ret = -2;
			else if (health_test_result & JENT_APT_FAILURE)
				ret = -3;
			else
				ret = -5;

			goto err;
		}

		if ((DATA_SIZE_BITS / 8) < len)
			tocopy = (DATA_SIZE_BITS / 8);
		else
			tocopy = len;
		memcpy(p, &ec->data, tocopy);

		len -= tocopy;
		p += tocopy;
	}

	/*
	 * To be on the safe side, we generate one more round of entropy
	 * which we do not give out to the caller. That round shall ensure
	 * that in case the calling application crashes, memory dumps, pages
	 * out, or due to the CPU Jitter RNG lingering in memory for long
	 * time without being moved and an attacker cracks the application,
	 * all he reads in the entropy pool is a value that is NEVER EVER
	 * being used for anything. Thus, he does NOT see the previous value
	 * that was returned to the caller for cryptographic purposes.
	 */
	/*
	 * If we use secured memory, do not use that precaution as the secure
	 * memory protects the entropy pool. Moreover, note that using this
	 * call reduces the speed of the RNG by up to half
	 */
#ifndef CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	jent_random_data(ec);
#endif

err:
	jent_notime_unsettick(ec);
	return ret ? ret : (ssize_t)orig_len;
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
		unsigned int osr, flags;

		ret = jent_read_entropy(*ec, p, len);

		switch (ret) {
		case -1:
		case -4:
			return ret;
		case -2:
		case -3:
		case -5:
			osr = (*ec)->osr;
			flags = (*ec)->flags;

			/* generic arbitrary cutoff */
			if (osr > 20)
				return ret;

			/* re-allocate entropy collector with higher OSR */
			jent_entropy_collector_free(*ec);

			/* Perform new health test */
			if (jent_entropy_init())
				return -1;

			*ec = jent_entropy_collector_alloc(osr++, flags);
			if (!*ec)
				return -1;
			break;

		default:
			len -= (size_t)ret;
			p += (size_t)ret;
		}
	}

	return (ssize_t)orig_len;
}

/***************************************************************************
 * Initialization logic
 ***************************************************************************/

static struct rand_data
*jent_entropy_collector_alloc_internal(unsigned int osr,
				       unsigned int flags)
{
	struct rand_data *entropy_collector;

	/*
	 * Requesting disabling and forcing of internal timer
	 * makes no sense.
	 */
	if ((flags & JENT_DISABLE_INTERNAL_TIMER) &&
	    (flags & JENT_FORCE_INTERNAL_TIMER))
		return NULL;

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	/*
	 * If the initial test code concludes to force the internal timer
	 * and the user requests it not to be used, do not allocate
	 * the Jitter RNG instance.
	 */
	if (jent_force_internal_timer && (flags & JENT_DISABLE_INTERNAL_TIMER))
		return NULL;
#endif

	entropy_collector = jent_zalloc(sizeof(struct rand_data));
	if (NULL == entropy_collector)
		return NULL;

	if (!(flags & JENT_DISABLE_MEMORY_ACCESS)) {
		/* Allocate memory for adding variations based on memory
		 * access
		 */
		entropy_collector->mem = 
			(unsigned char *)jent_zalloc(JENT_MEMORY_SIZE);
		if (entropy_collector->mem == NULL)
			goto err;

		entropy_collector->memblocksize = JENT_MEMORY_BLOCKSIZE;
		entropy_collector->memblocks = JENT_MEMORY_BLOCKS;
		entropy_collector->memaccessloops = JENT_MEMORY_ACCESSLOOPS;
	}

	/* verify and set the oversampling rate */
	if (osr < JENT_MIN_OSR)
		osr = JENT_MIN_OSR;
	entropy_collector->osr = osr;
	entropy_collector->flags = flags;

	if (jent_fips_enabled() || (flags & JENT_FORCE_FIPS))
		entropy_collector->fips_enabled = 1;

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
	}

	/* Use timer-less noise source */
	if (!(flags & JENT_DISABLE_INTERNAL_TIMER)) {
		if (jent_notime_enable(entropy_collector, flags))
			goto err;
	}

	return entropy_collector;

err:
	if (entropy_collector->mem != NULL)
		jent_zfree(entropy_collector->mem, JENT_MEMORY_SIZE);
	jent_zfree(entropy_collector, sizeof(struct rand_data));
	return NULL;
}

JENT_PRIVATE_STATIC
struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
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
void jent_entropy_collector_free(struct rand_data *entropy_collector)
{
	if (entropy_collector != NULL) {
		jent_notime_disable_thread(entropy_collector);
		if (entropy_collector->mem != NULL) {
			jent_zfree(entropy_collector->mem, JENT_MEMORY_SIZE);
			entropy_collector->mem = NULL;
		}
		jent_zfree(entropy_collector, sizeof(struct rand_data));
	}
}

static int jent_time_entropy_init(unsigned int enable_notime)
{
	struct rand_data *ec;
	uint64_t *delta_history;
	int i, time_backwards = 0, count_stuck = 0, ret = 0;
	unsigned int health_test_result;

	delta_history = jent_gcd_init(JENT_POWERUP_TESTLOOPCOUNT);
	if (!delta_history)
		return EMEM;

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	if (enable_notime)
		jent_force_internal_timer = 1;
#endif

	/*
	 * If the start-up health tests (including the APT and RCT) are not
	 * run, then the entropy source is not 90B compliant. We could test if
	 * fips_enabled should be set using the jent_fips_enabled() function,
	 * but this can be overridden using the JENT_FORCE_FIPS flag, which
	 * isn't passed in yet. It is better to run the tests on the small
	 * amount of data that we have, which should not fail unless things
	 * are really bad.
	 */
	ec = jent_entropy_collector_alloc_internal(0, JENT_FORCE_FIPS |
				(enable_notime ? JENT_FORCE_INTERNAL_TIMER :
						 JENT_DISABLE_INTERNAL_TIMER));
	if (!ec) {
		ret = EMEM;
		goto out;
	}

	if (jent_notime_settick(ec)) {
		ret = EMEM;
		goto out;
	}

	/* To initialize the prior time. */
	jent_measure_jitter(ec, 0, NULL);

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
		stuck = jent_measure_jitter(ec, 0, &delta);
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

	/* First, did we encounter a health test failure? */
	if ((health_test_result = jent_health_failure(ec))) {
		ret = (health_test_result & JENT_RCT_FAILURE) ? ERCT : EHEALTH;
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

	if (enable_notime && ec)
		jent_notime_unsettick(ec);

	jent_entropy_collector_free(ec);

	return ret;
}

JENT_PRIVATE_STATIC
int jent_entropy_init(void)
{
	int ret;

	jent_notime_switch_blocked = 1;

	if (sha3_tester())
		return EHASH;

	ret = jent_time_entropy_init(0);

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	jent_force_internal_timer = 0;
	if (ret)
		ret = jent_time_entropy_init(1);
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

	return ret;
}

JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread)
{
	return jent_notime_switch(new_thread);
}
