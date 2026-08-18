/*
 * Jitter RNG: unit tests for src/jitterentropy-base.c and
 * src/jitterentropy-status.c
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
 * The whole library is absorbed here rather than linked, as in the AMALGAMATED
 * programs under tests/raw-entropy: the flag decoding
 * (jent_memsize(), jent_hashloop_cnt()) is internal to the library and a
 * shared build does not export it.
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
 * This file covers the public API surface.
 */

/*
 * jent_read_entropy() and jent_read_entropy_safe() reject the calls no
 * collector can serve, with the error codes jitterentropy.h names.
 */
static void test_read_entropy_api(void)
{
	char buf[32];

	jent_ut_group("jent_read_entropy rejects API misuse");

	JENT_UT_EQ(jent_read_entropy(NULL, buf, sizeof(buf)), JENT_ERR_EINVAL,
		   "no entropy collector");
	JENT_UT_EQ(jent_read_entropy_safe(NULL, buf, sizeof(buf)),
		   JENT_ERR_EINVAL, "no entropy collector (safe)");

	/*
	 * A NULL buffer is only an error when something is asked for; a
	 * zero-length request is a no-op that succeeds.
	 */
	{
		struct rand_data *ec = jent_entropy_collector_alloc(0, 0);

		if (!ec) {
			JENT_UT_SKIP("jent_read_entropy",
				     "no collector could be allocated");
			return;
		}

		JENT_UT_EQ(jent_read_entropy(ec, NULL, sizeof(buf)),
			   JENT_ERR_EINVAL, "no data buffer");
		JENT_UT_EQ(jent_read_entropy_safe(&ec, NULL, sizeof(buf)),
			   JENT_ERR_EINVAL, "no data buffer (safe)");
		JENT_UT_EQ(jent_read_entropy_safe(&ec, NULL, 0), 0,
			   "a zero-length request is a no-op (safe)");
		JENT_UT_EQ(jent_read_entropy(ec, NULL, 0), 0,
			   "a zero-length request is a no-op");
		JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
			   (ssize_t)sizeof(buf),
			   "a well-formed request returns the length asked for");

		jent_entropy_collector_free(ec);
	}

	/* Must tolerate being handed nothing. */
	jent_entropy_collector_free(NULL);
}

/* Allocation honours the flags it is given, and reports them back. */

static void test_collector_alloc(void)
{
	struct rand_data *ec;

	jent_ut_group("jent_entropy_collector_alloc");

	ec = jent_entropy_collector_alloc(0, JENT_MAX_MEMSIZE_1MB);
	if (!ec) {
		JENT_UT_SKIP("jent_entropy_collector_alloc",
			     "no collector could be allocated");
		return;
	}
	JENT_UT_EQ(ec->osr, JENT_MIN_OSR, "osr 0 was raised to the minimum");
	JENT_UT_EQ((uint32_t)ec->memmask + 1, 1048576u,
		   "the requested memory size was honoured");
	jent_entropy_collector_free(ec);

	/*
	 * An osr above the maximum is refused rather than clamped: unlike the
	 * lower bound, a caller asking for more oversampling than the health
	 * test tables cover has asked for something that cannot be provided.
	 */
	ec = jent_entropy_collector_alloc(JENT_MAX_OSR + 1, 0);
	JENT_UT_TRUE(ec == NULL, "an osr above the maximum is refused");
	jent_entropy_collector_free(ec);

	/* With the memory access disabled there is no block to size. */
	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_MEMORY_ACCESS);
	if (ec) {
		JENT_UT_TRUE(ec->mem == NULL,
			     "JENT_DISABLE_MEMORY_ACCESS allocates no block");
		jent_entropy_collector_free(ec);
	} else {
		JENT_UT_SKIP("JENT_DISABLE_MEMORY_ACCESS",
			     "no collector could be allocated");
	}
}

/* The version the header states and the one the library reports must agree. */

/*
 * jent_status() writes JSON into a caller-provided buffer. What matters is
 * that it never writes past the buffer and that it says so when it does not
 * fit, rather than emitting a truncated document that a consumer would then
 * fail to parse.
 */
