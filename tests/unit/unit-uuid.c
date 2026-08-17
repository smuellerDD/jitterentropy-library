/*
 * Jitter RNG: unit tests for the per-instance UUID
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
 * Jitter RNG: unit tests for the UUID backend.
 *
 * Every assertion has to hold on every platform: what is checked is the
 * contract the backend header in arch/ states, not the behaviour of one
 * implementation.
 */

/*
 * As in the AMALGAMATED programs under tests/raw-entropy: several arch sources
 * are absorbed here, and the ones needing _GNU_SOURCE define it themselves
 * before their own includes - which is too late once an earlier source in this
 * translation unit has already pulled the headers in. Stated once up front.
 */
#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"
#include "jitterentropy-uuid.c"

/*
 * The UUID naming a collector instance in the status output. Checked against
 * the canonical 8-4-4-4-12 form and RFC 4122's version and variant nibbles,
 * and for being different every time - it identifies an instance.
 */
static void test_uuid(void)
{
	char a[JENT_UUID_STRLEN], b[JENT_UUID_STRLEN];
	size_t i;

	jent_ut_group("jent_uuid_generate");

	jent_uuid_generate(a);
	jent_uuid_generate(b);
	printf("  note: %s\n", a);

	JENT_UT_EQ(strlen(a), JENT_UUID_STRLEN - 1, "the length is canonical");

	for (i = 0; i < JENT_UUID_STRLEN - 1; i++) {
		int ok;

		if (i == 8 || i == 13 || i == 18 || i == 23)
			ok = (a[i] == '-');
		else
			ok = (a[i] >= '0' && a[i] <= '9') ||
			     (a[i] >= 'a' && a[i] <= 'f');

		if (!ok) {
			JENT_UT_FAIL("character %zu of \"%s\" is not canonical",
				     i, a);
			break;
		}
	}
	jent_ut_checks++;

	JENT_UT_EQ(a[14], '4', "the version nibble says version 4");
	jent_ut_checks++;
	if (a[19] != '8' && a[19] != '9' && a[19] != 'a' && a[19] != 'b')
		JENT_UT_FAIL("the variant nibble is '%c'", a[19]);

	jent_ut_checks++;
	if (!strcmp(a, b))
		JENT_UT_FAIL("%s", "two UUIDs in a row are identical");
}

#if defined(JENT_UUID_GETRANDOM) || defined(JENT_UUID_DEVURANDOM)
static void test_uuid_helpers(void)
{
	uint8_t b[16];
	char out[JENT_UUID_STRLEN];
	unsigned int i, nonzero = 0;

	jent_ut_group("the UUID helpers");

	/*
	 * The /dev/urandom fallback, which jent_uuid_random_bytes() only
	 * reaches when getrandom() fails.
	 */
	memset(b, 0, sizeof(b));
	if (jent_uuid_dev_urandom(b, sizeof(b))) {
		JENT_UT_SKIP("jent_uuid_dev_urandom", "/dev/urandom is unreadable");
	} else {
		for (i = 0; i < sizeof(b); i++) {
			if (b[i])
				nonzero++;
		}
		JENT_UT_NE(nonzero, 0, "it produced something other than zeros");
	}

	/*
	 * The formatting, which is deterministic and can therefore be checked
	 * against a fixed vector rather than only for plausibility.
	 */
	for (i = 0; i < sizeof(b); i++)
		b[i] = (uint8_t)(i * 0x11);
	jent_uuid_format(b, out);
	JENT_UT_TRUE(!strcmp(out, "00112233-4455-6677-8899-aabbccddeeff"),
		     "the canonical hex form is produced");
}
#else
static void test_uuid_helpers(void)
{
	JENT_UT_SKIP("the UUID helpers", "no CSPRNG backend for UUIDs");
}
#endif

#if defined(JENT_UUID_GETRANDOM) || defined(JENT_UUID_DEVURANDOM)
static void test_uuid_read_paths(void)
{
	uint8_t b[16];
	char path[] = "/tmp/jent-urnd-XXXXXX";
	int fd;

	jent_ut_group("the CSPRNG read behind the UUID");

	JENT_UT_TRUE(jent_uuid_read_random_file("/nonexistent/jent/urandom", b,
						sizeof(b)) != 0,
		     "an absent source is an error");

	/*
	 * A directory opens but cannot be read, which is the read-error arm
	 * rather than the end-of-file one below.
	 */
	JENT_UT_TRUE(jent_uuid_read_random_file("/tmp", b, sizeof(b)) != 0,
		     "a source that cannot be read is an error");

	fd = mkstemp(path);
	if (fd < 0) {
		JENT_UT_SKIP("the CSPRNG read", "no temporary file");
		return;
	}
	close(fd);

	/* An empty file: end of file before the requested length. */
	JENT_UT_TRUE(jent_uuid_read_random_file(path, b, sizeof(b)) != 0,
		     "a source that ends early is an error");

	/* A file with fewer bytes than asked for: the same. */
	{
		FILE *f = fopen(path, "w");

		if (f) {
			fputs("12345", f);
			fclose(f);
			JENT_UT_TRUE(jent_uuid_read_random_file(path, b,
								sizeof(b)) != 0,
				     "a source that is too short is an error");
		}
	}

	/* And one long enough, which must fill the whole buffer. */
	{
		FILE *f = fopen(path, "w");
		size_t i;

		if (f) {
			for (i = 0; i < 64; i++)
				fputc((int)(i + 1), f);
			fclose(f);
			memset(b, 0, sizeof(b));
			JENT_UT_EQ(jent_uuid_read_random_file(path, b,
							      sizeof(b)), 0,
				   "a source with enough bytes succeeds");
			JENT_UT_EQ(b[0], 1, "and the bytes are the ones read");
			JENT_UT_EQ(b[15], 16, "through to the last");
		}
	}
	remove(path);
}
#else
static void test_uuid_read_paths(void)
{
	JENT_UT_SKIP("the CSPRNG read behind the UUID", "no CSPRNG backend");
}
#endif

int main(void)
{
	test_uuid();
	test_uuid_helpers();
	test_uuid_read_paths();

	return jent_ut_report("unit-uuid");
}
