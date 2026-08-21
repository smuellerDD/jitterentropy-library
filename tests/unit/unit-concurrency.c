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
 * is shared, and that is what is exercised here.
 *
 * What is not per instance is what a race would break:
 *
 *   - the startup self test runs once per process and every thread reads its
 *     verdict, so every thread has to be given the same one,
 *   - the conditioning known answer tests, the common timer GCD and the
 *     internal timer's forced state are process-wide and are established by
 *     whichever thread arrives first,
 *   - the FIPS failure callback is a process-wide registration that is closed
 *     once a collector has bound it, and the closing is one-way,
 *   - the instance identifier and the entropy pool are per collector, so two
 *     collectors built at the same time must not come out sharing either,
 *   - the clock is per collector as well: one built with the internal timer
 *     carries a counting thread and a counter of its own.
 *
 * Three tests, because those are three different races:
 *
 *   - the whole life cycle at once - jent_entropy_init_ex() through the
 *     allocation, the reads and the free - which is where the one-time startup
 *     state is contended,
 *   - the process-wide registrations against the collectors that close them,
 *     where threads that only register race threads that only allocate, and
 *   - the counting thread against the platform clock: several collectors
 *     driving one each, at one OSR, while others read the platform clock at
 *     another.
 *
 * All of them release their threads from a gate rather than letting them start
 * where pthread_create() put them. Creating a thread takes long enough that
 * the first one can be through the one-time initialization before the last one
 * exists, which would leave the race the test is named for untested on a fast
 * machine and tested on a slow one - the worst of both. Past the gate they all
 * reach the library within a scheduling quantum of each other.
 *
 * The threads record their results into a slot of their own and assert
 * nothing themselves: the counters in unit.h are plain variables, and a suite
 * that raced on its own bookkeeping would report anything. Everything is
 * judged after the join.
 *
 * A machine with one CPU still runs this - the threads interleave rather than
 * overlap, which is a weaker test but not a broken one. The gate does not
 * deadlock there either: it is opened by the main thread, which waits only for
 * threads that have already announced themselves.
 *
 * The suite runs this program without a sanitizer, where it asserts the
 * outcome: the same verdict everywhere, every collector built, every
 * generation delivered, no two instances alike. Built with -fsanitize=thread
 * instead, it also checks how the shared state is reached, and it is expected
 * to report nothing beyond the two entries of tests/tsan.supp - the one-time
 * state is published through arch/jitterentropy-arch-atomic.h, and the counter
 * of an internal timer is unsynchronized on purpose. The CMake build has that
 * build:
 *
 *   cmake -S . -B build-tsan -DENABLE_THREAD_SANITIZER=ON
 *   cmake --build build-tsan
 *   TSAN_OPTIONS=halt_on_error=1:suppressions=$PWD/tests/tsan.supp \
 *     ctest --test-dir build-tsan -R unit-concurrency --output-on-failure
 *
 * The suppressions are needed because the third test drives counting threads.
 * That run is what found the races the atomics answer, so any other report is
 * a regression and not a property of the test.
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

/*
 * As many threads as the machine has CPUs, within these bounds: below the
 * lower one there is not enough overlap to be worth arranging, and above the
 * upper one the run grows without the contention growing with it - every
 * thread past the CPU count waits rather than races.
 */
#define UT_MIN_THREADS	4
#define UT_MAX_THREADS	8
#define UT_ROUNDS	3	/* life cycles per thread */
#define UT_BLOCK	32	/* bytes read per generation */
/* Registration attempts per thread in the second test. */
#define UT_REGISTRATIONS 16

/*
 * The third test. Different over sampling rates, so the arms differ in more
 * than their clock: the OSR is what the startup runs at, what the cutoffs are
 * looked up with and what the measurements per block come from. Both are at
 * least JENT_MIN_OSR, the slower arm taking the lower one.
 */
