/* Jitter RNG: Noise Sources
 *
 * Copyright (C) 2021 - 2025, Stephan Mueller <smueller@chronox.de>
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

#include "jitterentropy-noise.h"
#include "jitterentropy-health.h"
#include "jitterentropy-timer.h"
#include "jitterentropy-sha3.h"

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

/***************************************************************************
 * Noise sources
 ***************************************************************************/

/**
 * Insert a data block into the entropy pool
 *
 * The function inserts the intermediary buffer and the time delta together
 * into the entropy pool. The intermediary buffer is of exact the SHA3-256 rate
 * size to ensure that always one Keccak operation is triggered.
 *
 * Note, this function also clears the intermediary buffer immediately after it
 * was injected into the entropy pool.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] time_delta the time delta raw entropy value
 * @param[in] intermediary buffer that may hold other data in the first
 *			   JENT_SHA3_256_SIZE_DIGEST bytes
 */
static void jent_hash_insert(struct rand_data *ec, uint64_t time_delta,
			     uint8_t intermediary[JENT_SHA3_MAX_SIZE_BLOCK])
{
	/*
	 * Insert the time stamp into the intermediary buffer after the message
	 * digest of the intermediate data.
	 */
	memcpy(intermediary + JENT_SHA3_256_SIZE_DIGEST,
	       (uint8_t *)&time_delta, sizeof(uint64_t));

	/*
	 * Inject the data from the intermediary buffer, including the hash we
	 * are using for timing, and (if the timer is not stuck) the time stamp.
	 * Only the time is considered to contain any entropy. The intermediary
	 * buffer is exactly SHA3-256-rate-size to always cause a Keccak
	 * operation.
	 */
	jent_sha3_update(ec->hash_state, intermediary,
			 JENT_SHA3_MAX_SIZE_BLOCK);
	jent_memset_secure(intermediary, JENT_SHA3_MAX_SIZE_BLOCK);
}

/**
 * Hash loop noise source -- this is the noise source based on the CPU
 * 			     execution time jitter
 *
 * @param[in] ec entropy collector struct
 * @param[in] loop_cnt if a value not equal to 0 is set, use the given value as
 *		  number of loops to perform the hash operation
 * @param[in] stuck Is the time delta identified as stuck?
 */
static void jent_hash_loop(struct rand_data *ec,
			   uint8_t intermediary[JENT_SHA3_MAX_SIZE_BLOCK],
			   uint64_t loop_cnt)
{
	HASH_CTX_ON_STACK(ctx);
	uint64_t j = 0;

	/*
	 * testing purposes -- allow test app to set the counter, not
	 * needed during runtime
	 */
	uint64_t hash_loop_cnt = loop_cnt ? loop_cnt : JENT_HASH_LOOP_DEFAULT;

	jent_sha3_256_init(&ctx);

	/*
	 * This loop fills a buffer which is injected into the entropy pool.
	 * The main reason for this loop is to execute something over which we
	 * can perform a timing measurement. The injection of the resulting
	 * data into the pool is performed to ensure the result is used and
	 * the compiler cannot optimize the loop away in case the result is not
	 * used at all. Yet that data is considered "additional information"
	 * considering the terminology from SP800-90A without any entropy.
	 *
	 * Note, it does not matter which or how much data you inject, we are
	 * interested in one Keccack1600 compression operation performed with
	 * the sha3_final.
	 */
	for (j = 0; j < hash_loop_cnt; j++) {
		jent_sha3_update(&ctx, intermediary, JENT_SHA3_256_SIZE_DIGEST);
		jent_sha3_update(&ctx, (uint8_t *)&ec->rct_count,
				 sizeof(ec->rct_count));
		jent_sha3_update(&ctx, (uint8_t *)&ec->apt_cutoff,
				 sizeof(ec->apt_cutoff));
		jent_sha3_update(&ctx, (uint8_t *)&ec->apt_observations,
				 sizeof(ec->apt_observations));
		jent_sha3_update(&ctx, (uint8_t *)&ec->apt_count,
				 sizeof(ec->apt_count));
		jent_sha3_update(&ctx,(uint8_t *) &ec->apt_base,
				 sizeof(ec->apt_base));
		jent_sha3_update(&ctx, (uint8_t *)&j, sizeof(uint64_t));
		jent_sha3_final(&ctx, intermediary);
	}

	jent_memset_secure(&ctx, JENT_SHA_MAX_CTX_SIZE);
}

