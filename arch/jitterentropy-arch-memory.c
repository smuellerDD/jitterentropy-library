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

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

/*
 * Platform detection.
 */
#ifdef LINUX_KERNEL
# define JENT_ARCH_MEM_LINUX_KERNEL
#else
# if defined(_MSC_VER) || defined(__MINGW32__)
#  define JENT_ARCH_MEM_WINDOWS
# elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
       defined(__NetBSD__) || defined(__APPLE__)
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

#ifdef JENT_ARCH_MEM_WINDOWS
static size_t jent_pagesize(void)
{
	SYSTEM_INFO si;

	GetSystemInfo(&si);
	return si.dwPageSize;
}
#endif /* JENT_ARCH_MEM_WINDOWS */

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
		size_t minWS, maxWS;

		JENT_BUILD_BUG_ON(JENT_SECURE_MEMORY_SIZE_MAX / 2 == 0);
		/*
		 * On query failure assume a zero working set so the raise
		 * below always runs; the outputs are uninitialized otherwise.
		 */
		if (!GetProcessWorkingSetSize(GetCurrentProcess(), &minWS,
					      &maxWS))
			minWS = maxWS = 0;
		if (maxWS < JENT_SECURE_MEMORY_SIZE_MAX &&
		    !SetProcessWorkingSetSizeEx(
			GetCurrentProcess(),
			JENT_SECURE_MEMORY_SIZE_MAX / 2,
			JENT_SECURE_MEMORY_SIZE_MAX,
			QUOTA_LIMITS_HARDWS_MIN_ENABLE))
			return NULL;

		{
			/*
			 * Guard-page layout as on the POSIX path: commit the
			 * whole region inaccessible and enable only the
			 * payload, leaving a PAGE_NOACCESS guard page on each
			 * side that faults on any accidental access beyond
			 * the state.
			 */
			size_t page_size = jent_pagesize();
			size_t payload, total;
			uint8_t *base;
			DWORD oldprot;

			if (len > (size_t)-1 - 3 * page_size)
				return NULL;
			payload = (len + page_size - 1) & ~(page_size - 1);
			total = payload + 2 * page_size;

			base = VirtualAlloc(NULL, total,
					    MEM_COMMIT | MEM_RESERVE,
					    PAGE_NOACCESS);
			if (!base)
				return NULL;
			if (!VirtualProtect(base + page_size, payload,
					    PAGE_READWRITE, &oldprot)) {
				VirtualFree(base, 0, MEM_RELEASE);
				return NULL;
			}

			tmp = base + page_size;

			if (!VirtualLock(tmp, payload)) {
				VirtualFree(base, 0, MEM_RELEASE);
				return NULL;
			}
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
		 * The whole region is mapped PROT_NONE first and only the
		 * payload is made accessible, leaving one inaccessible guard
		 * page on each side: any accidental access beyond the state
		 * faults immediately instead of silently reading or
		 * corrupting adjacent data. The page-aligned payload is also
		 * what allows the madvise() dump exclusion below.
		 */
		size_t page_size = jent_pagesize();
		size_t payload, total;
		uint8_t *base;

		if (len > SIZE_MAX - 3 * page_size)
			return NULL;
		payload = (len + page_size - 1) & ~(page_size - 1);
		total = payload + 2 * page_size;

		base = mmap(NULL, total, PROT_NONE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (base == MAP_FAILED)
			return NULL;

		if (mprotect(base + page_size, payload,
			     PROT_READ | PROT_WRITE)) {
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

#undef JENT_IS_POWER_OF_2
#undef JENT_BUILD_BUG_ON