#define UT_OSR_NOTIME	3	/* the collectors driving a counting thread */
#define UT_OSR_TIMER	5	/* the collectors reading the platform clock */
/*
 * Life cycles per counting-thread thread. Few: each is a whole counting thread
 * started, measured through and joined, and the arm is here to have several at
 * once rather than many in a row.
 */
#define UT_NOTIME_ROUNDS 2
/*
 * The platform-clock arm reads until the other arm is done, so the two really
 * overlap. This bounds that wait, and is only reached when no counting-thread
 * thread was ever started.
 */
#define UT_TIMER_MAX_ROUNDS 4096

/*
 * One thread's work and what came of it. Written by that thread alone and read
 * after the join, so no lock is needed - and none is taken, as a lock here
 * would serialize the very overlap the test is for.
 */
struct ut_worker {
	unsigned int idx;
	unsigned int flags;		/* what its collectors are built with */
	unsigned int osr;		/* and the over sampling rate it asks for */
	void (*work)(struct ut_worker *w);

	int init_ret;			/* the startup verdict it was given */
	int init_differed;		/* a later round was told something else */
	int selftest_ret;		/* the conditioning self test verdict */
	int allocs;			/* collectors it built */
	int reads;			/* generations that delivered */
	int read_err;			/* the first read that did not */
	int status_err;			/* a status or UUID call that failed */
	int misc_err;			/* a call that answered outside contract */

	/* The second test: what the process-wide registration answered. */
	int registrations;		/* attempts it made */
	int reg_blocked;		/* attempts refused with -EAGAIN */
	int reg_reopened;		/* a refusal followed by an acceptance */

	/* The third test: which clock its collectors turned out to be on. */
	struct rand_data *ec;		/* a collector built before the run */
	int notime_used;		/* collectors that ran off a counting thread */
	int notime_crossed;		/* ... that were built for the other clock */
	int ticked;			/* counting threads seen to have counted */
	unsigned int osr_seen;		/* the OSR its last collector settled on */
	uint64_t divisor;		/* the common timer divisor it was given */

	char uuid[JENT_UUID_STRLEN];	/* the identity of its last collector */
	unsigned char block[UT_BLOCK];	/* the first block it generated */
};

/*
 * The starting gate. Every thread announces itself and then spins until the
 * main thread opens it, which it does once every thread that was created has
 * announced itself. Load and store through the library's own atomics: this is
 * the one piece of state the test itself shares between threads, and a test
 * for data races may not introduce one.
 */
static int ut_gate;
static int ut_ready[UT_MAX_THREADS];

static void ut_gate_reset(void)
{
	unsigned int i;

	jent_atomic_store_int(&ut_gate, 0);
	for (i = 0; i < UT_MAX_THREADS; i++)
		jent_atomic_store_int(&ut_ready[i], 0);
}

static void ut_gate_wait(const struct ut_worker *w)
{
	jent_atomic_store_int(&ut_ready[w->idx], 1);

	while (!jent_atomic_load_int(&ut_gate))
		jent_yield();
}

static void ut_gate_open(unsigned int started)
{
	unsigned int i;

	for (i = 0; i < started; i++) {
		while (!jent_atomic_load_int(&ut_ready[i]))
			jent_yield();
	}

	jent_atomic_store_int(&ut_gate, 1);
}

/*
 * The calls that take nothing per instance and answer from process-wide state.
 * Made from every thread on every round, so that they are in flight while the
 * collectors around them are being built and torn down.
 */
static void ut_check_stateless(struct ut_worker *w)
{
	int ret;

	if (jent_version() != JENT_VERSION)
		w->misc_err = 1;

	ret = jent_secure_memory_supported();
	if (ret != 0 && ret != 1)
		w->misc_err = 1;
}

