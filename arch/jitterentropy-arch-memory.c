/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific secure memory management.
 *
 * Definitions of jent_zalloc()/jent_zfree() and
 * jent_memset_secure() (declared in arch/jitterentropy-arch-memory.h). See that
 * header for the dispatch rationale. For the Linux kernel all allocations use
 * kvmalloc()/kvzalloc() (never kmalloc()) in order to be able to allocate large
 * buffers.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 *
 * License
 * =======
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU General Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
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
 * GetProcessWorkingSetSizeEx() and SetProcessWorkingSetSizeEx() are declared by
 * the Windows SDK only from Windows Vista onwards. mingw-w64 has defaulted to
 * older values across its releases, so the minimum is stated here rather than
 * left to the toolchain; it has to precede every system header, including the
 * <windows.h> included below. An externally supplied, higher value is left
 * alone.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

/*
 * MAP_ANONYMOUS, MAP_ANON and madvise()/MADV_DONTDUMP are all __USE_MISC on
 * glibc, so a strict -std=c11 - which the Makefile uses - hides them. Current
 * glibc happens to define MAP_ANONYMOUS unconditionally, which is why this was
 * only noticed on glibc 2.17 (RHEL 7), where neither spelling exists and the
 * MAP_ANON fallback below expands to an undeclared identifier.
 *
 * _DEFAULT_SOURCE is the modern spelling and _BSD_SOURCE the one glibc before
 * 2.19 understands; both are defined because 2.17 ignores the former and
 * everything from 2.20 on warns about the latter unless the former is present
 * too. Defined here rather than in the public jitterentropy.h so the header
 * imposes no feature-test macro on consumers; they must precede every system
 * header. Same reasoning as arch/jitterentropy-arch-timer.c.
 */
#if defined(__linux__)
# ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
# endif
# ifndef _BSD_SOURCE
#  define _BSD_SOURCE
# endif
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

/*
 * Platform detection.
 *
 * The POSIX backend is selected from the option macros the platform itself
 * publishes in <unistd.h>, not from a list of operating systems.
 *
 * _POSIX_MEMLOCK_RANGE is required to be strictly positive rather than merely
 * not -1: a value of 0 means "ask sysconf() at runtime", and since a failed
 * mlock() makes jent_zalloc() fail the allocation outright, such a platform is
 * better served by the malloc() path than by an allocator that might refuse
 * every request.
 */
#ifdef LINUX_KERNEL
# define JENT_ARCH_MEM_LINUX_KERNEL
#elif defined(_MSC_VER) || defined(__MINGW32__)
# define JENT_ARCH_MEM_WINDOWS
#else
# include <unistd.h>
# if defined(_POSIX_MAPPED_FILES) && (_POSIX_MAPPED_FILES - 0) > 0 &&	      \
     defined(_POSIX_MEMORY_PROTECTION) &&				      \
     (_POSIX_MEMORY_PROTECTION - 0) > 0 &&				      \
     defined(_POSIX_MEMLOCK_RANGE) && (_POSIX_MEMLOCK_RANGE - 0) > 0
#  define JENT_ARCH_MEM_POSIX_MLOCK
# endif
#endif

#ifdef JENT_ARCH_MEM_LINUX_KERNEL

#include <linux/mm.h>		/* kvmalloc(), kvzalloc(), kvfree_sensitive() */
#include <linux/slab.h>		/* GFP_KERNEL */
#include <linux/string.h>	/* memset() */

#else /* JENT_ARCH_MEM_LINUX_KERNEL */

#include <stdlib.h>
#include <string.h>

#ifdef LIBGCRYPT
# include <gcrypt.h>
#endif
#if defined(OPENSSL) || defined(AWSLC)
# include <openssl/crypto.h>
#endif
#ifdef OPENSSL
# include <openssl/evp.h>
#endif
#ifdef JENT_ARCH_MEM_WINDOWS
# include <windows.h>
#endif
#ifdef JENT_ARCH_MEM_POSIX_MLOCK
# include <sys/mman.h>
# include <errno.h>
# include <unistd.h>	/* sysconf() */
#endif

