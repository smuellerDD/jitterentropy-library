/*
 * Jitter RNG: unit tests for concurrent use of several instances
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
 * An entropy collector belongs to one user - the library states no locking for
 * an instance and this test claims none - but the library around the instances
 * is shared, and that is what is exercised here: several threads running the
 * whole life cycle at once, jent_entropy_init_ex() through
 * jent_entropy_collector_alloc(), the reads, and the free.
 *
 * What is not per instance is what a race would break:
 *
 *   - the startup self test runs once per process and every thread reads its
 *     verdict, so every thread has to be given the same one,
 *   - the conditioning known answer tests, the common timer GCD and the
 *     internal timer's forced state are process-wide and are established by
 *     whichever thread arrives first,
 *   - the instance identifier and the entropy pool are per collector, so two
 *     collectors built at the same time must not come out sharing either.
 *
 * The threads therefore record their results into a slot of their own and
 * assert nothing themselves: the counters in unit.h are plain variables, and a
 * suite that raced on its own bookkeeping would report anything. Everything is
 * judged after the join.
 *
 * A machine with one CPU still runs this - the threads interleave rather than
 * overlap, which is a weaker test but not a broken one.
 *
 * The suite runs this program without a sanitizer, where it asserts the
 * outcome: the same verdict everywhere, every collector built, every
 * generation delivered, no two instances alike. Built with -fsanitize=thread
 * instead, it also checks how the shared state is reached, and it is expected
 * to report nothing - the one-time state these threads race for is published
 * through arch/jitterentropy-arch-atomic.h:
 *
 *   clang -fsanitize=thread -O0 -I. -Isrc -Iarch -Itests -pthread \
 *         -DJENT_CONF_ENABLE_INTERNAL_TIMER -DJENT_STATIC_LIB \
 *         tests/unit/unit-concurrency.c -o unit-concurrency-tsan
 *
 * That run is what found the races those atomics answer, so a report from it
 * is a regression and not a property of the test.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <stdlib.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-sha3.c"
#include "jitterentropy-gcd.c"
#include "jitterentropy-health.c"
#include "jitterentropy-noise.c"
#include "jitterentropy-timer.c"
#include "jitterentropy-base.c"
#include "jitterentropy-uuid.c"
#include "jitterentropy-status.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * The threads this test runs in. Not the library's own thread backend: that
 * one exists to drive the counting thread of a single collector and starting
 * it here would be testing the timer rather than the concurrency. The two
 * spellings are the ones every other platform file in this project uses.
 */
#if defined(_MSC_VER) || defined(__MINGW32__)
# define UT_WIN_THREADS
#else
# include <pthread.h>
#endif

#define UT_THREADS	4	/* enough to overlap, few enough to stay quick */
#define UT_ROUNDS	3	/* life cycles per thread */
#define UT_BLOCK	32	/* bytes read per generation */

/*
 * One thread's work and what came of it. Written by that thread alone and read
 * after the join, so no lock is needed - and none is taken, as a lock here
 * would serialize the very overlap the test is for.
 */
struct ut_worker {
	unsigned int idx;
	unsigned int flags;		/* what its collectors are built with */

	int init_ret;			/* the startup verdict it was given */
	int init_differed;		/* a later round was told something else */
	int allocs;			/* collectors it built */
	int reads;			/* generations that delivered */
	int read_err;			/* the first read that did not */
	int status_err;			/* a status or UUID call that failed */
	char uuid[JENT_UUID_STRLEN];	/* the identity of its last collector */
	unsigned char block[UT_BLOCK];	/* the first block it generated */
};

static void ut_work(struct ut_worker *w)
{
	unsigned int round;

	/*
	 * The startup, once per round rather than once per thread: it is the
	 * call whose one-time state - the self test verdict, the conditioning
	 * known answer tests, the timer's common GCD - several threads race
	 * for, and running it repeatedly is what a library user does who calls
	 * it before every collector.
	 */
	for (round = 0; round < UT_ROUNDS; round++) {
		struct rand_data *ec;
		/*
		 * Large enough for the whole document: jent_status() reports a
		 * truncated one as a failure, and what this checks is that the
		 * call works while the other threads are in the library, not
		 * how it truncates - unit-base-api covers that.
		 */
		char status[4096];
		unsigned int i;
		int ret;

		ret = jent_entropy_init_ex(0, 0);
		if (!round)
			w->init_ret = ret;
		else if (ret != w->init_ret)
			w->init_differed = 1;

		ec = jent_entropy_collector_alloc(0, w->flags);
		if (!ec)
			continue;
		w->allocs++;

		for (i = 0; i < 2; i++) {
			unsigned char buf[UT_BLOCK];
			ssize_t rc = jent_read_entropy(ec, (char *)buf,
						       sizeof(buf));

			if (rc == (ssize_t)sizeof(buf)) {
				w->reads++;
				/* The first block of the thread, for the
				 * distinctness check after the join. */
				if (w->reads == 1)
					memcpy(w->block, buf, sizeof(buf));
			} else if (!w->read_err) {
				w->read_err = (int)rc;
			}
		}

		/*
		 * The two read-only entry points, on this thread's own
		 * instance while the others are being built and torn down.
		 */
		if (jent_status(ec, status, sizeof(status)))
			w->status_err = 1;
		if (jent_uuid(ec, w->uuid, sizeof(w->uuid)))
			w->status_err = 1;

		jent_entropy_collector_free(ec);
	}
}

