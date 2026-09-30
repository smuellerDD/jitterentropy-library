/*
 * Copyright (C) 2019 - 2026, Stephan Mueller <smueller@chronox.de>
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

/*
 * The feature-test macros ahead of the first system header: under a strict
 * -std=c11, glibc hides clock_gettime() and CLOCK_MONOTONIC otherwise.
 */
#include "arch/jitterentropy-arch-compat.h"

#include "jitterentropy.h"
#include "jitterentropy-internal.h"
/*
 * Before the <windows.h> below: the header selects the API level it needs,
 * which has to precede the first inclusion of the Windows headers.
 */
#include "jitterentropy-memlock.h"
#include "jitterentropy-options.h"

#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <math.h>
#include <float.h>
#include <assert.h>
#include <time.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
#endif

/*
 * Monotonic timestamp in nanoseconds. Returns 0 on success.
 *
 * The measured interval feeds the search invariants below, so a wall-clock
 * step (NTP, DST, manual adjustment) during a measurement must not be able to
 * corrupt or even negate the runtime.
 *
 * Windows has no clock_gettime(): QueryPerformanceCounter() is the monotonic
 * source there and its ticks are rescaled with the counter frequency.
 */
static int monotonic_nstime(uint64_t *out)
{
#if defined(_MSC_VER) || defined(__MINGW32__)
	LARGE_INTEGER freq, ticks;

	if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0)
		return 1;
	if (!QueryPerformanceCounter(&ticks) || ticks.QuadPart < 0)
		return 1;

	/*
	 * Whole seconds and remainder are scaled separately: a single
	 * ticks * 1000000000 overflows 64 bits after a few seconds of uptime
	 * with the usual 10 MHz performance counter.
	 */
	*out = (uint64_t)(ticks.QuadPart / freq.QuadPart) *
	       UINT64_C(1000000000) +
	       (uint64_t)(ticks.QuadPart % freq.QuadPart) *
	       UINT64_C(1000000000) / (uint64_t)freq.QuadPart;
	return 0;
#else
	struct timespec time;

	if (clock_gettime(CLOCK_MONOTONIC, &time) != 0)
		return 1;
	*out = (uint64_t)time.tv_sec * UINT64_C(1000000000) +
	       (uint64_t)time.tv_nsec;
	return 0;
#endif
}

/* We use a linear interpolation to estimate where the value is going to be.
 * The way these variable are named, this is technically the inverse function
 * for the resulting line, as we are trying to get the expected osr for a
 * provided time.
 *
 * Recall that point-point form for a line is
 * y - y1 = ((y2 - y1)/(x2 - x1))*(x - x1)
 * which is valid for non-vertical lines (i.e., so long as x1 != x2)
 * We actually want the functional inverse of this, so solving for x, we get
 * (y - y1) * ((x2 - x1) / (y2 - y1)) + x1 = x
 * This is valid so long as y2 != y1.
 * This functional inverse is the form that we use here.
 */
double linearInverse(double y, double x1, double y1, double x2, double y2) {
	/*
	 * A guard rather than an assertion. Two timings of the same code can
	 * come out equal - the measurement is of a machine under load, not of
	 * a function - and the division then produced a NaN that propagated
	 * into the oversampling rate the search went on to probe. Under
	 * NDEBUG the assertion was not there at all.
	 *
	 * With no slope there is nothing to extrapolate along, so the second
	 * point is the answer; the caller adjusts a guess that repeats one it
	 * has already measured.
	 */
	if (fabs(y1 - y2) < DBL_EPSILON)
		return x2;
	return (y-y1)*((x2-x1)/(y2-y1)) + x1;
}

/*
 * An oversampling rate the library will accept. Everything the search probes
 * goes through jent_output_time(), which ends the program when the
 * initialization refuses the rate - so an unclamped probe reports "selected
 * OSR 24 too high" for an OSR the caller never chose, and any target time
 * answering at 12 or above used to reach that.
 *
 * The argument is a double because that is what the interpolation produces,
 * and a negative or NaN value has no conversion to unsigned int at all: one
 * noisy measurement giving a negative slope used to land near UINT_MAX.
 */
static unsigned int jent_osr_clamp(double osr, unsigned int hi)
{
	/* NaN compares false here and lands on the floor. */
	if (!(osr >= (double)JENT_MIN_OSR))
		return JENT_MIN_OSR;
	if (osr >= (double)hi)
		return hi;
	return (unsigned int)osr;
}