#endif /* JENT_ARCH_MEM_LINUX_KERNEL */

#define JENT_BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#define JENT_IS_POWER_OF_2(n) (JENT_BUILD_BUG_ON(((n) & ((n) - 1)) != 0))

/*
 * Slack kept between the process minimum and maximum working set when the
 * Windows backend raises them (see jent_virtual_lock()). The maximum only has
 * to exceed the minimum; the value merely avoids pinning the two together.
 */
#define JENT_WS_HEADROOM	65536

/*
 * Whether the active backend provides secure (locked / wiped) memory. This
 * mirrors the dispatch priority in jent_zalloc() below: the crypto libraries
 * take precedence over the OS mlock paths.
 */
#if defined(JENT_ARCH_MEM_LINUX_KERNEL)
  /*
   * Kernel memory is never paged out to swap, does not appear in user space
   * core dumps and is wiped on free via kvfree_sensitive().
   */
# define JENT_MEM_SECURE
#elif defined(LIBGCRYPT) || defined(OPENSSL)
# define JENT_MEM_SECURE
#elif defined(AWSLC)
  /* AWS-LC memory is wiped but not locked; not advertised as secure. */
#elif (defined(JENT_ARCH_MEM_WINDOWS) || defined(JENT_ARCH_MEM_POSIX_MLOCK)) && \
      !defined(JENT_CONF_RELAX_MLOCK)
# define JENT_MEM_SECURE
#endif

int jent_secure_memory_supported(void)
{
#ifdef JENT_MEM_SECURE
	return 1;
#else
	return 0;
#endif
}

void jent_memset_secure(void *s, size_t n)
{
#if defined(JENT_ARCH_MEM_LINUX_KERNEL)
	memzero_explicit(s, n);
#elif defined(AWSLC) || defined(OPENSSL)
	OPENSSL_cleanse(s, n);
#elif defined(JENT_ARCH_MEM_WINDOWS)
	SecureZeroMemory(s, n);
#else
	memset(s, 0, n);
	__asm__ __volatile__("" : : "r" (s) : "memory");
#endif
}

#if defined(JENT_ARCH_MEM_WINDOWS) && !defined(JENT_CONF_RELAX_MLOCK)
static size_t jent_pagesize(void)
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	return si.dwPageSize;
}

/*
 * Lock @len bytes at @addr into RAM. Returns 0 on success.
 *
 * VirtualLock() charges its pages against the process *minimum* working set -
 * "the maximum number of pages a process can lock is equal to the number of
 * pages in its minimum working set minus a small overhead" - and fails with
 * ERROR_WORKING_SET_QUOTA once that budget is exhausted. Raising the minimum
 * is therefore part of locking, not an independent knob: without it every
 * request larger than the (small) default minimum working set failed and
 * jent_entropy_collector_alloc() returned EMEM - which is what a JENT_CACHE_ALL
 * collector, asking for the summed L1+L2+L3 size, ran into on every machine.
 *
 * The limits are read and extended rather than set to a fixed size: they are
 * process-wide, so overwriting them would evict the working set the host
 * application has reserved for itself. dwMaximumWorkingSetSize must stay
 * strictly greater than dwMinimumWorkingSetSize, otherwise the call is
 * rejected with ERROR_INVALID_PARAMETER.
 *
 * The same applies to the quota flags, which is why they are read back with
 * GetProcessWorkingSetSizeEx() and handed to SetProcessWorkingSetSizeEx()
 * unchanged. The non-Ex getter used here before cannot report them, so the
 * flags argument was a fixed QUOTA_LIMITS_HARDWS_MIN_ENABLE: that silently
 * replaced whatever quota policy the host application had established and left
 * the process with a hard working-set floor - one this code never lowers again
 * - as a side effect of allocating an entropy collector. Raising the minimum is
 * what lifts the lock quota; making that minimum a hard limit is not required
 * for it and is not a library's decision to make.
 *
 * The raise happens only after a lock has actually failed, and the quota is
 * not lowered again on free (another thread may have locked memory against it
 * in the meantime). Growing on demand is what keeps a repeated
 * allocate/free cycle from ratcheting the process working set up without
 * bound: once the quota covers the collector, every later lock succeeds on
 * the first attempt.
 */
