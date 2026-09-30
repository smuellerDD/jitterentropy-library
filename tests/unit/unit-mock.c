/*
 * Jitter RNG: unit tests for the mocked time source and for feeding external
 * time stamps to the health tests
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
 * Two ways of judging time stamps the library did not measure, and the tests
 * for both.
 *
 * jent_health_insert_timestamp() runs the health tests over stamps handed to
 * it - what a raw entropy recording is replayed through, the same code that
 * judges the noise source at runtime.
 *
 * jent_set_mock_timer() goes further and replaces the time source for the
 * whole library, so the startup self test and the noise source run on the
 * supplied stamps. It is internal and only compiled into a build that asked
 * for it, so this program defines JENT_CONF_ENABLE_MOCK_TIMER for its own copy
 * of the sources rather than depending on how the library was built.
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
 * A stamp source reading from an array, which is what replaying a recording
 * amounts to. The library decides how many stamps it wants, so a recording can
 * run out; past the end this keeps the clock moving by a varying step rather
 * than repeating the last value.
 *
 * That is not a detail: a clock that stops moving makes every measurement
 * stuck, and the generation loop in jent_random_data_one() only leaves on a
 * health test failure or a non-stuck measurement - outside FIPS mode the
 * health tests do not report, so it would spin forever.
 */
struct fi_replay {
	const uint64_t *stamps;
	size_t count;
	size_t pos;
	size_t served;
	size_t past_end;
	uint64_t tail;
	unsigned int step;
	/*
	 * Past the end of the recording, hold the last value instead of
	 * ticking on. Only safe where the caller is bounded - the startup self
	 * test is, jent_read_entropy() is not.
	 */
	int hold;
	/*
	 * Emit a constant stamp until this many have been served, so that a
	 * caller can be made to fail its health tests and then recover.
	 */
	size_t bad_until;
	/*
	 * What the steps of the ticker past the end and of FI_SHAPE_STEADY
	 * are multiplied by: the common timer divisor, where the recording
	 * was built in multiples of it - see fi_plausible_clock(). One
	 * otherwise.
	 */
	uint64_t scale;

	/*
	 * Shapes that need every reading controlled, not just a recording
	 * played back.
	 */
	enum {
		FI_SHAPE_NONE = 0,
		FI_SHAPE_DESCEND,	/* every reading below the last */
		FI_SHAPE_MOSTLY_STUCK,	/* long constant runs, briefly broken */
		FI_SHAPE_STEADY,	/* a fixed step: every delta the same */
	} shape;
};

static void fi_replay_cb(void *arg, uint64_t *out)
{
	struct fi_replay *r = arg;

	r->served++;

	if (r->served <= r->bad_until) {
		/* A clock that does not move: every measurement is stuck. */
		*out = 0x5a5a5a5a5a5a5a5aULL;
		return;
	}

	switch (r->shape) {
	case FI_SHAPE_DESCEND:
		/*
		 * A clock that runs down, by a different amount each time.
		 * Every measurement ends below where it started, which is what
		 * the monotonicity check counts; the deltas still vary, so
		 * nothing is stuck and the repetition count test stays quiet
		 * and lets the monotonicity verdict be the one that lands.
		 */
		r->step = (r->step * 1103515245u + 12345u);
		r->tail -= 4096 + (r->step >> 24);
		*out = r->tail;
		return;
	case FI_SHAPE_MOSTLY_STUCK:
		/*
		 * Long runs of a constant reading, broken often enough that
		 * the repetition count test never reaches its cutoff. What is
		 * left for the startup test to notice is the proportion of
		 * stuck measurements rather than a run of them.
		 */
		r->step++;
		if (r->step % 4096 == 0)
			r->tail += 1 + (r->step % 251);
		*out = r->tail;
		return;
	case FI_SHAPE_STEADY:
		r->tail += 1000 * r->scale;
		*out = r->tail;
		return;
	case FI_SHAPE_NONE:
	default:
		break;
	}

	if (r->pos < r->count) {
		*out = r->stamps[r->pos++];
		r->tail = *out;
		return;
	}

	r->past_end++;

	if (r->hold) {
		*out = r->tail;
		return;
	}

	r->step = (r->step * 1103515245u + 12345u);
	r->tail += (500 + (r->step >> 22)) * r->scale;
	*out = r->tail;
}

static void fi_replay_init(struct fi_replay *r, const uint64_t *stamps,
			   size_t count)
{
	r->stamps = stamps;
	r->count = count;
	r->pos = 0;
	r->served = 0;
	r->past_end = 0;
	r->tail = 0;
	r->step = 7;
	r->hold = 0;
	r->bad_until = 0;
	r->scale = 1;
	r->shape = FI_SHAPE_NONE;
}

