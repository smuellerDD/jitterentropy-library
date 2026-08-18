/*
 * Jitter RNG: Induced failure test for the SP800-90B health tests
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
 * This tool performs the induced failure testing of the Jitter RNG health
 * tests that FIPS 140-3 / SP800-90B validations require: for every health test
 * and for both its intermittent and its permanent cutoff, a known-bad sample
 * sequence is fed in and the resulting error read back with
 * jent_health_failure(). Each case checks that the expected error bit is
 * reported and that no unrelated one is, so the induced failure is attributed
 * to the test under test.
 *
 * The cases drive the individual health tests directly rather than going
 * through jent_stuck(), which feeds every sample into all four at once and so
 * cannot isolate them. The APT is the clearest example: its symbols are the
 * unmodified time deltas (JENT_APT_MASK covers all 64 bits), so a sequence
 * repeating one symbol to the APT cutoff is identical back-to-back deltas -
 * precisely the stuck condition, on which the RCT fires first.
 *
 * The per-test insert functions are static to the health test implementation.
 * Rather than giving them external linkage - at the cost of inlining the four
 * calls jent_stuck() performs for every time delta the noise source produces -
 * this tool compiles that translation unit into itself, and so does not link
 * against the library.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-health.c"

/*
 * The health test translation unit references this from the recovery loop of
 * the RCT with memory. A stub rather than the real generation: the loop is
 * exercised below, and what is under test is that reaching the cutoff outside
 * recovery clears the counter and enters the loop instead of raising the
 * error - not what the noise source produces while it runs. A stub also keeps
 * the case deterministic.
 */
void jent_random_data(struct rand_data *ec)
{
	(void)ec;
}

/*
 * Window size of the RCT with memory, i.e. the number of time deltas the noise
 * source generates for one output block. Mirrors the calculation of
 * JENT_ADJUSTED_MEASURE_JITTER_LOOP_CTR in jent_random_data_one() which is
 * where ec->rct_mem_nosr is established at runtime.
 */
static unsigned short jent_test_rct_mem_nosr(unsigned int osr)
{
	unsigned int nosr = (DATA_SIZE_BITS + ENTROPY_SAFETY_FACTOR) * osr;

	/* Round up to the nearest multiple of three. */
	nosr = ((nosr + 2) / 3) * 3;

	return (unsigned short)nosr;
}

/*
 * The RCT with memory only counts every third measurement, so reaching a
 * counter value of cnt requires 3 * cnt - 2 insertions.
 */
static unsigned int jent_test_rct_mem_samples(unsigned int cnt)
{
	return cnt ? (3 * cnt - 2) : 0;
}

struct jent_test_bit {
	unsigned int bit;
	const char *name;
};

static const struct jent_test_bit jent_test_bits[] = {
	{ JENT_RCT_FAILURE,		"RCT" },
	{ JENT_APT_FAILURE,		"APT" },
	{ JENT_LAG_FAILURE,		"Lag" },
	{ JENT_RCT_MEM_FAILURE,		"RCT-mem" },
	{ JENT_RCT_FAILURE_PERMANENT,	"RCT-permanent" },
	{ JENT_APT_FAILURE_PERMANENT,	"APT-permanent" },
	{ JENT_LAG_FAILURE_PERMANENT,	"Lag-permanent" },
	{ JENT_RCT_MEM_FAILURE_PERMANENT, "RCT-mem-permanent" },
};

static void jent_test_print_mask(unsigned int mask)
{
	unsigned int i, first = 1;

	if (!mask) {
		printf("none");
		return;
	}

	for (i = 0; i < JENT_ARRAY_SIZE(jent_test_bits); i++) {
		if (!(mask & jent_test_bits[i].bit))
			continue;
		printf("%s%s", first ? "" : "|", jent_test_bits[i].name);
		first = 0;
		mask &= ~jent_test_bits[i].bit;
	}

	if (mask)
		printf("%sunknown(0x%x)", first ? "" : "|", mask);
}

/*
 * The verdict, one line per health test that fired and what kind of failure it
 * was. The compact form above has to fit beside the case it belongs to in the
 * induced failure table; this is the one somebody reads to answer what a
 * recording tripped.
 */
