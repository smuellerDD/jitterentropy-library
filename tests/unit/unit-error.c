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
#include <limits.h>
#include <stdlib.h>

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

/*
 * The known answer test of the conditioning, interposed as unit-fault.c
 * interposes the allocator: renamed while its source is absorbed, and supplied
 * here under its own name, so that every absorbed caller reaches a version
 * that can be made to fail. The shipped library carries no testing hook.
 */
#define jent_sha3_tester jent_sha3_tester_real
#include "jitterentropy-sha3.c"
#undef jent_sha3_tester

static int ut_sha3_fail;

int jent_sha3_tester(void)
{
	return ut_sha3_fail ? 1 : jent_sha3_tester_real();
}
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
			JENT_UT_NO_COLLECTOR(failures[i].name, JENT_FORCE_FIPS);
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
			JENT_UT_NO_COLLECTOR(cases[i].name, JENT_FORCE_FIPS);
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
		JENT_UT_NO_COLLECTOR("non-FIPS mode", 0);
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

/*
 * Fail a self test from inside the read: what a jent_selftest() bound to the
 * collector does when it lands while the read is in flight.
 */
static int cb_fail_selftest;

/*
 * What the callback saw of the instance it was handed, copied while it ran: a
 * collector in its startup is freed again when that startup fails.
 */
#define CB_LOG_MAX	8
static struct {
	unsigned int failure;
	char uuid[JENT_UUID_STRLEN];
	uint64_t bytes_output;
	uint64_t read_invocations;
} cb_log[CB_LOG_MAX];
static unsigned int cb_log_len;

