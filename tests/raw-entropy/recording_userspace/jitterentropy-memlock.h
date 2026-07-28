/*
 * Copyright (C) 2019 - 2026, Stephan Mueller <smueller@chronox.de>
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
 * Memory lock limit handling and secure memory arena setup for the recording
 * tools.
 *
 * In the compliance modes - JENT_NTG1 and JENT_FORCE_FIPS, which imply
 * JENT_FORCE_SECURE_MEM - the memory of the entropy collector must be locked
 * into RAM: a lock the operating system refuses fails the allocation instead
 * of leaving the state in memory that may be swapped out. How much memory a
 * process may lock is bounded by the operating system, and the bound is well
 * below the memory block the collector maps:
 *
 *   - POSIX: RLIMIT_MEMLOCK, commonly 8 MB (and only 64 kB on older systems),
 *     which is why the raw entropy recording with large memory blocks has
 *     traditionally been run as root (see README.md).
 *
 *   - Windows: the process minimum working set. VirtualLock() charges its
 *     pages against it - "the maximum number of pages a process can lock is
 *     equal to the number of pages in its minimum working set minus a small
 *     overhead" - and fails with ERROR_WORKING_SET_QUOTA once that budget is
 *     exhausted.
 *
 * Both are process-wide state, which is why the library does not touch them:
 * raising them affects the whole host application - on Windows it evicts the
 * working set the application has reserved for itself - and neither can be
 * restored on free, as another thread may have locked memory against the
 * raised limit in the meantime. That decision belongs to the application -
 * here, to these test tools, which own their process.
 *
 * The same holds for the secure memory arena the library allocates from when
 * it is built with EXTERNAL_CRYPTO=LIBGCRYPT or EXTERNAL_CRYPTO=OPENSSL:
 * libgcrypt's secmem pool and OpenSSL's secure heap are each created once per
 * process, in a size that cannot be changed afterwards and that every other
 * user of those libraries in the process then shares. The library only checks
 * that the arena is there (see arch/jitterentropy-arch-memory.c); creating and
 * sizing it is done here, in the tool that owns the process.
 */

#ifndef _JITTERENTROPY_MEMLOCK_H
#define _JITTERENTROPY_MEMLOCK_H

/*
 * GetProcessWorkingSetSizeEx() and SetProcessWorkingSetSizeEx() are declared by
 * the Windows SDK only from Windows Vista onwards. mingw-w64 has defaulted to
 * older values across its releases, so the minimum is stated here rather than
 * left to the toolchain; it has to precede every system header, including the
 * <windows.h> included below. An externally supplied, higher value is left
 * alone. Same reasoning as in the sources under arch/, which is also why this
 * header is included before the Windows headers by its users.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

#include "jitterentropy.h"

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
#else
# include <sys/types.h>
# include <sys/time.h>
# include <sys/resource.h>
#endif

/*
 * The backend macros are the ones the library is compiled with: CMake puts
 * -D${EXTERNAL_CRYPTO} into JITTER_C_FLAGS, which every test program is built
 * with as well, so the arena configured below is exactly the one the library
 * then allocates from. AWS-LC needs no entry: it wipes its allocations but
 * keeps no arena to configure.
 */
#if defined(LIBGCRYPT)
# include <gcrypt.h>
#elif defined(OPENSSL)
# include <openssl/crypto.h>
#endif

/*
 * Amount of lockable memory requested when the tool does not pin the size of
 * the memory access block with JENT_MAX_MEMSIZE_*: the collector then derives
 * it from the cache geometry at runtime, which this code cannot predict. 64 MB
 * covers the derived size on ordinary hardware, including --all-caches on a
 * desktop; where it does not - the summed caches of a large server - the
 * request is simply larger than needed, and a limit the system refuses to
 * grant is handled as a partial raise below.
 */
#define JENT_MEMLOCK_DEFAULT	(64ULL << 20)

/*
 * Slack added to that block: the collector makes further (small) allocations
 * which are locked as well.
 */
#define JENT_MEMLOCK_SLACK	(1ULL << 20)

/*
 * Bytes the entropy collector instantiated with @flags may need to lock.
 *
 * The result is bounded by JENT_MAX_MEMSIZE_MAX plus the slack above, i.e. it
 * fits into a size_t (and into a SIZE_T / rlim_t) on every supported target,
 * 32-bit ones included.
 */
static inline unsigned long long jent_memlock_size(unsigned int flags)
{
	unsigned int memsize = JENT_FLAGS_TO_MAX_MEMSIZE(flags &
							 JENT_MAX_MEMSIZE_MASK);
	unsigned long long want;

	/*
	 * The memory size field is wider than the sizes the library defines,
	 * so an out-of-range value cannot make the shift below overflow.
	 */
	if (memsize > JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX))
		memsize = JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX);

	if (flags & JENT_DISABLE_MEMORY_ACCESS)
		want = 0;
	else if (memsize)
		want = 1ULL << (memsize + JENT_MAX_MEMSIZE_OFFSET);
	else
		want = JENT_MEMLOCK_DEFAULT;

	return want + JENT_MEMLOCK_SLACK;
}

