/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2013 - 2025
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
 e USE OF THIS SOFTWARE, EVEN IF NOT ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

#ifndef _JITTERENTROPY_BASE_WINDOWS_H
#define _JITTERENTROPY_BASE_WINDOWS_H

#include <stdint.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
typedef int64_t ssize_t;
#include <windows.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <intrin.h>

#ifdef LIBGCRYPT
#include <gcrypt.h>
#endif

#ifdef OPENSSL
#include <openssl/crypto.h>
#include <openssl/evp.h>
#endif

#if defined(AWSLC)
#include <openssl/crypto.h>
#endif

#if defined(_M_ARM) || defined(_M_ARM64)
#include <profileapi.h>
#include <windows.h>
#endif

/* Override this, if you want to allocate more than 2 MB of secure memory */
#ifndef JENT_SECURE_MEMORY_SIZE_MAX
#define JENT_SECURE_MEMORY_SIZE_MAX 2097152
#endif

static inline void jent_get_nstime(uint64_t *out)
{
#if defined(_M_ARM) || defined(_M_ARM64)

	/* Generic code. */
	LARGE_INTEGER ticks;
	QueryPerformanceCounter(&ticks);
	*out = ticks.QuadPart;

#else

	/* x86, x86_64 intrinsic */
	*out = __rdtsc();

#endif
}

static inline void *jent_zalloc(size_t len)
{
	void *tmp = NULL;
#ifdef LIBGCRYPT
	/* Set the maximum usable locked memory to 2 MiB at fist call.
	 *
	 * You may have to adapt or delete this, if you
	 * also use libgcrypt at other places in your software!
	 */
	if (!gcry_control (GCRYCTL_INITIALIZATION_FINISHED_P)) {
		gcry_control(GCRYCTL_INIT_SECMEM, JENT_SECURE_MEMORY_SIZE_MAX, 0);
		gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
	}
	/* When using the libgcrypt secure memory mechanism, all precautions
	 * are taken to protect our state. If the user disables secmem during
	 * runtime, it is his decision and we thus try not to overrule his
	 * decision for less memory protection. */
#define CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	tmp = gcry_xmalloc_secure(len);
#elif defined(AWSLC)
	tmp = OPENSSL_malloc(len);
#elif defined(OPENSSL)
	/* We only call secure malloc initialization here,
	 * if not already done. The 2 MiB max. reserved here
	 * are sufficient for jitterentropy but probably
	 * too small for a whole application doing crypto
	 * operations with OpenSSL.
	 *
	 * Both min and max value must be power of 2.
	 * min must be smaller than max.
	 *
	 * May preallocate more before making the first
	 * call into jitterentropy!*/
	if (CRYPTO_secure_malloc_initialized() ||
	    CRYPTO_secure_malloc_init(JENT_SECURE_MEMORY_SIZE_MAX, 32)) {
		tmp = OPENSSL_secure_malloc(len);
	}
#define CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY
	/* If secure memory was not available, OpenSSL
	 * falls back to "normal" memory. So double check. */
	if (tmp && !CRYPTO_secure_allocated(tmp)) {
		OPENSSL_secure_free(tmp);
		tmp = NULL;
	}
#else
	/* we have no secure memory allocation! Hence
	 * we do not set CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY */
	tmp = malloc(len);
#endif /* LIBGCRYPT */
	if(NULL != tmp)
		SecureZeroMemory(tmp, len);
	return tmp;
}

static inline void jent_memset_secure(void *s, size_t n)
{
#if defined(AWSLC) || defined(OPENSSL)
	OPENSSL_cleanse(s, n);
#else
	SecureZeroMemory(s, n);
#endif
}

static inline void jent_zfree(void *ptr, unsigned int len)
{
#ifdef LIBGCRYPT
	/* gcry_free automatically wipes memory allocated with
	 * gcry_(x)malloc_secure */
	(void) len;
	gcry_free(ptr);
#elif defined(AWSLC)
	/* AWS-LC stores the length of allocated memory internally and automatically wipes it in OPENSSL_free */
	(void) len;
	OPENSSL_free(ptr);
#elif defined(OPENSSL)
	OPENSSL_cleanse(ptr, len);
	OPENSSL_secure_free(ptr);
#else
	SecureZeroMemory(ptr, len);
	free(ptr);
#endif /* LIBGCRYPT */
}

static inline int jent_fips_enabled(void)
{
#ifdef LIBGCRYPT
	return gcry_fips_mode_active();
#elif defined(AWSLC)
	return FIPS_mode();
#elif defined(OPENSSL)
	return EVP_default_properties_is_fips_enabled(NULL);
#else
	return 0;
#endif /* LIBGCRYPT */
}

static inline long jent_ncpu(void)
{
	return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
}

static inline void jent_yield(void)
{
	YieldProcessor();
	SwitchToThread();
}

static inline uint32_t jent_cache_size_roundup(int all_caches)
{
	uint32_t cache_size = 0;
	DWORD l1 = 0, l2 = 0, l3 = 0;
	DWORD len = 0;
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;

	/* First call to get buffer size */
	if (!GetLogicalProcessorInformation(NULL, &len) &&
		GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
		return 0;
	}

	buffer = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION)malloc(len);
	if (!buffer)
		return 0;

	/* Second call to retrieve data */
	if (!GetLogicalProcessorInformation(buffer, &len)) {
		free(buffer);
		return 0;
	}

	DWORD count = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);

	for (DWORD i = 0; i < count; i++) {
		if (buffer[i].Relationship == RelationCache) {
			CACHE_DESCRIPTOR* cache = &buffer[i].Cache;

			if (cache->Level == 1 && cache->Type == CacheData) {
				l1 = cache->Size;
			}
			else if (cache->Level == 2 && (cache->Type == CacheUnified || cache->Type == CacheData)) {
				l2 = cache->Size;
			}
			else if (cache->Level == 3 && (cache->Type == CacheUnified || cache->Type == CacheData)) {
				l3 = cache->Size;
			}
		}
	}

	free(buffer);

	/* Cache size reported by system */
	if (l1 > 0)
		cache_size += (uint32_t)l1;
	if (all_caches) {
		if (l2 > 0)
			cache_size += (uint32_t)l2;
		if (l3 > 0)
			cache_size += (uint32_t)l3;
	}

	/*
	 * Force the output_size to be of the form
	 * (bounding_power_of_2 - 1).
	 */
	cache_size |= (cache_size >> 1);
	cache_size |= (cache_size >> 2);
	cache_size |= (cache_size >> 4);
	cache_size |= (cache_size >> 8);
	cache_size |= (cache_size >> 16);

	if (cache_size == 0)
		return 0;

	/*
	 * Make the output_size the smallest power of 2 strictly
	 * greater than cache_size.
	 */
	cache_size++;

	return cache_size;
}

/* --- helpers needed in user space -- */

/* note: these helper functions are shamelessly stolen from the kernel :-) */

static inline uint64_t rol64(uint64_t word, unsigned int shift)
{
	return (word << shift) | (word >> (64 - shift));
}

#endif /* _JITTERENTROPY_BASE_WINDOWS_H */

