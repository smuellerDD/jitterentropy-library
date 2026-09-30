/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test tool calling every function that jitterentropy.h declares.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
 *
 * This program is built against an *installed* library rather than from
 * inside this tree: it includes <jitterentropy.h> and links the library, and
 * nothing else. That is what makes it a linkage test - it fails when the
 * installed header, the exported symbols and the link dependencies recorded
 * in the CMake package and the pkg-config file do not add up to something a
 * consumer can build.
 *
 * It is built twice, once against the shared library and once against the
 * static one, because the two differ in exactly the places that break
 * quietly: the dllimport/dllexport decoration the public header selects on
 * Windows, the symbol visibility on ELF and Mach-O (the library is compiled
 * with -fvisibility=hidden, so a declaration that lost its JENT_PRIVATE_STATIC
 * links in the static case and not in the shared one), and the transitive
 * dependencies - bcrypt, pthread, the stack-protector runtime - that only a
 * static link has to name for itself.
 *
 * Every declared function is called, so that a symbol dropped from the export
 * set fails the link rather than going unnoticed until a consumer hits it.
 * Only outcomes that are properties of the machine rather than of the linkage
 * are tolerated; each of those is commented where it is allowed.
 *
 * Usage: jitterentropy-linktest
 */

#include <jitterentropy.h>

/*
 * jitterentropy.h happens to include these itself, but a consumer does not get
 * to rely on that, and this program is written the way a consumer would be.
 */
#include <stdio.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
# include <sys/resource.h>
#endif

/* Set by the callback registered with jent_set_fips_failure_callback(). */
static unsigned int fips_failure_seen;

static void fips_failure(struct rand_data *ec, unsigned int health_failure)
{
	(void)ec;
	fips_failure_seen = health_failure;
}

/*
 * Whether this process can lock the few pages a collector needs where secure
 * memory is forced. RLIMIT_MEMLOCK where there is one; elsewhere assumed.
 */
static int memlock_available(void)
{
#ifdef RLIMIT_MEMLOCK
	struct rlimit rl;

	if (getrlimit(RLIMIT_MEMLOCK, &rl))
		return 1;
	return rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur >= 16384;
#else
	return 1;
#endif
}

/*
 * Whether @ret, a startup self test that did not pass, is this machine's
 * verdict rather than the library's: a timer the startup rejects, or memory
 * it cannot lock. Secure memory is forced by the system's FIPS mode, which a
 * consumer has no call to ask the library about, so an EMEM is taken as the
 * machine's where the process cannot lock memory at all - and fails where it
 * can, as it is then a failed malloc(). EHASH and EGCD are the library's own
 * self tests and fail. The same verdicts as JENT_UT_MACHINE_VERDICT in
 * tests/unit/unit.h.
 */
static int machine_verdict(int ret)
{
	return ret == ENOTIME || ret == ECOARSETIME || ret == ENOMONOTONIC ||
	       ret == EMINVARVAR || ret == ESTUCK || ret == EHEALTH ||
	       ret == ERCT || (ret == EMEM && !memlock_available());
}

/*
 * Whether @rc, a read that did not deliver, is a health test failure: the
 * documented behaviour of FIPS mode on any machine, intermittently, and on a
 * noise source that keeps failing them for good. Only where the collector is
 * in FIPS mode - which implies JENT_FORCE_SECURE_MEM, and its flags say so;
 * outside it a read reports none.
 */
static int health_verdict(struct rand_data *ec, ssize_t rc)
{
	if (!(jent_entropy_collector_flags(ec) & JENT_FORCE_SECURE_MEM))
		return 0;
	return rc == JENT_ERR_RCT || rc == JENT_ERR_APT ||
	       rc == JENT_ERR_LAG || rc == JENT_ERR_RCT_MEM ||
	       rc == JENT_ERR_RCT_PERMANENT || rc == JENT_ERR_APT_PERMANENT ||
	       rc == JENT_ERR_LAG_PERMANENT || rc == JENT_ERR_RCT_MEM_PERMANENT;
}

#define FAIL(...)						\
	do {							\
		fprintf(stderr, "FAILED: " __VA_ARGS__);	\
		fputc('\n', stderr);				\
		return 1;					\
	} while (0)

