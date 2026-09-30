/*
 * Jitter RNG: libFuzzer harness driving the SP800-90B health tests
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
 * The health tests over time stamps chosen by an adversary.
 *
 * This is the second fuzzing target and it exists because the first one -
 * fuzz-api - cannot search. Every operation there allocates a collector or
 * generates from one, so it measures the machine's real timer, and even after
 * the work multipliers in the flags are capped the coverage-guided run manages
 * single-digit executions per second. A search that slow explores the argument
 * decoding and nothing behind it.
 *
 * Nothing here measures anything. jent_health_insert_timestamp() forms the
 * delta against the previously inserted stamp exactly as the noise source
 * forms it and runs every health test on it, so the fuzzer supplies the
 * numbers the noise source would have measured and the run costs what the
 * tests themselves cost - orders of magnitude more measurements per second
 * than fuzz-api reaches, spent inside the state machines that have counters,
 * windows, cutoff tables indexed by the oversampling rate, and a recovery loop
 * that re-enters the generation.
 *
 * That entry point is internal, so this harness compiles the health test
 * translation unit into itself as tests/health and the unit tests do, rather
 * than linking the library. The two harnesses are deliberately different that
 * way: fuzz-api may only reach what an application reaches, and this one has
 * to reach what no application can hand the library at all - a clock that
 * repeats, one that jumps by a constant, one that runs backwards, one that
 * wraps.
 *
 * What is asserted is what the health tests promise whatever they are fed,
 * because no verdict on adversarial stamps is wrong by itself - a sequence
 * designed to trip the RCT should trip it:
 *
 *   - the reported failure mask holds only defined bits, and a bit once
 *     reported is never taken back. A health failure that could be cleared by
 *     feeding more data is the one defect an attacker on the noise source
 *     would want,
 *   - every window counter stays inside its window, and the lag predictor's
 *     index stays inside the history it indexes,
 *   - the stamps are framed into blocks as the noise source frames them -
 *     a reported failure ends the block, and every later call measures only
 *     its priming - and the window of the RCT with memory advances by one
 *     per measurement while open - across a recovery too, which must restore
 *     it - and no count carries across a window start but one a duplicate
 *     primed,
 *   - the tests report nothing at all outside FIPS mode, whatever the state
 *     they accumulated,
 *   - no test writes outside the collector it was given, which is checked
 *     with guard bytes around it rather than left to the sanitizer, so it
 *     holds in a build without one, and
 *   - the stamp entry point and the delta entry point stay in step: the same
 *     stamps fed to jent_health_insert_timestamp() and the deltas they form
 *     fed to jent_stuck() leave the same state. The whole worth of replaying a
 *     raw entropy recording - which is what a SP800-90B validation submits -
 *     rests on the replay reaching the same verdict as the noise source, and
 *     that claim is otherwise untested against anything but recordings that
 *     behave.
 *
 * A failed assertion aborts, which is what libFuzzer records as a crash.
 */

/*
 * As in fuzz-api: the assertions are the test, so a build defining NDEBUG - any
 * CMake Release build - would run the health tests and check nothing.
 */
#ifdef NDEBUG
# undef NDEBUG
#endif

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-health.c"

/*
 * Measurements per input, recovery blocks included, in windows of the RCT
 * with memory: a recovery is ten blocks inside the outer one, and a stall
 * pattern can double what a block takes. Granted at one window per
 * FH_BYTES_PER_WINDOW input bytes, so a short input stays cheap however long
 * the runs it asks for, and a recovery costs about as many bytes at every
 * osr - the window, and so the budget, grows with it.
 */
#define FH_WINDOWS		(2 * (JENT_RCT_MEM_RECOVERY_LOOP_CNT + 2))
#define FH_BYTES_PER_WINDOW	16
/* The widest window, rounded up to a multiple of three as it is. */
#define FH_MAX_STAMPS		(FH_WINDOWS *				       \
				 (JENT_MEASURE_JITTER_LOOP_CTR(JENT_MAX_OSR,   \
					ENTROPY_SAFETY_FACTOR) + 2))
/* The guard around the collector, checked after every insertion. */
#define FH_GUARD		32
#define FH_FILL			0xa5
/* Stamp selectors from here up start a run - see fh_next_stamp(). */
#define FH_RUN_PICK		0xf0