/*
 * A plausible clock: monotonic with varying steps, recorded into @seq in
 * multiples of the divisor the collectors built on it will use. Returns that
 * divisor, for fi_replay.scale.
 *
 * The collectors these recordings are replayed into are allocated after a
 * startup has run on the machine's own clock, and the divisor that startup
 * measured is kept for the process (jent_gcd_store()): every collector divides
 * its deltas by it, whatever clock it then reads. Steps of 500 to 1500 on a
 * coarse clock - the emulated arm64 counter under qemu advances by 1000 -
 * divide down to 0 and 1, and the recording becomes a run of stuck
 * measurements that the health tests rightly refuse. test_timestamp_replay()
 * scales its stamps for the same reason.
 */
static uint64_t fi_platform_divisor(void)
{
	uint64_t gcd;

	/* Not established yet: the startup then measures this very clock. */
	if (jent_gcd_get(&gcd, 0) || !gcd)
		return 1;
	return gcd;
}

static uint64_t fi_plausible_clock(uint64_t *seq, size_t n)
{
	unsigned int step = 7;
	uint64_t gcd = fi_platform_divisor(), t = 1;
	size_t i;

	for (i = 0; i < n; i++) {
		step = (step * 1103515245u + 12345u);
		t += (500 + (step >> 22)) * gcd;
		seq[i] = t;
	}

	return gcd;
}

/*
 * A collector allocated with @flags on a constructed clock - fi_plausible_clock()
 * or the ticker past a recording's end - that did not come. Nothing about that
 * clock is the machine's: its stamps are in the units of the divisor, so the
 * deltas the startup sees are the same everywhere, and a startup that rejects
 * them is a defect. What is left of the machine is memory to lock, where the
 * flags or the system's FIPS mode force it.
 */
#define FI_NO_CONSTRUCTED_COLLECTOR(what, flags)			       \
	do {								       \
		if (!jent_ut_strict() && JENT_UT_SECURE_MEM_FORCED(flags) &&   \
		    !jent_ut_memlock_available())			       \
			JENT_UT_SKIP(what, "no memory can be locked on this "  \
				     "machine");			       \
		else							       \
			JENT_UT_FAIL("%s: no collector on the constructed "    \
				     "clock", what);			       \
	} while (0)

/* The registration itself, and what the status output says about it. */
static void test_registration(void)
{
	struct fi_replay r;
	char buf[8192];
	struct rand_data *ec;
	int ret;

	jent_ut_group("registering a mocked time source");

	fi_replay_init(&r, NULL, 0);

	JENT_UT_EQ(jent_mock_timer_active(), 0,
		   "nothing is registered to begin with");

	JENT_UT_EQ(jent_set_mock_timer(fi_replay_cb, &r), 0,
		   "a callback is accepted");
	JENT_UT_EQ(jent_mock_timer_active(), 1, "and reported as registered");

	/* It is the one the library reads through. */
	{
		uint64_t stamps[] = { 11, 22, 33 };
		uint64_t out = 0;

		fi_replay_init(&r, stamps, 3);
		jent_get_nstime(&out);
		JENT_UT_EQ(out, 11, "the first stamp comes from the callback");
		jent_get_nstime(&out);
		JENT_UT_EQ(out, 22, "and so does the next");
	}

	JENT_UT_EQ(jent_set_mock_timer(NULL, NULL), 0,
		   "it can be taken back out");
	JENT_UT_EQ(jent_mock_timer_active(), 0, "and is then not registered");

	/* The platform timer is back. */
	{
		uint64_t a = 0, b = 0;

		jent_get_nstime(&a);
		jent_get_nstime(&b);
		JENT_UT_NE(a, 0, "the platform timer reads again");
		JENT_UT_TRUE(a != 11 && b != 22, "and is not the callback");
	}

	/*
	 * The build carries the mock, which the status output has to say
	 * whether or not one is registered at the moment it is taken.
	 */
	ret = jent_entropy_init();
	if (ret) {
		JENT_UT_NO_STARTUP("the status report", ret);
		return;
	}
	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the status report", 0);
		return;
	}
	JENT_UT_EQ(jent_status(ec, buf, sizeof(buf)), 0, "the status renders");
	JENT_UT_TRUE(strstr(buf, "\"mockedTimerBuild\": true") != NULL,
		     "and reports that this build carries the mock");
	JENT_UT_TRUE(strstr(buf, "\"mockedTimerActive\": false") != NULL,
		     "and that none is registered right now");
	jent_entropy_collector_free(ec);
}

/*
 * Whether the startup rejects this machine's own clock when it may not fall
 * back to the internal timer - a clock too coarse for it, say. That is the
 * machine's verdict, and a case that needs the platform clock to pass skips
 * (it says so) rather than fail. A startup that fails for another reason is
 * not the machine's and does not excuse the case. Asked with no mock
 * installed.
 */
static int platform_clock_rejected(const char *what)
{
	int ret = jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER);

	if (ret && JENT_UT_MACHINE_VERDICT(ret)) {
		JENT_UT_SKIP(what, "the startup rejects the platform clock");
		return 1;
	}
	return 0;
}

/*
 * The startup self test on constructed clocks. These are the verdicts a
 * machine whose timer is unusable would get, and no such machine is available
 * to run the suite on.
 */
