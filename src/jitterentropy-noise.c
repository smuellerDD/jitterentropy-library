/* Jitter RNG: Noise Sources
 *
 * Copyright (C) 2021 - 2022, Stephan Mueller <smueller@chronox.de>
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

#ifdef __OPTIMIZE__
 #error "The CPU Jitter random number generator must not be compiled with optimizations. See documentation. Use the compiler switch -O0 for compiling jitterentropy.c."
#endif

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

/***************************************************************************
 * Noise sources
 ***************************************************************************/

/*
 * This is the xoshiro256** 1.0 PRNG, discussed here:
 * https://prng.di.unimi.it/
 * "xoshiro256++/xoshiro256** (XOR/shift/rotate) are our all-purpose generators
 * (not cryptographically secure generators, though, like all PRNGs in these pages).
 * They have excellent (sub-ns) speed, a state space (256 bits) that is large
 * enough for any parallel application, and they pass all tests we are aware of.
 * See the paper for a discussion of their differences."
 * Paper: http://vigna.di.unimi.it/ftp/papers/ScrambledLinear.pdf
 *
 * Any time you see a PRNG in a noise source, you should be concerned.
 *
 * This PRNG doesn’t directly produce the raw noise, or fundamentally
 * change the behavior of the underlying noise source. It is used in three
 * places:
 * 1) It use used to select the memory address being updated. The timing of
 * the update is the source of  the raw sample. This helps make each
 * read/update independent of the prior ones by making the addresses
 * statistically independent of each other.
 * 2) It is used to establish the number of memory read / hash cycles to do
 * prior to the one that is used as a source of memory read timing. This is
 * intended to help provide independence between used adjacent timings.
 * 3) It is used as a source of data for the hash loop. This use is
 * essentially a nonce.
 * This data is not required to be difficult for an attacker to guess, an
 * the output of this hash loop is not expected to contain any entropy.
 * The timing of this hash loop is used as an additional noise source,
 * which may contribute to the security of the system, but which is
 * assessed as providing no additional entropy.
 *
 * In summary, the output of this PRNG is used because it has excellent
 * statistical properties, but is never relied on for entropy production,
 * nor does it directly dictate the behavior of the primary or
 * secondary noise sources.
 */
static inline uint64_t uint64rotl(const uint64_t x, int k) {
	return (x << k) | (x >> (64 - k));
}

static inline uint64_t xoshiro256starstar(uint64_t *s)
{
	const uint64_t result = uint64rotl(s[1] * 5, 7) * 9;

	const uint64_t t = s[1] << 17;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];

	s[2] ^= t;

	s[3] = uint64rotl(s[3], 45);

	return result;
}

/**
 * Update of the loop count used for the next round of
 * an entropy collection.
 *
 * @ec [in] entropy collector struct
 * @bits [in] is the number of low bits of the timer to consider
 * @min [in] is the number of bits we shift the timer value to the right at
 *	     the end to make sure we have a guaranteed minimum value
 *
 * @return Newly calculated loop counter
 */
static uint64_t jent_loop_shuffle(struct rand_data *ec, unsigned int bits, unsigned int min)
{
	uint64_t shuffle = 0;
	uint64_t mask = (UINT64_C(1)<<bits) - 1;

	shuffle = xoshiro256starstar(ec->prngState.u) & mask;

	/*
	 * We add a lower boundary value to ensure we have a minimum
	 * RNG loop count.
	 */
	return (shuffle + (UINT64_C(1)<<min));
}

/**
 * Integrates the jent_memaccess timing into the conditioning function.
 *
 * @ec [in] entropy collector struct
 * @time [in] time delta to be injected
 *
 * Output:
 * updated hash context
 */
static void jent_hash_time(struct rand_data *ec, uint64_t time)
{
	/* If the data is within the expected distribution, then include
	 * the current_delta value is a noise source output from the primary noise source.
	 * In this case, if the health tests are currently passing, then this input is treated as
	 * contributing at least 1/osr bits of min entropy.
	 * If the health tests are not currently passing,  then this data is treated as not
	 * contributing any min entropy.
	 * If the data is outside of the expected distribution, this is treated as additional
	 * "supplemental data", and is treated as not contributing any min entropy.
	 */
	sha3_update(ec->hash_state, (uint8_t *)&time, sizeof(time));
}