/* Every bit jent_health_failure() is allowed to report. */
#define FH_FAILURE_MASK		(JENT_RCT_FAILURE | JENT_APT_FAILURE |	       \
				 JENT_LAG_FAILURE | JENT_RCT_MEM_FAILURE |     \
				 JENT_RCT_FAILURE_PERMANENT |		       \
				 JENT_APT_FAILURE_PERMANENT |		       \
				 JENT_LAG_FAILURE_PERMANENT |		       \
				 JENT_RCT_MEM_FAILURE_PERMANENT)

struct fh_state {
	const uint8_t *data;
	size_t len;
	size_t pos;
};

static int fh_eof(const struct fh_state *s)
{
	return s->pos >= s->len;
}

static uint8_t fh_u8(struct fh_state *s)
{
	return fh_eof(s) ? 0 : s->data[s->pos++];
}

static uint64_t fh_u64(struct fh_state *s)
{
	uint64_t v = 0;
	unsigned int i;

	for (i = 0; i < 8; i++)
		v = (v << 8) | fh_u8(s);

	return v;
}

/*
 * A collector inside a guard region. The health tests take a struct rand_data
 * and reach into a dozen of its members; that they reach no further is checked
 * here rather than assumed.
 */
struct fh_collector {
	unsigned char front[FH_GUARD];
	struct rand_data ec;
	unsigned char back[FH_GUARD];
};

static void fh_check_guards(const struct fh_collector *c)
{
	unsigned int i;

	for (i = 0; i < FH_GUARD; i++) {
		assert(c->front[i] == FH_FILL);
		assert(c->back[i] == FH_FILL);
	}
}

/*
 * The configuration the stamps are judged under, drawn from the input: which
 * cutoff tables are in force, at which oversampling rate, whether the tests
 * report at all, and what the startup established as the common divisor of
 * every delta.
 */
struct fh_config {
	unsigned int osr;
	enum jent_health_init_type inittype;
	unsigned int fips;
	uint64_t gcd;
};

/*
 * One input's run. Global, because the recovery stub below is reached from
 * inside the health tests and has to draw from the same input.
 *
 * Collector 0 is fed deltas through jent_stuck() and draws the stamps, which
 * are recorded; collector 1 is then fed the same stamps through
 * jent_health_insert_timestamp(). A recovery entered while collector 0
 * measures draws further stamps before collector 1 has seen the first, so
 * the record is what lets both judge the same sequence.
 */
struct fh_run {
	struct fh_state s;
	struct fh_config cfg;
	struct fh_collector c[2];

	/* The clock model. */
	uint64_t prev, step, rng;
	unsigned int run_left, run_idx, run_period, run_phase, run_const;
	unsigned int run_window;

	/* Measurements drawn, against this input's budget. */
	unsigned int n, budget;

	/* Stamps collector 0 drew in this step, for collector 1 to replay. */
	uint64_t rec[FH_MAX_STAMPS];
	unsigned int rec_n, rec_pos;

	/*
	 * Recoveries entered; of those, the ones entered with no failure
	 * reported - the others end before their first block - and the ones
	 * that ran all their blocks. Windows opened, and of those the ones
	 * after a completed recovery.
	 */
	unsigned int recoveries[2], live[2], completed[2];
	unsigned int windows, resumed;
};

static struct fh_run fh_run;

/* Across inputs, for the sweep to check what it reached. */
static unsigned long fh_total_windows, fh_total_recoveries;
static unsigned long fh_total_resumed, fh_total_rct_mem_failures;

static struct fh_config fh_draw_config(struct fh_state *s)
{
	struct fh_config cfg;
	uint8_t pick = fh_u8(s);

	/*
	 * Every rate the cutoff tables have an entry for. They are indexed
	 * with osr - 1 over [JENT_MIN_OSR, JENT_MAX_OSR], and a collector
	 * outside that range is one no allocation hands out - reaching past
	 * the tables here would be the harness's defect, not the library's.
	 */
	cfg.osr = JENT_MIN_OSR + (pick % (JENT_MAX_OSR - JENT_MIN_OSR + 1));

	pick = fh_u8(s);
	cfg.inittype = (pick & 1) ? jent_health_init_type_ntg1 :
				    jent_health_init_type_common;
	/*
	 * Rarely off, because the tests only report in FIPS mode and a run
	 * spent outside it would assert little. What it does check is that
	 * being outside it holds: no verdict escapes, however bad the stamps.
	 */
	cfg.fips = (pick & 0x7e) ? 1 : 0;

