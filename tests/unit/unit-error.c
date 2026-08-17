/*
 * Jitter RNG: unit tests for the health failure reporting and recovery paths
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
 * What a caller sees when a health test fails: which JENT_ERR_* code each
 * failure bit is reported as, which of them jent_read_entropy_safe() recovers
 * from and which it passes straight back, and what the FIPS failure callback
 * is handed.
 *
 * The failures are induced by setting the health failure bits on the collector
 * directly - what is under test is the reporting above the health tests, not
 * the tests themselves, which tests/health drives to their cutoffs.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <stdlib.h>

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

static const struct {
	unsigned int bit;
	ssize_t err;
	const char *name;
	int permanent;
} failures[] = {
	{ JENT_RCT_FAILURE,		JENT_ERR_RCT,		"RCT",		0 },
	{ JENT_APT_FAILURE,		JENT_ERR_APT,		"APT",		0 },
	{ JENT_LAG_FAILURE,		JENT_ERR_LAG,		"Lag",		0 },
	{ JENT_RCT_MEM_FAILURE,		JENT_ERR_RCT_MEM,	"RCT-mem",	0 },
	{ JENT_RCT_FAILURE_PERMANENT,	JENT_ERR_RCT_PERMANENT,
	  "RCT permanent",	1 },
	{ JENT_APT_FAILURE_PERMANENT,	JENT_ERR_APT_PERMANENT,
	  "APT permanent",	1 },
	{ JENT_LAG_FAILURE_PERMANENT,	JENT_ERR_LAG_PERMANENT,
	  "Lag permanent",	1 },
	{ JENT_RCT_MEM_FAILURE_PERMANENT, JENT_ERR_RCT_MEM_PERMANENT,
	  "RCT-mem permanent",	1 },
};

/* Every health failure bit maps to the JENT_ERR_* code jitterentropy.h names. */
static void test_error_mapping(void)
{
	size_t i;
	char buf[32];

	jent_ut_group("jent_read_entropy maps every health failure bit");

	for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);

		if (!ec) {
			JENT_UT_SKIP(failures[i].name, "no collector");
			continue;
		}

		ec->health_failure = failures[i].bit;
		JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
			   failures[i].err, failures[i].name);

		jent_entropy_collector_free(ec);
	}
}

/*
 * A permanent failure takes precedence over an intermittent one of any test:
 * the collector is unusable either way, and the caller has to be told which of
 * the two it is.
 */
static void test_permanent_precedence(void)
{
	static const struct {
		unsigned int bits;
		ssize_t err;
		const char *name;
	} cases[] = {
		{ JENT_RCT_FAILURE | JENT_RCT_FAILURE_PERMANENT,
		  JENT_ERR_RCT_PERMANENT, "RCT intermittent and permanent" },
		{ JENT_APT_FAILURE | JENT_APT_FAILURE_PERMANENT,
		  JENT_ERR_APT_PERMANENT, "APT intermittent and permanent" },
		{ JENT_LAG_FAILURE | JENT_LAG_FAILURE_PERMANENT,
		  JENT_ERR_LAG_PERMANENT, "Lag intermittent and permanent" },
		{ JENT_RCT_MEM_FAILURE | JENT_RCT_MEM_FAILURE_PERMANENT,
		  JENT_ERR_RCT_MEM_PERMANENT,
		  "RCT-mem intermittent and permanent" },
		/* And across tests, in the order jent_read_entropy checks. */
		{ JENT_APT_FAILURE | JENT_RCT_FAILURE_PERMANENT,
		  JENT_ERR_RCT_PERMANENT, "APT intermittent, RCT permanent" },
	};
	size_t i;
	char buf[32];

	jent_ut_group("a permanent failure outranks an intermittent one");

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);

		if (!ec) {
			JENT_UT_SKIP(cases[i].name, "no collector");
			continue;
		}

		ec->health_failure = cases[i].bits;
		JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
			   cases[i].err, cases[i].name);

		jent_entropy_collector_free(ec);
	}
}

/* Outside FIPS mode the health tests do not report, so nothing is raised. */
static void test_no_report_without_fips(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	char buf[32];

	jent_ut_group("health failures are only reported in FIPS mode");

	if (!ec) {
		JENT_UT_SKIP("non-FIPS mode", "no collector");
		return;
	}

	if (ec->is_fips_enabled) {
		/* The machine has FIPS mode on; the flag cannot be taken back. */
		JENT_UT_SKIP("non-FIPS mode", "FIPS mode is enabled system-wide");
		jent_entropy_collector_free(ec);
		return;
	}

	ec->health_failure = JENT_RCT_FAILURE_PERMANENT;
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   (ssize_t)sizeof(buf),
		   "a failure bit outside FIPS mode does not stop the output");

	jent_entropy_collector_free(ec);
}