/**
 * CPU Jitter additional noise source -- this is the noise source based on the CPU
 * 			      execution time jitter
 *
 * @ec [in] entropy collector struct
 *
 * Output:
 * updated hash context
 */
/*The same size as the internal SHA-3 state size*/
#define PR_DATA_LEN 25
static void jent_hash_additional(struct rand_data *ec)
{
	HASH_CTX_ON_STACK(ctx);
	uint8_t intermediary[SHA3_256_SIZE_DIGEST];
	uint64_t j = 0, hash_loops = UINT64_C(1) << ec->hash_loop_exp;;
	unsigned int i = 0;
	uint64_t endtime = 0, current_delta = 0;
	uint64_t pseudoRandomData[PR_DATA_LEN];

	/* Use the memset to shut up valgrind */
	memset(intermediary, 0, sizeof(intermediary));

	sha3_256_init(&ctx);

	/*
	* This loop populates the "intermediary" buffer.
	* The main reason for this loop is to execute something over which we
	* can perform a timing measurement. Feeding the resulting data into the
	* conditioning function is performed to ensure the result is used and
	* the compiler cannot optimize the loop away.
	*
	* Note, it does not matter which or how much data you inject, we are
	* interested in one Keccack1600 compression operation performed with
	* the sha3_final.
	*/
	for (j = 0; j < hash_loops; j++) {
		/* Chain any prior results from this process.
		 * There is no assumption of unpredictability here.
		 */
		sha3_update(&ctx, intermediary, sizeof(intermediary));

		/* Update the state using pseudo-random (and entropy free) results.
		 * There is no assumption of unpredictability here.
		 */
		for(i = 0; i < PR_DATA_LEN; i++) {
			pseudoRandomData[i] = xoshiro256starstar(ec->prngState.u);
		}
		sha3_update(&ctx, (uint8_t *)pseudoRandomData, sizeof(pseudoRandomData));

		/* Update the state using the current loop counter.
		 * There is no assumption of unpredictability here.
		 */
		sha3_update(&ctx, (uint8_t *)&j, sizeof(uint64_t));

		/* Finalize the hash, and copy it into the chaining variable. */
		sha3_final(&ctx, intermediary);
	}

        /*
         * Get time stamp and calculate time delta to previous
         * invocation to measure the timing variations. This is
	 * considered as an additional noise source, and is not
	 * credited as contributing any entropy.
         */
        jent_get_nstime_internal(ec, &endtime);
        current_delta = jent_delta(ec->prev_time, endtime) / ec->jent_common_timer_gcd;
	ec->prev_time = endtime;

	/* Data to provide to the conditioning function here:
	 * 1) The result of the hash operation.
	 * In SP 800-90B, this is viewed as "supplemental data" (as described in FIPS 140-3 IG D.K Resolution 6)
	 * and is viewed as contributing no entropy to the system (it acts essentially as a nonce).
	 */
	sha3_update(ec->hash_state, intermediary, sizeof(intermediary));

	/* 2) The overall timing data.
	 * In SP 800-90B, this is viewed as data coming from an additional noise source
	 * and is treated as if it contributes no entropy to the system.
	 * This is included to make the results at least as good as the historic design
	 * (which combined the two noise sources).
	 */
	sha3_update(ec->hash_state, (uint8_t *)&current_delta, sizeof(current_delta));
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
 *
 * @return The number of cycles the memory update took.
 */
static uint64_t jent_memaccess(struct rand_data *ec)
{
	/* This function relies on the "volatile" keyword to prevent the compiler from
	 * reordering the memory update and the timer calls.
	 * Writes to volatile variables are considered to be "observable behavior"
	 * so can't be moved past each other.
	 * The starttime / endtime / ec->mem values are all volatile.
	 */
	volatile uint64_t starttime = 0, endtime = 0;
	volatile unsigned char *tmpval;
	uint64_t j, memaccess_loops;

	if (NULL == ec || NULL == ec->mem)
		return 0;

	memaccess_loops = UINT64_C(1) << ec->memaccess_loop_exp;

	jent_get_nstime_internal(ec, &starttime);

	/* memaccess_loops should only be adjusted until the recorded distribution
	 * can be effectively captured with the available counter resolution.
	 * On some systems (like all modern Intel systems) the available timer
	 * is so fine that a memaccess_loops value of 1 is sufficient.
	 */

	for(j=0; j<memaccess_loops; j++) {
		/* Take PRNG output to find the memory location to update. */
		tmpval = ec->mem + (xoshiro256starstar(ec->prngState.u) & JENT_MEMSIZE_EXP_TO_MASK(ec->memsize_exp));

		/*
		 * memory access: just add 1 to one byte,
		 * wrap at 255 -- memory access implies read
		 * from and write to memory location
		 */
		*tmpval = (unsigned char)((*tmpval + 1) & 0xff);
	}

	jent_get_nstime_internal(ec, &endtime);

	return jent_delta(starttime, endtime) / ec->jent_common_timer_gcd;
}

/***************************************************************************
 * Start of entropy processing logic
 ***************************************************************************/

/**
 * This is the heart of the entropy generation: calculate time deltas and
 * use the CPU jitter in the time deltas. The jitter is injected into the
 * conditioning function.
 *
 * WARNING: ensure that ->prev_time is primed before using the output
 * 	    of this function! This can be done by calling this function
 * 	    and not using its result.
 *
 * @ec [in] Reference to entropy collector
 * @ret_current_delta [out] Test interface: return time delta - may be NULL
 *
 * @return: result of stuck test
 */
unsigned int jent_measure_jitter(struct rand_data *ec,
				 uint64_t *ret_current_delta)
{
	uint64_t current_delta;
	unsigned int stuck;
	uint64_t separation;
	uint64_t observed_symbols=0;

	separation = jent_loop_shuffle(ec, JENT_MEMORY_DEPTH_EXP, JENT_MEMORY_DEPTH_EXP);

	do {
		/* Invoke the primary noise source (the memory access noise source)*/
		current_delta = jent_memaccess(ec);

		/*
		 * Always include all the data, irrespective of its underlying distribution.
		 * In the instance where the data is not the desired distribution, then then
		 * data is considered supplemental information.
		 * Call the additional noise sources which also runs the conditioning algorithm.
		 */
		jent_hash_time(ec, current_delta);

		/*The distribution test will tell us if this is the desired sub-distribution.*/
		if(jent_dist_insert(ec, current_delta))
			observed_symbols++;

	} while (observed_symbols < separation);

	/*
	 * Integrate the additional noise source and supplemental information into the
	 * conditioning function.
	 */
	jent_hash_additional(ec);

	/*
	 * This is in the identified distribution that we are looking for,
	 * and we have the desired separation from the prior sample, so is formally a noise
	 * source output. Perform health testing.
	 */
	stuck = jent_stuck(ec, current_delta);

	/* return the raw entropy value */
	if (ret_current_delta)
		*ret_current_delta = current_delta;

	return stuck;
}

/**
 * Generator of one 256 bit random number
 * Function fills rand_data->hash_state
 *
 * @ec [in] Reference to entropy collector
 */
void jent_random_data(struct rand_data *ec)
{
	unsigned int k = 0, safety_factor = 0;

	if (ec->fips_enabled)
		safety_factor = ENTROPY_SAFETY_FACTOR;

	/* priming of the ->prev_time value */
	jent_measure_jitter(ec, NULL);

	while (!jent_health_failure(ec)) {
		/* If a stuck measurement is received, repeat measurement */
		if (jent_measure_jitter(ec, NULL))
			continue;

		/*
		 * We multiply the loop value with ->osr to obtain the
		 * oversampling rate requested by the caller
		 */
		if (++k >= ((DATA_SIZE_BITS + safety_factor) * ec->osr))
			break;
	}
}

void jent_read_random_block(struct rand_data *ec, char *dst, size_t dst_len)
{
	uint8_t jent_block[SHA3_256_SIZE_DIGEST];

	BUILD_BUG_ON(SHA3_256_SIZE_DIGEST != (DATA_SIZE_BITS / 8));

	/* The final operation automatically re-initializes the ->hash_state */
	sha3_final(ec->hash_state, jent_block);
	if (dst_len)
		memcpy(dst, jent_block, dst_len);

	/*
	 * Stir the new state with the data from the old state - the digest
	 * of the old data is not considered to have entropy.
	 */
	sha3_update(ec->hash_state, jent_block, sizeof(jent_block));
	jent_memset_secure(jent_block, sizeof(jent_block));
}
