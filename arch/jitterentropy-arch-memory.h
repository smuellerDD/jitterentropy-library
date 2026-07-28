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
 *   - jent_zalloc(len, flags): allocate zeroed memory, locked into RAM where
 *     the platform supports it (mlock, VirtualLock, libgcrypt secmem, OpenSSL
 *     secure heap, ...). flags are the collector flags; only
 *     JENT_FORCE_SECURE_MEM is consulted, which turns secure memory the
 *     environment does not grant from a (silent) fallback to ordinary memory
 *     into an allocation failure.
 *   - jent_zfree(ptr, len): zero and release memory previously allocated
 *     with jent_zalloc().
 *   - jent_memset_secure(s, n): wipe a buffer in a way the compiler may
 *     not optimize away.
 *
 * The dispatch order is:
 *   - LIBGCRYPT     -> gcry_malloc_secure / gcry_free
 *   - AWSLC         -> OPENSSL_malloc / OPENSSL_free (auto-wipe)
 *   - OPENSSL       -> OPENSSL_secure_malloc / OPENSSL_secure_free
 *   - Windows       -> VirtualAlloc + VirtualLock with PAGE_NOACCESS guard
 *                      pages around the payload
 *   - Linux/BSD/Mac -> mmap + mlock with PROT_NONE guard pages around the
 *                      payload; additionally excluded from core dumps via
 *                      madvise(MADV_DONTDUMP) on Linux and
 *                      madvise(MADV_NOCORE) on FreeBSD, best effort.
 *   - Linux Kernel  -> kvmalloc + kvfree_sensitive; counts as secure since
 *                      kernel memory is never paged out to swap, does not
 *                      appear in user space core dumps and is wiped on free
 *   - other         -> plain malloc
 *
 * Secure memory can be denied at runtime on four of these backends: Windows
 * and POSIX mlock, where the operating system refuses the memory lock, and
 * libgcrypt and OpenSSL, where the secure arena the allocation would come out
 * of is absent or exhausted. All four then fall back to ordinary memory unless
 * the caller set JENT_FORCE_SECURE_MEM, which makes the same situation fail
 * the allocation. Only that one property differs: on the mlock backends the
 * guard-page layout and the dump exclusion are established either way, and on
 * the crypto backends the free path tells the two kinds of pointer apart by
 * itself, which is why jent_zfree() does not take the flags.
 *
 * How much memory may be locked is a property of the environment that the
 * library does not change: RLIMIT_MEMLOCK on POSIX and the process working set
 * quota on Windows, both process-wide state that belongs to the application.
 * An application that needs a large collector locked (JENT_FORCE_SECURE_MEM
 * with JENT_CACHE_ALL, say) raises them itself before the allocation; the
 * recording tools do so in
 * tests/raw-entropy/recording_userspace/jitterentropy-memlock.h.
 *
 * The secure memory arenas of libgcrypt and OpenSSL are the same kind of
 * process-wide state and are likewise left to the application: libgcrypt's
 * secmem pool is created once with GCRYCTL_INIT_SECMEM and OpenSSL's secure
 * heap once with CRYPTO_secure_malloc_init(). A size chosen here would bound
 * every other user of those libraries in the process - and could not be
 * applied at all once the application had configured them itself - so
 * jent_zalloc() only checks that the allocation came out of a configured
 * arena. The recording tools configure the arena in the same header.
 *
 * Whether the active path provides locked / wiped memory is reported at
 * runtime by jent_secure_memory_supported() (defined in
 * arch/jitterentropy-arch-memory.c).
 */

#ifndef _JITTERENTROPY_ARCH_MEMORY_H
#define _JITTERENTROPY_ARCH_MEMORY_H

void jent_memset_secure(void *s, size_t n);
void *jent_zalloc(size_t len, unsigned int flags);
void jent_zfree(void *ptr, size_t len);

/*
 * Whether an allocation made with @flags is known to yield secure memory: the
 * backend capability reported by jent_secure_memory_supported(), narrowed to
 * the callers that set JENT_FORCE_SECURE_MEM on the backends where that
 * capability can be denied at runtime - only they are guaranteed to have got
 * it. This is the question the library asks internally - the exported
 * jent_secure_memory_supported() keeps reporting the backend alone, as it has
 * no collector to consult.
 */
int jent_memory_is_secure(unsigned int flags);

/*
 * jent_secure_memory_supported() - which reports whether the active backend
 * provides locked / wiped memory - is part of the public API and is declared
 * once, in jitterentropy.h. It is deliberately not repeated here: that
 * declaration carries JENT_PRIVATE_STATIC, and an undecorated copy in this
 * header silently dropped the attribute again ("redeclared without dllimport
 * attribute: previous dllimport ignored" on a Windows shared build). Like the
 * size_t above, it reaches every user of this header through jitterentropy.h,
 * which src/jitterentropy-internal.h includes first.
 */

#endif /* _JITTERENTROPY_ARCH_MEMORY_H */