static struct rand_data *cb_ec;
static unsigned int cb_failure;
static unsigned int cb_calls;

static void failure_cb(struct rand_data *ec, unsigned int health_failure)
{
	cb_ec = ec;
	cb_failure = health_failure;
	cb_calls++;
}

/*
 * The callback is how a caller learns about an intermittent failure, which
 * jent_read_entropy_safe() otherwise recovers from without reporting.
 */
static int cb_registered;

/*
 * Registered before anything else runs: the library blocks any further switch
 * of the callback the first time it initializes (jent_health_cb_block_switch()
 * out of jent_entropy_init_common_pre()), which is precisely so that a caller
 * cannot lose failure notifications half way through a run.
 */
static void test_failure_callback_register(void)
{
	jent_ut_group("jent_set_fips_failure_callback before initialization");

	cb_registered = !jent_set_fips_failure_callback(failure_cb);
	JENT_UT_TRUE(cb_registered,
		     "the callback can be set before the library initializes");
}

static void test_failure_callback(void)
{
	struct rand_data *ec;
	char buf[32];

	jent_ut_group("the callback is invoked on a health failure");

	if (!cb_registered) {
		JENT_UT_SKIP("jent_set_fips_failure_callback",
			     "the callback could not be registered");
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_SKIP("jent_set_fips_failure_callback", "no collector");
		return;
	}

	cb_ec = NULL;
	cb_failure = 0;
	cb_calls = 0;

	ec->health_failure = JENT_APT_FAILURE_PERMANENT;
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   JENT_ERR_APT_PERMANENT, "the failure is still returned");
	JENT_UT_NE(cb_calls, 0, "the callback was invoked");
	JENT_UT_TRUE(cb_ec == ec, "with the collector that failed");
	JENT_UT_EQ(cb_failure, JENT_APT_FAILURE_PERMANENT,
		   "and the failure bits that were raised");

	jent_entropy_collector_free(ec);

	/*
	 * Switching the callback is denied once the library is initialized, so
	 * that a caller cannot lose failure notifications half way through.
	 */
	JENT_UT_NE(jent_set_fips_failure_callback(NULL), 0,
		   "switching the callback afterwards is denied");
}

/*
 * jent_read_entropy_safe() returns a permanent failure to the caller and
 * recovers from an intermittent one by reallocating at a higher oversampling
 * rate.
 */
static void test_safe_recovery(void)
{
	size_t i;
	char buf[32];

	jent_ut_group("jent_read_entropy_safe recovers only from intermittent failures");

	for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
		unsigned int osr_before;
		ssize_t ret;

		if (!ec) {
			JENT_UT_SKIP(failures[i].name, "no collector");
			continue;
		}

		osr_before = ec->osr;
		ec->health_failure = failures[i].bit;
		ret = jent_read_entropy_safe(&ec, buf, sizeof(buf));

		if (failures[i].permanent) {
			JENT_UT_EQ(ret, failures[i].err,
				   "a permanent failure is returned");
			JENT_UT_EQ(ec->osr, osr_before,
				   "and no reallocation was attempted");
		} else {
			JENT_UT_EQ(ret, (ssize_t)sizeof(buf),
				   "an intermittent failure is recovered from");
			JENT_UT_TRUE(ec->osr > osr_before,
				     "by raising the oversampling rate");
			JENT_UT_EQ(ec->reinit_count, 1u,
				   "and the reinitialization is counted");
		}

		jent_entropy_collector_free(ec);
	}
}

/*
 * The recovery raises the oversampling rate each time and gives up once it
 * would exceed JENT_MAX_OSR, returning the failure rather than looping.
 */
static void test_recovery_gives_up(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(JENT_MAX_OSR, 0);
	char buf[32];

	jent_ut_group("recovery gives up above the maximum oversampling rate");

	if (!ec) {
		JENT_UT_SKIP("recovery limit", "no collector");
		return;
	}

	/*
	 * Forced on rather than taken from the flags: the collector is
	 * allocated without JENT_FORCE_FIPS so that its startup does not have
	 * to converge at the maximum oversampling rate on this machine, but
	 * the health failure still has to be reported.
	 */
	ec->is_fips_enabled = 1;
	ec->health_failure = JENT_RCT_FAILURE;

	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)), JENT_ERR_RCT,
		   "the intermittent failure is returned once recovery is exhausted");
	JENT_UT_EQ(ec->osr, (unsigned int)JENT_MAX_OSR,
		   "and the collector was left untouched");

	jent_entropy_collector_free(ec);
}