static void ut_work_lifecycle(struct ut_worker *w)
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

		ut_check_stateless(w);

		ec = jent_entropy_collector_alloc(0, w->flags);
		if (!ec)
			continue;
		w->allocs++;

		/*
		 * The conditioning known answer tests, bound to this thread's
		 * instance while the other threads are running theirs. The
		 * verdict is a property of the build, so it is compared across
		 * the threads after the join.
		 */
		if (!round)
			w->selftest_ret = jent_selftest(ec);
		else if (jent_selftest(ec) != w->selftest_ret)
			w->misc_err = 1;

		for (i = 0; i < 2; i++) {
			unsigned char buf[UT_BLOCK];
			ssize_t rc;

			/*
			 * Both entry points, because they reach the shared
			 * state differently: jent_read_entropy() only reads
			 * the startup verdict, while jent_read_entropy_safe()
			 * re-enters jent_entropy_init_ex() and reallocates
			 * the collector when a health test fires, which is
			 * the one-time state being contended all over again
			 * from a thread that is already generating.
			 */
			if (i & 1)
				rc = jent_read_entropy_safe(&ec, (char *)buf,
							    sizeof(buf));
			else
				rc = jent_read_entropy(ec, (char *)buf,
						       sizeof(buf));

			if (rc == (ssize_t)sizeof(buf)) {
				w->reads++;
				/*
				 * The first block of the thread, for the
				 * distinctness check after the join.
				 */
				if (w->reads == 1)
					memcpy(w->block, buf, sizeof(buf));
			} else if (!w->read_err) {
				w->read_err = (int)rc;
			}

			/*
			 * jent_read_entropy_safe() may have replaced the
			 * collector, and gives nothing back to free when it
			 * fails to build the replacement.
			 */
			if (!ec)
				break;
		}

		if (!ec)
			continue;

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

/*
 * The FIPS failure callback the second test registers. It may be called from
 * any of the threads that are generating, so it touches nothing that is not
 * written the way the gate is.
 */
static int ut_fips_cb_seen;

static void ut_fips_failure(struct rand_data *ec, unsigned int failure)
{
	(void)ec;
	(void)failure;

	jent_atomic_store_int(&ut_fips_cb_seen, 1);
}

/*
 * Half the threads do nothing but register, the other half nothing but
 * allocate in a compliance mode - which is what closes the registration, once
 * and for the life of the process.
 *
 * The registration is refused with -EAGAIN from then on, so which answer a
 * given attempt gets is a race the caller is meant to be able to lose safely.
 * What must not happen is the reverse: an attempt accepted after one was
 * refused would mean the closing is not one-way, and a caller could then
 * replace the callback of a collector already running in FIPS mode.
 */
static void ut_work_registrations(struct ut_worker *w)
{
	unsigned int i;

	if (w->idx & 1) {
		/*
		 * Fewer rounds than the registering threads make attempts: an
		 * allocation in a compliance mode is the expensive call in
		 * this program, and one is already enough to close the
		 * registration. The rest are there to keep the contention up
		 * while the other threads keep asking.
		 */
		for (i = 0; i < UT_ROUNDS; i++) {
			struct rand_data *ec =
				jent_entropy_collector_alloc(0, w->flags);
			unsigned char buf[UT_BLOCK];

			if (!ec)
				continue;
			w->allocs++;

			if (jent_read_entropy(ec, (char *)buf, sizeof(buf)) ==
			    (ssize_t)sizeof(buf))
				w->reads++;

			jent_entropy_collector_free(ec);
		}

		return;
	}

	for (i = 0; i < UT_REGISTRATIONS; i++) {
		/*
		 * Alternating between a callback and none, which is how a
		 * caller unregisters - both go through the same one-way
		 * gate and neither may reopen it.
		 */
		int ret = jent_set_fips_failure_callback((i & 1) ?
							 ut_fips_failure : NULL);

		w->registrations++;

		if (ret == -EAGAIN) {
			w->reg_blocked++;
		} else if (ret) {
			w->misc_err = 1;
		} else if (w->reg_blocked) {
			w->reg_reopened++;
		}

		ut_check_stateless(w);
	}
}


