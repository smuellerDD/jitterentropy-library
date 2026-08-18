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

/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-cache.c"
#include "jitterentropy-arch-fips.c"
#include "jitterentropy-arch-memory.c"
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * After the absorbed sources: which memory backend is in use, and so whether
 * the allocation is the one that carries guard pages, is decided by the macros
 * they define. Only the mmap() and VirtualAlloc() paths place them - the
 * arenas of libgcrypt, OpenSSL and AWS-LC hand out what they hand out, and
 * they are tried first where they are compiled in, so their presence takes the
 * guarded path out of the build.
 */
#if !defined(LIBGCRYPT) && !defined(AWSLC) && !defined(OPENSSL)
# if defined(JENT_ARCH_MEM_POSIX_MLOCK)
#  define JENT_UT_GUARD_POSIX
#  include <sys/resource.h>
#  include <sys/wait.h>
#  include <unistd.h>
# elif defined(JENT_ARCH_MEM_WINDOWS)
#  define JENT_UT_GUARD_WINDOWS
# endif
#endif

#ifdef JENT_UT_GUARD_POSIX
/*
 * Read one byte in a child process. Returns 1 when the access faulted, 0 when
 * it went through, and -1 when no child could be created - a fault is the
 * point of a guard page, so it has to be provoked somewhere that can die of
 * it.
 *
 * Probed with a read rather than a write, although the state around the
 * payload is what the guard pages protect from writes: a read faults only on a
 * page that is inaccessible altogether, which is what both backends set up. A
 * write would fault on a guard page left readable as well, and report a page
 * that leaks the memory next to the state as working.
 *
 * The verdict is "the child did not exit successfully" rather than "it was
 * killed by SIGSEGV or SIGBUS": which of the two signals a platform raises for
 * a protection violation differs, and a sanitizer or a debugger intercepts the
 * fault and exits with a status of its own. All of them say the access did not
 * quietly succeed, which is what is being asked.
 */
static int jent_ut_read_in_child(const volatile unsigned char *addr)
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return -1;

	if (!pid) {
		/*
		 * No core dump for a fault this test provokes on purpose: it
		 * would be written into the build tree of every run.
		 */
		struct rlimit no_core = { 0, 0 };

		(void)setrlimit(RLIMIT_CORE, &no_core);

		/*
		 * volatile through jent_ut_read_in_child()'s parameter, so the
		 * read is issued rather than optimized away as unused.
		 */
		(void)*addr;
		_exit(0);
	}

	if (waitpid(pid, &status, 0) != pid)
		return -1;

	return !(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
#endif /* JENT_UT_GUARD_POSIX */

/*
 * The guard pages: one inaccessible page on each side of the payload, so that
 * an access beyond the entropy collector's state faults immediately instead of
 * reading or corrupting whatever the allocator placed next to it. They are
 * part of the layout jent_zalloc() and jent_zfree() agree on - the payload is
 * page-aligned and the base is derived back from it as ptr - page_size - so a
 * wrong page count or a protection that did not take is not a hardening
 * measure that quietly does nothing but a free() of the wrong address.
 *
 * Not gated on JENT_FORCE_SECURE_MEM: the guard pages are placed by the
 * mapping, which every allocation of these backends goes through, while the
 * flag decides only whether a refused memory lock fails the allocation.
 */
static void test_guard_pages(void)
{
	jent_ut_group("the guard pages around an allocation");

#if !defined(JENT_UT_GUARD_POSIX) && !defined(JENT_UT_GUARD_WINDOWS)
	JENT_UT_SKIP("the guard pages", "this backend places none");
#else
	{
	/*
	 * Exactly one page, so that the page-rounded payload is one page and
	 * the trailing guard starts at p + page_size on either backend.
	 */
	const size_t page_size = jent_pagesize();
	unsigned char *p = jent_zalloc(page_size, 0);

	if (!p) {
		JENT_UT_SKIP("the guard pages", "allocation failed");
		return;
	}

	/*
	 * The alignment the layout rests on. Without it the guard pages sit
	 * somewhere inside the payload's own pages and protect nothing.
	 */
	JENT_UT_EQ((uintptr_t)p % page_size, 0,
		   "the payload starts on a page boundary");

# ifdef JENT_UT_GUARD_POSIX
	{
		int below, above, payload;

		payload = jent_ut_read_in_child(p);
		below = jent_ut_read_in_child(p - 1);
		above = jent_ut_read_in_child(p + page_size);

		if (payload < 0 || below < 0 || above < 0) {
			JENT_UT_SKIP("the guard pages",
				     "no child process to fault in");
		} else {
			/*
			 * The control: the same probe on the payload itself
			 * has to come back alive, or the two below would pass
			 * on a machine where every child dies.
			 */
			JENT_UT_EQ(payload, 0, "the payload is readable");
			JENT_UT_EQ(below, 1,
				   "the page below the payload faults");
			JENT_UT_EQ(above, 1,
				   "the page above the payload faults");
		}
	}
# else /* JENT_UT_GUARD_WINDOWS */
	{
		/*
		 * Windows has no fork(), and the structured exception handling
		 * that would catch the fault in-process is a compiler
		 * extension MinGW does not provide. The protection of the two
		 * pages is read back instead, which is the state the fault
		 * would follow from.
		 */
		MEMORY_BASIC_INFORMATION mbi;

		JENT_UT_TRUE(VirtualQuery(p - 1, &mbi, sizeof(mbi)) ==
			     sizeof(mbi) && mbi.Protect == PAGE_NOACCESS,
			     "the page below the payload is inaccessible");
		JENT_UT_TRUE(VirtualQuery(p + page_size, &mbi, sizeof(mbi)) ==
			     sizeof(mbi) && mbi.Protect == PAGE_NOACCESS,
			     "the page above the payload is inaccessible");
	}
# endif

	/* Everything in between is the caller's, and stays writable. */
	memset(p, 0x5a, page_size);
	JENT_UT_EQ(p[0], 0x5a, "the first byte of the payload is writable");
	JENT_UT_EQ(p[page_size - 1], 0x5a,
		   "the last byte of the payload is writable");

	/*
	 * And the free finds its way back to the base of the mapping: a
	 * release computed from a wrong layout faults here, or leaks the
	 * mapping.
	 */
	jent_zfree(p, page_size);
	}
#endif
}

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
	test_guard_pages();

	return jent_ut_report("unit-arch-memory");
}