	/*
	 * The divisor the startup found common to every delta. One is the
	 * usual answer, and zero - the one a collector assembled by hand can
	 * carry - is how it is drawn: jent_health_insert_timestamp()
	 * substitutes one for it rather than dividing by it, so a pick of one
	 * as well would only run that case twice. The rest are the coarse
	 * counters where it is a power of two, a small odd one no shift
	 * stands in for, and the values in between that no timer produces but
	 * a caller can still set.
	 */
	switch (fh_u8(s) % 6) {
	case 0:
		cfg.gcd = 0;
		break;
	case 1:
		cfg.gcd = 3;
		break;
	case 2:
		cfg.gcd = 2;
		break;
	case 3:
		cfg.gcd = 100;
		break;
	case 4:
		cfg.gcd = UINT64_MAX;
		break;
	default:
		cfg.gcd = fh_u64(s);
		break;
	}

	return cfg;
}

static void fh_init(struct fh_collector *c, const struct fh_config *cfg)
{
	memset(c, FH_FILL, sizeof(*c));
	memset(&c->ec, 0, sizeof(c->ec));

	c->ec.osr = cfg->osr;
	c->ec.jent_common_timer_gcd = cfg->gcd;

	/* A one-bit field, so it is set rather than assigned: the memset above
	 * already left it clear, and an assignment from an unsigned int is a
	 * narrowing conversion the build warns about. */
	if (cfg->fips)
		c->ec.is_fips_enabled = 1;

	/*
	 * jent_health_init() establishes the window of the RCT with memory -
	 * at this collector's own osr and FIPS setting, so the fuzzer drives
	 * the window the runtime would use. It was recomputed here from a copy
	 * of the formula while jent_health_init() left it at zero, which kept
	 * the test out of its window entirely.
	 */
	jent_health_init(&c->ec, cfg->inittype);
}

/*
 * Healthy noise: a delta no derivative of which is likely zero, in units of
 * the divisor so that dividing by it leaves the delta as drawn.
 */
static uint64_t fh_noise(struct fh_run *r)
{
	r->rng ^= r->rng << 13;
	r->rng ^= r->rng >> 7;
	r->rng ^= r->rng << 17;

	return (1 + (r->rng & 0xfff)) * (r->cfg.gcd ? r->cfg.gcd : 1);
}

/*
 * A stall every period-th stamp of a run, a stuck measurement placed where the
 * RCT with memory counts - or does not. Or every period-th position of the
 * open window: a run index drifts against the window from block to block, and
 * only this reaches the cutoff again inside a recovery block. Collector 0 is
 * always the one drawing.
 */
static int fh_run_stalls(const struct fh_run *r, unsigned int idx)
{
	const struct rand_data *ec = &r->c[0].ec;
	unsigned int pos = idx;

	if (!r->run_period)
		return 0;
	if (r->run_window) {
		if (ec->rct_mem_ctr >= ec->rct_mem_nosr)
			return 0;
		pos = ec->rct_mem_ctr;
	}

	return pos % r->run_period == r->run_phase % r->run_period;
}

/*
 * The next time stamp. A uniformly random 64-bit value is a delta that is
 * never stuck, never repeats a symbol and never lets a predictor guess right,
 * so it would leave every health test asleep. What is drawn instead is how the
 * clock behaves - stalled, ticking by a constant, cycling, jumping, running
 * backwards, wrapping - which is what the tests are written against.
 *
 * And runs of it, from a few bytes: a block is hundreds to thousands of
 * non-stuck measurements, and a recovery ten of them, which one byte per
 * stamp would not reach within any input libFuzzer grows by default.
 */
