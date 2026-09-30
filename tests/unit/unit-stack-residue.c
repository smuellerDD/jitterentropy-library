/*
 * Jitter RNG: what a noise source run leaves on the stack
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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
 * Checks that every entry point running the noise source clears the stack it
 * ran over (jent_stack_scrub() in src/jitterentropy-base.c), and that the
 * wipe reaches deeper than the path does.
 *
 * A slab is painted below the caller's frame, the library runs over it, and
 * the returned frame is read back. The mocked time source tags every stamp
 * with a magic, so a find is attributable. Each entry point must:
 *
 *   - leave JENT_STACK_SCRUB_LEN contiguous zero bytes (the wipe ran in full)
 *   - leave the band below the wipe untouched (the path did not outgrow it)
 *   - leave no raw time stamp anywhere on the slab
 *
 * Builds that write into the frame after the wipe, or lay it out with holes
 * the wipe does not write (sanitizers), make these checks meaningless; they
 * are skipped there, as decided by a wipe of the test's own - see
 * sr_control_observable().
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

/* Room for the deepest path; still fits a 1 MB Windows main-thread stack. */
#define SR_SLAB		(64u * 1024u)
#define SR_PAINT	0xA5
/* What the control leaves where a path's frames would have left state. */
#define SR_DIRT		0x5A

/* Depth from the top of the slab, where the entry point's frame begins. */
#define SR_AT(depth)	(sr_snap[SR_SLAB - (depth)])

/*
 * The wipe covers the band below the entry point's own frame, whose locals
 * (return address, canary, arguments) stay. That boundary is a frame size and
 * is measured by sr_zero_run() rather than fixed here.
 */

/* Assumed entry frame size when the wipe left no run to measure. */
#define SR_ENTRY_FRAME_MAX	256u

/*
 * The band below the wipe, which nothing may reach. It starts SR_BELOW under
 * the wipe to skip the frames of the wipe itself, written after the clear.
 */
#define SR_BELOW		512u
#define SR_UNTOUCHED_LEN	4096u

/* How deep the snapshot is searched at all. */
#define SR_SEARCH_BOTTOM	((size_t)JENT_STACK_SCRUB_LEN + SR_BELOW +     \
				 SR_UNTOUCHED_LEN + SR_ENTRY_FRAME_MAX)

/* Implausible as a length, count, pointer or return address. */
#define SR_MAGIC	0x5EC0DE0000000000ULL

/* The magic's top four bytes in either byte order. */
static const unsigned char sr_sig_le[4] = { 0x00, 0xDE, 0xC0, 0x5E };
static const unsigned char sr_sig_be[4] = { 0x5E, 0xC0, 0xDE, 0x00 };

static uint64_t sr_tick;

/* The step must vary or every measurement is stuck and the loop never ends. */
static void sr_timer(void *arg, uint64_t *out)
{
	(void)arg;

	sr_tick += 0x40000 + (sr_tick % 293);
	*out = SR_MAGIC | (sr_tick & 0xffffffffULL);
}

static volatile unsigned char *sr_painted;
static unsigned char sr_snap[SR_SLAB];

/* The frame is gone on return: the caller then runs the library over it. */
static void sr_paint_below(unsigned char paint)
{
	volatile unsigned char slab[SR_SLAB];
	size_t i;

	for (i = 0; i < SR_SLAB; i++)
		slab[i] = paint;

	/* Keeping the address of the dead frame is the point here. */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdangling-pointer"
#endif
	sr_painted = slab;
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 12
# pragma GCC diagnostic pop
#endif
}

/*
 * Copy the slab. A macro so no frame of its own lands on what it reads; must
 * directly follow the library call.
 */
#define SR_SNAPSHOT()							       \
	do {								       \
		volatile unsigned char *sl_ = sr_painted;		       \
		size_t i_;						       \
									       \
		for (i_ = 0; i_ < SR_SLAB; i_++)			       \
			sr_snap[i_] = sl_[i_];				       \
	} while (0)

/* Occurrences of @needle between the two depths, inclusive. */
static size_t sr_count(size_t top, size_t bottom,
		       const void *needle, size_t nlen)
{
	const unsigned char *n = needle;
	size_t d, j, found = 0;

	/* A value ending at @d occupies the nlen depths up to and including it. */
	for (d = top + nlen - 1; d <= bottom; d++) {
		for (j = 0; j < nlen; j++) {
			/* The bytes of a value run from deeper to shallower. */
			if (SR_AT(d - j) != n[j])
				break;
		}
		if (j == nlen)
			found++;
	}

	return found;
}

