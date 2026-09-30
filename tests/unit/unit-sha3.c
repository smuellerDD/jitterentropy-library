/*
 * Jitter RNG: unit tests for src/jitterentropy-sha3.c
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
 * jitterentropy-arch-memory.c is absorbed below, after the atomic accessors
 * have pulled in the system headers: its own feature macros come too late for
 * MAP_ANON on glibc under -std=c11. Stated once up front.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-sha3.c"
#include "jitterentropy-arch-memory.c"

/*
 * The known answers the library carries for itself. Run first: everything
 * below compares the implementation against itself and would agree just as
 * happily with a consistently wrong one.
 */
static void test_selftest(void)
{
	jent_ut_group("SHA-3 / XDRBG known answer self test");
	JENT_UT_EQ(jent_sha3_tester(), 0, "jent_sha3_tester");
}

/*
 * NIST FIPS 202 / CAVP: SHA3-256 of the empty message and of "abc".
 * Independent of the library's own vectors above.
 */
static void test_sha3_256_kat(void)
{
	static const uint8_t empty_md[] = {
		0xa7, 0xff, 0xc6, 0xf8, 0xbf, 0x1e, 0xd7, 0x66,
		0x51, 0xc1, 0x47, 0x56, 0xa0, 0x61, 0xd6, 0x62,
		0xf5, 0x80, 0xff, 0x4d, 0xe4, 0x3b, 0x49, 0xfa,
		0x82, 0xd8, 0x0a, 0x4b, 0x80, 0xf8, 0x43, 0x4a
	};
	static const uint8_t abc_md[] = {
		0x3a, 0x98, 0x5d, 0xa7, 0x4f, 0xe2, 0x25, 0xb2,
		0x04, 0x5c, 0x17, 0x2d, 0x6b, 0xd3, 0x90, 0xbd,
		0x85, 0x5f, 0x08, 0x6e, 0x3e, 0x9d, 0x52, 0x5b,
		0x46, 0xbf, 0xe2, 0x45, 0x11, 0x43, 0x15, 0x32
	};
	HASH_CTX_ON_STACK(ctx);
	uint8_t md[JENT_SHA3_256_SIZE_DIGEST];

	jent_ut_group("SHA3-256 against the FIPS 202 vectors");

	jent_sha3_256_init(&ctx);
	jent_sha3_final(&ctx, md);
	JENT_UT_MEM_EQ(md, empty_md, sizeof(md), "SHA3-256 of the empty input");

	jent_sha3_256_init(&ctx);
	jent_sha3_update(&ctx, (const uint8_t *)"abc", 3);
	jent_sha3_final(&ctx, md);
	JENT_UT_MEM_EQ(md, abc_md, sizeof(md), "SHA3-256 of \"abc\"");
}

/*
 * The absorb path buffers a partial block in ctx->partial and only permutes on
 * a full rate. Whether a message is handed over in one call or in pieces must
 * therefore not matter, and the block boundary is where it would.
 */
static void test_sha3_incremental(void)
{
	uint8_t msg[3 * JENT_SHA3_256_SIZE_BLOCK + 7];
	uint8_t oneshot[JENT_SHA3_256_SIZE_DIGEST];
	uint8_t piecewise[JENT_SHA3_256_SIZE_DIGEST];
	HASH_CTX_ON_STACK(ctx);
	size_t chunk, i;

	for (i = 0; i < sizeof(msg); i++)
		msg[i] = (uint8_t)i;

	jent_ut_group("SHA3-256 incremental update equals a single update");

	jent_sha3_256_init(&ctx);
	jent_sha3_update(&ctx, msg, sizeof(msg));
	jent_sha3_final(&ctx, oneshot);

	/*
	 * Chunk sizes around the rate: one below, exactly, one above, and the
	 * degenerate single byte.
	 */
	for (chunk = 1; chunk <= JENT_SHA3_256_SIZE_BLOCK + 1; chunk++) {
		char what[64];

		if (chunk > 1 && chunk < JENT_SHA3_256_SIZE_BLOCK - 1)
			continue;

		jent_sha3_256_init(&ctx);
		for (i = 0; i < sizeof(msg); i += chunk) {
			size_t len = sizeof(msg) - i;

			if (len > chunk)
				len = chunk;
			jent_sha3_update(&ctx, msg + i, len);
		}
		jent_sha3_final(&ctx, piecewise);

		snprintf(what, sizeof(what), "chunked by %zu bytes", chunk);
		JENT_UT_MEM_EQ(piecewise, oneshot, sizeof(oneshot), what);
	}

	/* A zero-length update must be a no-op rather than a state change. */
	jent_sha3_256_init(&ctx);
	jent_sha3_update(&ctx, msg, 0);
	jent_sha3_update(&ctx, msg, sizeof(msg));
	jent_sha3_update(&ctx, msg, 0);
	jent_sha3_final(&ctx, piecewise);
	JENT_UT_MEM_EQ(piecewise, oneshot, sizeof(oneshot),
		       "zero-length updates are a no-op");
}