static uint64_t fh_next_stamp(struct fh_run *r)
{
	struct fh_state *s = &r->s;
	uint64_t prev = r->prev;
	uint8_t pick;

	if (r->run_left) {
		r->run_left--;
		if (fh_run_stalls(r, r->run_idx++))
			return prev;
		return prev + (r->run_const ? r->step : fh_noise(r));
	}

	pick = fh_u8(s);
	if (pick >= FH_RUN_PICK) {
		uint8_t kind = fh_u8(s);

		r->run_period = kind & 7;
		r->run_const = (kind >> 3) & 1;
		r->run_window = (kind >> 4) & 1;
		r->run_phase = fh_u8(s);
		r->run_left = ((unsigned int)fh_u8(s) << 8) | fh_u8(s);
		r->run_idx = 0;
		r->rng = 0x9e3779b97f4a7c15ULL ^ fh_u8(s);
		return prev + fh_noise(r);
	}

	switch (pick % 8) {
	case 0:
		/* A clock that does not move: every delta is stuck. */
		return prev;
	case 1:
		/* Constant steps: the deltas repeat, so the APT sees one
		 * symbol and the third derivative is zero. */
		return prev + r->step;
	case 2:
		/* A new constant, so a run of one shape gives way to another. */
		r->step = (uint64_t)fh_u8(s) + 1;
		return prev + r->step;
	case 3:
		/* Small steps, where the deltas are drawn from a set small
		 * enough for the lag predictor to learn. A fresh draw: the
		 * selector above is pick % 8, so pick % 4 is fixed at 3 in
		 * this arm and the case was a constant step of 3 - case 1
		 * over again, and this distribution was never generated. */
		return prev + (fh_u8(s) % 4);
	case 4:
		/* Backwards, which no counter does and jent_delta() has to
		 * answer for anyway. */
		return prev - (uint64_t)fh_u8(s);
	case 5:
		/* Straight over the end of the range. */
		return UINT64_MAX - (uint64_t)pick;
	case 6:
		return 0;
	default:
		return fh_u64(s);
	}
}

/*
 * The delta jent_health_insert_timestamp() forms, so that the same numbers can
 * be handed to jent_stuck() - the entry point the noise source itself uses -
 * and the two states compared. Deliberately the same expression as the one
 * under test: what this establishes is not how the delta is computed but that
 * the stamp entry point does nothing else besides, which is the assumption
 * every replayed recording rests on.
 */
static uint64_t fh_delta(uint64_t prev, uint64_t stamp, uint64_t gcd)
{
	return jent_udiv64(jent_delta(prev, stamp), gcd ? gcd : 1);
}

/* What the health tests must hold to whatever they are fed. */
static void fh_check_invariants(const struct fh_collector *c,
				const struct fh_config *cfg,
				unsigned int failure)
{
	const struct rand_data *ec = &c->ec;

	fh_check_guards(c);

	/* Nothing outside the documented bits. The tests run either way and
	 * record what they find; the report is what is gated: all of it in
	 * FIPS mode, nothing at all outside it. */
	assert((ec->health_failure & ~(unsigned int)FH_FAILURE_MASK) == 0);
	assert(failure == (cfg->fips ? ec->health_failure : 0));

	/* The APT counts recurrences of one symbol inside a window and starts
	 * a new one when the window is full, so neither can outgrow it. */
	assert(ec->apt_observations <= JENT_APT_WINDOW_SIZE);
	assert(ec->apt_count <= JENT_APT_WINDOW_SIZE);

	/* The RCT with memory counts only inside its window, and counts at
	 * most one measurement per insertion. */
	assert(ec->rct_mem_ctr <= ec->rct_mem_nosr);
	assert(ec->rct_mem_count <= ec->rct_mem_nosr);

	/* Outside a recovery the intermittent cutoff is never left standing:
	 * reaching it enters the recovery, which restarts the count. */
	assert(ec->in_recovery ||
	       ec->rct_mem_count < ec->rct_mem_cutoff ||
	       ec->rct_mem_count >= ec->rct_mem_cutoff_permanent);

#ifdef JENT_HEALTH_LAG_PREDICTOR
	/* The predictor's window, and the index into the history it keeps -
	 * which is what a scoreboard update gone wrong would walk out of. */
	assert(ec->lag_observations <= JENT_LAG_WINDOW_SIZE);
	assert(ec->lag_best_predictor < JENT_LAG_HISTORY_SIZE);
	/* A run of correct guesses is a subset of all of them. */
	assert(ec->lag_prediction_success_run <=
	       ec->lag_prediction_success_count);
#endif
}

static unsigned int fh_which(const struct rand_data *ec)
{
	if (ec == &fh_run.c[0].ec)
		return 0;
	assert(ec == &fh_run.c[1].ec);
	return 1;
}

/*
 * One measurement into one collector, and what the RCT with memory must have
 * done with it. Returns 0 once the input or the budget is spent.
 */