#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/*
 * The third test: the counting thread against the platform clock.
 *
 * With the internal timer, a collector's clock is a thread of its own that
 * increments a counter, all of it per instance. One arm has several of those
 * running at once; the other reads the platform clock.
 *
 * The second arm cannot be arranged after the fact: asking any collector for
 * the internal timer forces it process-wide and one way, and every collector
 * built afterwards gets a counting thread whether it asked or not. So its
 * collectors are built before the run and only read from - which is also why
 * it uses jent_read_entropy() and not the safe variant, whose reallocation
 * would build the replacement after the forcing.
 */

/*
 * Which arm a thread belongs to. Odd indices drive a counting thread, even
 * ones the platform clock, so at UT_MIN_THREADS there are two of each.
 */
static int ut_notime_arm(unsigned int idx)
{
	return !!(idx & 1);
}

/*
 * How the platform-clock arm learns that the other one is finished. One flag
 * per thread rather than a counter: the atomics here are loads and stores, and
 * a counter read-modify-written by several threads would be a data race of the
 * kind this program exists to catch.
 */
static int ut_notime_done[UT_MAX_THREADS];

static void ut_notime_done_reset(unsigned int nthreads)
{
	unsigned int i;

	/* Anything but a counting-thread thread about to run counts as done. */
	for (i = 0; i < UT_MAX_THREADS; i++)
		jent_atomic_store_int(&ut_notime_done[i],
				      (i < nthreads && ut_notime_arm(i)) ? 0 : 1);
}

static int ut_notime_pending(void)
{
	unsigned int i;

	for (i = 0; i < UT_MAX_THREADS; i++) {
		if (!jent_atomic_load_int(&ut_notime_done[i]))
			return 1;
	}

	return 0;
}

static void ut_notime_read(struct ut_worker *w, struct rand_data *ec)
{
	unsigned char buf[UT_BLOCK];
	ssize_t rc;

	/* Which clock it is on was decided when it was built. */
	if (ec->enable_notime) {
		w->notime_used++;
		if (!(w->flags & JENT_FORCE_INTERNAL_TIMER))
			w->notime_crossed++;
	} else if (w->flags & JENT_FORCE_INTERNAL_TIMER) {
		w->notime_crossed++;
	}

	/* The rate and the divisor it ended up with, judged after the join. */
	w->osr_seen = ec->osr;
	w->divisor = ec->jent_common_timer_gcd;

	if (jent_uuid(ec, w->uuid, sizeof(w->uuid)))
		w->status_err = 1;

	rc = jent_read_entropy(ec, (char *)buf, sizeof(buf));
	if (rc == (ssize_t)sizeof(buf)) {
		w->reads++;
		if (w->reads == 1)
			memcpy(w->block, buf, sizeof(buf));
	} else if (!w->read_err) {
		w->read_err = (int)rc;
	}

	/*
	 * The read joined this instance's counting thread, so the counter is
	 * safe to look at - and one that moved is the evidence that the clock
	 * measured really was the thread.
	 */
	if (ec->enable_notime && ec->notime_timer)
		w->ticked++;
}

static void ut_work_notime(struct ut_worker *w)
{
	unsigned int round;

	if (!ut_notime_arm(w->idx)) {
		/*
		 * The platform-clock arm: one collector, built before the run,
		 * read from until the other arm is finished. A fixed round
		 * count would not do - a read costs a fraction of building a
		 * counting-thread collector, so this arm would be long done
		 * before the first counting thread existed.
		 */
		for (round = 0; round < UT_TIMER_MAX_ROUNDS && w->ec; round++) {
			if (round >= UT_ROUNDS && !ut_notime_pending())
				break;

			ut_notime_read(w, w->ec);
			ut_check_stateless(w);
		}

		return;
	}

	/*
	 * The counting-thread arm, building its collector here because that is
	 * the interesting part: it starts a counting thread, runs the startup
	 * through it and joins it, on every thread of the arm at once.
	 */
	for (round = 0; round < UT_NOTIME_ROUNDS; round++) {
		struct rand_data *ec =
			jent_entropy_collector_alloc(w->osr, w->flags);

		if (!ec)
			continue;
		w->allocs++;

		ut_notime_read(w, ec);
		ut_check_stateless(w);

		jent_entropy_collector_free(ec);
	}

	/* Releases the other arm, which reads until every one of these is set. */
	jent_atomic_store_int(&ut_notime_done[w->idx], 1);
}

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