/*
 * The longest zero run and its start. Nothing but the wipe writes a run that
 * long, so its start is where the entry point's frame ends.
 */
static size_t sr_zero_run(size_t *start)
{
	size_t d, run = 0, at = 0, best = 0, best_at = 0;

	for (d = 1; d <= SR_SEARCH_BOTTOM; d++) {
		if (SR_AT(d) != 0x00) {
			run = 0;
			continue;
		}

		if (!run)
			at = d;
		run++;

		if (run > best) {
			best = run;
			best_at = at;
		}
	}

	*start = best_at;
	return best;
}

/* Every timestamp carries the magic, whichever way the target stores it. */
static size_t sr_count_timestamps(size_t top, size_t bottom)
{
	return sr_count(top, bottom, sr_sig_le, sizeof(sr_sig_le)) +
	       sr_count(top, bottom, sr_sig_be, sizeof(sr_sig_be));
}

/*
 * Count the bytes no longer holding paint, and the non-zero ones among them,
 * bounded by @lo and @hi. All but @written may be NULL.
 */
static void sr_survey(size_t top, size_t bottom, size_t *written,
		      size_t *nonzero, size_t *lo, size_t *hi)
{
	size_t d, w = 0, nz = 0, first = 0, last = 0;

	for (d = top; d <= bottom; d++) {
		unsigned char b = SR_AT(d);

		if (b == SR_PAINT)
			continue;
		w++;
		if (b != 0x00) {
			nz++;
			if (!first)
				first = d;
			last = d;
		}
	}

	*written = w;
	if (nonzero)
		*nonzero = nz;
	if (lo)
		*lo = first;
	if (hi)
		*hi = last;
}

/* The deepest byte no longer holding paint. */
static size_t sr_reach(void)
{
	size_t d, deepest = 0;

	for (d = 1; d < SR_SLAB; d++) {
		if (SR_AT(d) != SR_PAINT)
			deepest = d;
	}

	return deepest;
}

/* Set by the controls, which run before any entry point. */
static int sr_wipe_measurable;
static int sr_stamps_measurable;
static size_t sr_observable_run;
static size_t sr_observable_holes;
static size_t sr_control_run;
static size_t sr_alloc_reach;
static size_t sr_random_reach;

/* The deeper of the allocation and the identifier draw. */
static size_t sr_os_reach(void)
{
	return (sr_random_reach > sr_alloc_reach) ? sr_random_reach :
						    sr_alloc_reach;
}

static const char sr_unmeasurable[] =
	"this build writes into the frame after the wipe returns or leaves "
	"holes in it";

/*
 * A copy of jent_stack_scrub()'s shape, independent of the library so that it
 * can tell "the wipe cannot be seen" from "the wipe did not run". volatile
 * keeps the dead stores.
 */
static JENT_NO_SANITIZE_ADDRESS void sr_own_wipe_array(void)
{
	volatile unsigned char scrub[JENT_STACK_SCRUB_LEN];
	size_t i;

	for (i = 0; i < (size_t)JENT_STACK_SCRUB_LEN; i++)
		scrub[i] = 0x00;
	(void)scrub;
}

static void sr_own_wipe_frame(void)
{
	volatile unsigned long z0 = 0, z1 = 0, z2 = 0, z3 = 0;
	volatile unsigned long z4 = 0, z5 = 0, z6 = 0, z7 = 0;

	(void)z0; (void)z1; (void)z2; (void)z3;
	(void)z4; (void)z5; (void)z6; (void)z7;
}

#define sr_own_wipe()							       \
	do {								       \
		sr_own_wipe_array();					       \
		sr_own_wipe_frame();					       \
	} while (0)

/*
 * Whether a wipe can be seen in this build at all. Instrumented builds
 * (ThreadSanitizer's exit hook, AddressSanitizer's redzones) write into the
 * frame after it returns; the checks are skipped there. Measured with the
 * test's own wipe: using the library's would turn a missing wipe into a skip.
 *
 * The slab starts out dirty, as the frames of a path would leave it, and the
 * wipe must clear it from the top of the slab down to the end of its run.
 * AddressSanitizer puts a redzone and its frame record between the entry
 * frame and the wipe's array, which neither the array nor the frame wipe
 * writes: what a path left there survives, next to a run of full length.
 * A hole is counted where it could hold a stamp's signature.
 */
