/*
 * Jitter RNG: unit tests for the wipe on release
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
 * That the state is gone once an allocation is released. Everything the
 * library keeps - the entropy pool, the SHAKE state carrying the collected
 * entropy, the previous time stamp and the health test counters - is secret
 * for as long as it exists, and the only guarantee that it does not outlive
 * the collector in freed heap memory or in a page handed to the next
 * allocation is the wipe jent_zfree() performs before it lets go.
 *
 * A wipe that quietly does nothing looks exactly like one that works: after
 * the release the memory is unmapped or back in the allocator, so the caller
 * cannot look at it - reading it would be reading freed memory, which is
 * undefined and, on the guard-page backends, a fault. So the release itself is
 * interposed and the memory is read in the one moment where it is both wiped
 * and still mapped: inside the wipe-then-release pair, just before the
 * primitive that ends the mapping is called.
 *
 * The interposition is the one used by unit-fault: these programs absorb the
 * library sources, so renaming a call through the preprocessor while
 * arch/jitterentropy-arch-memory.c is compiled makes every release the
 * allocator performs reach a wrapper here, and the shipped library carries no
 * testing conditional.
 */

#ifdef __linux__
#define _GNU_SOURCE
#endif

#include "unit.h"

#include <errno.h>
#include <stdlib.h>

/*
 * Which primitive ends an allocation is decided inside the arch/ source, and
 * the renaming has to be in place before it is included - so its platform
 * selection cannot be read here, only repeated, exactly as unit-fault does.
 * The mapping backends release with munmap() and VirtualFree(); the fallback
 * for a POSIX platform that provides no memory locking releases with free().
 *
 * The system headers are included before the names are taken over, so the
 * declarations the redirections shadow are already in place.
 */
#if defined(_MSC_VER) || defined(__MINGW32__)
# define ZE_WINDOWS
# include <windows.h>
#else
# include <sys/mman.h>
# include <unistd.h>
#endif

/*
 * The allocation currently watched, and what was found in it at the moment it
 * was released. One at a time: every check below arms the watch, releases that
 * one allocation and reads the result.
 */
static const unsigned char *ze_watch;
static size_t ze_watch_len;
static unsigned int ze_releases;
static size_t ze_dirty;

static void ze_arm(const void *ptr, size_t len)
{
	ze_watch = (const unsigned char *)ptr;
	ze_watch_len = len;
	ze_releases = 0;
	ze_dirty = 0;
}

static void ze_disarm(void)
{
	ze_watch = NULL;
	ze_watch_len = 0;
}

/* The watched range, as it stands in the instant before it is released. */
static void ze_scan(void)
{
	size_t i;

	ze_releases++;
	for (i = 0; i < ze_watch_len; i++) {
		if (ze_watch[i])
			ze_dirty++;
	}
}

#ifdef ZE_WINDOWS

/*
 * jent_zfree() releases the mapping the payload sits in the middle of, so what
 * VirtualFree() is handed is one page below the watched pointer. Matched
 * exactly rather than by containment: VirtualFree(MEM_RELEASE) is called with
 * a length of zero, which says "the whole mapping" and states no extent this
 * could test the watched range against.
 */
static size_t ze_pagesize(void)
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	return si.dwPageSize;
}

static JENT_UT_MAYBE_UNUSED BOOL ze_VirtualFree(LPVOID addr, SIZE_T len,
						DWORD type)
{
	if (ze_watch && (const unsigned char *)addr == ze_watch - ze_pagesize())
		ze_scan();

	return VirtualFree(addr, len, type);
}

#else /* ZE_WINDOWS */

static JENT_UT_MAYBE_UNUSED int ze_munmap(void *addr, size_t len)
{
	const unsigned char *base = (const unsigned char *)addr;

	/*
	 * By containment, as the mapping is the payload plus its two guard
	 * pages: an unmapping of anything else cannot cover the watched range
	 * and must not be read as the release of it.
	 */
	if (ze_watch && base <= ze_watch &&
	    base + len >= ze_watch + ze_watch_len)
		ze_scan();

	return munmap(addr, len);
}

#endif /* ZE_WINDOWS */

static JENT_UT_MAYBE_UNUSED void ze_free(void *ptr)
{
	if (ze_watch && (const unsigned char *)ptr == ze_watch)
		ze_scan();

	free(ptr);
}

/*
 * Compile the real allocator with its release calls redirected. Only this
 * source: the library's other absorbed sources release nothing themselves,
 * they go through jent_zfree(), and free() is a name too common to take over
 * for a whole translation unit.
 */
#ifdef ZE_WINDOWS
# define VirtualFree ze_VirtualFree
#else
# define munmap ze_munmap
#endif
#define free ze_free
/*
 * The atomic accessors of the process-wide state. Absorbed ahead of
 * everything else because it depends on nothing else and nearly everything
 * else depends on it - see arch/jitterentropy-arch-atomic.h.
 */
#include "jitterentropy-arch-atomic.c"

#include "jitterentropy-arch-memory.c"
#undef free
#ifdef ZE_WINDOWS
# undef VirtualFree
#else
# undef munmap
#endif

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
#include "jitterentropy-arch-ncpu.c"
#include "jitterentropy-arch-sched.c"
#include "jitterentropy-arch-thread.c"
#include "jitterentropy-arch-timer.c"
#include "jitterentropy-arch-random.c"

