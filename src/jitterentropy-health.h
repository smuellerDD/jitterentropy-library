/*
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

#ifndef JITTERENTROPY_HEALTH_H
#define JITTERENTROPY_HEALTH_H

#include "jitterentropy-internal.h"

#ifdef __cplusplus
extern "C"
{
#endif

void jent_health_cb_block_switch(void);
int jent_set_fips_failure_callback_internal(jent_fips_failure_cb cb);

static inline uint64_t jent_delta(uint64_t prev, uint64_t next)
{
	/*
	 * Return the delta between two values. If the values are monotonic
	 * increasing counters which can wrap, this caluculation implicitly
	 * returns the absolute value of the delta all the time.
	 */
	return (next - prev);
}

#if 0
static inline uint64_t jent_delta_abs(uint64_t prev, uint64_t next)
{
	/*
	 * Return the absolute value of the delta when the values are not a
	 * monotonic counter that may wrap.
	 */
	return (next > prev) ? (next - prev) : (prev - next);
}
#endif


/*
* The cutoff value is based on the following consideration:
* alpha = 2^-30 or 2^-60 as recommended in SP800-90B.
* In addition, we require an entropy value H of 1/osr as this is the minimum
* entropy required to provide full entropy. Note, we collect
* (DATA_SIZE_BITS + ENTROPY_SAFETY_FACTOR)*osr deltas for inserting them into
* the entropy pool which should then have (close to) DATA_SIZE_BITS bits of
* entropy in the conditioned output.
*
* Note, ec->rct_count (which equals to value B in the pseudo code of SP800-90B
* section 4.4.1) starts with zero (see jent_health_init(), jent_rct_insert()).
* Hence we need to subtract one from the cutoff value as calculated following
* SP800-90B. Thus C = ceil(-log_2(alpha)/H) = 30*osr or 60*osr.
*/
/* RCT: Intermittent cutoff threshold for alpha = 2**-30 */
#define JENT_HEALTH_RCT_INTERMITTENT_CUTOFF(x) ((x) * 30)
/* RCT: permanent cutoff threshold for alpha = 2**-60 */
#define JENT_HEALTH_RCT_PERMANENT_CUTOFF(x) ((x) * 60)

void jent_apt_reinit(struct rand_data *ec,
		     uint64_t current_delta,
		     unsigned int apt_count,
		     unsigned int apt_observations);
unsigned int jent_stuck(struct rand_data *ec, uint64_t current_delta);
unsigned int jent_health_failure(struct rand_data *ec);

enum jent_health_init_type {
	jent_health_init_type_common,
	jent_health_init_type_ntg1_startup,
	jent_health_init_type_ntg1_runtime,
};
void jent_health_init(struct rand_data *ec,
		      enum jent_health_init_type inittype);

#ifdef __cplusplus
}
#endif

#endif /* JITTERENTROPY_HEALTH_H */