static void failure_cb(struct rand_data *ec, unsigned int health_failure)
{
	cb_ec = ec;
	cb_failure = health_failure;
	cb_calls++;

	if (cb_log_len < CB_LOG_MAX) {
		cb_log[cb_log_len].failure = health_failure;
		memcpy(cb_log[cb_log_len].uuid, ec->uuid,
		       sizeof(cb_log[cb_log_len].uuid));
		cb_log[cb_log_len].bytes_output = ec->bytes_output;
		cb_log[cb_log_len].read_invocations = ec->read_invocations;
		cb_log_len++;
	}

	if (cb_fail_selftest)
		jent_atomic_store_int(&ec->selftest_failed, 1);
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
	/* Eight blocks: a request the generator checks the tests all through. */
	char buf[8 * (DATA_SIZE_BITS / 8)];
	unsigned int i;

	jent_ut_group("the callback is invoked on a health failure");

	if (!cb_registered) {
		JENT_UT_SKIP("jent_set_fips_failure_callback",
			     "the callback could not be registered");
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("jent_set_fips_failure_callback",
				     JENT_FORCE_FIPS);
		return;
	}

	cb_ec = NULL;
	cb_failure = 0;
	cb_calls = 0;

	ec->health_failure = JENT_APT_FAILURE_PERMANENT;
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   JENT_ERR_APT_PERMANENT, "the failure is still returned");
	JENT_UT_TRUE(cb_ec == ec, "with the collector that failed");
	JENT_UT_EQ(cb_failure, JENT_APT_FAILURE_PERMANENT,
		   "and the failure bits that were raised");
	/*
	 * Once per failure, not once per check of it. The collection loop asks
	 * jent_health_failure() before every measurement it takes and
	 * jent_read_entropy() asks again for every block, so one raised
	 * failure is seen over and over on the way out - a caller whose
	 * callback logs, counts or trips an alarm would get all of them.
	 */
	JENT_UT_EQ(cb_calls, 1, "the callback was invoked exactly once");

	/*
	 * And directly, where the repetition actually lives: a failure that
	 * stands is checked for as long as the instance is alive.
	 */
	cb_calls = 0;
	ec->health_failure = 0;
	ec->health_failure_reported = 0;

	ec->health_failure = JENT_RCT_FAILURE;
	for (i = 0; i < 1000; i++)
		jent_health_failure(ec);
	JENT_UT_EQ(cb_calls, 1,
		   "a thousand checks of one failure are one notification");

	/* A bit that was not reported yet is a new failure, and is. */
	ec->health_failure |= JENT_RCT_FAILURE_PERMANENT;
	for (i = 0; i < 1000; i++)
		jent_health_failure(ec);
	JENT_UT_EQ(cb_calls, 2,
		   "and an escalation to the permanent cutoff is a second");
	JENT_UT_EQ(cb_failure,
		   JENT_RCT_FAILURE | JENT_RCT_FAILURE_PERMANENT,
		   "reported with every bit standing, not just the new one");

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

	/* The FIPS collectors allocate, but their replacements may not. */
	if (!jent_ut_memlock_available()) {
		JENT_UT_SKIP("the recovery",
			     "this machine locks too little memory (RLIMIT_MEMLOCK)");
		return;
	}

	for (i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
		char uuid_before[JENT_UUID_STRLEN];
		uint64_t bytes_before, reads_before;
		unsigned int osr_before, reinits_before, flags_before;
		ssize_t ret;

		if (!ec) {
			JENT_UT_NO_COLLECTOR(failures[i].name, JENT_FORCE_FIPS);
			continue;
		}

		/*
		 * A history for the reallocation to carry: the instance's
		 * UUID and its output accounting.
		 */
		if (jent_read_entropy_safe(&ec, buf, sizeof(buf)) !=
		    (ssize_t)sizeof(buf)) {
			JENT_UT_SKIP(failures[i].name,
				     "the noise source did not converge on this machine");
			jent_entropy_collector_free(ec);
			continue;
		}
		memcpy(uuid_before, ec->uuid, sizeof(uuid_before));
		bytes_before = ec->bytes_output;
		reads_before = ec->read_invocations;

		/*
		 * Both from here: the initial allocation may already have
		 * walked a step of the startup ladder on this machine.
		 */
		osr_before = ec->osr;
		reinits_before = ec->reinit_count;
		flags_before = jent_entropy_collector_flags(ec);
		ec->health_failure = failures[i].bit;
		ret = jent_read_entropy_safe(&ec, buf, sizeof(buf));

		if (failures[i].permanent) {
			JENT_UT_EQ(ret, failures[i].err,
				   "a permanent failure is returned");
			JENT_UT_EQ(ec->osr, osr_before,
				   "and no reallocation was attempted");
			JENT_UT_EQ(ec->bytes_output, bytes_before,
				   "a read that delivered nothing counts nothing");
		} else {
			JENT_UT_EQ(ret, (ssize_t)sizeof(buf),
				   "an intermittent failure is recovered from");
			JENT_UT_TRUE(ec->osr > osr_before,
				     "by raising the oversampling rate");
			/*
			 * One reallocation per rate: the recovery's own, and
			 * any the replacement's startup ladder made.
			 */
			JENT_UT_EQ(ec->reinit_count - reinits_before,
				   ec->osr - osr_before,
				   "and every reallocation is counted");
			JENT_UT_TRUE(ec->uuid[0] != '\0',
				     "the replacement carries an identifier");
			JENT_UT_TRUE(!memcmp(ec->uuid, uuid_before,
					     sizeof(uuid_before)),
				     "and it is the one the instance had");
			JENT_UT_EQ(ec->bytes_output,
				   bytes_before + sizeof(buf),
				   "the output accounting spans the reallocation");
			JENT_UT_EQ(ec->read_invocations, reads_before + 1,
				   "as does the count of reads it answered");
			JENT_UT_EQ(jent_entropy_collector_reinitializations(ec),
				   ec->reinit_count,
				   "the accessor reports the reallocations");
			JENT_UT_EQ(jent_entropy_collector_flags(ec),
				   flags_before,
				   "and the flags stay the configured ones");
		}

		jent_entropy_collector_free(ec);
	}
}

/*
 * The reallocation replaces the instance, not its verdicts: a failed self test
 * stays failed on the replacement, whether it was bound before the recovery or
 * while the read that triggered it was in flight.
 */