#ifdef UT_WIN_THREADS

static DWORD WINAPI ut_thread_main(LPVOID arg)
{
	struct ut_worker *w = (struct ut_worker *)arg;

	ut_gate_wait(w);
	w->work(w);

	return 0;
}

#else /* UT_WIN_THREADS */

static void *ut_thread_main(void *arg)
{
	struct ut_worker *w = (struct ut_worker *)arg;

	ut_gate_wait(w);
	w->work(w);

	return NULL;
}

#endif /* UT_WIN_THREADS */

/* As many as the machine can run at once, within the bounds stated above. */
static unsigned int ut_threads(void)
{
	long ncpu = jent_ncpu();

	if (ncpu < UT_MIN_THREADS)
		return UT_MIN_THREADS;
	if (ncpu > UT_MAX_THREADS)
		return UT_MAX_THREADS;

	return (unsigned int)ncpu;
}

/*
 * Start them all, release them together, and only then join: a run that
 * started and joined one thread at a time would be a sequential test with
 * extra steps, and one that let them start where they were created would race
 * only on a machine slow enough to create them slowly.
 *
 * Returns the number of threads that ran, which is what the checks are made
 * over - a machine that refuses a thread is not a defect in the library, and
 * the ones that did start still say something.
 */
static unsigned int ut_run(struct ut_worker *workers, unsigned int nthreads)
{
#ifdef UT_WIN_THREADS
	HANDLE handles[UT_MAX_THREADS];
#else
	pthread_t handles[UT_MAX_THREADS];
#endif
	unsigned int started = 0, i;

	ut_gate_reset();

	for (i = 0; i < nthreads; i++) {
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

	ut_gate_open(started);

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

/*
 * One configuration per thread, so that the collectors being built at the same
 * time differ in the state the library derives per instance - memory size and
 * hash loop count - rather than all taking the same path through the
 * allocator. Threads past the end of the table take it from the top again.
 */
static unsigned int ut_flags(unsigned int idx)
{
	static const unsigned int flags[] = {
		0,
		JENT_DISABLE_MEMORY_ACCESS,
		/*
		 * A hash loop above the default, and only just above it: the
		 * setting is a multiplier on the conditioning of every single
		 * time delta, so JENT_HASHLOOP_32 would spend most of this
		 * program's run in one thread's measurements - two minutes of
		 * it under a sanitizer - and what is wanted here is only that
		 * the collectors differ in the state the library derives per
		 * instance.
		 */
		JENT_HASHLOOP_4,
		JENT_MAX_MEMSIZE_64kB,
	};

	return flags[idx % JENT_ARRAY_SIZE(flags)];
}

static void ut_init_workers(struct ut_worker *workers, unsigned int nthreads,
			    void (*work)(struct ut_worker *),
			    unsigned int (*flags)(unsigned int))
{
	unsigned int i;

	memset(workers, 0, sizeof(*workers) * nthreads);
	for (i = 0; i < nthreads; i++) {
		workers[i].idx = i;
		workers[i].work = work;
		workers[i].flags = flags(i);
	}
}

static void test_concurrent_lifecycle(void)
{
	struct ut_worker workers[UT_MAX_THREADS];
	unsigned int nthreads = ut_threads();
	unsigned int started, i, j, allocs = 0, reads = 0;
	unsigned int read_errs = 0, status_errs = 0, init_differed = 0;
	unsigned int misc_errs = 0;

	jent_ut_group("several instances through their life cycle at once");

	ut_init_workers(workers, nthreads, ut_work_lifecycle, ut_flags);

	started = ut_run(workers, nthreads);
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
		misc_errs += workers[i].misc_err ? 1u : 0u;

		if (workers[i].read_err)
			printf("  note: thread %u: a read returned %d\n", i,
			       workers[i].read_err);
	}

	JENT_UT_EQ(init_differed, 0,
		   "and the same one on every call it makes");
	JENT_UT_EQ(read_errs, 0u, "no thread was refused a generation");
	JENT_UT_EQ(status_errs, 0u,
		   "the status and UUID calls succeed alongside the others");
	JENT_UT_EQ(misc_errs, 0u,
		   "the process-wide queries answer the same on every thread");

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
	 * The conditioning is one implementation shared by every instance, so
	 * its known answer tests cannot come out differently on two of them.
	 */
	for (i = 1; i < started; i++) {
		JENT_UT_EQ(workers[i].selftest_ret, workers[0].selftest_ret,
			   "the self test agrees across the instances");
	}

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

/*
 * The compliance mode is what closes the registration, so the allocating
 * threads have to ask for it. The registering threads carry it too and never
 * use it, which costs nothing and keeps the table in step with the indices.
 */
static unsigned int ut_fips_flags(unsigned int idx)
{
	(void)idx;

	return JENT_FORCE_FIPS;
}

static void test_concurrent_registrations(void)
{
	struct ut_worker workers[UT_MAX_THREADS];
	unsigned int nthreads = ut_threads();
	unsigned int started, i, allocs = 0, registrations = 0;
	unsigned int blocked = 0, reopened = 0, misc_errs = 0;

	jent_ut_group("the process-wide registrations against the collectors");

	ut_init_workers(workers, nthreads, ut_work_registrations,
			ut_fips_flags);

	started = ut_run(workers, nthreads);
	if (!started) {
		JENT_UT_SKIP("the concurrent registration",
			     "no thread could be created");
		return;
	}

	for (i = 0; i < started; i++) {
		allocs += (unsigned int)workers[i].allocs;
		registrations += (unsigned int)workers[i].registrations;
		blocked += (unsigned int)workers[i].reg_blocked;
		reopened += (unsigned int)workers[i].reg_reopened;
		misc_errs += workers[i].misc_err ? 1u : 0u;
	}

	printf("  note: %u threads, %u registrations (%u refused), "
	       "%u collectors\n", started, registrations, blocked, allocs);

	JENT_UT_EQ(misc_errs, 0u,
		   "the registration answers only 0 or -EAGAIN");

	/*
	 * The one that matters: once the registration has been closed it stays
	 * closed. A thread that was refused and then accepted would have
	 * replaced the callback under a collector already generating in FIPS
	 * mode.
	 */
	JENT_UT_EQ(reopened, 0u,
		   "no registration is accepted after one was refused");

	if (!allocs) {
		/*
		 * A machine that grants no locked memory builds no
		 * compliance-mode collector, so nothing ever closed the
		 * registration and there was nothing to race for.
		 */
		JENT_UT_SKIP("the closing of the registration",
			     "no compliance-mode collector could be built "
			     "on this machine");
		return;
	}

	JENT_UT_TRUE(registrations > 0,
		     "the registering threads ran alongside them");
}


#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/* The clock each arm asks for, the counting-thread one explicitly. */
static unsigned int ut_notime_flags(unsigned int idx)
{
	return ut_notime_arm(idx) ? JENT_FORCE_INTERNAL_TIMER : 0;
}

static void test_concurrent_notime(void)
{
	struct ut_worker workers[UT_MAX_THREADS];
	unsigned int nthreads = ut_threads();
	unsigned int started, i, j;
	unsigned int notime_threads = 0, notime_used = 0, ticked = 0;
	unsigned int allocs = 0, reads = 0, timer_reads = 0;
	unsigned int read_errs = 0, status_errs = 0, misc_errs = 0;
	unsigned int crossed = 0, osr_low = 0, wrong_divisor = 0;
	uint64_t divisor[2];
	struct rand_data *probe;

	jent_ut_group("the counting thread against the platform clock");

	if (jent_notime_forced()) {
		/*
		 * The platform clock did not pass the startup, so every
		 * collector here is on the counting thread already and there
		 * is no second arm. The configuration the timer exists for.
		 */
		JENT_UT_SKIP("the two clocks against each other",
			     "this machine has no usable clock of its own");
		return;
	}

	ut_init_workers(workers, nthreads, ut_work_notime, ut_notime_flags);
	for (i = 0; i < nthreads; i++)
		workers[i].osr = ut_notime_arm(i) ? UT_OSR_NOTIME :
						    UT_OSR_TIMER;

	/*
	 * The platform-clock collectors, built here and not in their threads:
	 * the forcing below is one-way and process-wide.
	 */
	for (i = 0; i < nthreads; i++) {
		if (ut_notime_arm(i))
			continue;

		workers[i].ec = jent_entropy_collector_alloc(workers[i].osr,
							     workers[i].flags);
		if (!workers[i].ec)
			continue;

		workers[i].allocs++;
		JENT_UT_EQ(workers[i].ec->enable_notime, 0,
			   "a collector built first is on the platform clock");
	}

	/*
	 * The probe that says whether this machine can build one at all - the
	 * counting thread needs a CPU of its own - so that an arm building
	 * none does not leave the test passing on nothing. It also forces.
	 */
	probe = jent_entropy_collector_alloc(UT_OSR_NOTIME,
					     JENT_FORCE_INTERNAL_TIMER);
	if (probe) {
		JENT_UT_EQ(probe->enable_notime, 1,
			   "a collector asking for the internal timer gets it");
		jent_entropy_collector_free(probe);
	}

	/* And the property the arms are built around. */
	for (i = 0; i < nthreads; i++) {
		if (!ut_notime_arm(i) && workers[i].ec)
			JENT_UT_EQ(workers[i].ec->enable_notime, 0,
				   "and does not move one already built");
	}

	if (!probe) {
		for (i = 0; i < nthreads; i++)
			jent_entropy_collector_free(workers[i].ec);

		JENT_UT_SKIP("the counting-thread arm",
			     "no collector with an internal timer can be "
			     "built on this machine");
		return;
	}

	ut_notime_done_reset(nthreads);
	started = ut_run(workers, nthreads);

	for (i = 0; i < nthreads; i++)
		jent_entropy_collector_free(workers[i].ec);

	if (!started) {
		JENT_UT_SKIP("the two clocks against each other",
			     "no thread could be created");
		return;
	}

	/* What each clock established, substituting one as the library does. */
	for (i = 0; i < 2; i++) {
		if (jent_gcd_get(&divisor[i], i))
			divisor[i] = 1;
	}

	/* Counted over the threads that ran, not the ones asked for. */
	for (i = 0; i < started; i++) {
		allocs += (unsigned int)workers[i].allocs;
		reads += (unsigned int)workers[i].reads;
		notime_used += (unsigned int)workers[i].notime_used;
		ticked += (unsigned int)workers[i].ticked;
		crossed += (unsigned int)workers[i].notime_crossed;
		read_errs += workers[i].read_err ? 1u : 0u;
		status_errs += workers[i].status_err ? 1u : 0u;
		misc_errs += workers[i].misc_err ? 1u : 0u;

		if (ut_notime_arm(i))
			notime_threads++;
		else
			timer_reads += (unsigned int)workers[i].reads;

		/* Never below what was asked for - a health failure raises it. */
		if (workers[i].reads && workers[i].osr_seen < workers[i].osr)
			osr_low++;

		/*
		 * And its deltas by the divisor of the clock it read: handed
		 * the other one's, it divides the jitter away rather than
		 * measuring conservatively.
		 */
		if (workers[i].reads &&
		    workers[i].divisor != divisor[ut_notime_arm(i)])
			wrong_divisor++;

		if (workers[i].read_err)
			printf("  note: thread %u: a read returned %d\n", i,
			       workers[i].read_err);
	}

	printf("  note: %u threads, %u with a counting thread at OSR %u, "
	       "%u on the platform clock at OSR %u\n", started, notime_threads,
	       UT_OSR_NOTIME, started - notime_threads, UT_OSR_TIMER);
	printf("  note: %u collectors, %u generations (%u on the platform "
	       "clock)\n", allocs, reads, timer_reads);
	printf("  note: common timer divisor %llu for the platform clock, "
	       "%llu for the counting thread\n",
	       (unsigned long long)divisor[0], (unsigned long long)divisor[1]);

	JENT_UT_TRUE(notime_threads > 1,
		     "several collectors drive a counting thread at once");
	JENT_UT_EQ(allocs, started - notime_threads +
			   notime_threads * UT_NOTIME_ROUNDS,
		   "every collector of both arms was built");
	JENT_UT_EQ(read_errs, 0u, "no generation was refused on either clock");
	JENT_UT_EQ(status_errs, 0u,
		   "every collector reported an identity of its own");
	JENT_UT_EQ(misc_errs, 0u,
		   "the process-wide queries answer the same on every thread");

	/* The one that matters, whatever the other threads are doing. */
	JENT_UT_EQ(crossed, 0u,
		   "every collector ran on the clock it was built with");
	JENT_UT_EQ(notime_used, notime_threads * UT_NOTIME_ROUNDS,
		   "the counting-thread arm used a counting thread throughout");
	JENT_UT_EQ(ticked, notime_used,
		   "and every one of those threads was seen to count");
	JENT_UT_TRUE(timer_reads > 0,
		     "the platform-clock arm generated alongside it");
	JENT_UT_EQ(osr_low, 0u,
		   "no instance generated below the OSR it was built for");
	JENT_UT_EQ(wrong_divisor, 0u,
		   "and every one divided its deltas by its own clock's "
		   "divisor");

	/* And the instances stayed apart, across the two clocks as within. */
	for (i = 0; i < started; i++) {
		if (!workers[i].reads)
			continue;

		for (j = i + 1; j < started; j++) {
			if (!workers[j].reads)
				continue;

			JENT_UT_TRUE(memcmp(workers[i].block, workers[j].block,
					    UT_BLOCK) != 0,
				     "two threads generate different blocks");
			JENT_UT_TRUE(strcmp(workers[i].uuid,
					    workers[j].uuid) != 0,
				     "two collectors carry different UUIDs");
		}
	}
}

#else /* JENT_CONF_ENABLE_INTERNAL_TIMER */

static void test_concurrent_notime(void)
{
	jent_ut_group("the counting thread against the platform clock");
	JENT_UT_SKIP("the counting thread", "not compiled in");
}

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

int main(void)
{
	jent_ut_setup();

	/*
	 * The registrations first, and the order is the test rather than a
	 * matter of taste: every jent_entropy_init_ex() closes the callback
	 * registration for the life of the process, and the life cycle test
	 * calls it on every thread. Run afterwards, every attempt would be
	 * refused before the first thread reached it - which asserts the
	 * one-way property and races nobody for it.
	 */
	test_concurrent_registrations();
	test_concurrent_lifecycle();

	/*
	 * And the internal timer last, for the same kind of reason: the first
	 * collector that asks for it forces it for the life of the process,
	 * and every collector the tests above build would then drive a
	 * counting thread instead of reading the platform clock.
	 */
	test_concurrent_notime();

	return jent_ut_report("unit-concurrency");
}