static void test_startup_on_mocked_clocks(void)
{
	static uint64_t zero[] = { 0 };
	static uint64_t constant[] = { 0x4242424242424242ULL };
	struct fi_replay r;
	int ret;

	jent_ut_group("the startup self test on constructed clocks");

	/*
	 * Held clocks: the startup self test reads a bounded number of stamps
	 * and leaves on the first bad one, so holding is safe here.
	 */
	fi_replay_init(&r, zero, 1);
	r.hold = 1;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_time_entropy_init(JENT_MIN_OSR, JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);
	JENT_UT_EQ(ret, ENOTIME, "a clock stuck at zero is rejected");
	JENT_UT_NE(r.served, 0, "and the callback was what it read");

	fi_replay_init(&r, constant, 1);
	r.hold = 1;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_time_entropy_init(JENT_MIN_OSR, JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);
	JENT_UT_EQ(ret, ECOARSETIME, "a clock that does not move is rejected");

	/*
	 * A clock that runs backwards, by a varying amount so that nothing is
	 * stuck: the deltas differ, the repetition count test stays quiet, and
	 * the monotonicity check is left to be the one that speaks. It is the
	 * case that check exists for, and the one it could not see while
	 * start_time was reconstructed as prev_time - delta (see
	 * jent_time_entropy_init()).
	 */
	fi_replay_init(&r, NULL, 0);
	r.shape = FI_SHAPE_DESCEND;
	r.tail = 0x8000000000000000ULL;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_time_entropy_init(JENT_MIN_OSR, JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);
	JENT_UT_EQ(ret, ENOMONOTONIC, "a clock that runs backwards is rejected");

	/*
	 * And one that is stuck most of the time. A run of identical readings
	 * makes the delta zero, which the coarseness check catches before the
	 * proportion of stuck measurements is ever counted - so ESTUCK is not
	 * what comes back, and the rejection is what is asserted.
	 */
	fi_replay_init(&r, NULL, 0);
	r.shape = FI_SHAPE_MOSTLY_STUCK;
	r.tail = 0x1000;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_time_entropy_init(JENT_MIN_OSR, JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);
	printf("  note: a mostly-stuck clock gives %d\n", ret);
	JENT_UT_NE(ret, 0, "a mostly-stuck clock is rejected");

	/* And the real one still passes once the mock is out of the way. */
	if (platform_clock_rejected("the platform clock passing again"))
		return;
	JENT_UT_EQ(jent_time_entropy_init(JENT_MIN_OSR,
					  JENT_DISABLE_INTERNAL_TIMER), 0,
		   "the platform clock passes again");
}

/*
 * The startup measures with a collector of its own, in FIPS mode for the
 * health tests alone. The FIPS failure callback is the caller's, about the
 * caller's instances, and is not invoked for that one - in no mode.
 */
static unsigned int startup_cb_calls;
static int startup_cb_registered;

static void startup_cb(struct rand_data *ec, unsigned int health_failure)
{
	(void)ec;
	(void)health_failure;
	startup_cb_calls++;
}

static void test_startup_failure_no_callback(void)
{
	struct fi_replay r;
	int ret;

	jent_ut_group("a startup health failure does not invoke the callback");

	if (!startup_cb_registered) {
		JENT_UT_SKIP("the startup callback", "not registered");
		return;
	}

	/* Every delta the same: stuck throughout, so the RCT trips. */
	fi_replay_init(&r, NULL, 0);
	r.shape = FI_SHAPE_STEADY;
	/*
	 * A step the stored divisor leaves whole: divided down to zero it
	 * would be a clock that does not move, which fails differently.
	 */
	r.scale = fi_platform_divisor();
	startup_cb_calls = 0;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_time_entropy_init(JENT_MIN_OSR, JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);
	JENT_UT_EQ(ret, ERCT, "a clock with a fixed step fails the RCT");
	JENT_UT_EQ(startup_cb_calls, 0, "and the callback is not invoked");
}

/* A collector for replaying stamps into: FIPS mode, so the tests report. */
/*
 * A startup that fails under NTG.1 must leave later NTG.1 initializations
 * usable: the tighter NTG.1 cutoffs make an occasional startup health failure
 * normal.
 */