static void test_selftest_verdict_survives_reset(void)
{
	struct rand_data *ec, *before;
	char buf[32];

	jent_ut_group("a failed self test survives the reallocation");

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the carried verdict", JENT_FORCE_FIPS);
		return;
	}

	before = ec;
	jent_atomic_store_int(&ec->selftest_failed, 1);
	if (jent_health_failure_reset(&ec, 0)) {
		JENT_UT_SKIP("the carried verdict",
			     "the reallocation did not succeed on this machine");
		jent_entropy_collector_free(ec);
		return;
	}
	JENT_UT_TRUE(ec != before, "the collector was replaced");
	JENT_UT_EQ(jent_atomic_load_int(&ec->selftest_failed), 1,
		   "and the replacement carries the failed verdict");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)), JENT_ERR_SELFTEST,
		   "so it delivers no output either");
	{
		char status[4096];

		JENT_UT_EQ(jent_status(ec, status, sizeof(status)), 0,
			   "its status is rendered");
		JENT_UT_TRUE(strstr(status, "\"selftestFailed\": true") != NULL,
			     "and reports the failed self test");
	}
	jent_entropy_collector_free(ec);

	if (!cb_registered) {
		JENT_UT_SKIP("a verdict set during the read",
			     "the callback could not be registered");
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("a verdict set during the read",
				     JENT_FORCE_FIPS);
		return;
	}

	before = ec;
	ec->health_failure = JENT_RCT_FAILURE;
	cb_fail_selftest = 1;
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_SELFTEST,
		   "a verdict set while the recovery was due stops the output");
	cb_fail_selftest = 0;
	JENT_UT_TRUE(ec != before, "after the collector was replaced");
	jent_entropy_collector_free(ec);
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
		JENT_UT_NO_COLLECTOR("recovery limit", 0);
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

	/*
	 * Permanent, not the intermittent JENT_ERR_RCT that asked for the
	 * recovery: that would tell the caller to try again, on a collector
	 * nothing can put back into service.
	 */
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_RCT_PERMANENT,
		   "an exhausted recovery is returned as a permanent failure");
	JENT_UT_EQ(ec->osr, (unsigned int)JENT_MAX_OSR,
		   "and the collector was left in place");
	JENT_UT_TRUE((ec->health_failure & JENT_RCT_FAILURE_PERMANENT) != 0,
		     "with the permanent failure raised on it");

	/* The verdict is final: later reads report it again. */
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_RCT_PERMANENT,
		   "the same failure is reported again");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   JENT_ERR_RCT_PERMANENT, "by jent_read_entropy as well");

	jent_entropy_collector_free(ec);
}

/*
 * A self test of the conditioning that fails during a recovery is no reason to
 * raise the oversampling rate: no rate mends it. The recovery stops at once
 * and reports it as what it is, and the instance stops for good, as after a
 * failed bound jent_selftest().
 */