static void jent_test_print_failures(unsigned int mask)
{
	static const struct {
		unsigned int intermittent;
		unsigned int permanent;
		const char *name;
	} tests[] = {
		{ JENT_RCT_FAILURE, JENT_RCT_FAILURE_PERMANENT,
		  "repetition count test (RCT)" },
		{ JENT_APT_FAILURE, JENT_APT_FAILURE_PERMANENT,
		  "adaptive proportion test (APT)" },
		{ JENT_LAG_FAILURE, JENT_LAG_FAILURE_PERMANENT,
		  "lag predictor test" },
		{ JENT_RCT_MEM_FAILURE, JENT_RCT_MEM_FAILURE_PERMANENT,
		  "repetition count test with memory" },
	};
	unsigned int i, named = 0;

	if (!mask) {
		printf("  no health test fired\n");
		return;
	}

	printf("  health tests that fired:\n");

	for (i = 0; i < JENT_ARRAY_SIZE(tests); i++) {
		int intermittent = !!(mask & tests[i].intermittent);
		int permanent = !!(mask & tests[i].permanent);

		named |= tests[i].intermittent | tests[i].permanent;

		if (!intermittent && !permanent)
			continue;

		printf("    %-34s %s%s%s\n", tests[i].name,
		       intermittent ? "intermittent" : "",
		       (intermittent && permanent) ? " and " : "",
		       permanent ? "permanent" : "");
	}

	/* Whatever the table above does not account for. */
	if (mask & ~named)
		printf("    %-34s 0x%x\n", "unknown failure bits",
		       mask & ~named);
}

static unsigned int failures = 0;
static unsigned int skipped = 0;

/*
 * @param name Name of the induced failure
 * @param ec Entropy collector the failure was induced into
 * @param samples Number of known-bad samples that were inserted
 * @param expect The error bit that must be reported
 * @param testmask All error bits belonging to the health test under test
 */
static void jent_test_verify(const char *name, struct rand_data *ec,
			     unsigned int samples, unsigned int expect,
			     unsigned int testmask)
{
	unsigned int mask = jent_health_failure(ec);
	const char *result;

	if (!(mask & expect))
		result = "FAILED (expected error not reported)";
	else if (mask & ~testmask)
		result = "FAILED (unrelated health test reported an error)";
	else
		result = "passed";

	printf("  %-34s %6u samples -> ", name, samples);
	jent_test_print_mask(mask);
	printf(" : %s\n", result);

	if (result[0] == 'F')
		failures++;
}

/*
 * For a case whose expected outcome is that no error is raised at all.
 * jent_test_verify() cannot express this: it asks whether an expected bit is
 * present.
 */
static void jent_test_verify_clean(const char *name, struct rand_data *ec,
				   unsigned int samples)
{
	unsigned int mask = jent_health_failure(ec);
	const char *result = mask ? "FAILED (an error was reported)" : "passed";

	printf("  %-34s %6u samples -> ", name, samples);
	jent_test_print_mask(mask);
	printf(" : %s\n", result);

	if (mask)
		failures++;
}

static void jent_test_skip(const char *name, const char *reason)
{
	printf("  %-34s %6s    -> skipped: %s\n", name, "-", reason);
	skipped++;
}

static void jent_test_init(struct rand_data *ec, unsigned int osr,
			   enum jent_health_init_type inittype)
{
	memset(ec, 0, sizeof(struct rand_data));
	ec->osr = osr;

	/* The health tests only report errors in FIPS mode. */
	ec->is_fips_enabled = 1;

	jent_health_init(ec, inittype);
	ec->rct_mem_nosr = jent_test_rct_mem_nosr(osr);
}

/*
 * RCT: a stuck measurement in every iteration.
 */
static void jent_test_rct(unsigned int osr,
			  enum jent_health_init_type inittype)
{
	struct rand_data ec;
	unsigned int i, cutoff;
	const unsigned int testmask = JENT_RCT_FAILURE |
				      JENT_RCT_FAILURE_PERMANENT;

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.rct_cutoff;
	for (i = 0; i < cutoff; i++)
		jent_rct_insert(&ec, 1);
	jent_test_verify("RCT intermittent", &ec, cutoff, JENT_RCT_FAILURE,
			 testmask);

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.rct_cutoff_permanent;
	for (i = 0; i < cutoff; i++)
		jent_rct_insert(&ec, 1);
	jent_test_verify("RCT permanent", &ec, cutoff,
			 JENT_RCT_FAILURE_PERMANENT, testmask);
}