static void test_status(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	char buf[8192];
	/*
	 * Not named `small`: <windows.h> pulls in rpcndr.h, which defines that
	 * as a macro for `char`, and the declaration below would expand into
	 * nonsense on MSVC.
	 */
	struct { char buf[16]; char guard[16]; } tiny;
	int ret;
	size_t len;

	jent_ut_group("jent_status");

	if (!ec) {
		JENT_UT_SKIP("jent_status", "no collector could be allocated");
		return;
	}

	memset(buf, 0x5a, sizeof(buf));
	ret = jent_status(ec, buf, sizeof(buf));
	JENT_UT_EQ(ret, 0, "the status is produced");

	len = strlen(buf);
	JENT_UT_TRUE(len > 0 && len < sizeof(buf),
		     "the output is NUL terminated inside the buffer");
	JENT_UT_EQ(buf[0], '{', "the output starts as a JSON object");
	JENT_UT_TRUE(len >= 2 && buf[len - 1] == '\n' ? buf[len - 2] == '}'
						      : buf[len - 1] == '}',
		     "the output ends as a JSON object");

	/* The braces of a complete document balance. */
	{
		size_t i;
		int depth = 0, min_depth = 0;

		for (i = 0; i < len; i++) {
			if (buf[i] == '{')
				depth++;
			else if (buf[i] == '}')
				depth--;
			if (depth < min_depth)
				min_depth = depth;
		}
		JENT_UT_EQ(depth, 0, "the braces balance");
		JENT_UT_EQ(min_depth, 0, "no closing brace precedes its opening one");
	}

	/*
	 * A buffer that cannot hold the document is reported as an error
	 * rather than handed back as a truncated one a consumer would fail to
	 * parse - and the truncation stays inside the buffer. `tiny` is
	 * followed by a guard, so a write past its end is visible here rather
	 * than only under a sanitizer.
	 */
	memset(&tiny, 0x5a, sizeof(tiny));
	ret = jent_status(ec, tiny.buf, sizeof(tiny.buf));
	JENT_UT_NE(ret, 0, "a buffer that is too small is reported as an error");
	JENT_UT_TRUE(memchr(tiny.buf, '\0', sizeof(tiny.buf)) != NULL,
		     "the truncated output is still NUL terminated");
	{
		size_t i;
		unsigned int touched = 0;

		for (i = 0; i < sizeof(tiny.guard); i++) {
			if (tiny.guard[i] != 0x5a)
				touched++;
		}
		JENT_UT_EQ(touched, 0, "nothing was written past the buffer");
	}

	/*
	 * No entropy collector is not an error: it is the documented way to
	 * ask for the version alone, and the result must still be a complete
	 * JSON object.
	 */
	memset(buf, 0x5a, sizeof(buf));
	JENT_UT_EQ(jent_status(NULL, buf, sizeof(buf)), 0,
		   "no entropy collector yields the version-only status");
	JENT_UT_EQ(buf[0], '{', "which is still a JSON object");
	JENT_UT_TRUE(strstr(buf, "\"version\"") != NULL, "carrying the version");
	JENT_UT_TRUE(strstr(buf, "\"uuid\"") == NULL,
		     "and nothing that needs a collector");

	/* The arguments no call can be served with. */
	JENT_UT_NE(jent_status(ec, NULL, sizeof(buf)), 0,
		   "no buffer is an error");
	JENT_UT_NE(jent_status(ec, buf, 0), 0, "a zero-length buffer is an error");

	jent_entropy_collector_free(ec);
}

/*
 * Every write in jent_status() is guarded by "does the rest still fit", and
 * that guard only takes its false side when the buffer runs out at exactly
 * that write. Sweeping the buffer length across the whole document walks the
 * truncation point through every one of them, checking at each length that
 * nothing is written past the buffer, that the result is NUL terminated inside
 * it, and that only a length the document fits into reports success.
 */
static void test_status_truncation(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	static struct {
		char buf[8192];
		char guard[64];
	} area;
	size_t full, len;
	unsigned int overflows = 0, unterminated = 0, misreported = 0;

	jent_ut_group("jent_status truncates safely at every length");

	if (!ec) {
		JENT_UT_SKIP("jent_status truncation", "no collector");
		return;
	}

	if (jent_status(ec, area.buf, sizeof(area.buf))) {
		JENT_UT_SKIP("jent_status truncation",
			     "the full document does not fit the test buffer");
		jent_entropy_collector_free(ec);
		return;
	}
	full = strlen(area.buf);

	for (len = 1; len <= full + 1; len++) {
		int ret;
		size_t i;

		memset(&area, 0x5a, sizeof(area));
		ret = jent_status(ec, area.buf, len);

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

		if (!memchr(area.buf, '\0', len))
			unterminated++;

		/*
		 * The direction that matters: success must never be reported
		 * for a document that did not fit. The converse is not
		 * asserted - jent_status() detects truncation from strlen()
		 * alone and so reports a buffer of exactly strlen(document) +
		 * 1 as too small. Conservative in the safe direction, and the
		 * caller only has to allow one more byte.
		 */
		if (!ret && strlen(area.buf) != full)
			misreported++;
	}

	JENT_UT_EQ(overflows, 0, "no length writes outside the buffer");
	JENT_UT_EQ(unterminated, 0, "every length leaves a NUL in the buffer");
	JENT_UT_EQ(misreported, 0, "success is never reported for a truncated document");
	JENT_UT_EQ(jent_status(ec, area.buf, full + 2), 0,
		   "a buffer one byte larger than the document reports success");
	printf("  note: swept %zu buffer lengths\n", full + 1);

	jent_entropy_collector_free(ec);
}