/*
 * The reallocation carries the health test state over, so that a failure that
 * keeps occurring escalates to the permanent cutoff instead of restarting from
 * zero at every recovery.
 */
static void test_state_duplication(void)
{
	struct rand_data *old_ec, *new_ec;

	jent_ut_group("the health test state survives a reallocation");

	old_ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	new_ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!old_ec || !new_ec) {
		JENT_UT_SKIP("state duplication", "no collector");
		jent_entropy_collector_free(old_ec);
		jent_entropy_collector_free(new_ec);
		return;
	}

	/* RCT: the new instance starts primed at the intermittent cutoff. */
	new_ec->rct_count = 0;
	jent_rct_duplicate(new_ec);
	JENT_UT_EQ(new_ec->rct_count, new_ec->rct_cutoff,
		   "the RCT is primed at its intermittent cutoff");

	/*
	 * APT: a window that has not begun carries nothing over - there is no
	 * base symbol yet for the new instance to continue counting against.
	 */
	old_ec->apt_observations = 0;
	new_ec->apt_observations = 0xdead;
	jent_apt_duplicate(new_ec, old_ec);
	JENT_UT_EQ(new_ec->apt_observations, 0xdead,
		   "a window that has not begun carries nothing over");

	/* APT: an observation window in progress is carried over. */
	old_ec->apt_base = 0xc0ffee;
	old_ec->apt_observations = 42;
	old_ec->apt_base_set = 1;
	jent_apt_duplicate(new_ec, old_ec);
	JENT_UT_EQ(new_ec->apt_observations, 42,
		   "the APT window position is carried over");
	JENT_UT_EQ(new_ec->apt_base, 0xc0ffee, "with its base symbol");
	JENT_UT_EQ(new_ec->apt_count, new_ec->apt_cutoff,
		   "and the count primed at the intermittent cutoff");

	/* RCT with memory: likewise primed at its intermittent cutoff. */
	jent_rct_mem_duplicate(new_ec, old_ec);
	JENT_UT_EQ(new_ec->rct_mem_count, new_ec->rct_mem_cutoff,
		   "the RCT with memory is primed at its intermittent cutoff");

#ifdef JENT_HEALTH_LAG_PREDICTOR
	/* Lag: the whole predictor state, history and scoreboard included. */
	{
		unsigned int i;

		old_ec->lag_prediction_success_run = 7;
		old_ec->lag_prediction_success_count = 99;
		old_ec->lag_best_predictor = 3;
		old_ec->lag_observations = 1234;
		for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
			old_ec->lag_scoreboard[i] = i + 1;
			old_ec->lag_delta_history[i] = 0x1000 + i;
		}

		jent_lag_duplicate(new_ec, old_ec);

		JENT_UT_EQ(new_ec->lag_prediction_success_run, 7,
			   "the lag success run is carried over");
		JENT_UT_EQ(new_ec->lag_prediction_success_count, 99,
			   "the lag success count is carried over");
		JENT_UT_EQ(new_ec->lag_best_predictor, 3,
			   "the best predictor is carried over");
		JENT_UT_EQ(new_ec->lag_observations, 1234,
			   "the observation count is carried over");

		for (i = 0; i < JENT_LAG_HISTORY_SIZE; i++) {
			if (new_ec->lag_scoreboard[i] != i + 1 ||
			    new_ec->lag_delta_history[i] != 0x1000 + i) {
				JENT_UT_FAIL("the lag history differs at %u", i);
				break;
			}
		}
		jent_ut_checks++;
	}
#endif /* JENT_HEALTH_LAG_PREDICTOR */

	jent_entropy_collector_free(old_ec);
	jent_entropy_collector_free(new_ec);
}

static size_t count_occurrences(const char *haystack, const char *needle)
{
	size_t n = 0;
	const char *p = haystack;

	while ((p = strstr(p, needle)) != NULL) {
		n++;
		p += strlen(needle);
	}
	return n;
}

/*
 * The status output reports every health test and every flag as a JSON
 * boolean, so each of them is a two-armed choice that only ever takes one arm
 * on a healthy collector. Rendering a collector with every failure raised and
 * every flag set, and then one with none, walks both arms of all of them.
 */