/*
 * APT: the same symbol in every iteration of one APT window.
 */
static void jent_test_apt(unsigned int osr,
			  enum jent_health_init_type inittype)
{
	struct rand_data ec;
	unsigned int i, cutoff;
	const unsigned int testmask = JENT_APT_FAILURE |
				      JENT_APT_FAILURE_PERMANENT;

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.apt_cutoff;
	if (cutoff >= ec.apt_cutoff_permanent) {
		/*
		 * Expected for the common configuration from osr 15 on: the
		 * intermittent cutoff has grown into the maximum of 512 that
		 * FIPS 140-2 IG 7.19 resolution #16 caps it at (a cutoff the
		 * test cannot reach is not allowed), which is where the
		 * permanent cutoff already sits. The permanent error is
		 * checked first, so the intermittent one cannot be raised.
		 */
		jent_test_skip("APT intermittent",
			       "cutoff coincides with the permanent cutoff");
	} else {
		for (i = 0; i < cutoff; i++)
			jent_apt_insert(&ec, 0xc0ffee);
		jent_test_verify("APT intermittent", &ec, cutoff,
				 JENT_APT_FAILURE, testmask);
	}

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.apt_cutoff_permanent;
	for (i = 0; i < cutoff; i++)
		jent_apt_insert(&ec, 0xc0ffee);
	jent_test_verify("APT permanent", &ec, cutoff,
			 JENT_APT_FAILURE_PERMANENT, testmask);
}

#ifdef JENT_HEALTH_LAG_PREDICTOR
/*
 * Lag predictor: a constant time delta makes every prediction of the lag
 * predictor correct, which drives both the run of successful predictions
 * (local cutoff) and their total number (global cutoff).
 */
static void jent_test_lag(unsigned int osr,
			  enum jent_health_init_type inittype)
{
	struct rand_data ec;
	unsigned int i, cutoff, samples;
	const unsigned int testmask = JENT_LAG_FAILURE |
				      JENT_LAG_FAILURE_PERMANENT;

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.lag_local_cutoff;
	/* The first JENT_LAG_HISTORY_SIZE samples only prime the history. */
	samples = JENT_LAG_HISTORY_SIZE + cutoff;
	for (i = 0; i < samples; i++)
		jent_lag_insert(&ec, 0xc0ffee);
	jent_test_verify("Lag local intermittent", &ec, samples,
			 JENT_LAG_FAILURE, testmask);

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.lag_local_cutoff_permanent;
	samples = JENT_LAG_HISTORY_SIZE + cutoff;
	for (i = 0; i < samples; i++)
		jent_lag_insert(&ec, 0xc0ffee);
	jent_test_verify("Lag local permanent", &ec, samples,
			 JENT_LAG_FAILURE_PERMANENT, testmask);

	/*
	 * The global cutoff counts correct predictions across an entire lag
	 * window of JENT_LAG_WINDOW_SIZE samples. Reaching it with a sample
	 * sequence alone is not possible without tripping the local cutoff
	 * first, so the counter is primed to one below the cutoff and the
	 * decisive sample is then inserted.
	 */
	jent_test_init(&ec, osr, inittype);
	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++)
		jent_lag_insert(&ec, 0xc0ffee);
	ec.lag_prediction_success_count = ec.lag_global_cutoff - 1;
	jent_lag_insert(&ec, 0xc0ffee);
	jent_test_verify("Lag global intermittent (primed)", &ec,
			 JENT_LAG_HISTORY_SIZE + 1, JENT_LAG_FAILURE, testmask);

	jent_test_init(&ec, osr, inittype);
	for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++)
		jent_lag_insert(&ec, 0xc0ffee);
	ec.lag_prediction_success_count = ec.lag_global_cutoff_permanent - 1;
	jent_lag_insert(&ec, 0xc0ffee);
	jent_test_verify("Lag global permanent (primed)", &ec,
			 JENT_LAG_HISTORY_SIZE + 1, JENT_LAG_FAILURE_PERMANENT,
			 testmask);
}
#else /* JENT_HEALTH_LAG_PREDICTOR */
static void jent_test_lag(unsigned int osr,
			  enum jent_health_init_type inittype)
{
	(void)osr;
	(void)inittype;
	jent_test_skip("Lag local intermittent",
		       "lag predictor disabled at compile time");
	jent_test_skip("Lag local permanent",
		       "lag predictor disabled at compile time");
}
#endif /* JENT_HEALTH_LAG_PREDICTOR */