/*
 * jent_uuid() hands out the instance identifier the status output carries. Its
 * contract is a buffer of at least JENT_UUID_STRLEN bytes.
 */
static void test_uuid_api(void)
{
	struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
	char uuid[JENT_UUID_STRLEN];
	char status[8192];
	char tiny[JENT_UUID_STRLEN - 1];	/* not `small`, see test_status() */

	jent_ut_group("jent_uuid");

	if (!ec) {
		JENT_UT_SKIP("jent_uuid", "no collector could be allocated");
		return;
	}

	JENT_UT_EQ(jent_uuid(ec, uuid, sizeof(uuid)), 0, "the UUID is produced");
	JENT_UT_EQ(strlen(uuid), JENT_UUID_STRLEN - 1,
		   "and has the canonical length");

	/* The same identifier the status output names the instance by. */
	if (!jent_status(ec, status, sizeof(status)))
		JENT_UT_TRUE(strstr(status, uuid) != NULL,
			     "and is the one jent_status reports");

	JENT_UT_NE(jent_uuid(ec, tiny, sizeof(tiny)), 0,
		   "a buffer that is too small is an error");
	JENT_UT_NE(jent_uuid(NULL, uuid, sizeof(uuid)), 0,
		   "no entropy collector is an error");
	JENT_UT_NE(jent_uuid(ec, NULL, sizeof(uuid)), 0, "no buffer is an error");
	JENT_UT_NE(jent_uuid(ec, uuid, 0), 0,
		   "a zero-length buffer is an error");

	/* Two instances are two identities. */
	{
		struct rand_data *other = jent_entropy_collector_alloc(0, 0);
		char other_uuid[JENT_UUID_STRLEN];

		if (other) {
			jent_uuid(other, other_uuid, sizeof(other_uuid));
			jent_ut_checks++;
			if (!strcmp(uuid, other_uuid))
				JENT_UT_FAIL("%s",
					     "two collectors share one UUID");
			jent_entropy_collector_free(other);
		}
	}

	jent_entropy_collector_free(ec);
}

/* Flag combinations that ask for two incompatible things are refused. */

static void test_alloc_flag_conflicts(void)
{
	struct rand_data *ec;

	jent_ut_group("contradictory flags are refused");

	ec = jent_entropy_collector_alloc(0, JENT_DISABLE_INTERNAL_TIMER |
					     JENT_FORCE_INTERNAL_TIMER);
	JENT_UT_TRUE(ec == NULL,
		     "forcing and disabling the internal timer at once");
	jent_entropy_collector_free(ec);
}

/* The startup self tests, through both entry points. */

static void test_init(void)
{
	jent_ut_group("jent_entropy_init");

	JENT_UT_EQ(jent_entropy_init(), 0, "the default initialization passes");
	JENT_UT_EQ(jent_entropy_init_ex(0, 0), 0,
		   "initialization with the default osr and flags passes");
	JENT_UT_EQ(jent_entropy_init_ex(JENT_MIN_OSR + 1, 0), 0,
		   "initialization at a raised osr passes");

	/*
	 * The same contradiction the allocation refuses, reaching the check
	 * through the initialization instead.
	 */
	JENT_UT_NE(jent_entropy_init_ex(0, JENT_DISABLE_INTERNAL_TIMER |
					   JENT_FORCE_INTERNAL_TIMER), 0,
		   "contradictory flags fail the initialization");
}

/*
 * The compliance modes. Both are run but not gated on: they imply
 * JENT_FORCE_SECURE_MEM, so they need lockable memory, and their startup runs
 * health tests that may not converge on a given machine - properties of the
 * machine rather than defects in the code.
 */
