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

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
/*
 * A CPU the calling thread may run on, for the pinning check below to name.
 * CPU 0 is not always one of them - a cpuset, a container or a taskset may
 * exclude it - and the backends refuse a CPU outside the thread's affinity
 * with -EINVAL rather than widen it. Where the affinity can be read, the
 * lowest CPU in it is taken, so pinning is expected to succeed outright;
 * elsewhere CPU 0 is named and -EINVAL stays an accepted answer.
 *
 * Returns 1 when @cpu is taken from the thread's affinity, 0 otherwise.
 */
static int jent_ut_allowed_cpu(unsigned long *cpu)
{
	*cpu = 0;

#if defined(JENT_ARCH_THREAD_PIN_LINUX)
	{
		cpu_set_t set;
		unsigned long i;

		/* Fails on a kernel with more CPUs than a cpu_set_t holds. */
		CPU_ZERO(&set);
		if (sched_getaffinity(0, sizeof(set), &set))
			return 0;
		for (i = 0; i < (unsigned long)CPU_SETSIZE; i++) {
			if (CPU_ISSET((size_t)i, &set)) {
				*cpu = i;
				return 1;
			}
		}
	}
#elif defined(JENT_ARCH_THREAD_PIN_FREEBSD)
	{
		cpuset_t set;
		long first;

		CPU_ZERO(&set);
		if (cpuset_getaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1,
				       sizeof(set), &set))
			return 0;
		/* CPU_FFS() is 1-based, 0 for an empty set. */
		first = CPU_FFS(&set);
		if (first > 0) {
			*cpu = (unsigned long)(first - 1);
			return 1;
		}
	}
#endif

	return 0;
}
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

/*
 * Thread placement. Pinning is advisory throughout the library - a platform
 * with no affinity API reports -ENOTSUP and the internal timer works anyway -
 * so what is checked is that each backend answers as it documents: success
 * where it can pin, and the failure it names where it cannot.
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
		unsigned long cpu;
		int allowed = jent_ut_allowed_cpu(&cpu);
		int ret;

		ret = jent_thread_pin_to_cpu(cpu);

		/*
		 * A CPU from the thread's own affinity must be accepted. CPU 0,
		 * named where the affinity cannot be read, may lie outside it;
		 * the backends without CPU pinning answer as they document.
		 */
#if defined(JENT_ARCH_THREAD_PIN_WINDOWS) ||				       \
    defined(JENT_ARCH_THREAD_PIN_LINUX) ||				       \
    defined(JENT_ARCH_THREAD_PIN_FREEBSD)
		if (allowed)
			JENT_UT_EQ(ret, 0, "pinning to a CPU of the affinity");
		else
			JENT_UT_TRUE(!ret || ret == -EINVAL, "pinning to CPU 0");
#elif defined(JENT_ARCH_THREAD_PIN_MACOS)
		/* Intel honours the tag, Apple Silicon rejects the policy. */
		(void)allowed;
		JENT_UT_TRUE(!ret || ret == -ENOTSUP, "pinning to CPU 0");
#elif defined(JENT_ARCH_THREAD_PIN_NETBSD)
		/*
		 * pthread_setaffinity_np() is a privileged call on NetBSD: an
		 * unprivileged test run is refused with EPERM.
		 */
		(void)allowed;
		JENT_UT_TRUE(!ret || ret == -EINVAL || ret == -EPERM,
			     "pinning to CPU 0");
#elif defined(JENT_ARCH_THREAD_PIN_OPENBSD)
		(void)allowed;
		JENT_UT_EQ(ret, -ENOTSUP, "pinning to CPU 0");
#else
		(void)allowed;
		JENT_UT_EQ(ret, -ENOSYS, "pinning to CPU 0");
#endif

		/*
		 * An index no machine has. Whatever the backend does with it,
		 * it must come back rather than wander off.
		 */
		ret = jent_thread_pin_to_cpu((unsigned long)1 << 20);

		/*
		 * That it must also not report success holds only where the
		 * argument names a CPU, which is every backend but the macOS
		 * one: there it is a THREAD_AFFINITY_POLICY tag - a hint that
		 * threads sharing it should share a cache, not an index - and
		 * any value is accepted, so the answer is the same as for CPU
		 * 0 above. Keyed on the backend rather than on __APPLE__, as
		 * Apple Silicon rejects the policy outright and would pass the
		 * check for an unrelated reason.
		 */
#ifndef JENT_ARCH_THREAD_PIN_MACOS
		JENT_UT_TRUE(ret || ncpu <= 0 || ncpu >= (1 << 20),
			     "pinning to an out-of-range CPU is refused");
#else
		(void)ncpu;
		JENT_UT_TRUE(!ret || ret == -ENOTSUP,
			     "pinning to an out-of-range tag");
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