/*
 * RCT with memory: a stuck measurement in every iteration of one window.
 */
static void jent_test_rct_mem(unsigned int osr,
			      enum jent_health_init_type inittype)
{
	struct rand_data ec;
	unsigned int i, cutoff, samples;
	const unsigned int testmask = JENT_RCT_MEM_FAILURE |
				      JENT_RCT_MEM_FAILURE_PERMANENT;

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.rct_mem_cutoff;
	samples = jent_test_rct_mem_samples(cutoff);
	if (samples > ec.rct_mem_nosr) {
		jent_test_skip("RCT-mem intermittent",
			       "cutoff is not reachable within the window");
	} else {
		/*
		 * The intermittent error is only raised for a failure that
		 * survived the recovery loop. Enter that state directly - the
		 * recovery loop itself generates random numbers and therefore
		 * cannot be part of a deterministic induced failure.
		 */
		ec.in_recovery = 1;
		for (i = 0; i < samples; i++)
			jent_rct_mem_insert(&ec, 1);
		jent_test_verify("RCT-mem intermittent", &ec, samples,
				 JENT_RCT_MEM_FAILURE, testmask);
	}

	/*
	 * And the recovery loop, which is what the intermittent cutoff runs
	 * into first. Reaching the cutoff without already being in recovery
	 * must clear the counter and generate fresh data rather than raise the
	 * error, so that an expected false positive - the cutoffs of this test
	 * have a far higher false positive rate than the APT and RCT - does
	 * not stop the RNG.
	 */
	jent_test_init(&ec, osr, inittype);
	cutoff = ec.rct_mem_cutoff;
	samples = jent_test_rct_mem_samples(cutoff);
	if (samples > ec.rct_mem_nosr) {
		jent_test_skip("RCT-mem recovery loop",
			       "cutoff is not reachable within the window");
	} else {
		for (i = 0; i < samples; i++)
			jent_rct_mem_insert(&ec, 1);
		jent_test_verify_clean("RCT-mem recovery loop", &ec, samples);
	}

	/*
	 * Reaching the cutoff after the window has closed. The test keeps
	 * counting outside its window on purpose - returning early would make
	 * the noise source behave differently once the window ends - but it
	 * must not raise an error there.
	 */
	jent_test_init(&ec, osr, inittype);
	cutoff = ec.rct_mem_cutoff;
	samples = jent_test_rct_mem_samples(cutoff);
	if (samples > ec.rct_mem_nosr) {
		jent_test_skip("RCT-mem outside its window",
			       "cutoff is not reachable within the window");
	} else {
		ec.in_recovery = 1;
		for (i = 0; i < samples; i++) {
			/* Park the counter past the end of the window. */
			ec.rct_mem_ctr = ec.rct_mem_nosr;
			jent_rct_mem_insert(&ec, 1);
		}
		jent_test_verify_clean("RCT-mem outside its window", &ec,
				       samples);
	}

	jent_test_init(&ec, osr, inittype);
	cutoff = ec.rct_mem_cutoff_permanent;
	samples = jent_test_rct_mem_samples(cutoff);
	if (samples > ec.rct_mem_nosr) {
		/*
		 * This is the expected outcome for the common (non-NTG.1)
		 * configuration: its cutoffs use tau = 3 which places the
		 * permanent cutoff one count above the largest value the
		 * window can produce.
		 */
		jent_test_skip("RCT-mem permanent",
			       "cutoff is not reachable within the window");
	} else {
		ec.in_recovery = 1;
		for (i = 0; i < samples; i++)
			jent_rct_mem_insert(&ec, 1);
		jent_test_verify("RCT-mem permanent", &ec, samples,
				 JENT_RCT_MEM_FAILURE_PERMANENT, testmask);
	}
}