#ifdef UT_WIN_THREADS

static DWORD WINAPI ut_thread_main(LPVOID arg)
{
	ut_work((struct ut_worker *)arg);
	return 0;
}

#else /* UT_WIN_THREADS */

static void *ut_thread_main(void *arg)
{
	ut_work((struct ut_worker *)arg);
	return NULL;
}

#endif /* UT_WIN_THREADS */

/*
 * Start them all before joining any: a run that started and joined one thread
 * at a time would be a sequential test with extra steps.
 *
 * Returns the number of threads that ran, which is what the checks are made
 * over - a machine that refuses a thread is not a defect in the library, and
 * the ones that did start still say something.
 */
static unsigned int ut_run(struct ut_worker *workers)
{
#ifdef UT_WIN_THREADS
	HANDLE handles[UT_THREADS];
#else
	pthread_t handles[UT_THREADS];
#endif
	unsigned int started = 0, i;

	for (i = 0; i < UT_THREADS; i++) {
#ifdef UT_WIN_THREADS
		handles[started] = CreateThread(NULL, 0, ut_thread_main,
						&workers[i], 0, NULL);
		if (!handles[started])
			break;
#else
		if (pthread_create(&handles[started], NULL, ut_thread_main,
				   &workers[i]))
			break;
#endif
		started++;
	}

	for (i = 0; i < started; i++) {
#ifdef UT_WIN_THREADS
		WaitForSingleObject(handles[i], INFINITE);
		CloseHandle(handles[i]);
#else
		pthread_join(handles[i], NULL);
#endif
	}

	return started;
}

static void test_concurrent_lifecycle(void)
{
	/*
	 * One configuration per thread, so that the collectors being built at
	 * the same time differ in the state the library derives per instance -
	 * memory size and hash loop count - rather than all taking the same
	 * path through the allocator.
	 */
	static const unsigned int flags[UT_THREADS] = {
		0,
		JENT_DISABLE_MEMORY_ACCESS,
		JENT_HASHLOOP_32,
		JENT_MAX_MEMSIZE_64kB,
	};
	struct ut_worker workers[UT_THREADS];
	unsigned int started, i, j, allocs = 0, reads = 0;
	unsigned int read_errs = 0, status_errs = 0, init_differed = 0;

	jent_ut_group("several instances through their life cycle at once");

	memset(workers, 0, sizeof(workers));
	for (i = 0; i < UT_THREADS; i++) {
		workers[i].idx = i;
		workers[i].flags = flags[i];
	}

	started = ut_run(workers);
	if (!started) {
		JENT_UT_SKIP("the concurrent life cycle",
			     "no thread could be created");
		return;
	}
	printf("  note: %u threads, %u rounds each\n", started, UT_ROUNDS);

	/*
	 * The startup verdict is a property of the machine, not of the caller,
	 * so it cannot depend on which thread reached the one-time state
	 * first - the second thread must not be told the timer is broken
	 * because the first one is still measuring it.
	 */
	for (i = 1; i < started; i++) {
		JENT_UT_EQ(workers[i].init_ret, workers[0].init_ret,
			   "every thread is given the same startup verdict");
	}

	for (i = 0; i < started; i++) {
		allocs += (unsigned int)workers[i].allocs;
		reads += (unsigned int)workers[i].reads;
		read_errs += workers[i].read_err ? 1 : 0;
		status_errs += workers[i].status_err ? 1u : 0u;
		init_differed += workers[i].init_differed ? 1u : 0u;

		if (workers[i].read_err)
			printf("  note: thread %u: a read returned %d\n", i,
			       workers[i].read_err);
	}

	JENT_UT_EQ(init_differed, 0,
		   "and the same one on every call it makes");
	JENT_UT_EQ(read_errs, 0u, "no thread was refused a generation");
	JENT_UT_EQ(status_errs, 0u,
		   "the status and UUID calls succeed alongside the others");

	if (workers[0].init_ret) {
		/*
		 * A machine whose startup does not pass builds no collector,
		 * which is not what this test is about. The threads still ran,
		 * and that they agreed on the verdict is checked above.
		 */
		printf("  note: the startup gives %d here\n",
		       workers[0].init_ret);
		JENT_UT_SKIP("the concurrent generation",
			     "the startup does not pass on this machine");
		return;
	}

	JENT_UT_EQ(allocs, started * UT_ROUNDS,
		   "every thread built a collector in every round");
	JENT_UT_EQ(reads, allocs * 2, "and every generation delivered");

	/*
	 * Nothing is shared between two instances that must not be: the
	 * identity is drawn per collector and the output comes from a state
	 * built per collector, so two threads generating at the same time
	 * cannot produce the same bytes - and a library that let them would
	 * hand the same "random" block to two callers.
	 */
	for (i = 0; i < started; i++) {
		for (j = i + 1; j < started; j++) {
			JENT_UT_TRUE(memcmp(workers[i].block, workers[j].block,
					    UT_BLOCK) != 0,
				     "two threads generate different blocks");
			JENT_UT_TRUE(strcmp(workers[i].uuid,
					    workers[j].uuid) != 0,
				     "two collectors carry different UUIDs");
		}
	}
}

int main(void)
{
	jent_ut_setup();

	test_concurrent_lifecycle();

	return jent_ut_report("unit-concurrency");
}
