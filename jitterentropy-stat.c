/*
 * Non-physical true random number generator based on timing jitter --
 * Statistical calculations
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2013
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
 *
 */

#include "jitterentropy.h"

/*********************************************************
 * statistic gathering and validating functions
 *********************************************************/

void _jent_init_statistic(struct rand_data *rand_data)
{
	int i;
	struct entropy_stat *stat = &rand_data->entropy_stat;

	for(i = 0; i < 64; i++)
	{
		stat->bitslot[i] = 0;
		stat->bitvar[i] = 0;
	}

	jent_get_nstime(&stat->collection_begin);
}

void _jent_bit_count(struct rand_data *rand_data, __u64 prev_data)
{
	int i;

	if(!rand_data->entropy_stat.enable_bit_test)
		return;

	for(i = 0; i < 64; i++)
	{
		/* collect the count of set bits per bit position in the
		 * current ->data field */
		rand_data->entropy_stat.bitslot[i] += (rand_data->data & 1<<i) ? 1:0;

		/* collect the count of bit changes between the current
		 * and the previous random data value per bit position */
		if ((rand_data->data & 1<<i) != (prev_data & 1<<i))
			rand_data->entropy_stat.bitvar[i] += 1;
	}
}

static void jent_statistic_copy_stat(struct entropy_stat *src,
	       			     struct entropy_stat *dst)
{
	/* not copying bitslot and bitvar as they are not needed for statistic
	 * printout */
	dst->collection_begin 	= src->collection_begin;
	dst->collection_end	= src->collection_end;
	dst->old_delta		= src->old_delta;
	dst->setbits		= src->setbits;
	dst->varbits		= src->varbits;
	dst->obsbits		= src->obsbits;
	dst->collection_loop_cnt= src->collection_loop_cnt;
}

/*
 * Assessment of statistical behavior of the generated output and returning
 * the information to the caller by filling the target value.
 *
 * Details about the bit statistics are given in chapter 4 of the doc.
 * Chapter 5 documents the timer analysis and the resulting entropy.
 */
void _jent_calc_statistic(struct rand_data *rand_data,
			  struct entropy_stat *target, unsigned int loop_cnt)
{
	int i;
	struct entropy_stat *stat = &rand_data->entropy_stat;

	jent_get_nstime(&stat->collection_end);

	stat->collection_loop_cnt = loop_cnt;

	stat->setbits = 0;
	stat->varbits = 0;
	stat->obsbits = 0;

	for(i = 0; i < DATA_SIZE_BITS; i++)
	{
		stat->setbits += stat->bitslot[i];
		stat->varbits += stat->bitvar[i];

		/* This is the sum of set bits in the current observation
		 * of the random data. */
		stat->obsbits += (rand_data->data & 1<<i) ? 1:0;
	}

	jent_statistic_copy_stat(stat, target);

	stat->old_delta = (stat->collection_end - stat->collection_begin);
}