static int jent_virtual_lock(void *addr, size_t len)
{
	SIZE_T minWS = 0, maxWS = 0, want_min, want_max;
	DWORD ws_flags = 0;

	if (VirtualLock(addr, len))
		return 0;
	if (GetLastError() != ERROR_WORKING_SET_QUOTA)
		return -1;

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
	if (len > (SIZE_T)-1 - minWS)
		return -1;
	want_min = minWS + len;
	if (want_min > (SIZE_T)-1 - JENT_WS_HEADROOM)
		return -1;
	want_max = (maxWS > want_min + JENT_WS_HEADROOM) ?
		   maxWS : want_min + JENT_WS_HEADROOM;

	if (!SetProcessWorkingSetSizeEx(GetCurrentProcess(), want_min, want_max,
					ws_flags))
		return -1;

	return VirtualLock(addr, len) ? 0 : -1;
}
#endif /* JENT_ARCH_MEM_WINDOWS && !JENT_CONF_RELAX_MLOCK */

#ifdef JENT_ARCH_MEM_POSIX_MLOCK
/* Some BSDs / macOS only provide the older MAP_ANON spelling. */
# ifndef MAP_ANONYMOUS
#  define MAP_ANONYMOUS MAP_ANON
# endif

static size_t jent_pagesize(void)
{
	long page_size = sysconf(_SC_PAGESIZE);

	return (page_size <= 0) ? 4096 : (size_t)page_size;
}
#endif /* JENT_ARCH_MEM_POSIX_MLOCK */

#ifdef JENT_ARCH_MEM_LINUX_KERNEL

void *jent_zalloc(size_t len)
{
	return kvzalloc(len, GFP_KERNEL);
}

void jent_zfree(void *ptr, size_t len)
{
	kvfree_sensitive(ptr, len);
}

#else /* !JENT_ARCH_MEM_LINUX_KERNEL */

void *jent_zalloc(size_t len)
{
	void *tmp = NULL;

#ifdef LIBGCRYPT

	/*
	 * Set the maximum usable locked memory to 2 MiB at first call.
	 *
	 * You may have to adapt or delete this if you also use libgcrypt
	 * elsewhere in your software!
	 */
	if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
		gcry_control(GCRYCTL_INIT_SECMEM, JENT_SECURE_MEMORY_SIZE_MAX, 0);
		gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	}
	/*
	 * When using the libgcrypt secure memory mechanism, all precautions
	 * are taken to protect our state. If the user disables secmem during
	 * runtime, it is their decision and we thus try not to overrule that
	 * decision for less memory protection.
	 */
	/*
	 * gcry_malloc_secure(), not gcry_xmalloc_secure(): the x-variant
	 * invokes libgcrypt's fatal out-of-core handler when the secmem pool
	 * is exhausted, terminating the host process from inside the library.
	 * The NULL return is handled by all callers.
	 */
	tmp = gcry_malloc_secure(len);

#elif defined(AWSLC)

	tmp = OPENSSL_malloc(len);

#elif defined(OPENSSL)

	/*
	 * Initialize OpenSSL secure malloc here only if not already done.
	 * The 2 MiB max reserved is sufficient for jitterentropy but probably
	 * too small for a whole application doing crypto operations with
	 * OpenSSL. Both min and max value must be a power of 2; min must be
	 * smaller than max.
	 */
	JENT_IS_POWER_OF_2(JENT_SECURE_MEMORY_SIZE_MAX);
	if (CRYPTO_secure_malloc_initialized() ||
	    CRYPTO_secure_malloc_init(JENT_SECURE_MEMORY_SIZE_MAX, 32)) {
		tmp = OPENSSL_secure_malloc(len);
	}
	/*
	 * If secure memory was not available, OpenSSL falls back to "normal"
	 * memory. Double check.
	 */
	if (tmp && !CRYPTO_secure_allocated(tmp)) {
		OPENSSL_secure_free(tmp);
		tmp = NULL;
	}