static void sr_control_observable(void)
{
	static const unsigned char dirt[sizeof(sr_sig_le)] = {
		SR_DIRT, SR_DIRT, SR_DIRT, SR_DIRT
	};
	size_t start = 0;

	sr_paint_below(SR_DIRT);
	sr_own_wipe();
	SR_SNAPSHOT();

	sr_observable_run = sr_zero_run(&start);
	if (sr_observable_run)
		sr_observable_holes = sr_count(1, start + sr_observable_run - 1,
					       dirt, sizeof(dirt));
	sr_wipe_measurable =
		(sr_observable_run >= (size_t)JENT_STACK_SCRUB_LEN) &&
		!sr_observable_holes;

	/*
	 * Without holes, a short run only means the frames called after the
	 * wipe are larger (AddressSanitizer) and write over the bottom of it:
	 * their own locals, not what a path left. The run's length cannot be
	 * held to then, but a time stamp found anywhere still can.
	 */
	sr_stamps_measurable = sr_observable_run && !sr_observable_holes;
}

/*
 * What the library's wipe leaves on the same terms. Under AddressSanitizer a
 * redzone can cut it slightly short, so test_the_wipe_runs() asks little of
 * it; sr_check() holds the entry points to the exact length.
 */
static void sr_control_wipe(void)
{
	size_t start = 0;

	sr_paint_below(SR_PAINT);
	jent_stack_scrub();
	SR_SNAPSHOT();

	sr_control_run = sr_zero_run(&start);
}

/*
 * The two controls below measure the entry points' excursions into the
 * operating system - the allocation and the identifier draw - which can reach
 * below the wipe (on Windows) but hold no noise source state. The band below
 * the wipe starts under them; the stamp check still covers them. Their first
 * call in a process is the deep one, so they run before any entry point.
 */

/*
 * How deep the allocation alone reaches, without the startup on top. A NULL
 * return still measured it: the allocations come before anything that fails.
 *
 * As an instance that measures a clock (JENT_ALLOC_MEASURE_CLOCK), because that
 * is the one kind the allocation does not run a startup for. An ordinary one
 * runs the whole startup when none has passed - and none has, the controls
 * going first - with the startup's own wipe at its end: the control then grew
 * with the very path it is there to bound, and measured the wipe's depth as
 * the allocation's.
 */
static void sr_control_alloc(void)
{
	struct rand_data *ec;
	unsigned int flags;

	(void)jent_set_mock_timer(sr_timer, NULL);
	flags = jent_update_secure_mem(0);

	sr_paint_below(SR_PAINT);
	ec = jent_entropy_collector_alloc_internal(0, flags, 0,
						   JENT_ALLOC_MEASURE_CLOCK);
	SR_SNAPSHOT();

	sr_alloc_reach = sr_reach();

	if (ec)
		jent_entropy_collector_free(ec);
}

/*
 * How deep the UUID draw from the OS CSPRNG reaches. On Windows the first
 * BCryptGenRandom() of a process goes past the wipe.
 */
static void sr_control_random(void)
{
	char uuid[JENT_UUID_STRLEN];

	sr_paint_below(SR_PAINT);
	jent_uuid_generate(uuid);
	SR_SNAPSHOT();

	sr_random_reach = sr_reach();
}

