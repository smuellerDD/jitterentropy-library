/*
 * Jitter RNG: unit tests for the platform backends in arch/
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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
 * Jitter RNG: unit tests for the time source backend.
 *
 * Every assertion has to hold on every platform: what is checked is the
 * contract the backend header in arch/ states, not the behaviour of one
 * implementation.
 */

/*
 * As in the AMALGAMATED programs under tests/raw-entropy: several arch sources
 * are absorbed here, and the ones needing _GNU_SOURCE define it themselves
 * before their own includes - which is too late once an earlier source in this
 * translation unit has already pulled the headers in. Stated once up front.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * The time source. Everything the library does rests on this, and the two
 * properties it must have are that it moves and that it does not go backwards.
 */
static void test_timer(void)
{
	uint64_t first = 0, prev, now;
	unsigned int i, moved = 0, backwards = 0;

	jent_ut_group("jent_get_nstime");

	jent_get_nstime(&first);
	JENT_UT_NE(first, 0, "the first reading is not zero");

	prev = first;
	for (i = 0; i < 10000; i++) {
		jent_get_nstime(&now);
		if (now != prev)
			moved++;
		if (now < prev)
			backwards++;
		prev = now;
	}

	JENT_UT_NE(moved, 0, "the counter changes across repeated reads");

	/*
	 * Not asserted as zero. Every backend is monotonic by construction, but
	 * a counter read can still appear to go backwards when the thread
	 * migrates between CPUs whose counters are not perfectly in sync -
	 * which is why jent_time_entropy_init() tolerates a few of these too.
	 * What would be a defect is a source that moves backwards constantly.
	 */
	JENT_UT_TRUE(backwards < 10,
		     "the counter does not routinely move backwards");
	if (backwards)
		printf("  note: %u of 10000 reads moved backwards\n", backwards);
}

/* The CPU count, used to place the timer thread. */

int main(void)
{
	test_timer();

	return jent_ut_report("unit-arch-timer");
}