/* Whether @flags ask for memory that is guaranteed to be locked. */
static inline int jent_memlock_required(unsigned int flags)
{
#if defined(LIBGCRYPT) || defined(OPENSSL)
	/*
	 * These backends lock their whole arena when it is created, before any
	 * collector exists and whatever the collector flags say, so the limit
	 * has to be raised for every run - not only for the compliance modes.
	 */
	(void)flags;
	return 1;
#else
	return !!(flags &
		  (JENT_NTG1 | JENT_FORCE_FIPS | JENT_FORCE_SECURE_MEM));
#endif
}

#if defined(_MSC_VER) || defined(__MINGW32__)

/*
 * Slack kept between the process minimum and maximum working set: the maximum
 * has to stay strictly greater than the minimum, otherwise
 * SetProcessWorkingSetSizeEx() rejects the call with ERROR_INVALID_PARAMETER.
 */
#define JENT_WS_HEADROOM	((SIZE_T)1 << 20)

/* Floor of the fallback in jent_raise_memlock_limit(). */
#define JENT_WS_MIN		((SIZE_T)1 << 20)

/*
 * Extend the process working set limits by @want bytes. Returns 0 on success.
 *
 * The limits are read and extended rather than set to a fixed size, so that
 * the raise adds to whatever the process was already granted instead of
 * replacing it. The same applies to the quota flags, which are read back with
 * GetProcessWorkingSetSizeEx() and handed to SetProcessWorkingSetSizeEx()
 * unchanged: raising the minimum is what lifts the lock quota, making that
 * minimum a *hard* limit (QUOTA_LIMITS_HARDWS_MIN_ENABLE) is not required for
 * it and would replace the quota policy the process may have established.
 */
static inline int jent_set_working_set(SIZE_T want)
{
	SIZE_T minWS = 0, maxWS = 0, want_min, want_max;
	DWORD ws_flags = 0;

	if (!GetProcessWorkingSetSizeEx(GetCurrentProcess(), &minWS, &maxWS,
					&ws_flags))
		return -1;

	/*
	 * A process that has never had its quota configured reports
	 * QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE,
	 * i.e. soft limits on both ends - which is all VirtualLock() needs.
	 * Should an implementation ever answer with no flags at all, name the
	 * documented default explicitly rather than forwarding a zero the
	 * setter may reject with ERROR_INVALID_PARAMETER.
	 */
	if (!ws_flags)
		ws_flags = QUOTA_LIMITS_HARDWS_MIN_DISABLE |
			   QUOTA_LIMITS_HARDWS_MAX_DISABLE;

	/* Headroom above the new minimum, and the guard against wrapping. */
	if (want > (SIZE_T)-1 - minWS)
		return -1;
	want_min = minWS + want;
	if (want_min > (SIZE_T)-1 - JENT_WS_HEADROOM)
		return -1;
	want_max = (maxWS > want_min + JENT_WS_HEADROOM) ?
		   maxWS : want_min + JENT_WS_HEADROOM;

	return SetProcessWorkingSetSizeEx(GetCurrentProcess(), want_min,
					  want_max, ws_flags) ? 0 : -1;
}

/*
 * Raise the process working set so that the entropy collector instantiated
 * with @flags can lock its memory. Best effort: the size requested is an upper
 * bound on what the collector may map, and the caller goes on to allocate it
 * either way. Returns 0 once the quota has been raised as far as this process
 * can raise it, and 1 only when the working set could not be changed at all.
 *
 * A request the system does not grant - it fails as the values approach the
 * physical memory available - is retried with half the size, down to
 * JENT_WS_MIN, so that an over-estimated request still leaves the process with
 * the largest quota the machine does allow.
 */
static inline int jent_raise_memlock_limit(unsigned int flags)
{
	SIZE_T want;

	if (!jent_memlock_required(flags))
		return 0;

	want = (SIZE_T)jent_memlock_size(flags);

	if (!jent_set_working_set(want))
		return 0;

	for (want /= 2; want >= JENT_WS_MIN; want /= 2) {
		if (!jent_set_working_set(want))
			return 0;
	}

	return 1;
}

#else /* !Windows */

/*
 * Raise RLIMIT_MEMLOCK so that the entropy collector instantiated with @flags
 * can lock its memory. Best effort: the size requested is an upper bound on
 * what the collector may map - the size it derives from the cache geometry is
 * not knowable here - and the caller goes on to allocate it either way.
 * Returns 0 once the limit has been raised as far as this process can raise
 * it, and 1 only when it could not be read or changed at all.
 *
 * Any process may raise its soft limit up to its hard limit; raising the hard
 * limit requires privilege (CAP_SYS_RESOURCE on Linux, root elsewhere). Both
 * are therefore requested in one call, and a rejected call is retried with the
 * hard limit left alone - which is what an unprivileged process gets, and all
 * it can get. Ending up at a hard limit below the requested size is not
 * reported as an error: the request is an upper bound, so the collector may
 * well fit under it, and where it does not the allocation failure the caller
 * reports is the accurate message. That is the case the recording README
 * describes as needing root.
 */
