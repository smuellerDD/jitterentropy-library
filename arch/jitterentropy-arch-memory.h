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
 * Provides (defined in arch/jitterentropy-arch-memory.c):
 *   - jent_zalloc(len): allocate zeroed memory, locked into RAM where the
 *     platform supports it (mlock, VirtualLock, libgcrypt secmem, OpenSSL
 *     secure heap, ...). jent_zalloc_large() is the same for larger buffers;
 *     raise JENT_SECURE_MEMORY_SIZE_MAX (and the memlock limits) when
 *     requesting memory sizes beyond the default secure-arena capacity.
 *   - jent_zfree(ptr, len): zero and release memory previously allocated
 *     with jent_zalloc() or jent_zalloc_large().
 *   - jent_memset_secure(s, n): wipe a buffer in a way the compiler may
 *     not optimize away.
 *
 * The dispatch order is:
 *   - LIBGCRYPT     -> gcry_malloc_secure / gcry_free
 *   - AWSLC         -> OPENSSL_malloc / OPENSSL_free (auto-wipe)
 *   - OPENSSL       -> OPENSSL_secure_malloc / OPENSSL_secure_free
 *   - Windows       -> VirtualAlloc + VirtualLock with PAGE_NOACCESS guard
 *                      pages around the payload (or plain malloc with
 *                      JENT_CONF_RELAX_MLOCK)
 *   - Linux/BSD/Mac -> mmap + mlock with PROT_NONE guard pages around the
 *                      payload; additionally excluded from core dumps via
 *                      madvise(MADV_DONTDUMP) on Linux and
 *                      madvise(MADV_NOCORE) on FreeBSD, best effort. With
 *                      JENT_CONF_RELAX_MLOCK only the mlock failure is
 *                      tolerated; the layout is unchanged.
 *   - Linux Kernel  -> kvmalloc + jent_memset_secure
 *   - other         -> plain malloc
 *
 * Whether the active path provides locked / wiped memory is reported at
 * runtime by jent_secure_memory_supported() (defined in
 * arch/jitterentropy-arch-memory.c).
 */

#ifndef _JITTERENTROPY_ARCH_MEMORY_H
#define _JITTERENTROPY_ARCH_MEMORY_H

/*
 * Platform detection. Only the selector macros are set here; the corresponding
 * headers are included by arch/jitterentropy-arch-memory.c, so consumers of
 * this header pull in nothing beyond the declarations below.
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

/* Override this if you want to allocate more than 2 MB of secure memory */
#ifndef JENT_SECURE_MEMORY_SIZE_MAX
# define JENT_SECURE_MEMORY_SIZE_MAX 2097152
#endif

void jent_memset_secure(void *s, size_t n);
void *jent_zalloc(size_t len);
void *jent_zalloc_large(size_t len);
void jent_zfree(void *ptr, size_t len);

/*
 * Return whether the active memory backend provides secure (locked / wiped)
 * memory. Also declared in the public jitterentropy.h.
 */
int jent_secure_memory_supported(void);

#endif /* _JITTERENTROPY_ARCH_MEMORY_H */