#ifdef JENT_RANDOM_MEMACCESS

static inline uint32_t uint32rotl(const uint32_t x, int k)
{
	return (x << k) | (x >> (32 - k));
}

static inline uint32_t xoshiro128starstar(uint32_t *s)
{
	const uint32_t result = uint32rotl(s[1] * 5, 7) * 9;
	const uint32_t t = s[1] << 9;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];

	s[2] ^= t;

	s[3] = uint32rotl(s[3], 11);

	return result;
}

/**
 * Memory access noise source -- this is the noise source based on the memory
 *				 access time jitter
 *
 * @param[in] ec entropy collector struct
 * @param[in] loop_cnt if a value not equal to 0 is set, use the given value as
 *		  number of loops to perform the hash operation
 */
static void jent_memaccess(struct rand_data *ec, uint64_t loop_cnt)
{
	uint64_t i = 0, time_now = 0;
	union {
		uint32_t u[4];
		uint8_t b[sizeof(uint32_t) * 4];
	} prngState = { .u = {0x8e93eec0, 0xce65608a, 0xa8d46b46, 0xe83cef69} };
	uint32_t addressMask;

	/*
	 * testing purposes -- allow test app to set the counter, not
	 * needed during runtime
	 */
	uint64_t acc_loop_cnt = loop_cnt ? loop_cnt : JENT_MEM_ACC_LOOP_DEFAULT;

	if (NULL == ec || NULL == ec->mem)
		return;
	addressMask = ec->memmask;

	/*
	 * Mix the current data into prngState
	 *
	 * Any time you see a PRNG in a noise source, you should be concerned.
	 *
	 * The PRNG doesn’t directly produce the raw noise, it just adjusts the
	 * location being updated. The timing of the update is part of the raw
	 * sample. The main thing this process gets you isn’t better
	 * “per-update” timing, it gets you mostly independent “per-update”
	 * timing, so we can now benefit from the Central Limit Theorem!
	 */
	for (i = 0; i < sizeof(prngState); i++) {
		jent_get_nstime_internal(ec, &time_now);
		prngState.b[i] ^= (uint8_t)(time_now & 0xff);
	}

	for (i = 0; i < (ec->memaccessloops + acc_loop_cnt); i++) {
		/* Take PRNG output to find the memory location to update. */
		unsigned char *tmpval = ec->mem +
					(xoshiro128starstar(prngState.u) &
					 addressMask);

		/*
		 * memory access: just add 1 to one byte,
		 * wrap at 255 -- memory access implies read
		 * from and write to memory location
		 */
		*tmpval = (unsigned char)((*tmpval + 1) & 0xff);
	}
}

#else /* JENT_RANDOM_MEMACCESS */

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
 * @param[in] ec Reference to the entropy collector with the memory access data -- if
 *	    the reference to the memory block to be accessed is NULL, this noise
 *	    source is disabled
 * @param[in] loop_cnt if a value not equal to 0 is set, use the given value as
 *		  number of loops to perform the hash operation
 */
