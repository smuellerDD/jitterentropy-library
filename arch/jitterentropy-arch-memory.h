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
 * Secure memory is memory that is zeroized when it is freed, and every backend
 * below provides it. Locking it into RAM and excluding it from core dumps are
 * extras that only some backends provide - see
 * arch/jitterentropy-arch-memory.c for which.
 *
 * Provides (defined in arch/jitterentropy-arch-memory.c):
 *   - jent_zalloc(len, flags): allocate zeroed secure memory, with the extras
 *     where the backend offers them (mlock, VirtualLock, libgcrypt secmem,
 *     OpenSSL secure heap, ...). Of the collector flags only
 *     JENT_FORCE_SECURE_MEM is consulted: it turns extras the environment
 *     refuses from a silent fallback to memory without them into an
 *     allocation failure.
 *   - jent_zalloc_unlocked(len): what jent_zalloc() falls back to - zeroed,
 *     wiped on free, guard-paged and excluded from core dumps where the
 *     backend does so, but neither locked nor from a secure arena. For the
 *     memory access region, which holds no collector state.
 *   - jent_zfree(ptr, len): zero and release either kind of allocation.
 *   - jent_memset_secure(s, n): wipe a buffer in a way the compiler may
 *     not optimize away.
 *   - jent_secure_memory_supported(): whether the build provides secure
 *     memory - always, as every backend wipes on free.
 *
 * The dispatch order is:
 *   - LIBGCRYPT     -> gcry_malloc_secure / gcry_free, wiped before free
 *   - AWSLC         -> OPENSSL_malloc / OPENSSL_free, wiped before free, as
 *                      OPENSSL_free() does not wipe for an application's
 *                      own allocator; no extras, so JENT_FORCE_SECURE_MEM is
 *                      not refused
 *   - OPENSSL       -> OPENSSL_secure_malloc / OPENSSL_secure_free
 *   - Windows       -> VirtualAlloc + VirtualLock with PAGE_NOACCESS guard
 *                      pages around the payload
 *   - Linux/BSD/Mac -> mmap + mlock with PROT_NONE guard pages around the
 *                      payload; also excluded from core dumps via
 *                      madvise(MADV_DONTDUMP) on Linux and MADV_NOCORE on
 *                      FreeBSD, best effort
 *   - Linux Kernel  -> kvmalloc + kvfree_sensitive, which wipes on free; the
 *                      extras by construction, as kernel memory is never
 *                      paged out to swap and does not appear in user space
 *                      core dumps
 *   - FreeBSD Kernel -> malloc(9) with M_ZERO + zfree(9); the same
 *   - other         -> plain malloc, wiped before free; no extras
 *
 * The crypto library backends use the library's allocator as it is: no memory
 * lock and no core dump exclusion is added around it here.
 *
 * Four backends can be refused their extras at runtime: the two mlock ones
 * when the operating system refuses the lock, libgcrypt and OpenSSL when
 * their secure arena is absent or exhausted. All four then fall back to
 * memory without them - still wiped on free - unless JENT_FORCE_SECURE_MEM
 * makes the same situation fail the allocation. Only that differs - the guard pages and the dump exclusion
 * are established either way, and the free path tells the two kinds of pointer
 * apart itself, which is why jent_zfree() does not take the flags.
 *
 * How much memory may be locked (RLIMIT_MEMLOCK, the Windows working set
 * quota) and how large the libgcrypt and OpenSSL arenas are is process-wide
 * state belonging to the application, which the library does not change: a
 * size chosen here would bound every other user of those libraries in the
 * process. A collector locks one page of state whatever its memory size, one
 * more with the internal timer and two more while its startup runs, so the
 * defaults cover several collectors. An application that needs more raises
 * the limits and configures the arena itself, as the test programs do in
 * tests/jitterentropy-memlock.h; jent_zalloc() only checks that the
 * allocation came out of a configured arena.
 */

#ifndef _JITTERENTROPY_ARCH_MEMORY_H
#define _JITTERENTROPY_ARCH_MEMORY_H

JENT_INTERNAL
void jent_memset_secure(void *s, size_t n);
JENT_INTERNAL
void *jent_zalloc(size_t len, unsigned int flags);
JENT_INTERNAL
void *jent_zalloc_unlocked(size_t len);

/*
 * Releases what jent_zalloc() returned, wiping @len bytes first. @len must be
 * the length the allocation was made with - the guard-page backends derive the
 * extent of their mapping from it.
 *
 * A NULL @ptr is a no-op, as it is for free().
 */
JENT_INTERNAL
void jent_zfree(void *ptr, size_t len);

/*
 * Whether an allocation made with @flags yields secure memory - memory wiped on
 * free, which every allocation is, whatever @flags and whether the extras were
 * granted. This is the question the library asks internally for a collector -
 * the exported jent_secure_memory_supported() answers it for the build, as it
 * has no collector to consult.
 */
JENT_INTERNAL
int jent_memory_is_secure(unsigned int flags);

/*
 * The name of the secure memory backend this build allocates the collector
 * state from - "posix-mlock", "windows-virtuallock", "libgcrypt", "openssl",
 * "aws-lc", "linux-kernel", "freebsd-kernel", "baremetal" or "malloc" - as the
 * status document reports it.
 */
JENT_INTERNAL
const char *jent_memory_backend_name(void);

/*
 * jent_secure_memory_supported() - which reports whether the active backend
 * provides memory wiped on free - is part of the public API and is declared
 * once, in jitterentropy.h. It is deliberately not repeated here: that
 * declaration carries JENT_PRIVATE_STATIC, and an undecorated copy in this
 * header silently dropped the attribute again ("redeclared without dllimport
 * attribute: previous dllimport ignored" on a Windows shared build). Like the
 * size_t above, it reaches every user of this header through jitterentropy.h,
 * which src/jitterentropy-internal.h includes first.
 */

#endif /* _JITTERENTROPY_ARCH_MEMORY_H */