static void test_recovery_selftest_failure(void)
{
	struct rand_data *ec, *before;
	unsigned int osr_before, reinits_before;
	char buf[32];

	jent_ut_group("a self test failing during recovery ends it");

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the failed recovery", JENT_FORCE_FIPS);
		return;
	}

	before = ec;
	osr_before = ec->osr;
	reinits_before = ec->reinit_count;
	ec->health_failure = JENT_RCT_FAILURE;

	ut_sha3_fail = 1;
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_SELFTEST,
		   "the failed self test is returned, not a health failure");
	ut_sha3_fail = 0;

	JENT_UT_TRUE(ec == before, "the collector was left in place");
	JENT_UT_EQ(ec->osr, osr_before,
		   "at the oversampling rate it had");
	JENT_UT_EQ(ec->reinit_count, reinits_before,
		   "without a reallocation");
	JENT_UT_EQ(jent_atomic_load_int(&ec->selftest_failed), 1,
		   "and marked as failed its self test");
	JENT_UT_EQ(ec->health_failure & JENT_PERMANENT_FAILURES, 0,
		   "with no permanent health failure made up for it");

	/* The verdict is final, with the self test passing again. */
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_SELFTEST, "later reads report it again");
	JENT_UT_EQ(jent_selftest(ec), EHASH,
		   "as does a self test run bound to the instance");

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

	/* Different rates, so the two collectors' cutoffs can be told apart. */
	old_ec = jent_entropy_collector_alloc(3, JENT_FORCE_FIPS);
	new_ec = jent_entropy_collector_alloc(4, JENT_FORCE_FIPS);
	if (!old_ec || !new_ec) {
		JENT_UT_NO_COLLECTOR("state duplication", JENT_FORCE_FIPS);
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

	/*
	 * APT: an observation window in progress is carried over, with the
	 * repetitions it holds - not primed at a cutoff, which would credit
	 * the window with repetitions it never saw (see jent_apt_duplicate()).
	 */
	old_ec->apt_base = 0xc0ffee;
	old_ec->apt_count = 27;
	old_ec->apt_observations = 42;
	old_ec->apt_base_set = 1;
	jent_apt_duplicate(new_ec, old_ec);
	JENT_UT_EQ(new_ec->apt_observations, 42,
		   "the APT window position is carried over");
	JENT_UT_EQ(new_ec->apt_base, 0xc0ffee, "with its base symbol");
	JENT_UT_EQ(new_ec->apt_count, 27,
		   "and the count of repetitions the window holds");
	JENT_UT_TRUE(new_ec->apt_count <= new_ec->apt_observations,
		     "which is no more than the window has observed");

	/*
	 * RCT with memory: primed at the OLD collector's intermittent cutoff,
	 * which the replacement's first window continues from. The window is
	 * closed until a block opens it, so the ->prev_time priming
	 * measurement ahead of the first block leaves the priming alone.
	 */
	JENT_UT_TRUE(old_ec->rct_mem_cutoff < new_ec->rct_mem_cutoff,
		     "the replacement's RCT-with-memory cutoff is the higher");
	new_ec->rct_mem_count = 0;
	new_ec->rct_mem_ctr = 0;
	jent_rct_mem_duplicate(new_ec, old_ec);
	JENT_UT_EQ(new_ec->rct_mem_count, old_ec->rct_mem_cutoff,
		   "the RCT with memory is primed at the old cutoff");
	JENT_UT_EQ(new_ec->rct_mem_ctr, new_ec->rct_mem_nosr,
		   "with the window closed");

	new_ec->health_failure = 0;
	new_ec->health_failure_reported = 0;
	jent_rct_mem_insert(new_ec, 1);
	JENT_UT_EQ(new_ec->rct_mem_count, old_ec->rct_mem_cutoff,
		   "a measurement before the first block counts nothing");

	/* The first window continues from the priming. */
	new_ec->rct_mem_ctr = 0;
	jent_rct_mem_insert(new_ec, 1);
	JENT_UT_EQ(new_ec->rct_mem_count, old_ec->rct_mem_cutoff + 1,
		   "the first window of the replacement counts on from it");
	JENT_UT_EQ(jent_health_failure(new_ec), 0,
		   "below its own cutoff, so the priming raises nothing");

	/* And only the first: the next one starts from zero. */
	new_ec->rct_mem_ctr = 0;
	jent_rct_mem_insert(new_ec, 1);
	JENT_UT_EQ(new_ec->rct_mem_count, 1,
		   "the window after it starts from zero again");

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


/*
 * The same state when the reallocation changed the clock, which the recovery
 * does on its own: the startup re-run at the raised OSR may fall back to
 * forcing the internal timer. The state built from delta values must not
 * follow to a different source; the state built from counters must. The stuck
 * test primes its reference from the new source: unjudged, those deltas leave
 * the RCT priming alone, where a zeroed reference would clear it.
 */
static void test_state_duplication_clock_change(void)
{
	struct rand_data *old_ec, *new_ec;

	jent_ut_group("a replacement that reads the other clock");

	old_ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	new_ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!old_ec || !new_ec) {
		JENT_UT_NO_COLLECTOR("the clock change", JENT_FORCE_FIPS);
		jent_entropy_collector_free(old_ec);
		jent_entropy_collector_free(new_ec);
		return;
	}

	old_ec->apt_base = 0xc0ffee;
	old_ec->apt_observations = 42;
	old_ec->apt_base_set = 1;
#ifdef JENT_HEALTH_LAG_PREDICTOR
	old_ec->lag_observations = 1234;
	JENT_LAG_HISTORY(old_ec, 0) = 0x1000;
	JENT_LAG_HISTORY(old_ec, 1) = 0x2000;
#else
	old_ec->last_delta = 0x1000;
	old_ec->last_delta2 = 0x2000;
#endif

	/* On one clock, everything is carried over as before. */
	old_ec->enable_notime = 0;
	new_ec->enable_notime = 0;
	new_ec->apt_base = 0;
	new_ec->apt_observations = 0;
	new_ec->rct_count = 0;
	jent_health_duplicate(new_ec, old_ec);
	JENT_UT_EQ(new_ec->apt_base, 0xc0ffee,
		   "two collectors on one clock carry the APT base over");
	JENT_UT_EQ(new_ec->apt_observations, 42, "with the window position");
	JENT_UT_EQ(new_ec->rct_count, new_ec->rct_cutoff,
		   "and the RCT primed at its intermittent cutoff");

	/*
	 * On one clock, but in another startup stage - as every FIPS / NTG.1
	 * replacement begins, with the memory access source alone, where the
	 * collector it replaces sampled the full noise source: the APT and
	 * lag state stays behind as it does across a clock change, and the
	 * stuck test primes its reference anew.
	 */
	new_ec->apt_base = 0;
	new_ec->apt_observations = 0;
	new_ec->apt_base_set = 0;
	new_ec->rct_count = 0;
#ifdef JENT_HEALTH_LAG_PREDICTOR
	new_ec->lag_observations = 0;
	new_ec->lag_delta_history[0] = 0;
#else
	new_ec->last_delta = 0;
	new_ec->last_delta2 = 0;
#endif
	new_ec->stuck_prime = 0;
	old_ec->startup_state = jent_startup_completed;
	new_ec->startup_state = jent_startup_memory;

	jent_health_duplicate(new_ec, old_ec);

	JENT_UT_EQ(new_ec->apt_base_set, 0,
		   "a replacement in another startup stage takes no APT base");
	JENT_UT_EQ(new_ec->apt_observations, 0,
		   "nor the window position of the other source");
	JENT_UT_EQ(new_ec->rct_count, new_ec->rct_cutoff,
		   "while the RCT is primed as ever");
#ifdef JENT_HEALTH_LAG_PREDICTOR
	JENT_UT_EQ(new_ec->lag_observations, 0,
		   "and the lag predictor starts on its own source");
#endif
	JENT_UT_EQ(new_ec->stuck_prime, JENT_STUCK_PRIME,
		   "as does the stuck test's reference");

	/*
	 * Which keeps the RCT priming: the priming deltas are not judged, and
	 * a source repeating its delta after them is stuck and counts on from
	 * the cutoff.
	 */
	jent_stuck(new_ec, 0x1000);
	jent_stuck(new_ec, 0x1000);
	JENT_UT_EQ(new_ec->rct_count, new_ec->rct_cutoff,
		   "the priming deltas leave the RCT count alone");
	jent_stuck(new_ec, 0x1000);
	JENT_UT_EQ(new_ec->rct_count, new_ec->rct_cutoff + 1,
		   "a repeated delta counts on from the priming");
	new_ec->health_failure = 0;
	new_ec->health_failure_reported = 0;
	new_ec->startup_state = jent_startup_completed;

	/*
	 * On the other, the counters still are, and the APT and lag state and
	 * the stuck test's reference are not.
	 */
	new_ec->apt_base = 0;
	new_ec->apt_observations = 0;
	new_ec->apt_base_set = 0;
	new_ec->rct_count = 0;
	new_ec->rct_mem_count = 0;
#ifdef JENT_HEALTH_LAG_PREDICTOR
	new_ec->lag_observations = 0;
	new_ec->lag_delta_history[0] = 0;
#else
	new_ec->last_delta = 0;
	new_ec->last_delta2 = 0;
#endif
	new_ec->stuck_prime = 0;
	new_ec->enable_notime = 1;

	jent_health_duplicate(new_ec, old_ec);

	JENT_UT_EQ(new_ec->apt_base, 0,
		   "a replacement on the other clock takes no APT base");
	JENT_UT_EQ(new_ec->apt_base_set, 0,
		   "and takes it from its own first measurement instead");
	JENT_UT_EQ(new_ec->apt_observations, 0,
		   "nor the window position of the other source");
	JENT_UT_EQ(new_ec->rct_count, new_ec->rct_cutoff,
		   "while the RCT is primed as ever - the priming is a cutoff, "
		   "not a measurement of the old clock");
	JENT_UT_EQ(new_ec->rct_mem_count, old_ec->rct_mem_cutoff,
		   "as is the RCT with memory, at the old collector's cutoff");
#ifdef JENT_HEALTH_LAG_PREDICTOR
	JENT_UT_EQ(new_ec->lag_observations, 0,
		   "the lag predictor starts on its own source");
#endif
	JENT_UT_EQ(new_ec->stuck_prime, JENT_STUCK_PRIME,
		   "and the stuck test primes its reference from it");

	/* Not left claiming a counting thread it never had. */
	new_ec->enable_notime = 0;

	jent_entropy_collector_free(old_ec);
	jent_entropy_collector_free(new_ec);
}