static void test_ntg1_failure_does_not_force_notime(void)
{
	static uint64_t constant[] = { 0x4242424242424242ULL };
	struct fi_replay r;
	int ret;

	jent_ut_group("a failed NTG.1 startup and the internal timer");

	/* A clock that does not move, so the startup has to reject it. */
	fi_replay_init(&r, constant, 1);
	r.hold = 1;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_entropy_init_ex(0, JENT_NTG1);
	jent_set_mock_timer(NULL, NULL);

	JENT_UT_NE(ret, 0, "the startup rejects a clock that does not move");

	/* The contradiction stated outright is refused. */
	JENT_UT_EQ(jent_entropy_init_ex(0, JENT_NTG1 |
					   JENT_FORCE_INTERNAL_TIMER),
		   ENOTIME,
		   "NTG.1 with the internal timer is refused as a contradiction");

	/*
	 * And the platform clock still initialises under NTG.1 afterwards -
	 * where memory can be locked at all, which NTG.1 demands.
	 */
	if (!jent_ut_strict() && !jent_ut_memlock_available()) {
		JENT_UT_SKIP("a later NTG.1 startup",
			     "this machine locks no memory (RLIMIT_MEMLOCK)");
		return;
	}
	/* NTG.1 is on the platform clock only, so it has to pass. */
	if (platform_clock_rejected("a later NTG.1 startup"))
		return;
	/*
	 * Passes, not merely "is not EMEM": the regression this names showed
	 * as EMEM once and as ENOTIME after the allocation learned to refuse
	 * the combination, and a check for one code passes the other. A few
	 * attempts, as the NTG.1 cutoffs let an honest clock fail once in a
	 * while - a startup that is refused for good fails all of them.
	 */
	{
		struct rand_data *ec;
		unsigned int i;

		for (i = 0; i < 3; i++) {
			ret = jent_entropy_init_ex(0, JENT_NTG1);
			if (!ret)
				break;
		}
		/*
		 * A clock that keeps failing the NTG.1 health tests - not the
		 * EMEM or ENOTIME the regression showed as - is the machine.
		 */
		if (!jent_ut_strict() &&
		    (ret == EHEALTH || ret == ERCT || ret == ESTUCK)) {
			JENT_UT_SKIP("a later NTG.1 startup",
				     "the NTG.1 health tests do not pass on "
				     "this machine");
			return;
		}
		JENT_UT_EQ(ret, 0, "a later NTG.1 startup passes");

		/*
		 * Where the regression showed: the collector allocation. It
		 * runs an NTG.1 startup of its own, which a coarse clock - the
		 * 24 MHz counter of the macOS arm64 runners - fails once in a
		 * while just the same, so it gets the same attempts.
		 */
		ec = NULL;
		for (i = 0; i < 3 && !ec; i++)
			ec = jent_entropy_collector_alloc(0, JENT_NTG1);
		JENT_UT_TRUE(ec != NULL,
			     "and an NTG.1 collector can be allocated");
		if (ec) {
			JENT_UT_EQ(ec->enable_notime, 0,
				   "on the platform clock");
			jent_entropy_collector_free(ec);
		}
	}
}

static struct rand_data *replay_collector(uint64_t first)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);

	if (!ec)
		return NULL;

	/*
	 * Prime the reference the first delta is formed against. A collector
	 * fresh from jent_entropy_collector_alloc() has whatever its own
	 * startup left there, so a replay that did not do this would begin
	 * with one delta between the machine's clock and the recording -
	 * meaningless, and different on every run.
	 */
	ec->prev_time = first;

	/*
	 * The same holds for the health tests, and for the same reason: the
	 * startup ran them over the machine's own measurements and left every
	 * counter part-way through a window. The APT window is 512 wide, so a
	 * collector whose startup ended at observation 500 resets it 12 stamps
	 * into the replay and one that ended at 70 resets it 442 stamps in -
	 * different verdicts on identical input. A replay therefore has to
	 * start from the state a collector has before it has measured
	 * anything, which tests/health gets from a zeroed struct.
	 *
	 * Only the running counters are cleared: the cutoffs, the block size
	 * and rct_mem_nosr are configuration, not accumulated state.
	 */
	ec->health_failure = 0;
	ec->apt_base = 0;
	ec->apt_base_set = 0;
	ec->apt_count = 0;
	ec->apt_observations = 0;
	ec->rct_mem_ctr = 0;
	ec->rct_mem_count = 0;
#ifdef JENT_HEALTH_LAG_PREDICTOR
	jent_lag_reset(ec);
	memset(ec->lag_delta_history, 0, sizeof(ec->lag_delta_history));
#else
	ec->last_delta = 0;
	ec->last_delta2 = 0;
#endif /* JENT_HEALTH_LAG_PREDICTOR */
	jent_health_init(ec, (ec->flags & JENT_NTG1) ?
			     jent_health_init_type_ntg1 :
			     jent_health_init_type_common);

	return ec;
}

/*
 * Replaying stamps through the health tests. Each sequence below is one a
 * recording could contain and that the health tests exist to reject.
 */
