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

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#include "jitterentropy-gcd.h"

#define ELEM 1000
#define EXP_GCD 50ULL

/*
 * Runs the analysis over ELEM deltas, EXP_GCD times mult[i % nmult] - or times
 * i without @mult -, on the given clock, and checks that it arrives at EXP_GCD. The clock keeps the
 * first GCD stored on it, so each case takes one of its own. Returns 0, or
 * the failed step (1: analysis, 2: nothing stored, 3: wrong GCD, 4: no
 * memory) plus @base.
 */
static int gcd_case(const uint64_t *mult, unsigned int nmult,
		    unsigned int clock, int base)
{
	uint64_t *gcd = jent_gcd_init(ELEM, 0);
	uint64_t val;
	unsigned int i;

	/*
	 * Assumed over sampling rate. Equal to the default over sampling rate.
	 */
	const size_t osr = JENT_MIN_OSR;

	if (!gcd)
		return base + 4;

	for (i = 0; i < ELEM; i++)
		jent_gcd_add_value(gcd, (mult ? mult[i % nmult] : i) * EXP_GCD,
				   i);

	if (jent_gcd_analyze(gcd, ELEM, osr, clock)) {
		jent_gcd_fini(gcd, ELEM);
		return base + 1;
	}

	jent_gcd_fini(gcd, ELEM);

	if (jent_gcd_get(&val, clock))
		return base + 2;

	if (val != EXP_GCD)
		return base + 3;

	return 0;
}

int main(int argc, char *argv[])
{
	/*
	 * Neither the first delta (300), the smallest (300) nor the GCD of any
	 * two of them (100, 150, 250) is EXP_GCD: only the GCD of all three is,
	 * as 6, 10 and 15 have no common factor while each pair has one.
	 */
	static const uint64_t coprime_as_a_whole[] = { 6, 10, 15 };
	int ret;

	(void)argc;
	(void)argv;

	/*
	 * 0, EXP_GCD, 2 * EXP_GCD, ...: arithmetic, so that "the first nonzero
	 * delta" passes as well, hence the second case.
	 */
	ret = gcd_case(NULL, 0, JENT_GCD_CLOCK_PLATFORM, 0);
	if (ret)
		return ret;

	return gcd_case(coprime_as_a_whole, 3, JENT_GCD_CLOCK_NOTIME, 4);
}