/*
 * A clock nothing has measured has no common divisor, and an instance that
 * would generate from it is refused rather than given an invented one. Only
 * the instances that do the measuring - the startup's own collector and the
 * raw noise recording - run without one. A caller cannot claim to be one: it
 * is an argument of the internal allocation, not a flag.
 */
static void test_alloc_needs_a_measured_clock(void)
{
	struct rand_data *ec;
	uint64_t divisor;
	int saved_selftest_run;

	jent_ut_group("an instance whose clock nothing has measured");

	if (jent_gcd_get(&divisor, JENT_GCD_CLOCK_PLATFORM)) {
		JENT_UT_SKIP("the unmeasured clock",
			     "no divisor was established on this machine");
		return;
	}

	/*
	 * Pinned, so that the allocations below skip the startup - which would
	 * establish the divisor again and defeat the check.
	 */
	saved_selftest_run =
		jent_atomic_load_int(&jent_selftest_run[JENT_CLOCK_PLATFORM]);
	jent_atomic_store_int(&jent_selftest_run[JENT_CLOCK_PLATFORM], 1);
	jent_atomic_store_u32(&jent_common_timer_gcd[JENT_GCD_CLOCK_PLATFORM],
			      0);

	ec = jent_entropy_collector_alloc_internal(JENT_MIN_OSR, 0, 0, 0);
	JENT_UT_TRUE(ec == NULL, "the allocation is refused");
	jent_entropy_collector_free(ec);

	ec = jent_entropy_collector_alloc(JENT_MIN_OSR, 0);
	JENT_UT_TRUE(ec == NULL, "the public allocation just as much");
	jent_entropy_collector_free(ec);

	ec = jent_entropy_collector_alloc_internal(JENT_MIN_OSR, 0, 0, 1);
	JENT_UT_TRUE(ec != NULL, "the instance that measures the clock is not");
	if (ec)
		JENT_UT_EQ(ec->jent_common_timer_gcd, 1,
			   "and takes the deltas as the clock produces them");
	jent_entropy_collector_free(ec);

	jent_atomic_store_u32(&jent_common_timer_gcd[JENT_GCD_CLOCK_PLATFORM],
			      (uint32_t)divisor);
	jent_atomic_store_int(&jent_selftest_run[JENT_CLOCK_PLATFORM],
			      saved_selftest_run);
}

