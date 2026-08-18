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
 * Jitter RNG: unit tests for the scheduler and thread backends.
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
 * Thread placement. Pinning is advisory throughout the library - a platform
 * with no affinity API reports -ENOTSUP and the internal timer works anyway -
 * so what is checked is that the failure is reported rather than that it
 * succeeds.
 */
static void test_sched(void)
{
	jent_ut_group("jent_thread_pin_to_cpu and jent_yield");

	/*
	 * Pinning exists only to place the counting thread, so the whole thread
	 * back-end - jent_thread_pin_to_cpu() included - is declared and
	 * defined only when the internal timer is compiled in. jent_yield() is
	 * not: it is the pause hint the collector itself uses and is built
	 * either way.
	 */
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	{
		long ncpu = jent_ncpu();
		int ret;

		ret = jent_thread_pin_to_cpu(0);
		jent_ut_checks++;
		if (ret && ret != -ENOTSUP && ret != -EINVAL)
			printf("  note: pinning to CPU 0 returned %d\n", ret);

		/*
		 * An index no machine has. Whatever the backend does with it,
		 * it must come back rather than wander off.
		 */
		ret = jent_thread_pin_to_cpu((unsigned long)1 << 20);
		jent_ut_checks++;

		/*
		 * That it must also not report success holds only where the
		 * argument names a CPU, which is every backend but the macOS
		 * one: there it is a THREAD_AFFINITY_POLICY tag - a hint that
		 * threads sharing it should share a cache, not an index - and
		 * any value is accepted. Keyed on the backend rather than on
		 * __APPLE__, as Apple Silicon rejects the policy outright and
		 * would pass the check for an unrelated reason.
		 */
#ifndef JENT_ARCH_THREAD_PIN_MACOS
		if (!ret && ncpu > 0 && ncpu < (1 << 20))
			JENT_UT_FAIL("%s",
				     "pinning to an out-of-range CPU succeeded");
#else
		(void)ncpu;
		(void)ret;
#endif
	}
#else
	JENT_UT_SKIP("jent_thread_pin_to_cpu",
		     "built without the internal timer");
#endif

	/* Has no return value; it is here to be called on every platform. */
	jent_yield();
}

int main(void)
{
	test_sched();

	return jent_ut_report("unit-arch-sched");
}
