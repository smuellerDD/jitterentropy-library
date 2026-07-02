/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific secure memory management.
 *
 * Definitions of jent_zalloc()/jent_zalloc_large()/jent_zfree() and
 * jent_memset_secure() (declared in arch/jitterentropy-arch-memory.h). See that
 * header for the dispatch rationale. For the Linux kernel all allocations use
 * kvmalloc()/kvzalloc() (never kmalloc()): the buffers are only accessed by the
 * CPU, so a vmalloc() fallback for large sizes is fine.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

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
#endif

#endif /* JENT_ARCH_MEM_LINUX_KERNEL */

#define JENT_BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))
#define JENT_IS_POWER_OF_2(n) (JENT_BUILD_BUG_ON((n & (n - 1)) != 0))

/*
 * Whether the active backend provides secure (locked / wiped) memory. This
 * mirrors the dispatch priority in jent_zalloc() below: the crypto libraries
 * take precedence over the OS mlock paths.
 */
#if defined(LIBGCRYPT) || defined(OPENSSL)
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
static size_t jent_round_up_to_pagesize(size_t size)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	size_t page_size = si.dwPageSize;

	return (size + page_size - 1) & ~(page_size - 1);
}
#endif /* JENT_ARCH_MEM_WINDOWS */

#ifdef JENT_ARCH_MEM_LINUX_KERNEL

void *jent_zalloc(size_t len)
{
	void *tmp = kvmalloc(len, GFP_KERNEL);

	if (tmp)
		jent_memset_secure(tmp, len);

	return tmp;
}

void *jent_zalloc_large(size_t len)
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

void *jent_zalloc_large(size_t len)
{
	return jent_zalloc(len);
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

#endif /* JENT_ARCH_MEM_LINUX_KERNEL */

#undef JENT_IS_POWER_OF_2
#undef JENT_BUILD_BUG_ON
