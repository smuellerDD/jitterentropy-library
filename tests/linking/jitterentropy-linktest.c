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

/* Set by the callback registered with jent_set_fips_failure_callback(). */
static unsigned int fips_failure_seen;

static void fips_failure(struct rand_data *ec, unsigned int health_failure)
{
	(void)ec;
	fips_failure_seen = health_failure;
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
	int ret;

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

	/* Reports the memory backend of this build; both answers are valid. */
	printf("jent_secure_memory_supported: %d\n",
	       jent_secure_memory_supported());

	ret = jent_entropy_init();
	if (ret)
		FAIL("jent_entropy_init: %d", ret);

	ret = jent_entropy_init_ex(0, 0);
	if (ret)
		FAIL("jent_entropy_init_ex: %d", ret);

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

	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec)
		FAIL("jent_entropy_collector_alloc returned NULL");

	rc = jent_read_entropy(ec, data, sizeof(data));
	if (rc != (ssize_t)sizeof(data))
		FAIL("jent_read_entropy: %ld", (long)rc);

	/*
	 * The safe variant reallocates the collector on a health failure, so
	 * it takes the collector by pointer and may replace it.
	 */
	rc = jent_read_entropy_safe(&ec, data, sizeof(data));
	if (rc != (ssize_t)sizeof(data))
		FAIL("jent_read_entropy_safe: %ld", (long)rc);

	if (jent_status(ec, status, sizeof(status)))
		FAIL("jent_status");
	printf("jent_status:\n%s\n", status);

	if (jent_uuid(ec, uuid, sizeof(uuid)))
		FAIL("jent_uuid");
	if (strlen(uuid) != (size_t)(JENT_UUID_STRLEN - 1))
		FAIL("jent_uuid returned %u characters, expected %u",
		     (unsigned int)strlen(uuid),
		     (unsigned int)(JENT_UUID_STRLEN - 1));
	printf("jent_uuid: %s\n", uuid);

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
