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
 * Each measurement mixes its time delta into the entropy pool and, with
 * @health, runs the health tests on it. Returns the stuck test result, 0
 * without @health. A call with @health 0 takes a time stamp without a delta
 * that is not one of the measurement reaching the health tests.
 *
 * One measurement of the memory access noise source alone, for the first
 * FIPS / NTG.1 startup stage.
 */
JENT_INTERNAL
unsigned int jent_measure_jitter_ntg1_memaccess(struct rand_data *ec,
						uint64_t loop_cnt,
						uint64_t *ret_current_delta,
						int health);
/* The same for the hash loop noise source, the second startup stage. */
JENT_INTERNAL
unsigned int jent_measure_jitter_ntg1_sha3(struct rand_data *ec,
					   uint64_t loop_cnt,
					   uint64_t *ret_current_delta,
					   int health);
/*
 * One measurement of both noise sources; ->prev_time must be primed, which
 * a call with @health 0 does.
 */
JENT_INTERNAL
unsigned int jent_measure_jitter(struct rand_data *ec,
				 uint64_t loop_cnt,
				 uint64_t *ret_current_delta,
				 int health);
JENT_INTERNAL
void jent_random_data(struct rand_data *ec);
JENT_INTERNAL
void jent_read_random_block(struct rand_data *ec, char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif

#endif /* JITTERENTROPY_NOISE_H */