static void test_timestamp_replay(void)
{
	struct rand_data *ec;
	unsigned int i, stuck = 0;
	uint64_t gcd;

	jent_ut_group("replaying time stamps through the health tests");

	/*
	 * A constant delta: the second derivative is zero, so every
	 * measurement is stuck and the repetition count test reaches its
	 * cutoff.
	 */
	ec = replay_collector(0);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the replay", JENT_FORCE_FIPS);
		return;
	}

	/*
	 * The replay normalizes each delta by the common timer divisor the
	 * startup measured on this machine's clock, as the noise source does.
	 * The stamps below are multiples of it, so that the deltas they imply
	 * are the ones written here on every clock: under qemu the emulated
	 * arm64 counter advances in steps of 1000, the divisor is 1000, and a
	 * stamp 100 on would otherwise be a delta of 0.
	 */
	gcd = ec->jent_common_timer_gcd ? ec->jent_common_timer_gcd : 1;
	printf("  note: the common timer divisor is %llu\n",
	       (unsigned long long)gcd);

	for (i = 0; i < 4096; i++)
		stuck += jent_health_insert_timestamp(ec,
						      (uint64_t)i * 100 * gcd);

	JENT_UT_NE(stuck, 0, "a constant delta produces stuck measurements");
	JENT_UT_TRUE((jent_health_failure(ec) & JENT_RCT_FAILURE) != 0,
		     "and the repetition count test reports it");
	jent_entropy_collector_free(ec);

	/*
	 * The reference the first delta is formed against is the caller's to
	 * set: inserting the same stamp the collector was primed with is a
	 * delta of zero, which is stuck.
	 */
	ec = replay_collector(12345);
	if (ec) {
		JENT_UT_EQ(jent_health_insert_timestamp(ec, 12345), 1,
			   "a stamp equal to the primed reference is stuck");
		jent_entropy_collector_free(ec);
	} else {
		JENT_UT_NO_COLLECTOR("the primed reference", JENT_FORCE_FIPS);
	}

	/*
	 * A sequence whose deltas vary: nothing is stuck and no test fires.
	 * This is the case that says the replay is not simply failing
	 * everything handed to it.
	 */
	ec = replay_collector(0);
	if (ec) {
		uint64_t t = 0;
		unsigned int step = 1;

		stuck = 0;
		for (i = 0; i < 4096; i++) {
			step = (step * 1103515245u + 12345u);
			t += (1000 + (step >> 20)) * gcd;
			stuck += jent_health_insert_timestamp(ec, t);
		}

		JENT_UT_EQ(jent_health_failure(ec), 0,
			   "a varying sequence raises no health test");
		printf("  note: %u of 4096 varying stamps were stuck\n", stuck);
		jent_entropy_collector_free(ec);
	} else {
		/* The negative control: without it the replay proves little. */
		JENT_UT_NO_COLLECTOR("a varying sequence", JENT_FORCE_FIPS);
	}

	/*
	 * The same verdict the noise source would reach, because it is the
	 * same code: a collector fed the stuck sequence through
	 * jent_health_insert_timestamp() ends in the same state as one fed
	 * the equivalent deltas through jent_stuck().
	 */
	{
		struct rand_data *a = replay_collector(0);
		struct rand_data *b = replay_collector(0);

		if (a && b) {
			for (i = 0; i < 512; i++) {
				jent_health_insert_timestamp(a,
					(uint64_t)i * 100 * gcd);
				/* The delta those stamps imply, primed at 0. */
				jent_stuck(b, i ? 100 : 0);
			}
			JENT_UT_EQ(jent_health_failure(a), jent_health_failure(b),
				   "stamps and the deltas they imply agree");
		} else {
			JENT_UT_NO_COLLECTOR("stamps against deltas",
					     JENT_FORCE_FIPS);
		}
		jent_entropy_collector_free(a);
		jent_entropy_collector_free(b);
	}

	/* And that it does not need a collector the library built. */
	{
		struct rand_data hand;

		memset(&hand, 0, sizeof(hand));
		hand.osr = JENT_MIN_OSR;
		hand.is_fips_enabled = 1;
		jent_health_init(&hand, jent_health_init_type_common);

		for (i = 0; i < 4096; i++)
			jent_health_insert_timestamp(&hand, (uint64_t)i * 100);

		JENT_UT_TRUE((jent_health_failure(&hand) & JENT_RCT_FAILURE) != 0,
			     "a hand-built collector works the same way");
	}
}

/*
 * The whole library on a mocked clock, which is what separates this from
 * feeding the health tests directly: the noise source, the conditioning and
 * the health tests all run on the supplied stamps.
 */