/*
 * Returns the number of nanoseconds per output for the selected flags and osr.
 *
 * A failed measurement terminates the program: any numeric return value would
 * be indistinguishable from a legitimate (fast) timing and would corrupt the
 * search invariants of the caller (e.g. drive the doubling search into an
 * endless loop).
 */
uint64_t jent_output_time(uint64_t rounds, unsigned int osr, unsigned int flags)
{
	struct rand_data *ec_nostir;
	uint64_t start, finish;
	uint64_t runtime;
	int ret;

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		if (osr > JENT_MAX_OSR)
			fprintf(stderr,
				"The initialization failed with error code %d due to selected OSR %u too high (max %u)\n",
				ret, osr, JENT_MAX_OSR);
		else
			fprintf(stderr,
				"The initialization failed with error code %d\n",
				ret);
		exit(1);
	}

	ec_nostir = jent_entropy_collector_alloc(osr, flags);

	if (!ec_nostir) {
		fprintf(stderr, "Jitter RNG handle cannot be allocated\n");
		exit(1);
	}

	if (monotonic_nstime(&start)) {
		fprintf(stderr, "Unable to get start time\n");
		exit(1);
	}

	for (unsigned long size = 0; size < rounds; size++) {
		char tmp[32];

		if (0 > jent_read_entropy_safe(&ec_nostir, tmp, sizeof(tmp))) {
			fprintf(stderr, "FIPS 140-2 continuous test failed\n");
			exit(1);
		}
	}

	if (monotonic_nstime(&finish)) {
		fprintf(stderr, "Unable to get finish time\n");
		exit(1);
	}
	runtime = finish - start;

	jent_entropy_collector_free(ec_nostir);

	return runtime / (uint64_t)rounds;
}