int main(void)
{
	struct rand_data *ec;
	void *notime_ctx = NULL;
	char status[4096];
	char uuid[JENT_UUID_STRLEN];
	char data[32];
	unsigned int version;
	ssize_t rc;
	int ret, no_startup;

	version = jent_version();
	printf("jent_version: %u\n", version);

	/*
	 * The header this was compiled against and the library it was linked
	 * against must be the same release. Only an installed tree can get
	 * this wrong - a header left behind by a previous install, or a
	 * library resolved from a different prefix at run time - which is why
	 * the check lives here and not in a tool built inside this tree.
	 */
	if (version != JENT_VERSION)
		FAIL("jent_version reports %u, the header says %u",
		     version, (unsigned int)JENT_VERSION);

	/*
	 * The three configuration calls below have to come before
	 * jent_entropy_init*(), which blocks any further switching.
	 *
	 * A null handler is rejected rather than installed: the point is to
	 * call the symbol, and a handler built from the two thread helpers
	 * this header exports would still lack the start and stop callbacks,
	 * leaving the internal timer without a counting thread.
	 */
	ret = jent_entropy_switch_notime_impl(NULL);
	printf("jent_entropy_switch_notime_impl(NULL): %d\n", ret);
	if (!ret)
		FAIL("jent_entropy_switch_notime_impl accepted a null handler");

	/*
	 * Pinning is best-effort and the return value is not gated on: a build
	 * without the internal timer has nothing to pin, and several platforms
	 * (OpenBSD, macOS) accept the index without being able to honour it.
	 */
	ret = jent_entropy_set_notime_cpu(0);
	printf("jent_entropy_set_notime_cpu(0): %d\n", ret);

	ret = jent_set_fips_failure_callback(fips_failure);
	if (ret)
		FAIL("jent_set_fips_failure_callback: %d", ret);

	/*
	 * Secure memory is memory zeroized on free, which every backend
	 * provides, so the only valid answer is 1 - whatever else (locking,
	 * guard pages) the backend adds.
	 */
	ret = jent_secure_memory_supported();
	printf("jent_secure_memory_supported: %d\n", ret);
	if (ret != 1)
		FAIL("jent_secure_memory_supported: %d, expected 1", ret);

	/*
	 * A startup the machine does not pass - its timer, or memory it cannot
	 * lock - leaves nothing to allocate a collector from, which is not what
	 * this program tests. Every call has linked by the time it runs, and
	 * the ones that need no collector are still made below.
	 */
	ret = jent_entropy_init();
	if (ret && !machine_verdict(ret))
		FAIL("jent_entropy_init: %d", ret);
	no_startup = ret;

	ret = jent_entropy_init_ex(0, 0);
	if (ret && !machine_verdict(ret))
		FAIL("jent_entropy_init_ex: %d", ret);
	if (!no_startup)
		no_startup = ret;

	/*
	 * The known answer tests of the conditioning component. Asserted, not
	 * reported: their verdict is a property of the library, not of the
	 * machine it runs on.
	 */
	ret = jent_selftest(NULL);
	if (ret)
		FAIL("jent_selftest: %d", ret);

	/*
	 * The thread helpers are exported so that an external handler can
	 * reuse them instead of duplicating them. The builtin implementation
	 * needs two CPUs and returns -ENOENT below that, and a build without
	 * the internal timer succeeds without producing a context at all, so
	 * only the call itself is checked. jent_notime_fini() tolerates the
	 * null context that leaves.
	 */
	ret = jent_notime_init(&notime_ctx);
	printf("jent_notime_init: %d\n", ret);
	if (!ret)
		jent_notime_fini(notime_ctx);

	/*
	 * The allocation runs the same startup, so it is the machine's where
	 * that is - asked again, as it may have come out differently on the
	 * health tests this time.
	 */
	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		ret = no_startup ? no_startup : jent_entropy_init_ex(0, 0);
		if (!ret || !machine_verdict(ret))
			FAIL("jent_entropy_collector_alloc returned NULL");
		printf("skipped: the startup does not pass on this machine "
		       "(%d), so there is no collector to call the rest "
		       "with\n", ret);
		return 0;
	}

	rc = jent_read_entropy(ec, data, sizeof(data));
	if (rc != (ssize_t)sizeof(data) && !health_verdict(ec, rc))
		FAIL("jent_read_entropy: %ld", (long)rc);
	if (rc != (ssize_t)sizeof(data))
		printf("jent_read_entropy: %ld, a FIPS mode health test "
		       "failure\n", (long)rc);

	/*
	 * The safe variant reallocates the collector on a health failure, so
	 * it takes the collector by pointer and may replace it. A failure it
	 * could not recover from leaves the collector it had, or none when
	 * the replacement could not be built.
	 */
	rc = jent_read_entropy_safe(&ec, data, sizeof(data));
	if (rc != (ssize_t)sizeof(data) && !(ec && health_verdict(ec, rc)))
		FAIL("jent_read_entropy_safe: %ld", (long)rc);
	if (rc != (ssize_t)sizeof(data))
		printf("jent_read_entropy_safe: %ld, a FIPS mode health test "
		       "failure\n", (long)rc);

	if (jent_status(ec, status, sizeof(status)))
		FAIL("jent_status");
	printf("jent_status:\n%s\n", status);

	if (jent_entropy_collector_uuid(ec, uuid, sizeof(uuid)))
		FAIL("jent_entropy_collector_uuid");
	if (strlen(uuid) != (size_t)(JENT_UUID_STRLEN - 1))
		FAIL("jent_entropy_collector_uuid returned %u characters, "
		     "expected %u",
		     (unsigned int)strlen(uuid),
		     (unsigned int)(JENT_UUID_STRLEN - 1));
	printf("jent_entropy_collector_uuid: %s\n", uuid);

	if (jent_entropy_collector_osr(ec) < 3)
		FAIL("jent_entropy_collector_osr: %u",
		     jent_entropy_collector_osr(ec));
	printf("jent_entropy_collector_osr: %u\n",
	       jent_entropy_collector_osr(ec));
	printf("jent_entropy_collector_flags: 0x%x\n",
	       jent_entropy_collector_flags(ec));
	printf("jent_entropy_collector_memsize: %lu\n",
	       (unsigned long)jent_entropy_collector_memsize(ec));
	printf("jent_entropy_collector_health_failure: 0x%x\n",
	       jent_entropy_collector_health_failure(ec));
	printf("jent_entropy_collector_reinitializations: %u\n",
	       jent_entropy_collector_reinitializations(ec));
	printf("jent_entropy_collector_read_invocations: %llu\n",
	       (unsigned long long)jent_entropy_collector_read_invocations(ec));
	printf("jent_entropy_collector_bytes_output: %llu\n",
	       (unsigned long long)jent_entropy_collector_bytes_output(ec));
	printf("jent_entropy_collector_hashloops: %u\n",
	       jent_entropy_collector_hashloops(ec));

	jent_entropy_collector_free(ec);

	/*
	 * Nothing above forces a health failure, so the callback is expected
	 * not to have fired. It is reported rather than asserted: what this
	 * program is here to prove is that registering it linked and that the
	 * library holds a usable pointer to it.
	 */
	printf("FIPS failure callback: %u\n", fips_failure_seen);

	printf("all public functions called successfully\n");
	return 0;
}
