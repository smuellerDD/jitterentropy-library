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
 * tests themselves cost - four orders of magnitude more executions per second
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
 * Referenced from the recovery loop of the RCT with memory. A stub, for the
 * reason tests/health gives for its own: what is under test is that reaching
 * the cutoff outside recovery clears the counter and enters the loop, not what
 * the noise source produces while it runs - and a stub keeps the harness
 * deterministic, which a fuzzing target has to be or its crashes do not
 * reproduce.
 */
void jent_random_data(struct rand_data *ec)
{
	(void)ec;
}

/* Time stamps per input: enough to cross the APT window several times. */
#define FH_MAX_STAMPS		4096
/* The guard around the collector, checked after every insertion. */
#define FH_GUARD		32
#define FH_FILL			0xa5

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
 * Window size of the RCT with memory: the number of time deltas the noise
 * source produces for one output block. Mirrors the calculation of
 * JENT_ADJUSTED_MEASURE_JITTER_LOOP_CTR in jent_random_data_one(), which is
 * where ec->rct_mem_nosr is established at runtime - as tests/health does,
 * for the same reason: without it the test never enters its window and the
 * fuzzer would never reach it.
 */
static unsigned short fh_rct_mem_nosr(unsigned int osr)
{
	unsigned int nosr = (DATA_SIZE_BITS + ENTROPY_SAFETY_FACTOR) * osr;

	/* Round up to the nearest multiple of three. */
	nosr = ((nosr + 2) / 3) * 3;

	return (unsigned short)nosr;
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
	 * usual answer and zero is the one a collector assembled by hand can
	 * carry, which jent_health_insert_timestamp() substitutes for rather
	 * than dividing by; the rest are the coarse counters where it is a
	 * power of two, and the values in between that no timer produces but
	 * a caller can still set.
	 */
	switch (fh_u8(s) % 6) {
	case 0:
		cfg.gcd = 0;
		break;
	case 1:
		cfg.gcd = 1;
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

	jent_health_init(&c->ec, cfg->inittype);
	c->ec.rct_mem_nosr = fh_rct_mem_nosr(cfg->osr);
}

/*
 * The next time stamp. A uniformly random 64-bit value is a delta that is
 * never stuck, never repeats a symbol and never lets a predictor guess right,
 * so it would leave every health test asleep. What is drawn instead is how the
 * clock behaves - stalled, ticking by a constant, cycling, jumping, running
 * backwards, wrapping - which is what the tests are written against.
 */
static uint64_t fh_next_stamp(struct fh_state *s, uint64_t prev, uint64_t *step)
{
	uint8_t pick = fh_u8(s);

	switch (pick % 8) {
	case 0:
		/* A clock that does not move: every delta is stuck. */
		return prev;
	case 1:
		/* Constant steps: the deltas repeat, so the APT sees one
		 * symbol and the third derivative is zero. */
		return prev + *step;
	case 2:
		/* A new constant, so a run of one shape gives way to another. */
		*step = (uint64_t)fh_u8(s) + 1;
		return prev + *step;
	case 3:
		/* Small steps, where the deltas are drawn from a set small
		 * enough for the lag predictor to learn. */
		return prev + (pick % 4);
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

	/* Nothing outside the documented bits, and nothing at all when the
	 * tests are not reporting. */
	assert((failure & ~(unsigned int)FH_FAILURE_MASK) == 0);
	if (!cfg->fips)
		assert(failure == 0);

	/* The APT counts recurrences of one symbol inside a window and starts
	 * a new one when the window is full, so neither can outgrow it. */
	assert(ec->apt_observations <= JENT_APT_WINDOW_SIZE);
	assert(ec->apt_count <= JENT_APT_WINDOW_SIZE);

	/* The RCT with memory counts only inside its window, and counts at
	 * most one measurement per insertion. */
	assert(ec->rct_mem_ctr <= ec->rct_mem_nosr);
	assert(ec->rct_mem_count <= ec->rct_mem_nosr);

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

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct fh_collector stamps, deltas;
	struct fh_config cfg;
	struct fh_state s;
	uint64_t prev = 0, step = 1;
	unsigned int seen = 0, n;

	s.data = data;
	s.len = size;
	s.pos = 0;

	cfg = fh_draw_config(&s);

	/*
	 * Two collectors in the same configuration: one fed the stamps, one
	 * fed the deltas those stamps form. They must not drift apart.
	 */
	fh_init(&stamps, &cfg);
	fh_init(&deltas, &cfg);

	for (n = 0; n < FH_MAX_STAMPS && !fh_eof(&s); n++) {
		uint64_t stamp = fh_next_stamp(&s, prev, &step);
		unsigned int stuck_stamp, stuck_delta, failure;

		stuck_delta = jent_stuck(&deltas.ec,
					 fh_delta(prev, stamp, cfg.gcd));
		stuck_stamp = jent_health_insert_timestamp(&stamps.ec, stamp);
		prev = stamp;

		/* A verdict on one measurement, not a count. */
		assert(stuck_stamp == 0 || stuck_stamp == 1);
		assert(stuck_stamp == stuck_delta);

		failure = jent_health_failure(&stamps.ec);
		assert(failure == jent_health_failure(&deltas.ec));

		/*
		 * A reported failure is never taken back. Checked across the
		 * whole input rather than per insertion, so a bit that appears
		 * and disappears between two stamps is caught as well.
		 */
		assert((failure & seen) == seen);
		seen = failure;

		fh_check_invariants(&stamps, &cfg, failure);
		fh_check_invariants(&deltas, &cfg,
				    jent_health_failure(&deltas.ec));
	}

	/*
	 * The stamp entry point is the delta entry point plus the delta: the
	 * two collectors saw the same measurements, so nothing but the time
	 * stamp they were derived from may differ.
	 */
	deltas.ec.prev_time = stamps.ec.prev_time;
	assert(!memcmp(&stamps.ec, &deltas.ec, sizeof(stamps.ec)));

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
 * every run. The inputs are long, unlike fuzz-api's, because here a long one
 * costs nothing and the windows worth crossing are hundreds of stamps wide.
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
#define FH_SWEEP_LEN		2048

int main(int argc, char *argv[])
{
	static unsigned char input[FH_SWEEP_LEN];
	unsigned int i, j;
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

	printf("fuzz-health: %u inputs survived\n", FH_SWEEP_INPUTS);

	return 0;
}

#endif /* JENT_FUZZ_STANDALONE */