static void test_generation_on_mocked_clock(void)
{
	/*
	 * Long enough that the whole run comes out of the recording. At 4096
	 * it was not: the library asked for some 26000 stamps and 84% of them
	 * came from the fallback ticker behind the replay, so "a block is
	 * produced from the supplied stamps" was mostly about stamps nobody
	 * supplied. r.past_end below is what says so.
	 */
	static uint64_t seq[65536];
	struct fi_replay r;
	struct rand_data *ec;
	char buf[32];
	uint64_t scale;
	ssize_t ret;
	int made;

	jent_ut_group("generating on a mocked clock");

	scale = fi_plausible_clock(seq, JENT_ARRAY_SIZE(seq));
	fi_replay_init(&r, seq, JENT_ARRAY_SIZE(seq));
	r.scale = scale;

	jent_set_mock_timer(fi_replay_cb, &r);
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	made = ec != NULL;
	if (ec) {
		ret = jent_read_entropy(ec, buf, sizeof(buf));
		JENT_UT_EQ(ret, (ssize_t)sizeof(buf),
			   "a block is produced from the supplied stamps");
		jent_entropy_collector_free(ec);
	}
	jent_set_mock_timer(NULL, NULL);

	/*
	 * What the library read is only worth judging behind a collector: one
	 * the startup refused may have read nothing from the recording.
	 */
	if (made) {
		JENT_UT_NE(r.served, 0,
			   "the callback was what the library read");
		printf("  note: the library asked for %zu stamps, %zu past the "
		       "recording\n", r.served, r.past_end);
		JENT_UT_EQ(r.past_end, 0,
			   "and every one of them came out of the recording");
	} else {
		FI_NO_CONSTRUCTED_COLLECTOR("generating on a mocked clock",
					    JENT_DISABLE_INTERNAL_TIMER);
	}

	/*
	 * The same recording replayed twice gives the same bytes: the output
	 * is a function of the stamps alone, which is what makes a replay
	 * reproducible and a recording worth keeping beside a verdict.
	 *
	 * This is what the group was named for and what it did not check. It
	 * asserted JENT_UT_TRUE(1, ...) instead, on the grounds that the
	 * collector mixes its own address and a per-instance UUID into the
	 * pool - it does not reach the output, as the comparison below shows -
	 * and it did so nested inside "if (a && read ok)", so a replay that
	 * produced nothing skipped the check rather than failing it.
	 */
	{
		char first[32], second[32];
		struct rand_data *a, *b;
		ssize_t ra = -1, rb = -1;

		r.pos = 0;
		jent_set_mock_timer(fi_replay_cb, &r);
		a = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
		if (a)
			ra = jent_read_entropy(a, first, sizeof(first));

		r.pos = 0;
		b = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
		if (b)
			rb = jent_read_entropy(b, second, sizeof(second));
		jent_set_mock_timer(NULL, NULL);

		if (!a || !b) {
			FI_NO_CONSTRUCTED_COLLECTOR("two replays of one sequence",
						    JENT_DISABLE_INTERNAL_TIMER);
		} else {
			JENT_UT_EQ(ra, (ssize_t)sizeof(first),
				   "the first replay of the sequence produces a block");
			JENT_UT_EQ(rb, (ssize_t)sizeof(second),
				   "and so does the second");
			JENT_UT_MEM_EQ(first, second, sizeof(first),
				       "the two replays produce the same block");
		}

		jent_entropy_collector_free(a);
		jent_entropy_collector_free(b);
	}
}

/*
 * The reallocation the collector performs when its own startup trips a health
 * test. Only reachable when the measurements taken during startup are bad, so
 * only reachable with a clock that can be made to produce bad ones.
 */
/*
 * The clock is chosen per collector: one whose platform clock does not pass
 * falls back to the internal timer on its own, and that choice is not carried
 * over to the next collector.
 */
static void test_fallback_per_collector(void)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	static uint64_t constant[] = { 0x4242424242424242ULL };
	struct fi_replay r;
	struct rand_data *ec;
	int ret;

	jent_ut_group("the internal timer fallback is per collector");

	ret = jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER);
	if (ret) {
		JENT_UT_NO_STARTUP("the fallback", ret);
		return;
	}

	/*
	 * The verdicts are per clock and an allocation only falls back where
	 * none passed on the platform clock: both are cleared, so the
	 * allocations below run the startups.
	 */
	jent_atomic_store_int(&jent_selftest_run[JENT_CLOCK_PLATFORM], 0);
	jent_atomic_store_int(&jent_selftest_run[JENT_CLOCK_NOTIME], 0);

	/* A clock that does not move. */
	fi_replay_init(&r, constant, 1);
	r.hold = 1;
	jent_set_mock_timer(fi_replay_cb, &r);

	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	JENT_UT_TRUE(ec == NULL,
		     "a collector pinned to a failing platform clock is refused");
	jent_entropy_collector_free(ec);

	ec = jent_entropy_collector_alloc(0, 0);
	jent_set_mock_timer(NULL, NULL);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the fallback", JENT_FORCE_INTERNAL_TIMER);
		return;
	}
	JENT_UT_EQ(ec->enable_notime, 1,
		   "an unpinned one falls back to the internal timer");
	JENT_UT_EQ(jent_startup_passed(JENT_CLOCK_NOTIME), 1,
		   "after a startup passed on the internal timer");
	jent_entropy_collector_free(ec);

	ec = jent_entropy_collector_alloc(0, 0);
	JENT_UT_TRUE(ec != NULL, "a collector is built on the real clock");
	if (ec)
		JENT_UT_EQ(ec->enable_notime, 0,
			   "on the platform clock: the fallback is forgotten");
	jent_entropy_collector_free(ec);
#else
	jent_ut_group("the internal timer fallback is per collector");
	JENT_UT_SKIP("the fallback", "the internal timer is not compiled in");
#endif
}