static inline int jent_raise_memlock_limit(unsigned int flags)
{
#ifdef RLIMIT_MEMLOCK
	unsigned long long want;
	struct rlimit lim;

	if (!jent_memlock_required(flags))
		return 0;

	want = jent_memlock_size(flags);

	if (getrlimit(RLIMIT_MEMLOCK, &lim))
		return 1;

	/* Nothing to do - the limit already covers the collector. */
	if (lim.rlim_cur == RLIM_INFINITY || (unsigned long long)lim.rlim_cur >= want)
		return 0;

	lim.rlim_cur = (rlim_t)want;
	if (lim.rlim_max != RLIM_INFINITY &&
	    (unsigned long long)lim.rlim_max < want)
		lim.rlim_max = (rlim_t)want;

	if (!setrlimit(RLIMIT_MEMLOCK, &lim))
		return 0;

	/*
	 * Unprivileged: take the soft limit up to the hard limit, which is as
	 * far as this process can go and is what allows the smaller memory
	 * sizes to be recorded without root.
	 */
	if (getrlimit(RLIMIT_MEMLOCK, &lim))
		return 1;
	if (lim.rlim_cur >= lim.rlim_max)
		return 0;

	lim.rlim_cur = lim.rlim_max;

	return setrlimit(RLIMIT_MEMLOCK, &lim) ? 1 : 0;
#else
	/*
	 * The platform has no per-process memory lock limit to raise (Solaris,
	 * for one, bounds the locked memory per project instead).
	 */
	(void)flags;
	return 0;
#endif /* RLIMIT_MEMLOCK */
}

#endif /* Windows */

/*
 * Floor and ceiling of the secure memory arena created below. The floor is the
 * size the library used to reserve itself and is what a run with no memory
 * access (JENT_DISABLE_MEMORY_ACCESS) still needs for the collector state and
 * the small allocations around it. The ceiling bounds the largest request -
 * JENT_MAX_MEMSIZE_MAX rounded up - to a value that still fits the unsigned
 * int libgcrypt takes for GCRYCTL_INIT_SECMEM.
 */
#define JENT_SECMEM_MIN		(2ULL << 20)
#define JENT_SECMEM_MAX		(1ULL << 30)

/*
 * Size of the arena for a collector instantiated with @flags: the memory the
 * collector may lock, rounded up to a power of two because OpenSSL's secure
 * heap requires that of its size.
 */
static inline unsigned long long jent_secmem_size(unsigned int flags)
{
	unsigned long long want = jent_memlock_size(flags);
	unsigned long long size = JENT_SECMEM_MIN;

	while (size < want && size < JENT_SECMEM_MAX)
		size <<= 1;

	return size;
}

/*
 * Create the secure memory arena the library allocates the entropy collector
 * from when it is built against libgcrypt or OpenSSL. Returns 0 on success and
 * when the build has no arena to configure; 1 when the arena could not be
 * created, in which case the allocation of the collector is what fails later
 * on - the library refuses to hand out memory that did not come from the
 * arena.
 *
 * Call this after jent_raise_memlock_limit(): both libraries lock the arena
 * into RAM as they create it, and only the raised limit lets them.
 */
static inline int jent_init_secure_memory(unsigned int flags)
{
#if defined(LIBGCRYPT)

	/*
	 * The initialization sequence libgcrypt documents. An application that
	 * has already run it, which is what GCRYCTL_INITIALIZATION_FINISHED
	 * records, is left alone: its arena is sized the way it wants it, and
	 * the size cannot be changed after the fact anyway.
	 *
	 * The secmem warning is suspended around the pool creation only, where
	 * libgcrypt would otherwise announce the yet-unlocked pool on stderr,
	 * and is resumed right after so that a genuinely insecure pool - one
	 * whose lock the system refused - is still reported.
	 */
	if (!gcry_check_version(GCRYPT_VERSION))
		return 1;

	if (gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P))
		return 0;

	gcry_control(GCRYCTL_SUSPEND_SECMEM_WARN);
	if (gcry_control(GCRYCTL_INIT_SECMEM,
			 (unsigned int)jent_secmem_size(flags), 0)) {
		gcry_control(GCRYCTL_RESUME_SECMEM_WARN);
		return 1;
	}
	gcry_control(GCRYCTL_RESUME_SECMEM_WARN);
	gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

	return 0;

#elif defined(OPENSSL)

	/*
	 * Both sizes must be powers of two and the minimum allocation must be
	 * smaller than the arena; 32 bytes is the granularity the library's own
	 * allocations are happy with. An arena another part of the process has
	 * already created is kept, as it cannot be resized either.
	 */
	if (CRYPTO_secure_malloc_initialized())
		return 0;

	return CRYPTO_secure_malloc_init((size_t)jent_secmem_size(flags), 32) ?
	       0 : 1;

#else

	(void)flags;

	return 0;

#endif
}

#endif /* _JITTERENTROPY_MEMLOCK_H */
