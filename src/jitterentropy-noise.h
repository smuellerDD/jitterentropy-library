/*
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

#ifndef JITTERENTROPY_NOISE_H
#define JITTERENTROPY_NOISE_H

#include "jitterentropy-internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

/*
 * One measurement of the memory access noise source alone, for the first
 * FIPS / NTG.1 startup stage. Returns the stuck test result.
 */
JENT_INTERNAL
unsigned int jent_measure_jitter_ntg1_memaccess(struct rand_data *ec,
						uint64_t loop_cnt,
						uint64_t *ret_current_delta);
/* The same for the hash loop noise source, the second startup stage. */
JENT_INTERNAL
unsigned int jent_measure_jitter_ntg1_sha3(struct rand_data *ec,
					   uint64_t loop_cnt,
					   uint64_t *ret_current_delta);
/*
 * One measurement of both noise sources, its time delta health tested and
 * mixed into the entropy pool; ->prev_time must be primed. Returns the stuck
 * test result.
 */
JENT_INTERNAL
unsigned int jent_measure_jitter(struct rand_data *ec,
				 uint64_t loop_cnt,
				 uint64_t *ret_current_delta);
/*
 * The same, health testing the time delta only with @health set. A call with
 * @health 0 primes ->prev_time, keeping that delta out of the health tests;
 * at the loop count of the measurements that follow, so that the delta of the
 * first of them spans a measurement at their count - see jent_hash_loop().
 */
JENT_INTERNAL
unsigned int jent_measure_jitter_one(struct rand_data *ec,
				     uint64_t loop_cnt,
				     uint64_t *ret_current_delta,
				     int health);
/*
 * Collect the entropy for one 256-bit output block into ->hash_state, running
 * the startup stage the collector is in.
 */
JENT_INTERNAL
void jent_random_data(struct rand_data *ec);
/* The blocks of an RCT-with-memory recovery, the startup stage unchanged. */
JENT_INTERNAL
void jent_random_data_recovery(struct rand_data *ec, unsigned int loops);
/* Generate up to one 256-bit block from ->hash_state with XDRBG-256. */
JENT_INTERNAL
void jent_read_random_block(struct rand_data *ec, char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif /* JITTERENTROPY_NOISE_H */