static void test_realloc_during_startup(void)
{
	struct fi_replay r;
	struct rand_data *ec;
	int ret;

	jent_ut_group("reallocation while the collector is starting up");

	/*
	 * The startup self test has already passed on the real clock, so the
	 * allocations below do not re-run it and go straight to the startup
	 * sequence - which is where the constructed clock is waiting.
	 *
	 * JENT_DISABLE_INTERNAL_TIMER on every one of them: with the counting
	 * thread in use jent_get_nstime_internal() reads that thread's counter
	 * and never calls the time source, so the mock would not be in the
	 * path.
	 */
	ret = jent_entropy_init();
	if (ret) {
		JENT_UT_NO_STARTUP("reallocation", ret);
		return;
	}

	/*
	 * A clock that never moves, for good. Every measurement is stuck, the
	 * startup sequence trips the repetition count test and the oversampling
	 * rate climbs until it passes JENT_MAX_OSR. The allocation then has to
	 * give up rather than loop.
	 */
	fi_replay_init(&r, NULL, 0);
	r.bad_until = (size_t)-1;
	jent_set_mock_timer(fi_replay_cb, &r);
	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS |
					     JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);

	JENT_UT_TRUE(ec == NULL,
		     "a clock that stays bad makes the allocation give up");
	jent_entropy_collector_free(ec);
	printf("  note: gave up after %zu stamps\n", r.served);

	/*
	 * And the case where the clock recovers: the startup sequence trips a
	 * health test, the collector is reallocated, and the retry succeeds.
	 * That is the path an expected false positive at startup takes. The
	 * clock stays bad long enough that the reallocation has to raise the
	 * oversampling rate several times before its self test passes, which
	 * is the retry loop inside jent_health_failure_reset().
	 */
	/*
	 * The give-up above left the self test marked as not passed, so let it
	 * pass again on the real clock. Otherwise the allocation below re-runs
	 * it against the constructed clock and never reaches the startup
	 * sequence this case is about.
	 */
	ret = jent_entropy_init();
	if (ret) {
		JENT_UT_NO_STARTUP("a recovered startup", ret);
		return;
	}

	fi_replay_init(&r, NULL, 0);
	r.bad_until = 400;
	/* The recovery is the ticker's, in the divisor's units as well. */
	r.scale = fi_platform_divisor();
	jent_set_mock_timer(fi_replay_cb, &r);
	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS |
					     JENT_DISABLE_INTERNAL_TIMER);
	jent_set_mock_timer(NULL, NULL);

	if (ec) {
		JENT_UT_TRUE(ec->osr > JENT_MIN_OSR,
			     "a recovered startup raises the oversampling rate");
		JENT_UT_NE(ec->reinit_count, 0, "and counts the reallocation");
		printf("  note: recovered at osr %u after %u reallocation(s)\n",
		       ec->osr, ec->reinit_count);
		jent_entropy_collector_free(ec);
	} else {
		FI_NO_CONSTRUCTED_COLLECTOR("a recovered startup",
					    JENT_FORCE_FIPS |
					    JENT_DISABLE_INTERNAL_TIMER);
	}
}

/*
 * The reallocation jent_read_entropy_safe() performs, driven to the point
 * where it gives up: the replacement collector has to pass the startup self
 * test, and a clock that has gone bad means it never will.
 */
static void test_realloc_on_read_gives_up(void)
{
	struct fi_replay r;
	struct rand_data *ec;
	char buf[32];
	ssize_t ret;
	unsigned int osr_before;

	jent_ut_group("reallocation on read against a clock that has gone bad");

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS |
					     JENT_DISABLE_INTERNAL_TIMER);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("reallocation on read",
				     JENT_FORCE_FIPS |
				     JENT_DISABLE_INTERNAL_TIMER);
		return;
	}
	osr_before = ec->osr;

	/*
	 * An intermittent failure to recover from, and a clock that will not
	 * let the replacement pass its startup self test. Every attempt raises
	 * the oversampling rate; once it passes JENT_MAX_OSR the failure is
	 * returned to the caller and the collector is left as it was.
	 */
	ec->health_failure = JENT_RCT_FAILURE;

	fi_replay_init(&r, NULL, 0);
	r.bad_until = (size_t)-1;
	jent_set_mock_timer(fi_replay_cb, &r);
	ret = jent_read_entropy_safe(&ec, buf, sizeof(buf));
	jent_set_mock_timer(NULL, NULL);

	/* As a permanent one: _safe returns no intermittent failure. */
	JENT_UT_EQ(ret, JENT_ERR_RCT_PERMANENT,
		   "the health failure is returned once recovery is exhausted");
	JENT_UT_EQ(ec->osr, osr_before,
		   "and the caller keeps the collector it had");

	jent_entropy_collector_free(ec);
}

/*
 * The status output of a build carrying the mock, swept across every buffer
 * length. It has two fields the ordinary build does not, so the truncation of
 * every write in it is a different set of cut points than the sweep in
 * unit-base covers - and the property to hold is the same: nothing written
 * past the buffer, and no success reported for a document that did not fit.
 */