/* And a compliance-mode instance is not moved to the other clock at all. */
static void test_recovery_pins_the_clock(void)
{
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
	struct rand_data *ec, *before;
	unsigned int flags;

	jent_ut_group("the recovery of a compliance-mode instance keeps its clock");

	if (!jent_ut_memlock_available()) {
		JENT_UT_SKIP("the pinned clock",
			     "this machine locks too little memory (RLIMIT_MEMLOCK)");
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec || ec->enable_notime) {
		JENT_UT_SKIP("the pinned clock",
			     "no compliance-mode collector on the platform "
			     "clock could be built here");
		jent_entropy_collector_free(ec);
		return;
	}

	before = ec;
	flags = ec->flags;
	JENT_UT_EQ(jent_health_failure_reset(&ec, 0), 0,
		   "the reallocation succeeds");
	JENT_UT_TRUE(ec != before, "with a replacement");
	JENT_UT_EQ(ec->enable_notime, 0, "still on the platform clock");

	/*
	 * Pinned for the reset only: the replacement keeps the flags the
	 * caller configured, which jent_status() reports, and the next reset
	 * pins the clock afresh.
	 */
	JENT_UT_EQ(ec->flags, flags,
		   "with the caller's flags, not the pin the reset used");
	before = ec;
	JENT_UT_EQ(jent_health_failure_reset(&ec, 0), 0,
		   "a second reallocation succeeds");
	JENT_UT_TRUE(ec != before && !ec->enable_notime,
		     "on the platform clock again");
	jent_entropy_collector_free(ec);
#else
	jent_ut_group("the recovery of a compliance-mode instance keeps its clock");
	JENT_UT_SKIP("the pinned clock", "the internal timer is not compiled in");
#endif
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
		JENT_UT_NO_COLLECTOR("jent_status", 0);
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
		JENT_UT_NO_COLLECTOR("recovery", JENT_MAX_MEMSIZE_1MB);
		return;
	}

	ec->is_fips_enabled = 1;

	JENT_UT_TRUE(JENT_FLAGS_TO_MAX_MEMSIZE(ec->flags),
		     "the collector keeps the size the caller configured");
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
	JENT_UT_TRUE(JENT_FLAGS_TO_MAX_MEMSIZE(ec->flags),
		     "as is the fact that they chose it");

	jent_entropy_collector_free(ec);
}

/*
 * The recovery of jent_read_entropy_safe() carries the old collector's state
 * into the replacement before the replacement's startup runs, not after it:
 * the startup's health tests continue from the old ones, and a failure they
 * report during it is reported for the instance the caller holds.
 *
 * Made deterministic through the RCT with memory: an old intermittent cutoff
 * above every cutoff of the replacement primes the replacement's first
 * window past its permanent cutoff, so the first block of its startup fails
 * permanently. Duplicated only after the startup, as the recovery used to,
 * the startup ran on fresh tests and passed.
 */
