/*
 * Non-physical true random number generator based on timing jitter.
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
 * Architecture / OS-specific secure memory management.
 *
 * Provides:
 *   - jent_zalloc(len): allocate zeroed memory, locked into RAM where the
 *     platform supports it (mlock, VirtualLock, libgcrypt secmem, OpenSSL
 *     secure heap, ...).
 *   - jent_zfree(ptr, len): zero and release memory previously allocated
 *     with jent_zalloc().
 *   - jent_memset_secure(s, n): wipe a buffer in a way the compiler may
 *     not optimize away.
 *
 * The dispatch order is:
 *   - LIBGCRYPT     -> gcry_xmalloc_secure / gcry_free
 *   - AWSLC         -> OPENSSL_malloc / OPENSSL_free (auto-wipe)
 *   - OPENSSL       -> OPENSSL_secure_malloc / OPENSSL_secure_free
 *   - Windows       -> VirtualAlloc + VirtualLock (or plain malloc with
 *                      JENT_CONF_RELAX_MLOCK)
 *   - Linux/BSD/Mac -> malloc + mlock (or plain malloc with
 *                      JENT_CONF_RELAX_MLOCK)
 *   - other         -> plain malloc
 *
 * When the active path provides locked / wiped memory, the macro
 * CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY is defined; consumers
 * (e.g. src/jitterentropy-base.c) check this to advertise secure memory
 * support at runtime.
 */

#ifndef _JITTERENTROPY_ARCH_MEMORY_H
#define _JITTERENTROPY_ARCH_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef LIBGCRYPT
# include <gcrypt.h>
#endif

#if defined(OPENSSL)
# include <openssl/crypto.h>
# include <openssl/evp.h>
#endif

#if defined(AWSLC)
# include <openssl/crypto.h>
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# define JENT_ARCH_MEM_WINDOWS
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__APPLE__)
# include <sys/mman.h>
# include <errno.h>
# define JENT_ARCH_MEM_POSIX_MLOCK
#endif

/* Override this if you want to allocate more than 2 MB of secure memory */
#ifndef JENT_SECURE_MEMORY_SIZE_MAX
# define JENT_SECURE_MEMORY_SIZE_MAX 2097152
#endif

#define JENT_BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#define JENT_IS_POWER_OF_2(n) (JENT_BUILD_BUG_ON((n & (n - 1)) != 0))

static inline void jent_memset_secure(void *s, size_t n)
{
#if defined(AWSLC) || defined(OPENSSL)
	OPENSSL_cleanse(s, n);
#elif defined(JENT_ARCH_MEM_WINDOWS)
	SecureZeroMemory(s, n);
#else
	memset(s, 0, n);
	__asm__ __volatile__("" : : "r" (s) : "memory");
#endif
}

#ifdef JENT_ARCH_MEM_WINDOWS
static inline size_t jent_round_up_to_pagesize(size_t size)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	size_t page_size = si.dwPageSize;

	return (size + page_size - 1) & ~(page_size - 1);
}
#endif /* JENT_ARCH_MEM_WINDOWS */

static inline void *jent_zalloc(size_t len)
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
# define CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	tmp = gcry_xmalloc_secure(len);

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
# define CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
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
#  define CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	{
		size_t minWS, maxWS;

		JENT_BUILD_BUG_ON(JENT_SECURE_MEMORY_SIZE_MAX / 2 == 0);
		GetProcessWorkingSetSize(GetCurrentProcess(), &minWS, &maxWS);
		if (maxWS < JENT_SECURE_MEMORY_SIZE_MAX &&
		    !SetProcessWorkingSetSizeEx(
			GetCurrentProcess(),
			JENT_SECURE_MEMORY_SIZE_MAX / 2,
			JENT_SECURE_MEMORY_SIZE_MAX,
			QUOTA_LIMITS_HARDWS_MIN_ENABLE))
			return NULL;

		len = jent_round_up_to_pagesize(len);
		tmp = VirtualAlloc(NULL, len, MEM_COMMIT | MEM_RESERVE,
				   PAGE_READWRITE);
		if (!tmp)
			return NULL;
		if (!VirtualLock(tmp, len)) {
			VirtualFree(tmp, 0, MEM_RELEASE);
			return NULL;
		}
	}
# else
	tmp = malloc(len);
# endif

#elif defined(JENT_ARCH_MEM_POSIX_MLOCK)

	tmp = malloc(len);
	if (!tmp)
		return NULL;
	/*
	 * Prevent paging out of the memory state to swap space. If this
	 * fails, check the current memory lock limits and capabilities
	 * (e.g. RLIMIT_MEMLOCK and CAP_IPC_LOCK).
	 */
# ifndef JENT_CONF_RELAX_MLOCK
#  define CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	if (mlock(tmp, len)) {
# else
	/*
	 * Use this only for CI or restricted containers if not possible
	 * otherwise.
	 */
	if (mlock(tmp, len) && errno != EPERM && errno != EAGAIN) {
# endif
		free(tmp);
		return NULL;
	}

#else /* no secure memory mechanism available */

	tmp = malloc(len);

#endif

	if (tmp != NULL)
		jent_memset_secure(tmp, len);
	return tmp;
}

static inline void jent_zfree(void *ptr, size_t len)
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
	len = jent_round_up_to_pagesize(len);
	VirtualUnlock(ptr, len);
	VirtualFree(ptr, 0, MEM_RELEASE);
# else
	free(ptr);
# endif

#elif defined(JENT_ARCH_MEM_POSIX_MLOCK)

	/*
	 * While memory returned to the OS is automatically unlocked, it is
	 * not known how long libc keeps this memory cached internally;
	 * therefore it is more robust to unlock here.
	 */
	munlock(ptr, len);
	jent_memset_secure(ptr, len);
	free(ptr);

#else

	jent_memset_secure(ptr, len);
	free(ptr);

#endif
}

#undef JENT_IS_POWER_OF_2
#undef JENT_BUILD_BUG_ON

#endif /* _JITTERENTROPY_ARCH_MEMORY_H */