static void test_status_truncation(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	static struct {
		char buf[8192];
		char guard[64];
	} area;
	size_t full, len;
	unsigned int overflows = 0, misreported = 0;

	jent_ut_group("the status output of a mocked build truncates safely");

	if (!ec) {
		JENT_UT_NO_COLLECTOR("the status sweep", 0);
		return;
	}
	if (jent_status(ec, area.buf, sizeof(area.buf))) {
		JENT_UT_SKIP("the status sweep", "no status to sweep");
		jent_entropy_collector_free(ec);
		return;
	}
	full = strlen(area.buf);

	for (len = 1; len <= full + 1; len++) {
		size_t i;

		memset(&area, 0x5a, sizeof(area));
		/*
		 * The terminator looked for within the length given, not with
		 * strlen(): a success reported for a document left without one
		 * is the very failure counted here, and strlen() would run on
		 * through the fill pattern and past the guard to find a zero.
		 */
		if (!jent_status(ec, area.buf, len)) {
			const char *nul = memchr(area.buf, '\0', len);

			if (!nul || (size_t)(nul - area.buf) != full)
				misreported++;
		}

		for (i = 0; i < sizeof(area.guard); i++) {
			if (area.guard[i] != 0x5a) {
				overflows++;
				break;
			}
		}
		for (i = len; i < sizeof(area.buf); i++) {
			if (area.buf[i] != 0x5a) {
				overflows++;
				break;
			}
		}
	}

	JENT_UT_EQ(overflows, 0, "no length writes outside the buffer");
	JENT_UT_EQ(misreported, 0,
		   "success is never reported for a truncated document");

	/* Nor failure for a complete one: the exact fit is the edge case. */
	memset(&area, 0x5a, sizeof(area));
	JENT_UT_EQ(jent_status(ec, area.buf, full + 1), 0,
		   "a buffer that holds the document exactly is no error");
	JENT_UT_TRUE(memchr(area.buf, '\0', full + 1) == area.buf + full,
		     "and receives all of it, terminated");

	printf("  note: swept %zu buffer lengths\n", full + 1);

	jent_entropy_collector_free(ec);
}

/*
 * A request of several blocks that fails on a later one. The failure is a self
 * test verdict bound while the first block is generated - what a concurrent
 * jent_selftest() does - so that the first block is copied out and the second
 * is refused. The failed request must not leave that first block behind.
 */
static struct fi_replay *fi_late_replay;
static struct rand_data *fi_late_victim;

static void fi_late_failure_cb(void *arg, uint64_t *out)
{
	(void)arg;

	if (fi_late_victim) {
		jent_atomic_store_int(&fi_late_victim->selftest_failed, 1);
		fi_late_victim = NULL;
	}
	fi_replay_cb(fi_late_replay, out);
}

static void test_failure_after_output_wipes_it(void)
{
	static uint64_t seq[4096];
	struct fi_replay r;
	struct rand_data *ec;
	unsigned char buf[3 * 32];
	unsigned int i;
	uint64_t scale;
	size_t nonzero = 0;

	jent_ut_group("a request failing after its first block delivers nothing");

	scale = fi_plausible_clock(seq, JENT_ARRAY_SIZE(seq));
	fi_replay_init(&r, seq, JENT_ARRAY_SIZE(seq));
	r.scale = scale;
	fi_late_replay = &r;

	jent_set_mock_timer(fi_late_failure_cb, NULL);
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER);
	if (!ec) {
		jent_set_mock_timer(NULL, NULL);
		FI_NO_CONSTRUCTED_COLLECTOR("a late failure",
					    JENT_DISABLE_INTERNAL_TIMER);
		return;
	}

	memset(buf, 0, sizeof(buf));
	fi_late_victim = ec;

	JENT_UT_EQ(jent_read_entropy(ec, (char *)buf, sizeof(buf)),
		   JENT_ERR_SELFTEST,
		   "the verdict bound during the first block stops the second");
	JENT_UT_TRUE(fi_late_victim == NULL,
		     "and it was bound while the first block was generated");

	for (i = 0; i < sizeof(buf); i++)
		nonzero += buf[i] != 0;
	JENT_UT_EQ(nonzero, 0, "the block already copied out is wiped again");
	JENT_UT_EQ(ec->bytes_output, 0, "and nothing is counted as delivered");

	jent_entropy_collector_free(ec);
	jent_set_mock_timer(NULL, NULL);
}

int main(void)
{
	jent_ut_setup();

	/* Before anything initializes, which closes the window. */
	startup_cb_registered = !jent_set_fips_failure_callback(startup_cb);

	test_registration();
	test_startup_failure_no_callback();
	test_status_truncation();
	test_startup_on_mocked_clocks();
	test_ntg1_failure_does_not_force_notime();
	test_timestamp_replay();
	test_generation_on_mocked_clock();
	test_fallback_per_collector();
	test_realloc_during_startup();
	test_realloc_on_read_gives_up();
	test_failure_after_output_wipes_it();

	return jent_ut_report("unit-mock");
}
