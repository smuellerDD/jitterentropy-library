/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2013
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

#ifndef _JITTERENTROPY_BASE_X86_H
#define _JITTERENTROPY_BASE_X86_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef uint64_t __u64;

/* taken from Linux kernel */
#ifdef X86_64
#define DECLARE_ARGS(val, low, high)    unsigned low, high
#define EAX_EDX_VAL(val, low, high)     ((low) | ((__u64)(high) << 32))
#define EAX_EDX_RET(val, low, high)     "=a" (low), "=d" (high)
#else   
#define DECLARE_ARGS(val, low, high)    unsigned long long val
#define EAX_EDX_VAL(val, low, high)     (val)
#define EAX_EDX_RET(val, low, high)     "=A" (val)
#endif

static inline void jent_get_nstime(__u64 *out)
{
	DECLARE_ARGS(val, low, high);
	asm volatile("rdtsc" : EAX_EDX_RET(val, low, high));
	*out = EAX_EDX_VAL(val, low, high);
}

static inline void *jent_zalloc(size_t len)
{
	void *tmp = NULL;
	/* we have no secure memory allocation! Hence
	 * we do not sed CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY */
	tmp = malloc(len);
	if(NULL != tmp)
		memset(tmp, 0, len);
	return tmp;
}

static inline void jent_zfree(void *ptr, unsigned int len)
{
	memset(ptr, 0, len);
	free(ptr);
}

static inline int jent_fips_enabled(void)
{
        return 0;
}

/* --- helpers needed in user space -- */

/* note: these helper functions are shamelessly stolen from the kernel :-) */

static inline __u64 rol64(__u64 word, unsigned int shift)
{
	return (word << shift) | (word >> (64 - shift));
}


#endif /* _JITTERENTROPY_BASE_X86_H */