#elif defined(JENT_ARCH_MEM_WINDOWS)

# ifndef JENT_CONF_RELAX_MLOCK
	{
		/*
		 * Guard-page layout as on the POSIX path: commit the whole
		 * region inaccessible and enable only the payload, leaving a
		 * PAGE_NOACCESS guard page on each side that faults on any
		 * accidental access beyond the state.
		 */
		size_t page_size = jent_pagesize();
		size_t payload, total;
		uint8_t *base;
		DWORD oldprot;

		if (len > (size_t)-1 - 3 * page_size)
			return NULL;
		payload = (len + page_size - 1) & ~(page_size - 1);
		total = payload + 2 * page_size;

		base = VirtualAlloc(NULL, total, MEM_COMMIT | MEM_RESERVE,
				    PAGE_NOACCESS);
		if (!base)
			return NULL;
		if (!VirtualProtect(base + page_size, payload, PAGE_READWRITE,
				    &oldprot)) {
			VirtualFree(base, 0, MEM_RELEASE);
			return NULL;
		}

		tmp = base + page_size;

		if (jent_virtual_lock(tmp, payload)) {
			VirtualFree(base, 0, MEM_RELEASE);
			return NULL;
		}
	}
# else
	tmp = malloc(len);
# endif

#elif defined(JENT_ARCH_MEM_POSIX_MLOCK)

	{
		/*
		 * Layout: [guard page | payload (page-rounded) | guard page]
		 *
		 * One inaccessible guard page on each side of the payload, so
		 * that any accidental access beyond the state faults
		 * immediately instead of silently reading or corrupting
		 * adjacent data. The page-aligned payload is also what allows
		 * the madvise() dump exclusion below.
		 *
		 * The region is mapped readable and writable and the two guard
		 * pages are then protected *down* to PROT_NONE. The reverse -
		 * map the whole region PROT_NONE and raise the payload to
		 * PROT_READ|PROT_WRITE - is the more common spelling of this
		 * idiom and is what this code did until NetBSD rejected it:
		 * mprotect() there returns EACCES, "the requested protection
		 * would exceed the maximum protection allowed on the region",
		 * because NetBSD clamps a mapping's maximum protection to the
		 * protection mmap() was called with. A region mapped PROT_NONE
		 * can then never be made accessible again, so jent_zalloc()
		 * failed every allocation on that platform and every caller
		 * reported it as an out-of-memory condition.
		 *
		 * Lowering protection is permitted everywhere, so this order
		 * needs no platform conditional and reaches the same final
		 * layout. The guard pages are briefly writable, which is not
		 * observable: the mapping is fresh and its address has not
		 * been handed out yet.
		 */
		size_t page_size = jent_pagesize();
		size_t payload, total;
		uint8_t *base;
		/*
		 * OpenBSD excludes a mapping from core dumps at mmap() time
		 * rather than through madvise(); it has no MADV_DONTDUMP or
		 * MADV_NOCORE. The flag is the OpenBSD counterpart of the
		 * madvise() call below.
		 */
		int mmap_flags = MAP_PRIVATE | MAP_ANONYMOUS;

# ifdef MAP_CONCEAL
		mmap_flags |= MAP_CONCEAL;
# endif

		if (len > SIZE_MAX - 3 * page_size)
			return NULL;
		payload = (len + page_size - 1) & ~(page_size - 1);
		total = payload + 2 * page_size;

		base = mmap(NULL, total, PROT_READ | PROT_WRITE, mmap_flags,
			    -1, 0);
		if (base == MAP_FAILED)
			return NULL;

		if (mprotect(base, page_size, PROT_NONE) ||
		    mprotect(base + page_size + payload, page_size,
			     PROT_NONE)) {
			munmap(base, total);
			return NULL;
		}

		tmp = base + page_size;

		/*
		 * Exclude the secret state from core dumps. Best effort: the
		 * advice is a hardening measure, so its absence or failure
		 * (e.g. old kernels) does not fail the allocation. No revert
		 * is needed on free: munmap() destroys the mapping including
		 * its madvise state.
		 *
		 * macOS provides no equivalent - it has neither MADV_DONTDUMP
		 * nor MADV_NOCORE nor MAP_CONCEAL - so on that platform the
		 * state stays mlock()ed but is not excluded from a core dump.
		 * The only lever there is process-wide (RLIMIT_CORE), which is
		 * the application's decision to make, not a library's.
		 */
# if defined(MADV_DONTDUMP)
		madvise(tmp, len, MADV_DONTDUMP);	/* Linux */
# elif defined(MADV_NOCORE)
		madvise(tmp, len, MADV_NOCORE);		/* FreeBSD */
# endif

		/*
		 * Prevent paging out of the memory state to swap space. If
		 * this fails, check the current memory lock limits and
		 * capabilities (e.g. RLIMIT_MEMLOCK and CAP_IPC_LOCK).
		 */
# ifndef JENT_CONF_RELAX_MLOCK
		if (mlock(tmp, len)) {
# else
		/*
		 * Use this only for CI or restricted containers if not
		 * possible otherwise.
		 */
		if (mlock(tmp, len) && errno != EPERM && errno != EAGAIN) {
# endif
			munmap(base, total);
			return NULL;
		}
	}

#else /* no secure memory mechanism available */

	tmp = malloc(len);

#endif

	if (tmp != NULL)
		jent_memset_secure(tmp, len);
	return tmp;
}