/*
 * The cutoff tables are indexed by the oversampling rate and clamped to their
 * last entry above it. JENT_MAX_OSR currently equals the length of the longest
 * of them, so nothing the public API accepts reaches the clamp of every table
 * - it is what keeps a raised JENT_MAX_OSR, which is a compile-time tunable,
 * from indexing past the end. Reached here by initializing the health tests
 * directly at an oversampling rate above all of them.
 */
static void jent_test_cutoff_clamping(enum jent_health_init_type inittype)
{
	struct rand_data ec, max_ec;
	const unsigned int beyond = JENT_MAX_OSR + 5;

	jent_test_init(&max_ec, JENT_MAX_OSR, inittype);
	jent_test_init(&ec, beyond, inittype);

	printf("  %-34s %6u osr    -> ", "cutoff clamping", beyond);

	if (ec.rct_mem_cutoff == max_ec.rct_mem_cutoff &&
	    ec.apt_cutoff == max_ec.apt_cutoff &&
#ifdef JENT_HEALTH_LAG_PREDICTOR
	    ec.lag_local_cutoff == max_ec.lag_local_cutoff &&
	    ec.lag_global_cutoff == max_ec.lag_global_cutoff &&
	    ec.lag_local_cutoff_permanent == max_ec.lag_local_cutoff_permanent &&
	    ec.lag_global_cutoff_permanent == max_ec.lag_global_cutoff_permanent &&
#endif
	    ec.rct_mem_cutoff_permanent == max_ec.rct_mem_cutoff_permanent &&
	    ec.apt_cutoff_permanent == max_ec.apt_cutoff_permanent) {
		printf("every table clamped : passed\n");
	} else {
		printf("a table was not clamped : FAILED\n");
		failures++;
	}

	/*
	 * The RCT is computed rather than looked up, so it scales instead of
	 * clamping - stated here so that the difference is deliberate.
	 */
	if (ec.rct_cutoff <= max_ec.rct_cutoff) {
		printf("  %-34s %6s    -> the computed RCT cutoff did not "
		       "scale : FAILED\n", "RCT cutoff scaling", "-");
		failures++;
	}
}

static void jent_test_run(unsigned int osr,
			  enum jent_health_init_type inittype)
{
	struct rand_data ec;

	jent_test_init(&ec, osr, inittype);

	printf("Induced failure tests with osr %u, %s configuration\n", osr,
	       (inittype == jent_health_init_type_ntg1) ? "NTG.1" : "common");
	printf("  cutoffs: RCT %u/%u, APT %u/%u, RCT-mem %u/%u"
#ifdef JENT_HEALTH_LAG_PREDICTOR
	       ", Lag local %u/%u, Lag global %u/%u"
#endif
	       " (intermittent/permanent)\n",
	       ec.rct_cutoff, ec.rct_cutoff_permanent,
	       ec.apt_cutoff, ec.apt_cutoff_permanent,
	       ec.rct_mem_cutoff, ec.rct_mem_cutoff_permanent
#ifdef JENT_HEALTH_LAG_PREDICTOR
	       , ec.lag_local_cutoff, ec.lag_local_cutoff_permanent,
	       ec.lag_global_cutoff, ec.lag_global_cutoff_permanent
#endif
	       );

	jent_test_rct(osr, inittype);
	jent_test_apt(osr, inittype);
	jent_test_lag(osr, inittype);
	jent_test_rct_mem(osr, inittype);
	jent_test_cutoff_clamping(inittype);
	printf("\n");
}

/*
 * Replay a file of time stamps through the health tests.
 *
 * One decimal (or 0x-prefixed hexadecimal) value per line; blank lines and
 * lines beginning with # are skipped, so a recording can carry a header. This
 * is what judges a raw entropy recording with the very tests that will judge
 * the noise source at runtime - the same code, reaching the same verdict on
 * the same numbers.
 *
 * Returns 0 when the file was read and no health test fired, 1 when one did,
 * and 2 when the file could not be read or did not parse.
 */
static int jent_test_replay(const char *file, unsigned int osr,
			    enum jent_health_init_type inittype)
{
	struct rand_data ec;
	FILE *f;
	char line[128];
	unsigned long lineno = 0;
	unsigned long long stamps = 0, stuck = 0;
	unsigned int mask;
	int primed = 0;

	f = strcmp(file, "-") ? fopen(file, "r") : stdin;
	if (!f) {
		fprintf(stderr, "Cannot open %s\n", file);
		return 2;
	}