static void test_safe_reset_carries_state_into_startup(void)
{
	struct rand_data *ec, *before;
	char uuid_before[JENT_UUID_STRLEN];
	uint64_t bytes_before, reads_before;
	unsigned int osr_before, i;
	int seen = 0;
	char buf[32];
	ssize_t ret;

	jent_ut_group("the recovery carries the state into the replacement's startup");

	if (!cb_registered) {
		JENT_UT_SKIP("the carried state",
			     "the callback could not be registered");
		return;
	}
	if (!jent_ut_memlock_available()) {
		JENT_UT_SKIP("the carried state",
			     "this machine locks too little memory (RLIMIT_MEMLOCK)");
		return;
	}

	ec = jent_entropy_collector_alloc(0, JENT_FORCE_FIPS);
	if (!ec) {
		JENT_UT_NO_COLLECTOR("the carried state", JENT_FORCE_FIPS);
		return;
	}

	/* An identity and a history for the replacement to carry. */
	if (jent_read_entropy_safe(&ec, buf, sizeof(buf)) !=
	    (ssize_t)sizeof(buf)) {
		JENT_UT_SKIP("the carried state",
			     "the noise source did not converge on this machine");
		jent_entropy_collector_free(ec);
		return;
	}
	JENT_UT_TRUE(ec->uuid[0] != '\0', "the collector has an identifier");
	memcpy(uuid_before, ec->uuid, sizeof(uuid_before));
	bytes_before = ec->bytes_output;
	reads_before = ec->read_invocations;
	osr_before = ec->osr;
	before = ec;

	/*
	 * Above every cutoff of the replacement, with room to count on: the
	 * first measurement it judges may be stuck, and at USHRT_MAX the count
	 * would wrap to zero instead of reaching the permanent cutoff.
	 */
	ec->rct_mem_cutoff = USHRT_MAX / 2;
	ec->health_failure = JENT_APT_FAILURE;

	cb_log_len = 0;
	ret = jent_read_entropy_safe(&ec, buf, sizeof(buf));

	/*
	 * Findings of the replacement's startup: the callback saw it fail,
	 * permanently, on an instance bearing the caller's identity and totals.
	 */
	for (i = 0; i < cb_log_len; i++) {
		if (!(cb_log[i].failure & JENT_RCT_MEM_FAILURE_PERMANENT))
			continue;
		if (seen++)
			continue;

		JENT_UT_TRUE(!memcmp(cb_log[i].uuid, uuid_before,
				     sizeof(uuid_before)),
			     "the callback during the replacement's startup "
			     "sees the caller's UUID");
		JENT_UT_EQ(cb_log[i].bytes_output, bytes_before,
			   "and the caller's output total");
		JENT_UT_EQ(cb_log[i].read_invocations, reads_before,
			   "and the caller's read count");
	}
	JENT_UT_TRUE(seen,
		     "the primed RCT with memory failed the replacement's startup");
	JENT_UT_EQ(seen, 1,
		   "and the callback hears of it once, not again when the "
		   "caller's collector takes the same failure over");

	/*
	 * A permanent failure ends that startup, and is what the caller is
	 * told - not retried at the next rate, and not turned back into the
	 * intermittent failure that asked for the recovery.
	 */
	JENT_UT_EQ(ret, JENT_ERR_RCT_MEM_PERMANENT,
		   "the replacement's permanent failure is returned");
	JENT_UT_TRUE(ec == before, "the caller keeps the collector it had");
	JENT_UT_EQ(ec->osr, osr_before, "unchanged");
	/*
	 * Reported as that same failure from then on, as the API promises -
	 * not as the permanent counterpart of its own intermittent failure,
	 * which would tell the caller another story on every later call.
	 */
	JENT_UT_EQ(jent_read_entropy_safe(&ec, buf, sizeof(buf)),
		   JENT_ERR_RCT_MEM_PERMANENT,
		   "and out of service for good, with the failure it returned");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
		   JENT_ERR_RCT_MEM_PERMANENT, "for a plain read as well");
	JENT_UT_TRUE(ec == before, "without another recovery attempt");

	jent_entropy_collector_free(ec);
}

/*
 * The memory size derived from the cache. A cache too small to yield even the
 * smallest size the field expresses is taken as unknown, and the reallocation
 * ladder then grows from the default - it used to count up from zero, i.e.
 * shrink the region to 1 kB, 2 kB, ...
 */