/*
 * SHAKE256 is an XOF: the same state must produce the same stream, a different
 * state a different one, and the length asked for must be the length written.
 */
static void test_shake256(void)
{
	HASH_CTX_ON_STACK(ctx);
	uint8_t a[128], b[128];
	size_t i;

	jent_ut_group("SHAKE256 / XDRBG block generation");

	jent_shake256_init(&ctx);
	jent_shake256_set_digestsize(&ctx, sizeof(a));
	jent_sha3_update(&ctx, (const uint8_t *)"seed", 4);
	jent_sha3_final(&ctx, a);

	jent_shake256_init(&ctx);
	jent_shake256_set_digestsize(&ctx, sizeof(b));
	jent_sha3_update(&ctx, (const uint8_t *)"seed", 4);
	jent_sha3_final(&ctx, b);
	JENT_UT_MEM_EQ(b, a, sizeof(a), "the same input gives the same output");

	jent_shake256_init(&ctx);
	jent_shake256_set_digestsize(&ctx, sizeof(b));
	jent_sha3_update(&ctx, (const uint8_t *)"seeD", 4);
	jent_sha3_final(&ctx, b);
	jent_ut_checks++;
	if (!memcmp(a, b, sizeof(a)))
		JENT_UT_FAIL("%s", "a different input gives the same output");

	/*
	 * jent_drbg_generate_block() produces one digest at most: the squeeze
	 * is deliberately a single block and the length is clamped to
	 * JENT_SHA3_256_SIZE_DIGEST. So what has to hold for a request longer
	 * than that is not that the request is filled - it never is - but that
	 * exactly a digest is written and the rest of the caller's buffer is
	 * left alone.
	 *
	 * Checked against a non-zero fill. Against a zeroed buffer "some byte
	 * is non-zero" is satisfied by the first digest alone, so the check
	 * passed with 96 of the 128 bytes never written, and would have gone on
	 * passing had the clamp been lowered to a single byte.
	 */
	jent_shake256_init(&ctx);
	jent_sha3_update(&ctx, (const uint8_t *)"seed", 4);
	memset(a, 0xcc, sizeof(a));
	jent_drbg_generate_block(&ctx, a, sizeof(a));
	jent_ut_checks++;
	{
		size_t n = 0;

		for (i = 0; i < JENT_SHA3_256_SIZE_DIGEST; i++) {
			if (a[i] != 0xcc)
				n++;
		}
		if (!n)
			JENT_UT_FAIL("%s", "the digest was not written at all");
	}
	jent_ut_checks++;
	for (i = JENT_SHA3_256_SIZE_DIGEST; i < sizeof(a); i++) {
		if (a[i] != 0xcc) {
			JENT_UT_FAIL(
				"byte %u past the digest was written by a clamped request",
				(unsigned int)i);
			break;
		}
	}

	/*
	 * A length that is neither a multiple of the rate nor of the digest
	 * size: exactly 17 bytes must be written and the rest of the buffer
	 * left alone, which the two loops below check. There used to be a
	 * "guard" array beside this as well, filled with 0xa5 and then
	 * asserted to still hold 0xa5 - an unrelated local that is not
	 * adjacent to anything and that nothing writes to, so it held
	 * whatever the generator did.
	 */
	jent_shake256_init(&ctx);
	jent_sha3_update(&ctx, (const uint8_t *)"seed", 4);
	memset(a, 0, sizeof(a));
	jent_drbg_generate_block(&ctx, a, 17);

	jent_ut_checks++;
	for (i = 0; i < 17; i++) {
		if (a[i])
			break;
	}
	if (i == 17)
		JENT_UT_FAIL("%s", "the generated block is all zero");

	jent_ut_checks++;
	for (i = 17; i < sizeof(a); i++) {
		if (a[i]) {
			JENT_UT_FAIL("something was written past the "
				     "requested length of 17, at %zu", i);
			break;
		}
	}
}

/* The rate an initialized SHA3-256 state reports. */
static void test_rate(void)
{
	HASH_CTX_ON_STACK(ctx);

	jent_ut_group("SHA-3 state rate");

	jent_sha3_256_init(&ctx);
	JENT_UT_EQ(jent_sha3_rate(&ctx), JENT_SHA3_256_SIZE_BLOCK,
		   "the rate of an initialized SHA3-256 state");
}

int main(void)
{
	test_selftest();
	test_sha3_256_kat();
	test_sha3_incremental();
	test_shake256();
	test_rate();

	return jent_ut_report("unit-sha3");
}