/* The checks every entry point gets, on the snapshot already taken. */
static void sr_check(const char *what, const char *entry)
{
	size_t run, start = 0, top, bottom, below;
	size_t written = 0, nonzero = 0, lo = 0, hi = 0;
	size_t beyond = 0, beyond_hi = 0;
	size_t stamps;

	if (!sr_wipe_measurable) {
		if (!sr_stamps_measurable) {
			JENT_UT_SKIP(what, sr_unmeasurable);
			return;
		}

		stamps = sr_count_timestamps(1, SR_SEARCH_BOTTOM);
		printf("  %s: %zu time stamps found\n", entry, stamps);
		JENT_UT_EQ(stamps, 0, what);
		JENT_UT_SKIP(entry, "the wipe's length is not checked, this "
			     "build writes over the bottom of the wipe after "
			     "it returns");
		return;
	}

	run = sr_zero_run(&start);

	/* The bands below are placed by this run. */
	JENT_UT_TRUE(run >= (size_t)JENT_STACK_SCRUB_LEN, what);
	if (run < (size_t)JENT_STACK_SCRUB_LEN)
		start = SR_ENTRY_FRAME_MAX;

	top = start;
	bottom = start + JENT_STACK_SCRUB_LEN - 1;

	/* Clear of the wipe's own frames and of the OS excursions. */
	below = bottom + SR_BELOW;
	if (sr_os_reach() + SR_BELOW > below)
		below = sr_os_reach() + SR_BELOW;
	if (below + SR_UNTOUCHED_LEN >= SR_SLAB)
		below = SR_SLAB - SR_UNTOUCHED_LEN - 1;

	sr_survey(top, bottom, &written, &nonzero, &lo, &hi);
	sr_survey(below, below + SR_UNTOUCHED_LEN, &beyond, NULL, NULL,
		  &beyond_hi);

	/*
	 * Including the entry point's own frame, which the wipe does not
	 * reach: no frame legitimately holds a raw time stamp.
	 */
	stamps = sr_count_timestamps(1, SR_SEARCH_BOTTOM);

	printf("  %s: the wipe cleared %zu bytes from %zu down; of the %u it "
	       "covers, %zu were written and %zu left non-zero; %zu bytes "
	       "written below it\n",
	       entry, run, start, JENT_STACK_SCRUB_LEN, written, nonzero,
	       beyond);
	if (nonzero)
		printf("    residue lies %zu to %zu bytes below the entry "
		       "frame\n", lo, hi);
	if (beyond)
		printf("    the path reached %zu bytes below the entry frame, "
		       "past the %u the wipe covers and the %zu the operating "
		       "system excursions reach\n",
		       beyond_hi, JENT_STACK_SCRUB_LEN, sr_os_reach());

	JENT_UT_EQ(nonzero, 0, what);
	JENT_UT_EQ(stamps, 0, what);

	/* The only check that notices a path deeper than the wipe. */
	JENT_UT_EQ(beyond, 0, what);
}

/*
 * That jent_stack_scrub() runs at all, independent of the entry points, which
 * may each skip for reasons of their own. Half the length suffices: a wipe
 * that ran leaves thousands of zero bytes, one that did not leaves none.
 */
static void test_the_wipe_runs(void)
{
	static const char *what = "the wipe leaves a run where one can be seen";

	jent_ut_group("the wipe runs where the test can see it");

	if (!sr_stamps_measurable) {
		JENT_UT_SKIP(what, sr_unmeasurable);
		return;
	}

	JENT_UT_TRUE(sr_control_run >= (size_t)JENT_STACK_SCRUB_LEN / 2, what);
}

/*
 * Must be the first startup of the process: only that one runs the known
 * answer tests and the GCD analysis, which makes it the deepest.
 */
static void test_startup_scrubs_its_stack(void)
{
	static const char *what = "a startup leaves no state on the stack";
	int rc;

	jent_ut_group("the startup path does not leave state on the stack");

	if (jent_set_mock_timer(sr_timer, NULL)) {
		JENT_UT_SKIP(what, "the mock time source is not available");
		return;
	}

	sr_paint_below(SR_PAINT);
	rc = jent_entropy_init();
	SR_SNAPSHOT();

	if (rc) {
		JENT_UT_NO_STARTUP(what, rc);
		return;
	}

	sr_check(what, "jent_entropy_init");
}

/* The allocation runs a startup collection of its own. */
static void test_alloc_scrubs_its_stack(void)
{
	static const char *what = "an allocation leaves no state on the stack";
	struct rand_data *ec;
	int rc;

	jent_ut_group("the allocation does not leave state on the stack");

	if (jent_set_mock_timer(sr_timer, NULL)) {
		JENT_UT_SKIP(what, "the mock time source is not available");
		return;
	}

	rc = jent_entropy_init();
	if (rc) {
		JENT_UT_NO_STARTUP(what, rc);
		return;
	}

	sr_paint_below(SR_PAINT);
	ec = jent_entropy_collector_alloc(0, 0);
	SR_SNAPSHOT();

	if (!ec) {
		JENT_UT_NO_COLLECTOR(what, 0);
		return;
	}

	sr_check(what, "jent_entropy_collector_alloc");

	jent_entropy_collector_free(ec);
}

/*
 * A collector on the mock clock, or NULL. One on the internal timer is
 * refused, as its stamps would not carry the magic.
 */