static void test_status_both_arms(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	char buf[8192];
	unsigned int all_failures =
		JENT_RCT_FAILURE | JENT_APT_FAILURE | JENT_LAG_FAILURE |
		JENT_RCT_MEM_FAILURE | JENT_RCT_FAILURE_PERMANENT |
		JENT_APT_FAILURE_PERMANENT | JENT_LAG_FAILURE_PERMANENT |
		JENT_RCT_MEM_FAILURE_PERMANENT;
	unsigned int all_flags =
		JENT_DISABLE_MEMORY_ACCESS | JENT_FORCE_INTERNAL_TIMER |
		JENT_DISABLE_INTERNAL_TIMER | JENT_FORCE_FIPS | JENT_NTG1 |
		JENT_CACHE_ALL | JENT_FORCE_SECURE_MEM;
	unsigned int saved_flags;
	size_t set_true, clear_true;

	jent_ut_group("the status output renders both arms of every field");

	if (!ec) {
		JENT_UT_SKIP("jent_status", "no collector");
		return;
	}

	saved_flags = ec->flags;

	/* Everything set. */
	ec->health_failure = all_failures;
	ec->flags = saved_flags | all_flags;
	JENT_UT_EQ(jent_status(ec, buf, sizeof(buf)), 0,
		   "a collector with every failure and flag renders");
	JENT_UT_TRUE(strstr(buf, "\"true\"") == NULL,
		     "the booleans are unquoted JSON");
	set_true = count_occurrences(buf, "true");

	/* Nothing set. */
	ec->health_failure = 0;
	ec->flags = saved_flags & ~all_flags;
	JENT_UT_EQ(jent_status(ec, buf, sizeof(buf)), 0,
		   "a collector with none of them renders");
	clear_true = count_occurrences(buf, "true");

	/*
	 * Not asserted as all-true and all-false: a few of the fields are read
	 * from the collector rather than from the flags word (whether the
	 * internal timer is in use, whether the memory is really locked), and
	 * those do not follow the flags being forced here.
	 */
	JENT_UT_TRUE(set_true > clear_true,
		     "the fields follow the state they report");
	JENT_UT_TRUE(clear_true < 4,
		     "a collector with nothing raised reports almost nothing true");

	ec->flags = saved_flags;
	ec->health_failure = 0;
	jent_entropy_collector_free(ec);
}

/*
 * Recovery when the caller fixed the memory size. The reallocation normally
 * steps the memory size up along with the oversampling rate, but not over a
 * size the caller chose - that is a deliberate constraint, not a default.
 */
static void test_recovery_keeps_caller_memsize(void)
{
	/*
	 * JENT_FORCE_FIPS is deliberately not in the flags, for the reason
	 * test_recovery_gives_up() above states: every reallocation recovery
	 * makes would then have to converge on the FIPS startup sequence on
	 * this machine. The failure still has to be reported, which is what
	 * is_fips_enabled below is for.
	 */
	struct rand_data *ec =
		jent_entropy_collector_alloc(0, JENT_MAX_MEMSIZE_1MB);
	char buf[32];
	uint32_t memsize_before;
	ssize_t ret;

	jent_ut_group("recovery with a caller-configured memory size");

	if (!ec) {
		JENT_UT_SKIP("recovery", "no collector");
		return;
	}

	ec->is_fips_enabled = 1;

	JENT_UT_EQ(ec->max_mem_set, 1u, "the size counts as caller-configured");
	memsize_before = ec->memmask + 1;

	ec->health_failure = JENT_APT_FAILURE;
	ret = jent_read_entropy_safe(&ec, buf, sizeof(buf));

	/*
	 * Generating with the fresh collector can fail its health tests again
	 * on a machine whose noise source does not converge - a property of
	 * the machine, as for the tests labelled unreliable. The reallocation
	 * has happened by then either way, which is what this case is about.
	 */
	if (ret < 0)
		JENT_UT_SKIP("the failure is recovered from",
			     "the noise source did not converge on this machine");
	else
		JENT_UT_EQ(ret, (ssize_t)sizeof(buf),
			   "the failure is recovered from");

	/* Asserted, or the checks below could pass on no reallocation. */
	JENT_UT_TRUE(ec->reinit_count >= 1, "the collector was reallocated");
	JENT_UT_EQ(ec->memmask + 1, memsize_before,
		   "and the memory size the caller chose is kept");
	JENT_UT_EQ(ec->max_mem_set, 1u, "as is the fact that they chose it");

	jent_entropy_collector_free(ec);
}

int main(void)
{
	jent_ut_setup();

	/* Before any allocation: the first one blocks the callback switch. */
	test_failure_callback_register();

	test_error_mapping();
	test_permanent_precedence();
	test_no_report_without_fips();
	test_safe_recovery();
	test_recovery_gives_up();
	test_recovery_keeps_caller_memsize();
	test_state_duplication();
	test_status_both_arms();
	test_failure_callback();

	return jent_ut_report("unit-error");
}