static int fh_measure(unsigned int which, unsigned int *stuck)
{
	struct fh_run *r = &fh_run;
	struct rand_data *ec = &r->c[which].ec;
	unsigned int ctr = ec->rct_mem_ctr, count = ec->rct_mem_count;
	unsigned int primed = ec->rct_mem_primed;
	unsigned int inrec = ec->in_recovery;
	unsigned int recs = r->recoveries[which];
	uint64_t stamp, delta;
	unsigned int s;

	if (!which) {
		if (r->n >= r->budget || (fh_eof(&r->s) && !r->run_left))
			return 0;
		stamp = fh_next_stamp(r);
		r->n++;
		r->rec[r->rec_n++] = stamp;

		/* Ahead of the insertion: a recovery it enters draws on. */
		delta = fh_delta(r->prev, stamp, r->cfg.gcd);
		r->prev = stamp;
		s = jent_stuck(ec, delta);
	} else {
		if (r->rec_pos == r->rec_n)
			return 0;
		stamp = r->rec[r->rec_pos++];
		s = jent_health_insert_timestamp(ec, stamp);
	}

	/* A verdict on one measurement, not a count. */
	assert(s == 0 || s == 1);
	assert(ec->in_recovery == inrec);

	/*
	 * The window advances by one while open, whatever happened inside -
	 * a recovery included, which has to restore the position it left.
	 */
	assert(ec->rct_mem_ctr == ctr + (ctr < ec->rct_mem_nosr));

	/* No carry across a window start but the one a duplicate primed. */
	if (!ctr && !primed)
		count = 0;
	assert(ec->rct_mem_count <= count + s);
	if (!ctr)
		assert(!ec->rct_mem_primed);

	/* A recovery entered here left a fresh count behind. */
	if (r->recoveries[which] != recs) {
		assert(!inrec);
		assert(ec->rct_mem_count == 0);
	}

	fh_check_invariants(&r->c[which], &r->cfg, jent_health_failure(ec));

	*stuck = s;
	return 1;
}

/*
 * The ->prev_time priming measurement: the next stamp only sets the reference,
 * as jent_measure_jitter_one() primes with the health tests off.
 */
static int fh_prime(unsigned int which)
{
	struct fh_run *r = &fh_run;
	struct rand_data *ec = &r->c[which].ec;
	uint64_t stamp;

	if (!which) {
		if (r->n >= r->budget || (fh_eof(&r->s) && !r->run_left))
			return 0;
		stamp = fh_next_stamp(r);
		r->n++;
		r->rec[r->rec_n++] = stamp;
		r->prev = stamp;
	} else {
		if (r->rec_pos == r->rec_n)
			return 0;
		stamp = r->rec[r->rec_pos++];
	}
	ec->prev_time = stamp;

	return 1;
}

/*
 * The framing of jent_random_data() in the completed state: the ->prev_time
 * priming measurement, which the health tests do not judge, then
 * jent_random_data_one() opens the window and ends after ->rct_mem_nosr
 * non-stuck measurements or on a failure. The NTG.1 / FIPS startup blocks have
 * no priming measurement and are not modelled.
 */
static int fh_block(unsigned int which)
{
	struct rand_data *ec = &fh_run.c[which].ec;
	unsigned int block = 0, s;

	assert(ec->rct_mem_ctr == ec->rct_mem_nosr);
	if (!fh_prime(which))
		return 0;
	ec->rct_mem_ctr = 0;

	while (!jent_health_failure(ec)) {
		if (!fh_measure(which, &s))
			return 0;
		if (!s && ++block >= ec->rct_mem_nosr)
			break;
	}

	return 1;
}

/*
 * Referenced from the recovery loop of the RCT with memory. The real one
 * generates fresh blocks; this one frames the next stamps of the input as
 * those blocks, so the loop - its intermittent failure, and the window the
 * caller saves around it - is fuzzed as the noise source runs it, and stays
 * deterministic, which a fuzzing target has to be or its crashes do not
 * reproduce.
 */