static struct rand_data *sr_collector(const char *what)
{
	struct rand_data *ec;
	int rc;

	if (jent_set_mock_timer(sr_timer, NULL)) {
		JENT_UT_SKIP(what, "the mock time source is not available");
		return NULL;
	}

	rc = jent_entropy_init();
	if (rc) {
		JENT_UT_NO_STARTUP(what, rc);
		return NULL;
	}

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		JENT_UT_NO_COLLECTOR(what, 0);
		return NULL;
	}

	if (ec->enable_notime) {
		JENT_UT_SKIP(what,
			     "this collector runs on the internal timer, so "
			     "the mock is not its time source");
		jent_entropy_collector_free(ec);
		return NULL;
	}

	return ec;
}

static void test_generate_scrubs_its_stack(void)
{
	static const char *what = "a generate leaves no state on the stack";
	struct rand_data *ec;
	unsigned char out[32];
	ssize_t rc;

	jent_ut_group("the generation path does not leave state on the stack");

	ec = sr_collector(what);
	if (!ec)
		return;

	sr_paint_below(SR_PAINT);
	rc = jent_read_entropy(ec, (char *)out, sizeof(out));
	SR_SNAPSHOT();

	if (rc != (ssize_t)sizeof(out)) {
		JENT_UT_SKIP(what, "the generate did not deliver a block");
		jent_entropy_collector_free(ec);
		return;
	}

	sr_check(what, "jent_read_entropy");

	jent_entropy_collector_free(ec);
}

/*
 * Independent of the wipe: the output block goes from the conditioning
 * context to the caller's buffer and never onto the stack.
 */
static void test_generate_leaves_no_output(void)
{
	static const char *what = "a generate leaves no output on the stack";
	struct rand_data *ec;
	unsigned char out[32];
	size_t i, j, whole = 0, half = 0;
	ssize_t rc;

	jent_ut_group("the generate does not leave its output on the stack");

	ec = sr_collector(what);
	if (!ec)
		return;

	sr_paint_below(SR_PAINT);
	rc = jent_read_entropy(ec, (char *)out, sizeof(out));
	SR_SNAPSHOT();

	for (i = 0; i + sizeof(out) <= SR_SLAB; i++) {
		for (j = 0; j < sizeof(out); j++) {
			if (sr_snap[i + j] != out[j])
				break;
		}
		if (j == sizeof(out))
			whole++;
		/* Half of an output block is still half an output block. */
		if (j >= sizeof(out) / 2)
			half++;
	}

	if (rc != (ssize_t)sizeof(out)) {
		JENT_UT_SKIP(what, "the generate did not deliver a block");
		jent_entropy_collector_free(ec);
		return;
	}

	JENT_UT_EQ(whole, 0, "the returned block is not on the stack");
	JENT_UT_EQ(half, 0, "no half of the returned block is on the stack");

	jent_entropy_collector_free(ec);
}

int main(void)
{
	jent_ut_setup();

	/*
	 * The controls first, so the allocation and the identifier draw are
	 * measured cold. None of them runs a startup, leaving the first one
	 * of the process to the test that follows.
	 */
	sr_control_observable();
	sr_control_wipe();
	sr_control_alloc();
	sr_control_random();

	/* Which the allocation control is the one to break. */
	JENT_UT_TRUE(!jent_startup_passed(JENT_CLOCK_PLATFORM) &&
		     !jent_startup_passed(JENT_CLOCK_NOTIME),
		     "the controls run no startup");

	printf("the controls: a wipe of the test's own leaves a run of %zu with "
	       "%zu holes above it and the library's leaves %zu, where %u is "
	       "written; an allocation alone reaches %zu bytes down and an "
	       "identifier draw %zu\n",
	       sr_observable_run, sr_observable_holes, sr_control_run,
	       JENT_STACK_SCRUB_LEN, sr_alloc_reach, sr_random_reach);
	if (!sr_stamps_measurable)
		printf("  %s, so what is found in that frame afterwards is "
		       "not the library's to answer for\n", sr_unmeasurable);
	else if (!sr_wipe_measurable)
		printf("  this build writes over the bottom of the wipe after "
		       "it returns, so only time stamps are looked for\n");

	test_the_wipe_runs();

	test_startup_scrubs_its_stack();
	test_alloc_scrubs_its_stack();
	test_generate_scrubs_its_stack();
	test_generate_leaves_no_output();

	return jent_ut_report("unit-stack-residue");
}