static void jent_memaccess(struct rand_data *ec, uint64_t loop_cnt)
{
	unsigned int wrap = 0;
	uint64_t i = 0;

	/*
	 * testing purposes -- allow test app to set the counter, not
	 * needed during runtime
	 */
	uint64_t acc_loop_cnt = loop_cnt ? loop_cnt : JENT_MEM_ACC_LOOP_DEFAULT;

	if (NULL == ec || NULL == ec->mem)
		return;
	wrap = ec->memblocksize * ec->memblocks;

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

#endif /* JENT_RANDOM_MEMACCESS */

/***************************************************************************
 * Start of entropy processing logic
 ***************************************************************************/
/**
 * This is the heart of the entropy generation for NTG.1 startup, invoking only
 * the memory access noise source: calculate time deltas and use the CPU jitter
 * in the time deltas. The jitter is injected into the entropy pool.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] loop_cnt see jent_hash_time
 * @param[out] ret_current_delta Test interface: return time delta - may be NULL
 *
 * @return: result of stuck test
 */
unsigned int jent_measure_jitter_ntg1_memaccess(struct rand_data *ec,
						uint64_t loop_cnt,
						uint64_t *ret_current_delta)
{
	uint8_t intermediary[JENT_SHA3_MAX_SIZE_BLOCK] = { 0 };
	uint64_t time_now = 0;
	uint64_t current_delta = 0;
	unsigned int stuck;

	/*
	 * Get time stamp to only measure the execution time of the memory
	 * access to make this part an independent entropy source (even
         * excluding the SHA3 update to insert the data into the entropy pool).
	 */
	jent_get_nstime_internal(ec, &ec->prev_time);

	/*
	 * Now call the memory noise source with tripple the default iteration
	 * count considering this is the only noise source.
	 */
	jent_memaccess(ec, loop_cnt ? loop_cnt : JENT_MEM_ACC_LOOP_DEFAULT * 3);

	/*
	 * Get time stamp and calculate time delta to previous
	 * invocation to measure the timing variations
	 */
	jent_get_nstime_internal(ec, &time_now);
	current_delta = jent_delta(ec->prev_time, time_now) /
				   ec->jent_common_timer_gcd;

	/*
         * Check whether we have a stuck measurement - and apply the health
         * tests.
         */
	stuck = jent_stuck(ec, current_delta);

	/* Insert the data into the entropy pool */
	jent_hash_insert(ec, current_delta, intermediary);

	/* return the raw entropy value */
	if (ret_current_delta)
		*ret_current_delta = current_delta;

	return stuck;
}

/**
 * This is the heart of the entropy generation for NTG.1 startup, invoking only
 * the hash loop noise source: calculate time deltas and use the CPU jitter in
 * the time deltas. The jitter is injected into the entropy pool.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] loop_cnt see jent_hash_loop
 * @param[out] ret_current_delta Test interface: return time delta - may be NULL
 *
 * @return: result of stuck test
 */
unsigned int jent_measure_jitter_ntg1_sha3(struct rand_data *ec,
					   uint64_t loop_cnt,
					   uint64_t *ret_current_delta)
{
	uint8_t intermediary[JENT_SHA3_MAX_SIZE_BLOCK] = { 0 };
	uint64_t time_now = 0;
	uint64_t current_delta = 0;
	unsigned int stuck;

	/*
	 * Get time stamp to only measure the execution time of the hash loop
	 * to make this part an independent entropy source (even excluding the
         * SHA3 update to insert the data into the entropy pool).
	 */
	jent_get_nstime_internal(ec, &ec->prev_time);

	/*
	 * Now call the hash noise source with tripple the default iteration
	 * count considering this is the only noise source.
	 */
	jent_hash_loop(ec, intermediary, loop_cnt ? loop_cnt :
						    JENT_HASH_LOOP_DEFAULT * 3);

	/*
	 * Get time stamp and calculate time delta to previous
	 * invocation to measure the timing variations
	 */
	jent_get_nstime_internal(ec, &time_now);
	current_delta = jent_delta(ec->prev_time, time_now) /
				   ec->jent_common_timer_gcd;

	/*
         * Check whether we have a stuck measurement - and apply the health
         * tests.
         */
	stuck = jent_stuck(ec, current_delta);

	/* Insert the data into the entropy pool */
	jent_hash_insert(ec, current_delta, intermediary);

	/* return the raw entropy value */
	if (ret_current_delta)
		*ret_current_delta = current_delta;

	return stuck;
}

/**
 * This is the heart of the entropy generation: calculate time deltas and
 * use the CPU jitter in the time deltas. The jitter is injected into the
 * entropy pool.
 *
 * WARNING: ensure that ->prev_time is primed before using the output
 * 	    of this function! This can be done by calling this function
 * 	    and not using its result.
 *
 * @param[in] ec Reference to entropy collector
 * @param[in] loop_cnt see jent_hash_loop
 * @param[out] ret_current_delta Test interface: return time delta - may be NULL
 *
 * @return: result of stuck test
 */
unsigned int jent_measure_jitter(struct rand_data *ec,
				 uint64_t loop_cnt,
				 uint64_t *ret_current_delta)
{
	/* Size of intermediary ensures a Keccak operation during hash_update */
	uint8_t intermediary[JENT_SHA3_MAX_SIZE_BLOCK] = { 0 };

	uint64_t time_now = 0;
	uint64_t current_delta = 0;
	unsigned int stuck;

	/* Ensure that everything will fit into the intermediary buffer. */
	BUILD_BUG_ON(sizeof(intermediary) < (JENT_SHA3_256_SIZE_DIGEST +
					     sizeof(uint64_t)));

	/* Invoke memory access loop noise source */
	jent_memaccess(ec, loop_cnt);

	/*
	 * Get time stamp and calculate time delta to previous
	 * invocation to measure the timing variations
	 */
	jent_get_nstime_internal(ec, &time_now);
	current_delta = jent_delta(ec->prev_time, time_now) /
						ec->jent_common_timer_gcd;
	ec->prev_time = time_now;

	/* Check whether we have a stuck measurement. */
	stuck = jent_stuck(ec, current_delta);

	/* Invoke hash loop noise source */
	jent_hash_loop(ec, intermediary, loop_cnt);

	/* Insert the data into the entropy pool */
	jent_hash_insert(ec, current_delta, intermediary);

	/* return the raw entropy value */
	if (ret_current_delta)
		*ret_current_delta = current_delta;

	return stuck;
}

static void jent_random_data_one(
	struct rand_data *ec,
	unsigned int (*measure_jitter)(struct rand_data *ec,
			               uint64_t loop_cnt,
				       uint64_t *ret_current_delta))
{
	unsigned int k = 0, safety_factor = 0;

	if (ec->fips_enabled)
		safety_factor = ENTROPY_SAFETY_FACTOR;

	while (!jent_health_failure(ec)) {
		/* If a stuck measurement is received, repeat measurement */
		if (measure_jitter(ec, 0, NULL))
			continue;

		/*
		 * We multiply the loop value with ->osr to obtain the
		 * oversampling rate requested by the caller
		 */
		if (++k >= ((DATA_SIZE_BITS + safety_factor) * ec->osr))
			break;
	}
}

/**
 * Generator of one 256 bit random number
 * Function fills rand_data->hash_state
 *
 * @param[in] ec Reference to entropy collector
 */
void jent_random_data(struct rand_data *ec)
{
	/*
	 * Select which noise source to use for the entropy collection
	 */
	switch (ec->startup_state) {
	case jent_startup_memory:
		jent_random_data_one(ec, jent_measure_jitter_ntg1_memaccess);
		ec->startup_state--;

		/*
		 * Initialize the health tests as we fall through to
		 * independently invoke the next noise source.
		 */
		jent_health_init(ec);

		/* FALLTHROUGH */
	case jent_startup_sha3:
		jent_random_data_one(ec, jent_measure_jitter_ntg1_sha3);
		ec->startup_state--;

		/*
		 * Initialize the health tests as we fall through to
		 * independently invoke the next noise source.
		 */
		jent_health_init(ec);

		break;
	case jent_startup_completed:
	default:
		/* priming of the ->prev_time value */
		jent_measure_jitter(ec, 0, NULL);
		jent_random_data_one(ec, jent_measure_jitter);
	}
}
void jent_read_random_block(struct rand_data *ec, char *dst, size_t dst_len)
{
	uint8_t jent_block[JENT_SHA3_256_SIZE_DIGEST];

	BUILD_BUG_ON(JENT_SHA3_256_SIZE_DIGEST != (DATA_SIZE_BITS / 8));

	/* The final operation automatically re-initializes the ->hash_state */
	jent_sha3_final(ec->hash_state, jent_block);
	if (dst_len)
		memcpy(dst, jent_block, dst_len);

	/*
	 * Stir the new state with the data from the old state - the digest
	 * of the old data is not considered to have entropy.
	 */
	jent_sha3_update(ec->hash_state, jent_block, sizeof(jent_block));
	jent_memset_secure(jent_block, sizeof(jent_block));
}