void jent_random_data_recovery(struct rand_data *ec, unsigned int loops)
{
	unsigned int which = fh_which(ec), i;

	fh_run.recoveries[which]++;
	/* A failure reported ahead of it ends it before the first block. */
	if (!jent_health_failure(ec))
		fh_run.live[which]++;

	/* The caller closed the outer window and cleared the count. */
	assert(ec->in_recovery);
	assert(ec->rct_mem_ctr == ec->rct_mem_nosr);
	assert(ec->rct_mem_count == 0);

	for (i = 0; i < loops; i++) {
		if (jent_health_failure(ec) || !fh_block(which))
			break;
	}
	if (i == loops && !jent_health_failure(ec))
		fh_run.completed[which]++;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fh_run *r = &fh_run;
	struct rand_data *d = &r->c[0].ec, *t = &r->c[1].ec;
	unsigned int seen = 0, block = 0, nosr;
	uint64_t budget;
	int priming = 1, first = 1;

	/* Everything but the record itself, only read below rec_n. */
	memset(r, 0, offsetof(struct fh_run, rec));
	memset(&r->rec_n, 0, sizeof(*r) - offsetof(struct fh_run, rec_n));
	r->step = 1;

	r->s.data = data;
	r->s.len = size;
	r->s.pos = 0;

	r->cfg = fh_draw_config(&r->s);

	/*
	 * Two collectors in the same configuration: one fed the stamps, one
	 * fed the deltas those stamps form. They must not drift apart.
	 */
	fh_init(&r->c[0], &r->cfg);
	fh_init(&r->c[1], &r->cfg);

	nosr = t->rct_mem_nosr;
	budget = (uint64_t)size * nosr / FH_BYTES_PER_WINDOW;
	r->budget = budget < (uint64_t)FH_WINDOWS * nosr ?
		    (unsigned int)budget : FH_WINDOWS * nosr;

	for (;;) {
		unsigned int stuck_delta, stuck_stamp, failure;

		/*
		 * The ->prev_time priming of the next jent_random_data() call
		 * finds the window closed behind a completed block. After a
		 * failure it is wherever that left it, or at zero behind a call
		 * that measured nothing but its priming. The first is taken as
		 * the allocation leaves the collector, with the window open at
		 * zero.
		 */
		if (priming && !first && !jent_health_failure(t)) {
			assert(d->rct_mem_ctr == d->rct_mem_nosr);
			assert(t->rct_mem_ctr == t->rct_mem_nosr);
		}

		r->rec_n = r->rec_pos = 0;

		/*
		 * jent_random_data_one() opens the window whatever was
		 * reported, and measures nothing once something is: the next
		 * call primes again.
		 */
		if (priming) {
			if (!fh_prime(0))
				break;
			/* Collector 1 replays what collector 0 drew. */
			if (!fh_prime(1))
				assert(0);
			assert(r->rec_pos == r->rec_n);

			d->rct_mem_ctr = 0;
			t->rct_mem_ctr = 0;
			first = 0;
			if (!jent_health_failure(t)) {
				priming = 0;
				r->windows++;
				if (r->completed[1])
					r->resumed++;
			}
			continue;
		}

		if (!fh_measure(0, &stuck_delta))
			break;
		/* Collector 1 replays what collector 0 drew; it cannot run
		 * dry first. */
		if (!fh_measure(1, &stuck_stamp))
			assert(0);
		assert(r->rec_pos == r->rec_n);

		assert(stuck_stamp == stuck_delta);
		assert(r->recoveries[0] == r->recoveries[1]);
		assert(r->live[0] == r->live[1]);
		assert(r->completed[0] == r->completed[1]);

		failure = jent_health_failure(t);
		assert(failure == jent_health_failure(d));

		/*
		 * A reported failure is never taken back. Checked across the
		 * whole input rather than per insertion, so a bit that appears
		 * and disappears between two stamps is caught as well.
		 */
		assert((failure & seen) == seen);
		seen = failure;

		/* A failure ends a block as it does. */
		if (failure || (!stuck_stamp && ++block >= t->rct_mem_nosr)) {
			block = 0;
			priming = 1;
		}
	}

	fh_total_windows += r->windows;
	fh_total_recoveries += r->live[1];
	fh_total_resumed += r->resumed;
	if (jent_health_failure(t) & JENT_RCT_MEM_FAILURE)
		fh_total_rct_mem_failures++;

	/*
	 * The stamp entry point is the delta entry point plus the delta: the
	 * two collectors saw the same measurements, so nothing but the time
	 * stamp they were derived from may differ.
	 */
	assert(t->prev_time == r->prev);
	d->prev_time = t->prev_time;
	assert(!memcmp(d, t, sizeof(*t)));

	return 0;
}