/*
 * The arenas of libgcrypt, OpenSSL and AWS-LC are released through their own
 * calls, which wipe what they take back and hand out no address this could
 * watch. Where one of them is compiled in it is what jent_zalloc() uses, so
 * there is no release here to interpose and the checks below say so rather
 * than reporting a wipe they did not see.
 */
#if defined(LIBGCRYPT) || defined(AWSLC) || defined(OPENSSL)
# define ZE_FOREIGN_ARENA
#endif

/*
 * What every check below asks of one allocation: that it carried something
 * before the release - a wipe of memory that was already zero proves nothing -
 * and that not one byte of it was left at the release.
 */
static void ze_check_wiped(const void *ptr, size_t len, const char *what)
{
	const unsigned char *p = (const unsigned char *)ptr;
	size_t before = 0, i;

	for (i = 0; i < len; i++) {
		if (p[i])
			before++;
	}

	if (!before)
		printf("  note: %s held nothing before the release\n", what);

	if (!ze_releases) {
		JENT_UT_SKIP(what, "its release was not seen");
		return;
	}

	JENT_UT_EQ(ze_dirty, 0u, what);
}

/*
 * The allocator's own contract: what jent_zfree() is given is wiped before it
 * is released, in every memory mode.
 */
static void test_zfree_wipes(void)
{
	static const struct {
		unsigned int flags;
		const char *name;
	} modes[] = {
		{ 0,			"ordinary memory is wiped on release" },
		{ JENT_FORCE_SECURE_MEM,
					"secure memory is wiped on release" },
	};
	size_t m;

	jent_ut_group("jent_zfree wipes before it releases");

#ifdef ZE_FOREIGN_ARENA
	JENT_UT_SKIP("the wipe on release",
		     "this build allocates from a foreign secure arena");
	return;
#else
	for (m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
		const size_t len = 4096;
		unsigned char *p = jent_zalloc(len, modes[m].flags);

		if (!p) {
			JENT_UT_SKIP(modes[m].name, "allocation failed");
			continue;
		}

		memset(p, 0x5a, len);

		ze_arm(p, len);
		jent_zfree(p, len);
		ze_disarm();

		jent_ut_checks++;
		if (!ze_releases)
			JENT_UT_FAIL("%s: the release was not seen",
				     modes[m].name);
		else
			JENT_UT_EQ(ze_dirty, 0u, modes[m].name);
	}
#endif
}

/*
 * The collector's state, which is what the wipe exists for. Three allocations
 * carry it and each is released by jent_entropy_collector_free():
 *
 *   - the entropy pool, the memory the noise source walks,
 *   - the SHAKE state, which holds the collected entropy itself, and
 *   - struct rand_data, with the previous time stamp, the health test
 *     counters and the pointers to the other two.
 *
 * One collector per allocation: the watch follows one address at a time, and
 * the free path releases all three in the same call.
 */
static void test_collector_state_wiped(void)
{
	enum { ZE_POOL, ZE_HASH, ZE_STATE, ZE_PARTS };
	static const char *names[ZE_PARTS] = {
		"the entropy pool is wiped on free",
		"the hash state is wiped on free",
		"the collector state is wiped on free",
	};
	unsigned int part;

	jent_ut_group("the collector's state does not survive its free");

#ifdef ZE_FOREIGN_ARENA
	JENT_UT_SKIP("the collector state",
		     "this build allocates from a foreign secure arena");
	return;
#else
	if (jent_entropy_init()) {
		JENT_UT_SKIP("the collector state",
			     "the startup does not pass on this machine");
		return;
	}

	for (part = 0; part < ZE_PARTS; part++) {
		struct rand_data *ec = jent_entropy_collector_alloc(0, 0);
		unsigned char buf[64];
		const void *ptr;
		size_t len;

		if (!ec) {
			JENT_UT_SKIP(names[part], "no collector");
			continue;
		}

		/*
		 * Fill the state: before the first generation the pool is the
		 * zeroed memory it was allocated as and the SHAKE state holds
		 * nothing but its initialization.
		 */
		if (jent_read_entropy(ec, (char *)buf, sizeof(buf)) !=
		    (ssize_t)sizeof(buf)) {
			JENT_UT_SKIP(names[part], "no entropy generated");
			jent_entropy_collector_free(ec);
			continue;
		}

		switch (part) {
		case ZE_POOL:
			ptr = ec->mem;
			len = (size_t)ec->memmask + 1;
			break;
		case ZE_HASH:
			ptr = ec->hash_state;
			len = JENT_SHA_MAX_CTX_SIZE;
			break;
		default:
			ptr = ec;
			len = sizeof(struct rand_data);
			break;
		}

		if (!ptr) {
			/* No pool without a memory access loop. */
			JENT_UT_SKIP(names[part], "this collector has none");
			jent_entropy_collector_free(ec);
			continue;
		}

		ze_arm(ptr, len);
		/*
		 * Read before the free, as afterwards the memory is gone: what
		 * ze_check_wiped() compares is the state as it stands now
		 * against what the release saw.
		 */
		{
			unsigned char *copy = malloc(len);

			if (copy) {
				memcpy(copy, ptr, len);
				jent_entropy_collector_free(ec);
				ze_disarm();
				ze_check_wiped(copy, len, names[part]);
				jent_memset_secure(copy, len);
				free(copy);
			} else {
				jent_entropy_collector_free(ec);
				ze_disarm();
				JENT_UT_SKIP(names[part], "out of memory");
			}
		}
	}
#endif
}

int main(void)
{
	jent_ut_setup();

	test_zfree_wipes();
	test_collector_state_wiped();

	return jent_ut_report("unit-zeroize");
}