	jent_test_init(&ec, osr, inittype);

	printf("Replaying %s with osr %u, %s configuration\n", file, osr,
	       (inittype == jent_health_init_type_ntg1) ? "NTG.1" : "common");
	/* So a parse error on stderr does not appear before this line. */
	fflush(stdout);

	while (fgets(line, sizeof(line), f)) {
		char *p = line, *endptr;
		uint64_t stamp;

		lineno++;

		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0')
			continue;

		errno = 0;
		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
			stamp = (uint64_t)strtoull(p + 2, &endptr, 16);
		else
			stamp = (uint64_t)strtoull(p, &endptr, 10);

		if (errno || endptr == p) {
			fprintf(stderr, "%s:%lu: not a time stamp: %s", file,
				lineno, line);
			if (f != stdin)
				fclose(f);
			return 2;
		}

		/*
		 * The first stamp only establishes what the second is a delta
		 * against. Inserting it twice makes the reference and the
		 * recording start together, so that the first measurement
		 * judged is the first one the recording actually describes.
		 */
		if (!primed) {
			jent_health_insert_timestamp(&ec, stamp);
			primed = 1;
		}

		stuck += jent_health_insert_timestamp(&ec, stamp);
		stamps++;
	}

	if (f != stdin)
		fclose(f);

	if (!stamps) {
		fprintf(stderr, "%s holds no time stamps\n", file);
		return 2;
	}

	mask = jent_health_failure(&ec);

	printf("  %llu time stamps, %llu stuck measurement(s)\n", stamps, stuck);
	jent_test_print_failures(mask);

	/*
	 * The deltas are judged as they are. A Jitter RNG whose startup found a
	 * common divisor greater than one would divide by it first, which
	 * changes what counts as stuck - so a recording taken from a coarse
	 * counter is judged more harshly here than at runtime.
	 */
	printf("  note: no common timer divisor is applied to the deltas\n");

	return mask ? 1 : 0;
}

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s [osr]\n", name);
	fprintf(stderr, "       %s --replay FILE [osr] [--ntg1]\n\n", name);
	fprintf(stderr,
		"Induced failure test of the Jitter RNG health tests. Feeds\n"
		"known-bad samples into each health test and verifies that the\n"
		"expected error is reported by jent_health_failure().\n\n"
		"With --replay, instead reads time stamps from FILE and runs\n"
		"the health tests over them - one decimal or 0x-prefixed value\n"
		"per line, blank lines and # comments skipped, \"-\" for stdin.\n"
		"Exits 0 when no health test fired and 1 when one did.\n\n"
		"  osr\t\tOversampling rate to test, default %u\n"
		"  --replay FILE\tReplay time stamps instead of inducing failures\n"
		"  --ntg1\tUse the NTG.1 cutoffs for the replay\n",
		(unsigned int)JENT_MIN_OSR);
}

int main(int argc, char *argv[])
{
	unsigned long osr = JENT_MIN_OSR;
	const char *replay = NULL;
	enum jent_health_init_type inittype = jent_health_init_type_common;
	int i;

	for (i = 1; i < argc; i++) {
		char *endptr;

		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage(argv[0]);
			return 0;
		}

		if (!strcmp(argv[i], "--replay")) {
			if (++i == argc) {
				fprintf(stderr, "--replay needs a file\n");
				return 1;
			}
			replay = argv[i];
			continue;
		}

		if (!strcmp(argv[i], "--ntg1")) {
			inittype = jent_health_init_type_ntg1;
			continue;
		}

		osr = strtoul(argv[i], &endptr, 10);
		if (*endptr || osr < JENT_MIN_OSR || osr > JENT_MAX_OSR) {
			fprintf(stderr,
				"Oversampling rate must be in the range of %u - %u\n",
				(unsigned int)JENT_MIN_OSR,
				(unsigned int)JENT_MAX_OSR);
			return 1;
		}
	}

	if (replay)
		return jent_test_replay(replay, (unsigned int)osr, inittype);

	jent_test_run((unsigned int)osr, jent_health_init_type_common);
	jent_test_run((unsigned int)osr, jent_health_init_type_ntg1);

	printf("%u induced failure test(s) failed, %u skipped\n", failures,
	       skipped);

	return failures ? 1 : 0;
}