#ifdef JENT_FUZZ_STANDALONE

/*
 * Without libFuzzer, so that any compiler can run it: with arguments it
 * replays the files it is given - which is how a crash the fuzzer found is
 * reproduced - and without them it runs a fixed sweep, which is what the suite
 * registers as a test case.
 *
 * The sweep's inputs come from a counter through a mixing function rather than
 * from rand(): a regression case has to be the same on every machine and in
 * every run. Each input is granted FH_SWEEP_LEN / FH_BYTES_PER_WINDOW windows.
 */

static int fh_run_file(const char *path)
{
	unsigned char buf[65536];
	size_t len;
	FILE *f = fopen(path, "rb");

	if (!f) {
		fprintf(stderr, "fuzz-health: cannot open %s\n", path);
		return 1;
	}

	len = fread(buf, 1, sizeof(buf), f);
	fclose(f);

	LLVMFuzzerTestOneInput(buf, len);
	printf("fuzz-health: %s (%zu bytes) survived\n", path, len);

	return 0;
}

#define FH_SWEEP_INPUTS		64
#define FH_SWEEP_LEN		256

/*
 * What the random sweep is unlikely to get to. Zero-padded to FH_SWEEP_LEN for
 * the budget; the runs outlast it, so the padding is never read.
 *
 * osr 3, common tables, FIPS, divisor 1, then a run of noise stalled at every
 * counted position of the window: the count reaches the intermittent cutoff at
 * the end of the first window and again in the first recovery block, which
 * raises the intermittent failure.
 *
 * osr 20, the same, but the stalled run ends on the stamp that reaches the
 * cutoff (0x1912 = 6417 + 1, the last counted position of the 6420 wide
 * window) and plain noise follows: the recovery completes all its blocks and
 * the outer block runs on into the next window.
 */
static const unsigned char fh_seeds[][FH_SWEEP_LEN] = {
	{ 0x00, 0x02, 0x01, FH_RUN_PICK, 0x13, 0x00, 0xff, 0xff, 0x01 },
	{ 0x11, 0x02, 0x01, FH_RUN_PICK, 0x13, 0x00, 0x19, 0x12, 0x01,
	  FH_RUN_PICK, 0x00, 0x00, 0xff, 0xff, 0x01,
	  FH_RUN_PICK, 0x00, 0x00, 0xff, 0xff, 0x02 },
};

int main(int argc, char *argv[])
{
	static unsigned char input[FH_SWEEP_LEN];
	unsigned int i, j, resumed;
	int ret = 0;

	if (argc > 1) {
		for (i = 1; i < (unsigned int)argc; i++)
			ret |= fh_run_file(argv[i]);

		return ret;
	}

	for (i = 0; i < FH_SWEEP_INPUTS; i++) {
		uint64_t x = 0x9e3779b97f4a7c15ULL * (i + 1);

		for (j = 0; j < FH_SWEEP_LEN; j++) {
			x ^= x >> 30;
			x *= 0xbf58476d1ce4e5b9ULL;
			x ^= x >> 27;
			input[j] = (unsigned char)(x >> 24);
		}

		LLVMFuzzerTestOneInput(input, sizeof(input));
	}

	LLVMFuzzerTestOneInput(fh_seeds[0], sizeof(fh_seeds[0]));
	LLVMFuzzerTestOneInput(fh_seeds[1], sizeof(fh_seeds[1]));
	/* The second seed's own: the sweep may resume only at a low osr. */
	resumed = fh_run.resumed;

	printf("fuzz-health: %u inputs survived: %lu window(s), %lu recovery "
	       "loop(s), %lu resumed after one, %lu RCT-mem failure(s)\n",
	       FH_SWEEP_INPUTS + 2, fh_total_windows, fh_total_recoveries,
	       fh_total_resumed, fh_total_rct_mem_failures);

	/* That the window reopens, the recovery is reached, and the outer
	 * block runs on after one completed is what the framing is for; a
	 * sweep that no longer gets there tests less. */
	if (fh_total_windows < 2 || !fh_total_recoveries ||
	    !fh_total_rct_mem_failures || !resumed) {
		fprintf(stderr, "fuzz-health: framing not exercised\n");
		return 1;
	}

	return 0;
}

#endif /* JENT_FUZZ_STANDALONE */