static void test_derived_memsize(void)
{
	unsigned int def = JENT_DEFAULT_MEMORY_BITS - JENT_MAX_MEMSIZE_OFFSET;
	unsigned int inc;
	static const uint64_t tiny[] = { 0, 1, 2, 64, 128 };
	size_t i;

	jent_ut_group("the memory size derived from the cache size");

	for (i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
		JENT_UT_EQ(jent_derive_memsize(tiny[i], 0, 0), def,
			   "a cache of 128 bytes or less is taken as unknown");
		JENT_UT_EQ(jent_derive_memsize(tiny[i], 1, 0), def,
			   "with JENT_CACHE_ALL as well");
	}

	for (inc = 1; inc <= 3; inc++) {
		unsigned int expect = def + inc;

		if (expect > JENT_MAX_AUTO_MEMSIZE)
			expect = JENT_MAX_AUTO_MEMSIZE;
		JENT_UT_EQ(jent_derive_memsize(128, 0, inc), expect,
			   "and a reallocation grows from the default");
	}

	/* A real L1 is unchanged: 32 kB, four-fold, is 128 kB. */
	JENT_UT_EQ(jent_derive_memsize(32768, 0, 0),
		   JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_128kB) +
			JENT_CACHE_SHIFT_BITS,
		   "a 32 kB L1 still derives 128 kB");
	JENT_UT_EQ(jent_derive_memsize(32768, 0, 1),
		   JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_256kB) +
			JENT_CACHE_SHIFT_BITS,
		   "and grows by one step per reallocation");
}

/* Flag bits this version does not define are refused, not ignored. */
static void test_reserved_flags(void)
{
	static const unsigned int valid =
		JENT_DISABLE_STIR | JENT_DISABLE_UNBIAS |
		JENT_DISABLE_MEMORY_ACCESS | JENT_FORCE_INTERNAL_TIMER |
		JENT_DISABLE_INTERNAL_TIMER | JENT_FORCE_FIPS | JENT_NTG1 |
		JENT_CACHE_ALL | JENT_FORCE_SECURE_MEM | JENT_MAX_HASHLOOP |
		JENT_MAX_MEMSIZE_MAX;
	unsigned int bit;

	jent_ut_group("reserved flag bits");

	JENT_UT_EQ(jent_flags_invalid(valid), 0,
		   "every defined flag and the largest fields are accepted");
	JENT_UT_EQ(jent_flags_invalid(0), 0, "as are no flags at all");

	for (bit = 9; bit <= 22; bit++) {
		if (!jent_flags_invalid(1U << bit)) {
			JENT_UT_FAIL("reserved bit %u is accepted", bit);
			return;
		}
	}
	jent_ut_checks++;

	JENT_UT_NE(jent_flags_invalid(JENT_MAX_MEMSIZE_TO_FLAGS(
			JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX) + 1)), 0,
		   "a memory size field above JENT_MAX_MEMSIZE_MAX is refused");
	JENT_UT_NE(jent_flags_invalid(JENT_MAX_MEMSIZE_MASK), 0,
		   "up to the top of the field");
	JENT_UT_NE(jent_flags_invalid(JENT_HASHLOOP_TO_FLAGS(
			JENT_FLAGS_TO_HASHLOOP(JENT_MAX_HASHLOOP) + 1)), 0,
		   "a hash loop field above JENT_MAX_HASHLOOP is refused");
	JENT_UT_NE(jent_flags_invalid(JENT_MAX_HASHLOOP_MASK), 0,
		   "up to the top of that field");

	JENT_UT_EQ(jent_entropy_init_ex(0, 1U << 9), EPROGERR,
		   "jent_entropy_init_ex refuses a reserved bit with EPROGERR");
	JENT_UT_EQ(jent_entropy_init_ex(0, JENT_MAX_MEMSIZE_MASK), EPROGERR,
		   "and an out-of-range memory size");
	JENT_UT_EQ(jent_entropy_init_ex(0, JENT_MAX_HASHLOOP_MASK), EPROGERR,
		   "and an out-of-range hash loop count");
	JENT_UT_TRUE(jent_entropy_collector_alloc(0, 1U << 22) == NULL,
		     "jent_entropy_collector_alloc refuses a reserved bit");
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
	test_safe_reset_carries_state_into_startup();
	test_selftest_verdict_survives_reset();
	test_recovery_gives_up();
	test_recovery_selftest_failure();
	test_recovery_keeps_caller_memsize();
	test_state_duplication();
	test_state_duplication_clock_change();
	test_alloc_needs_a_measured_clock();
	test_status_both_arms();
	test_derived_memsize();
	test_reserved_flags();
	test_failure_callback();

	/*
	 * Last, because it forces the internal timer for the rest of the
	 * process - every collector allocated afterwards would come back
	 * driving a counting thread.
	 */
	test_recovery_pins_the_clock();

	return jent_ut_report("unit-error");
}
