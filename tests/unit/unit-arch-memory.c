/*
 * Jitter RNG: unit tests for the platform backends in arch/
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
 * Jitter RNG: unit tests for the memory backend.
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

static void test_memory(void)
{
	static const unsigned int modes[] = { 0, JENT_FORCE_SECURE_MEM };
	unsigned int m;

	jent_ut_group("jent_zalloc / jent_zfree / jent_memset_secure");

	for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		const size_t len = 4096;
		unsigned char *p = jent_zalloc(len, modes[m]);
		const char *what = modes[m] ? "secure memory" : "ordinary memory";
		size_t i;
		unsigned int nonzero = 0;

		if (!p) {
			/*
			 * Secure memory can legitimately be unavailable: it is
			 * locked into RAM, and RLIMIT_MEMLOCK may not allow it.
			 */
			if (modes[m])
				JENT_UT_SKIP(what, "allocation failed, "
					     "possibly an RLIMIT_MEMLOCK limit");
			else
				JENT_UT_FAIL("%s: allocation failed", what);
			continue;
		}

		/* The z in the name: the memory comes back zeroed. */
		for (i = 0; i < len; i++) {
			if (p[i])
				nonzero++;
		}
		JENT_UT_EQ(nonzero, 0, "the allocation is zeroed");

		/* And is writable across its whole length. */
		memset(p, 0x5a, len);
		JENT_UT_EQ(p[0], 0x5a, "the first byte is writable");
		JENT_UT_EQ(p[len - 1], 0x5a, "the last byte is writable");

		jent_memset_secure(p, len);
		nonzero = 0;
		for (i = 0; i < len; i++) {
			if (p[i])
				nonzero++;
		}
		JENT_UT_EQ(nonzero, 0, "jent_memset_secure clears the buffer");

		jent_zfree(p, len);
	}

	/* A no-op, as free(NULL) is. */
	jent_zfree(NULL, 4096);

	/* A zero-length wipe must be a no-op rather than a fault. */
	{
		unsigned char byte = 0x5a;

		jent_memset_secure(&byte, 0);
		JENT_UT_EQ(byte, 0x5a, "a zero-length wipe writes nothing");
	}

	jent_ut_checks++;
	{
		int secure = jent_memory_is_secure(JENT_FORCE_SECURE_MEM);

		if (secure != 0 && secure != 1)
			JENT_UT_FAIL("jent_memory_is_secure returned %d", secure);
		else
			printf("  note: secure memory is %s\n",
			       secure ? "available" : "unavailable");
	}
}

int main(void)
{
	test_memory();

	return jent_ut_report("unit-arch-memory");
}