void jent_zfree(void *ptr, size_t len)
{
#ifdef LIBGCRYPT

	/*
	 * gcry_free automatically wipes memory allocated with
	 * gcry_(x)malloc_secure.
	 */
	(void)len;
	gcry_free(ptr);

#elif defined(AWSLC)

	/*
	 * AWS-LC stores the length of allocated memory internally and
	 * automatically wipes it in OPENSSL_free.
	 */
	(void)len;
	OPENSSL_free(ptr);

#elif defined(OPENSSL)

	OPENSSL_cleanse(ptr, len);
	OPENSSL_secure_free(ptr);

#elif defined(JENT_ARCH_MEM_WINDOWS)

	SecureZeroMemory(ptr, len);
# ifndef JENT_CONF_RELAX_MLOCK
	{
		/*
		 * Mirror the guard-page layout of jent_zalloc(): the region
		 * starts one page before the returned pointer.
		 */
		size_t page_size = jent_pagesize();
		size_t payload = (len + page_size - 1) & ~(page_size - 1);
		uint8_t *base = (uint8_t *)ptr - page_size;

		VirtualUnlock(ptr, payload);
		VirtualFree(base, 0, MEM_RELEASE);
	}
# else
	free(ptr);
# endif

#elif defined(JENT_ARCH_MEM_POSIX_MLOCK)

	{
		/*
		 * Mirror the guard-page layout of jent_zalloc(): the mapping
		 * starts one page before the returned pointer and covers the
		 * page-rounded payload plus the two guard pages.
		 */
		size_t page_size = jent_pagesize();
		size_t payload = (len + page_size - 1) & ~(page_size - 1);
		uint8_t *base = (uint8_t *)ptr - page_size;

		jent_memset_secure(ptr, len);

		/*
		 * munmap() unlocks the pages and drops the madvise state with
		 * the mapping; the memory does not travel through a heap
		 * allocator, so nothing can be handed out again unwiped or
		 * with stale dump exclusion.
		 */
		munmap(base, payload + 2 * page_size);
	}

#else

	jent_memset_secure(ptr, len);
	free(ptr);

#endif
}

#endif /* JENT_ARCH_MEM_LINUX_KERNEL */

#undef JENT_WS_HEADROOM
#undef JENT_IS_POWER_OF_2
#undef JENT_BUILD_BUG_ON