int main(int argc, char * argv[])
{
	unsigned long rounds;
	unsigned int flags = 0;
	char *endtimeparam;
	double timeBoundIn;
	uint64_t timeBound;
	unsigned int maxBound, minBound, firstLinearGuess, secondLinearGuess;
	uint64_t minTime, maxTime, firstLinearTime, secondLinearTime;

	if (argc < 3) {
		fprintf(stderr, "%s <number of measurements> <target time> [" JENT_OPTIONS_USAGE "]\n", argv[0]);
		return 1;
	}

	if (parse_ulong(argv[1], &rounds) || rounds >= UINT_MAX || rounds == 0)
		return 1;
	argc--;
	argv++;

	/*
	 * Reject NaN, infinities and values whose nanosecond representation
	 * does not fit uint64_t: converting such a double to uint64_t below is
	 * undefined behavior and would yield a garbage time bound.
	 */
	timeBoundIn = strtod(argv[1], &endtimeparam);
	if (!isfinite(timeBoundIn) || (timeBoundIn <= 0.0) ||
	    (timeBoundIn > 1.8e10) || (endtimeparam == argv[1]) ||
	    (*endtimeparam != '\0'))
		return 1;

	/* The time upper bound, expressed as an integer number of nanoseconds. */
	timeBound = (uint64_t)floor(timeBoundIn * 1000000000.0);
	argc--;
	argv++;

	/* No --osr: the oversampling rate is what this tool searches for. */
	while (argc > 1) {
		int ret = jent_parse_option(&argc, &argv, &flags, NULL);

		if (ret < 0)
			return 1;
		if (!ret) {
			fprintf(stderr, "Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	/*
	 * The compliance modes require the collector memory to be locked into
	 * RAM, which the operating system permits only within a per-process
	 * limit. See jitterentropy-memlock.h.
	 */
	if (jent_raise_memlock_limit(flags))
		fprintf(stderr,
			"Cannot raise the memory lock limit, allocating the entropy collector may fail\n");

	/*
	 * Likewise the secure memory arena of the external crypto backends,
	 * which is created once for the process and is what the library
	 * allocates the collector from. See jitterentropy-memlock.h.
	 */
	if (jent_init_secure_memory(flags))
		fprintf(stderr,
			"Cannot create the secure memory arena, allocating the entropy collector will fail\n");

	/* We don't start with a maxBound. */
	maxBound = 0;
	maxTime = 0;

	/* Verify the first invariant: generation using minBound occurs in less than or equal time than the targeted time. */
	minBound = JENT_MIN_OSR;
	if((minTime = jent_output_time(rounds, minBound, flags)) > timeBound) {
		fprintf(stderr, "Minimum osr %u exceeds the target time. Invariant not met.\n", minBound);
		return 1;
	} else
		fprintf(stderr, "A minimum was found: osr upper bound is >= %u.\n", minBound);

	/* If there were no constant value in the linear expression, then we could estimate a cutoff
	 * using only this timing. We imagine that there is a fixed cost, so simple division overestimates
	 * the time cost per osr, and so this produces a likely underestimate.
	 */
	/*
	 * One below JENT_MAX_OSR, so that the collision adjustment below has a
	 * rate above it left to move to.
	 */
	firstLinearGuess = jent_osr_clamp((double)(timeBound /
						   (1U + minTime / minBound)),
					  JENT_MAX_OSR - 1);
	fprintf(stderr, "The initial linear estimate is osr=%u\n", firstLinearGuess);
	firstLinearTime = jent_output_time(rounds, firstLinearGuess, flags);

	/* We now have two points (minBound, minTime) and (firstLinearGuess, firstLinearTime), so we can
	 * perform a full linear interpolation.
	 * We are presently looking for an overestimate, so let's round up here.
	 */
	secondLinearGuess = jent_osr_clamp(ceil(linearInverse((double)timeBound, (double)minBound, (double)minTime, (double)firstLinearGuess, (double)firstLinearTime)),
					   JENT_MAX_OSR);
	fprintf(stderr, "Linear interpolation suggests a cutoff of %u\n", secondLinearGuess);

	/* These estimates were done in two related ways, but they could have produced the same value.
	 * Looking at the timing of two identical guesses isn't helpful, so adjust the result in this case.*/
	if(secondLinearGuess == firstLinearGuess) {
		secondLinearGuess++;
	}
	secondLinearTime = jent_output_time(rounds, secondLinearGuess, flags);

	/* We now expect that secondLinearGuess > firstLinearGuess but weird things could have occurred.
	 * They can't be equal, by the above adjustment.
	 * Exchange them if they don't have the expected relationship.
	 */
	if(secondLinearGuess < firstLinearGuess) {
		uint64_t tmpTime;
		unsigned int tmpGuess;

		tmpGuess = firstLinearGuess;
		tmpTime = firstLinearTime;

		firstLinearGuess = secondLinearGuess;
		firstLinearTime = secondLinearTime;

		secondLinearGuess = tmpGuess;
		secondLinearTime = tmpTime;
	}

	/*
	 * Now secondLinearGuess > firstLinearGuess, and the time values should
	 * have a similar relationship - but need not: nanoseconds per output
	 * are not strictly monotonic in the oversampling rate on a machine
	 * doing anything else at the same time. This used to be an assertion,
	 * which turned an ordinary scheduling artefact into a SIGABRT (and
	 * into nothing at all under NDEBUG). The branches below decide on the
	 * timings against timeBound and do not need the ordering.
	 */
	if(secondLinearTime <= firstLinearTime)
		fprintf(stderr, "Note: osr %u did not measure slower than osr %u; the machine is busy.\n",
			secondLinearGuess, firstLinearGuess);

	/* Use the linear interpolation guesses as bounds where possible. */
	if(firstLinearTime > timeBound) {
		/* Here, we have timeBound < firstLinearTime < secondLinearTime.
		 * In this case, the linear interpolations didn't yield a minBound
		 * so we'll proceed with the initial minBound.
		 *
		 * firstLinearGuess can be minBound itself: the estimate is
		 * clamped to JENT_MIN_OSR, and the exchange above can move a
		 * clamped secondLinearGuess here. The rate then measured once
		 * within and once above the target time, and there is no lower
		 * rate to search - this used to fail assert(maxBound > minBound).
		 * The target is met at the minimum rate, report it.
		 */
		if(firstLinearGuess <= minBound) {
			fprintf(stderr, "The target time is at the edge of JENT_MIN_OSR (%u); no lower rate exists.\n",
				minBound);
			printf("%u\n", minBound);
			return 0;
		}
		maxBound = firstLinearGuess;
		maxTime = firstLinearTime;
	} else if(secondLinearTime > timeBound) {
		/* Here, we have firstLinearTime <= timeBound < secondLinearTime.
		 * so the linear interpolations provide both a minBound and maxBound. */
		minBound = firstLinearGuess;
		minTime = firstLinearTime;
		maxBound = secondLinearGuess;
		maxTime = secondLinearTime;
	} else {
		/* here, we have know that firstLinearTime < secondLinearTime <= timeBound */
		/* so linear interpolation didn't supply a maxBound, and will have to look for it. */
		minBound = secondLinearGuess;
		minTime = secondLinearTime;
	}

	/*
	 * If we don't yet have a maxBound, find one. This will also adjust
	 * minBound up as the search goes.
	 *
	 * Every probe is clamped to JENT_MAX_OSR. The doubling had no ceiling,
	 * so once minBound reached 11 the next probe was 22 and
	 * jent_output_time() ended the program complaining about an
	 * oversampling rate the caller never asked for - which any target time
	 * answering at 12 or above reached.
	 */
	if(maxBound == 0) {
		maxBound = jent_osr_clamp((double)minBound * 2.0, JENT_MAX_OSR);

		if(maxBound <= minBound) {
			/* minBound is JENT_MAX_OSR: there is nothing above it. */
			fprintf(stderr, "The target time is met at JENT_MAX_OSR (%u); no higher rate exists.\n",
				(unsigned int)JENT_MAX_OSR);
			printf("%u\n", minBound);
			return 0;
		}

		fprintf(stderr, "Trying to find a maximum: %u", maxBound);
		/* Locate the maxBound */
		while((maxTime = jent_output_time(rounds, maxBound, flags)) <= timeBound) {
			minBound = maxBound;
			minTime = maxTime;

			if(maxBound >= JENT_MAX_OSR) {
				fprintf(stderr, ".\nThe target time is met at JENT_MAX_OSR (%u); no higher rate exists.\n",
					(unsigned int)JENT_MAX_OSR);
				printf("%u\n", minBound);
				return 0;
			}

			maxBound = jent_osr_clamp((double)maxBound * 2.0,
						  JENT_MAX_OSR);
			fprintf(stderr, " %u", maxBound);
		}
		fprintf(stderr, ".\nMaximum found: osr upper bound is < %u.\n", maxBound);
	}

	fprintf(stderr, "Desired osr upper bound is in [%u, %u)\n", minBound, maxBound);
	assert(maxBound > minBound);
	assert(maxTime > timeBound);
	assert(minTime <= timeBound);

	/* All invariants are now verified:
	 * maxBound > minBound
	 * maxTime > timeBound >= minTime
	 *
	 * We now perform a binary search (if necessary).
	 */
	while(maxBound - minBound > 1) {
		unsigned int curosr;
		uint64_t curTime;
		/* Calculate (minBound + maxBound)/2 without risk of overflow. */
		curosr = minBound + (maxBound - minBound) / 2;
		assert(curosr > minBound);
		assert(curosr < maxBound);

		fprintf(stderr, "Trying osr=%u. ", curosr);
		curTime = jent_output_time(rounds, curosr, flags);

		/*
		 * curTime is expected between minTime and maxTime, and used to
		 * be asserted to be. It routinely is not: the measurement is of
		 * a machine under load, so nanoseconds per output are not
		 * strictly monotonic in the oversampling rate, and an ordinary
		 * scheduling artefact aborted the tool - while under NDEBUG the
		 * check was absent and left linearInverse() dividing by zero
		 * instead. The search itself does not need the ordering: it
		 * only asks which side of timeBound this rate falls on, and
		 * that decision keeps the bracket valid either way.
		 */
		if(curTime <= minTime || curTime >= maxTime)
			fprintf(stderr, "(timing out of order, the machine is busy) ");

		if(curTime <= timeBound) {
			fprintf(stderr, "Timing is less than or equal to the target time. ");
			minBound = curosr;
			minTime = curTime;
		} else {
			fprintf(stderr, "Timing is greater than the target time. ");
			maxBound = curosr;
			maxTime = curTime;
		}

		fprintf(stderr, "Desired osr upper bound is in [%u, %u)\n", minBound, maxBound);
		/*
		 * What the search maintains by construction, and nothing about
		 * the ordering of the timings, which the machine decides.
		 */
		assert(maxBound > minBound);
		assert(maxTime > timeBound);
		assert(minTime <= timeBound);
	}

	printf("%u\n", minBound);

	return 0;
}