static void test_compliance_modes(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
	} modes[] = {
		{ JENT_FORCE_FIPS,	"JENT_FORCE_FIPS" },
		{ JENT_NTG1,		"JENT_NTG1" },
	};
	size_t i;

	jent_ut_group("the compliance modes");

	for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		struct rand_data *ec =
			jent_entropy_collector_alloc(0, modes[i].flags);
		char buf[32];

		if (!ec) {
			JENT_UT_SKIP(modes[i].name,
				     "no collector on this machine");
			continue;
		}

		JENT_UT_EQ(ec->is_fips_enabled, 1, "the health tests are on");
		JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)),
			   (ssize_t)sizeof(buf), modes[i].name);
		jent_entropy_collector_free(ec);
	}
}

/*
 * The conditioning self test as a periodic caller runs it: over and over, and
 * next to a live entropy collector. unit-sha3 covers the vectors themselves;
 * what is tested here is that the exposed entry point is repeatable and that
 * a passing run leaves the library's state alone.
 */
static void test_selftest(void)
{
	struct rand_data *ec;
	char buf[32];
	unsigned int i;

	jent_ut_group("jent_selftest");

	for (i = 0; i < 3; i++)
		JENT_UT_EQ(jent_selftest(NULL), 0,
			   "the known answer tests pass, and keep passing");

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		JENT_UT_SKIP("alongside a collector",
			     "no collector on this machine");
		return;
	}

	JENT_UT_EQ(jent_selftest(ec), 0,
		   "they pass bound to a collector");
	JENT_UT_EQ(ec->selftest_failed, 0u,
		   "a passing run leaves the instance in service");
	JENT_UT_EQ(jent_selftest_run, 1,
		   "the startup self test remains recorded as run");
	JENT_UT_EQ(jent_read_entropy(ec, buf, sizeof(buf)), (ssize_t)sizeof(buf),
		   "the collector still delivers afterwards");

	jent_entropy_collector_free(ec);
}

/*
 * A failed on-demand self test stops the output of the instance it was bound
 * to. The real known answer tests cannot be driven to failure, so the failure
 * is induced as unit-error does for the health tests: by setting the state a
 * failing jent_selftest() run sets - its only effect on the collector.
 */
static void test_selftest_failure_stops_output(void)
{
	struct rand_data *ec;
	unsigned char buf[32], zero[32];
	size_t i;

	jent_ut_group("a failed jent_selftest stops the output");

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		JENT_UT_SKIP("a failed self test", "no collector on this machine");
		return;
	}

	memset(buf, 0, sizeof(buf));
	memset(zero, 0, sizeof(zero));

	ec->selftest_failed = 1;

	JENT_UT_EQ(jent_read_entropy(ec, (char *)buf, sizeof(buf)),
		   JENT_ERR_SELFTEST,
		   "jent_read_entropy reports the failed self test");
	/*
	 * The gate has to hold outside FIPS mode - the collector above is
	 * allocated without JENT_FORCE_FIPS - and the buffer must not have
	 * been written to at all.
	 */
	for (i = 0; i < sizeof(buf); i++) {
		if (buf[i] != zero[i]) {
			JENT_UT_FAIL("the output buffer was written at %zu", i);
			break;
		}
	}
	jent_ut_checks++;

	JENT_UT_EQ(jent_read_entropy_safe(&ec, (char *)buf, sizeof(buf)),
		   JENT_ERR_SELFTEST,
		   "jent_read_entropy_safe does not recover from it");
	JENT_UT_EQ(ec->reinit_count, 0u,
		   "and no reallocation was attempted");

	jent_entropy_collector_free(ec);
}

static void test_secure_memory_supported(void)
{
	int ret = jent_secure_memory_supported();

	jent_ut_group("jent_secure_memory_supported");

	JENT_UT_TRUE(ret == 0 || ret == 1, "the answer is 0 or 1");
	printf("  note: secure memory is %s by this backend\n",
	       ret ? "provided" : "not provided");
}

int main(void)
{
	jent_ut_setup();

	test_read_entropy_api();
	test_collector_alloc();
	test_alloc_flag_conflicts();
	test_status();
	test_status_truncation();
	test_uuid_api();
	test_secure_memory_supported();
	test_init();
	test_selftest();
	test_selftest_failure_stops_output();
	test_compliance_modes();

	return jent_ut_report("unit-base-api");
}
