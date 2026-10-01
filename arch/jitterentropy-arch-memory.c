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
 * The feature-test macros that make glibc declare MAP_ANONYMOUS, MAP_ANON and
 * madvise()/MADV_DONTDUMP - all __USE_MISC, and so hidden by the strict
 * -std=c11 the Makefile uses. Current glibc happens to define MAP_ANONYMOUS
 * unconditionally, which is why this was only noticed on glibc 2.17 (RHEL 7),
 * where neither spelling exists and the MAP_ANON fallback below expands to an
 * undeclared identifier. Must be the first line: they have to precede every
 * system header.
 */
#include "jitterentropy-arch-compat.h"

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

/*
 * Platform detection.
 *
 * The POSIX backend is selected from the option macros the platform itself
 * publishes in <unistd.h>, not from a list of operating systems.
 *
 * _POSIX_MEMLOCK_RANGE is required to be strictly positive rather than merely
 * not -1: a value of 0 means "ask sysconf() at runtime", and a platform that
 * will only say at runtime whether it can lock at all is better served by the
 * malloc() path than by an allocator that might refuse every request.
 *
 * macOS is the one platform named here rather than detected. It publishes
 * _POSIX_MEMLOCK_RANGE as -1 while providing mmap(), mprotect() and a working
 * mlock() - the option macro describes the POSIX option it does not claim
 * conformance to, not the calls. Going by the macro alone put the whole
 * collector state on the plain malloc() path there: no memory lock, no guard
 * pages, no mapping to munmap() on free, and JENT_FORCE_SECURE_MEM silently
 * granted by a backend that provides none of it. The comments below on the
 * dump exclusion this platform lacks were written for the path it was not
 * taking.
 */
#ifdef LINUX_KERNEL
# define JENT_ARCH_MEM_LINUX_KERNEL
#elif defined(_KERNEL) && defined(__FreeBSD__)
# define JENT_ARCH_MEM_FREEBSD_KERNEL
#elif defined(JENT_BAREMETAL)
/*
 * Neither backend: there is no kernel here to ask for a locked page, and no
 * VirtualLock() either. The plain allocator below stands, and it needs neither:
 * there is no swap device, no second process and no core dump - see where
 * JENT_MEM_EXTRAS_REFUSABLE is selected further down.
 */
#elif defined(_MSC_VER) || defined(__MINGW32__)
# define JENT_ARCH_MEM_WINDOWS
#else
# include <unistd.h>
# if defined(_POSIX_MAPPED_FILES) && (_POSIX_MAPPED_FILES - 0) > 0 &&	      \
     defined(_POSIX_MEMORY_PROTECTION) &&				      \
     (_POSIX_MEMORY_PROTECTION - 0) > 0 &&				      \
     ((defined(_POSIX_MEMLOCK_RANGE) && (_POSIX_MEMLOCK_RANGE - 0) > 0) ||     \
      defined(__APPLE__))
#  define JENT_ARCH_MEM_POSIX_MLOCK
# endif
#endif

#ifdef JENT_ARCH_MEM_LINUX_KERNEL

#include <linux/mm.h>		/* kvmalloc(), kvzalloc(), kvfree_sensitive() */
#include <linux/slab.h>		/* GFP_KERNEL */
#include <linux/string.h>	/* memset() */

#elif defined(JENT_ARCH_MEM_FREEBSD_KERNEL)

#include <sys/param.h>
#include <sys/systm.h>		/* explicit_bzero() */
#include <sys/kernel.h>		/* MALLOC_DEFINE() */
#include <sys/malloc.h>		/* malloc(9), zfree(9) */

static MALLOC_DEFINE(M_JITTERENTROPY, "jitterentropy",
		     "Jitter RNG entropy collector state");

#else /* JENT_ARCH_MEM_LINUX_KERNEL / JENT_ARCH_MEM_FREEBSD_KERNEL */

#include <stdlib.h>
#include <string.h>

#ifdef LIBGCRYPT
# include <gcrypt.h>
#endif
#if defined(OPENSSL) || defined(AWSLC)
# include <openssl/crypto.h>
#endif
#ifdef OPENSSL
# include <openssl/err.h>
# include <openssl/evp.h>
#endif
#ifdef JENT_ARCH_MEM_WINDOWS
# include <windows.h>
#endif
/* The crypto libraries allocate themselves: no mapping, lock or madvise(). */
#if defined(JENT_ARCH_MEM_POSIX_MLOCK) && !defined(LIBGCRYPT) &&	      \
    !defined(OPENSSL) && !defined(AWSLC)
# include <sys/mman.h>
# include <errno.h>
# include <unistd.h>	/* sysconf() */
#endif

#endif /* JENT_ARCH_MEM_LINUX_KERNEL / JENT_ARCH_MEM_FREEBSD_KERNEL */

/*
 * Secure memory, as this library uses the term, is memory that is zeroized
 * when it is freed. Every backend provides that: the kernel ones through
 * kvfree_sensitive() and zfree(9), libgcrypt, OpenSSL and AWS-LC through
 * their own free functions, and jent_zfree() wipes before release whatever
 * the allocator does not wipe itself. Every build therefore has secure memory.
 *
 * Locking the memory into RAM and excluding it from core dumps are extras on
 * top of that, which only some backends provide:
 *
 *   - Linux and FreeBSD kernel: by construction - kernel memory is never
 *     paged out to swap and does not appear in a user space core dump.
 *   - baremetal: by construction as well - there is no swap device to page it
 *     out to, no second process to read it and no core dump to land in. This
 *     is a statement about the environment the build is for, which is what
 *     asking for a freestanding build asserts; a firmware that does have
 *     paging underneath it - a hypervisor, an SMM handler with a backing
 *     store - is not one this can speak for, and neither are the kernel
 *     backends.
 *   - libgcrypt and OpenSSL: whatever their secure arena provides - both lock
 *     it where the process may. The arena is created by the application, so
 *     it can be absent altogether and it can run out.
 *   - Windows: VirtualLock(), which the working set quota can refuse.
 *   - POSIX: mlock(), which RLIMIT_MEMLOCK can refuse, and a best effort core
 *     dump exclusion through madvise() where the platform offers one.
 *   - AWS-LC and the plain malloc() fallback: none.
 *
 * JENT_MEM_EXTRAS_REFUSABLE marks the backends whose extras the environment
 * can refuse at runtime: the memory lock, and the arena the application did
 * not provide. Only there does JENT_FORCE_SECURE_MEM have an effect: it turns
 * that refusal from a fallback to memory without the extras into a failed
 * allocation. The backends that provide the extras by construction, and those
 * that offer none, satisfy the flag as they are.
 *
 * The selection mirrors the dispatch priority in jent_zalloc() below: the
 * crypto libraries take precedence over the OS mlock paths.
 */
#if defined(JENT_ARCH_MEM_LINUX_KERNEL) ||				      \
    defined(JENT_ARCH_MEM_FREEBSD_KERNEL)
  /* Extras by construction. */
#elif defined(LIBGCRYPT) || defined(OPENSSL)
# define JENT_MEM_EXTRAS_REFUSABLE
#elif defined(AWSLC) || defined(JENT_BAREMETAL)
  /* No extras to refuse: none at all, or all of them by construction. */
#elif defined(JENT_ARCH_MEM_WINDOWS) || defined(JENT_ARCH_MEM_POSIX_MLOCK)
# define JENT_MEM_EXTRAS_REFUSABLE
#endif

/* Zeroized on free, whatever the backend - see above. */
int jent_secure_memory_supported(void)
{
	return 1;
}

/*
 * The same answer for every allocation: whether the extras were granted does
 * not decide whether the memory is secure, and without JENT_FORCE_SECURE_MEM
 * it is not recorded.
 */
int jent_memory_is_secure(unsigned int flags)
{
	(void)flags;

	return jent_secure_memory_supported();
}

/* The backend jent_zalloc() dispatches to, in its priority order. */
const char *jent_memory_backend_name(void)
{
#if defined(JENT_ARCH_MEM_LINUX_KERNEL)
	return "linux-kernel";
#elif defined(JENT_ARCH_MEM_FREEBSD_KERNEL)
	return "freebsd-kernel";
#elif defined(LIBGCRYPT)
	return "libgcrypt";
#elif defined(AWSLC)
	return "aws-lc";
#elif defined(OPENSSL)
	return "openssl";
#elif defined(JENT_ARCH_MEM_WINDOWS)
	return "windows-virtuallock";
#elif defined(JENT_ARCH_MEM_POSIX_MLOCK)
	return "posix-mlock";
#elif defined(JENT_BAREMETAL)
	return "baremetal";
#else
	return "malloc";
#endif
}

void jent_memset_secure(void *s, size_t n)
{
#if defined(JENT_ARCH_MEM_LINUX_KERNEL)
	memzero_explicit(s, n);
#elif defined(JENT_ARCH_MEM_FREEBSD_KERNEL)
	explicit_bzero(s, n);
#elif defined(AWSLC) || defined(OPENSSL)
	OPENSSL_cleanse(s, n);
#elif defined(JENT_ARCH_MEM_WINDOWS)
	SecureZeroMemory(s, n);
#elif defined(_MSC_VER) && !defined(__clang__)
	/*
	 * MSVC without <windows.h> - a JENT_BAREMETAL build, firmware among
	 * them - and so without SecureZeroMemory(). Neither has it the inline
	 * assembly barrier below, so the stores are made volatile instead,
	 * which is what SecureZeroMemory() itself does.
	 */
	volatile unsigned char *p = (volatile unsigned char *)s;

	while (n--)
		*p++ = 0;
#else
	memset(s, 0, n);
	__asm__ __volatile__("" : : "r" (s) : "memory");
#endif
}

/* The crypto libraries place their memory themselves. */
#if !defined(LIBGCRYPT) && !defined(OPENSSL) && !defined(AWSLC)
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
#endif /* !LIBGCRYPT && !OPENSSL && !AWSLC */

#ifdef JENT_ARCH_MEM_LINUX_KERNEL

void *jent_zalloc(size_t len, unsigned int flags)
{
	/* Kernel memory is not paged out, so there is nothing to relax. */
	(void)flags;

	/*
	 * Charged to the caller's memory cgroup: every open of the character
	 * device allocates a collector of up to 512 MB.
	 */
	return kvzalloc(len, GFP_KERNEL_ACCOUNT);
}

/* Kernel memory is not locked: the same allocation as above. */
void *jent_zalloc_unlocked(size_t len)
{
	return jent_zalloc(len, 0);
}

void jent_zfree(void *ptr, size_t len)
{
	/* See the NULL guard of the userspace variant below. */
	if (!ptr)
		return;

	kvfree_sensitive(ptr, len);
}

#elif defined(JENT_ARCH_MEM_FREEBSD_KERNEL)

void *jent_zalloc(size_t len, unsigned int flags)
{
	/* Kernel memory is not paged out, so there is nothing to relax. */
	(void)flags;

	/*
	 * M_NOWAIT fails rather than sleep: a collector takes up to 512 MB
	 * and the health test recovery doubles it from within a read, which
	 * M_WAITOK would stall indefinitely under memory pressure and confine
	 * to a sleepable context. The callers handle the NULL as ENOMEM, as
	 * with kvzalloc() on Linux.
	 */
	return malloc(len, M_JITTERENTROPY, M_NOWAIT | M_ZERO);
}

/* Kernel memory is not locked: the same allocation as above. */
void *jent_zalloc_unlocked(size_t len)
{
	return jent_zalloc(len, 0);
}

void jent_zfree(void *ptr, size_t len)
{
	/* See the NULL guard of the userspace variant below. */
	if (!ptr)
		return;

	/* zfree(9) wipes the whole allocation, which covers @len. */
	(void)len;
	zfree(ptr, M_JITTERENTROPY);
}

#else /* !JENT_ARCH_MEM_LINUX_KERNEL && !JENT_ARCH_MEM_FREEBSD_KERNEL */

/*
 * @secure asks for the extras - the lock or the secure arena - and only then
 * is @flags consulted. Zeroing on free, guard pages and dump exclusion are the
 * same either way, so jent_zfree() releases both kinds.
 */
static void *jent_zalloc_common(size_t len, unsigned int flags, int secure)
{
	void *tmp = NULL;

#ifndef JENT_MEM_EXTRAS_REFUSABLE
	/* Only a backend whose extras can be refused reads these. */
	(void)flags;
	(void)secure;
#endif

#ifdef LIBGCRYPT

	/*
	 * The secmem pool is not initialized here: it is created once per
	 * process with GCRYCTL_INIT_SECMEM, which fixes its size for every
	 * user of libgcrypt in that process, and libgcrypt has to be
	 * initialized (gcry_check_version(), GCRYCTL_INITIALIZATION_FINISHED)
	 * by the application anyway. Sizing it from in here would overrule a
	 * decision that is not the library's to make - see
	 * arch/jitterentropy-arch-memory.h.
	 *
	 * gcry_malloc_secure(), not gcry_xmalloc_secure(): the x-variant
	 * invokes libgcrypt's fatal out-of-core handler when the secmem pool
	 * is exhausted, terminating the host process from inside the library.
	 * The NULL return is handled by all callers.
	 *
	 * gcry_malloc_secure() wraps a length near SIZE_MAX around into a
	 * small block, so such a length is refused here.
	 */
	if (len > SIZE_MAX / 2)
		return NULL;
	/*
	 * Not before the application has initialized libgcrypt: the first
	 * secure allocation would create the pool at libgcrypt's default size,
	 * and the application's GCRYCTL_INIT_SECMEM would then be ignored.
	 */
	if (secure && JENT_GCRY_INITIALIZED())
		tmp = gcry_malloc_secure(len);

	/*
	 * Check that the memory really came out of the pool. libgcrypt returns
	 * NULL when the pool is absent or exhausted, so this is the belt to
	 * the braces.
	 */
	if (tmp && !gcry_is_secure(tmp)) {
		gcry_free(tmp);
		tmp = NULL;
	}

	/*
	 * No pool, or none left in it: fall back to ordinary memory, which is
	 * what an application that did not configure secmem asks for by not
	 * setting the flag. It is secure memory all the same - jent_zfree()
	 * wipes it before gcry_free() - only without the pool's lock.
	 */
	if (!tmp && !(flags & JENT_FORCE_SECURE_MEM))
		tmp = gcry_malloc(len);

#elif defined(AWSLC)

	/*
	 * OPENSSL_malloc() is secure memory: jent_zfree() wipes it before
	 * OPENSSL_free(). It offers none of the extras - it is not locked,
	 * not guard-paged and not excluded from a core dump - and none is
	 * added around the allocation here: the memory is AWS-LC's, and so is
	 * what it does with it. With no extras to refuse, JENT_FORCE_SECURE_MEM
	 * is satisfied as it is.
	 */
	tmp = OPENSSL_malloc(len);

#elif defined(OPENSSL)

	/*
	 * The secure heap is not initialized here: CRYPTO_secure_malloc_init()
	 * reserves one arena for the whole process whose size cannot be
	 * changed afterwards, so the size belongs to the application, which
	 * knows what else in the process allocates from it - see
	 * arch/jitterentropy-arch-memory.h. Only its presence is checked.
	 */
	if (secure && CRYPTO_secure_malloc_initialized()) {
		/*
		 * An exhausted heap pushes an error onto the thread's queue;
		 * the fallback below is no error, so it must not be left for
		 * the application's next ERR_get_error().
		 */
		ERR_set_mark();
		tmp = OPENSSL_secure_malloc(len);
		ERR_pop_to_mark();
	}
	/*
	 * If its secure heap was not available, OpenSSL falls back to "normal"
	 * memory. Double check.
	 */
	if (tmp && !CRYPTO_secure_allocated(tmp)) {
		OPENSSL_secure_free(tmp);
		tmp = NULL;
	}

	/*
	 * No secure heap, or none left in it: fall back to ordinary memory,
	 * which is what an application that did not configure the secure heap
	 * asks for by not setting the flag. It is secure memory all the same -
	 * jent_zfree() wipes it - only without the heap's extras, and
	 * jent_zfree() releases either kind: OPENSSL_secure_free() forwards a
	 * pointer that is not in the arena to the regular free().
	 */
	if (!tmp && !(flags & JENT_FORCE_SECURE_MEM))
		tmp = OPENSSL_malloc(len);

#elif defined(JENT_ARCH_MEM_WINDOWS)

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

		/*
		 * A refused lock is only an error when the caller demanded
		 * the lock. Either way the mapping and its guard pages are
		 * the same, so jent_zfree() needs no knowledge of the flag.
		 *
		 * VirtualLock() charges its pages against the process
		 * *minimum* working set and fails with ERROR_WORKING_SET_QUOTA
		 * once that budget is exhausted. A collector locks one page of
		 * state (its memory access region is not locked), one more
		 * with the internal timer and two more while its startup runs,
		 * so the 200 kB default holds some 44 collectors.
		 *
		 * Raising it here is deliberately not done: the working set
		 * limits are process-wide state, extending them evicts what
		 * the host application reserved, and they cannot be restored
		 * on free. That is the application's decision, as
		 * RLIMIT_MEMLOCK is on the POSIX path below; the test programs
		 * raise both in tests/jitterentropy-memlock.h.
		 */
		if (secure && !VirtualLock(tmp, payload) &&
		    (flags & JENT_FORCE_SECURE_MEM)) {
			VirtualFree(base, 0, MEM_RELEASE);
			return NULL;
		}
	}

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
		 *
		 * With JENT_FORCE_SECURE_MEM the caller demanded the lock and
		 * any failure fails the allocation. Without it the errors that
		 * say the environment does not permit locking are tolerated,
		 * but not one that indicates a malformed request (EINVAL),
		 * which would be a bug here rather than a property of the
		 * environment:
		 *
		 *  - EPERM:  no privilege, which is what Linux reports for a
		 *            RLIMIT_MEMLOCK of 0 without CAP_IPC_LOCK.
		 *  - ENOMEM: the limit would be exceeded - the case of a
		 *            container with a small but non-zero
		 *            RLIMIT_MEMLOCK on Linux and of the per-process
		 *            and system limits on the BSDs. It cannot mean
		 *            "range not mapped" here, the mapping was just
		 *            established above.
		 *  - EAGAIN: the same condition on macOS.
		 *  - ENOSYS: no mlock() at all, which a seccomp policy may
		 *            report for it.
		 *
		 * The mapping itself is unaffected by the flag, so
		 * jent_zfree() needs no knowledge of it.
		 */
		if (secure && mlock(tmp, len) &&
		    ((flags & JENT_FORCE_SECURE_MEM) ||
		     (errno != EPERM && errno != ENOMEM && errno != EAGAIN &&
		      errno != ENOSYS))) {
			munmap(base, total);
			return NULL;
		}
	}

#else /* no memory lock: baremetal, or no lock on this platform at all */

	/*
	 * Secure memory as everywhere - jent_zfree() wipes it before free().
	 * Nothing here can lock it, so there is no extra to refuse, and
	 * JENT_FORCE_SECURE_MEM is satisfied as it is.
	 */
	tmp = malloc(len);

#endif

	if (tmp != NULL)
		jent_memset_secure(tmp, len);
	return tmp;
}

void *jent_zalloc(size_t len, unsigned int flags)
{
	return jent_zalloc_common(len, flags, 1);
}

void *jent_zalloc_unlocked(size_t len)
{
	return jent_zalloc_common(len, 0, 0);
}

void jent_zfree(void *ptr, size_t len)
{
	/*
	 * A no-op, as free(NULL) is, so callers need not each guard. Here
	 * rather than in the backends because every one of them would do
	 * something worse than nothing with NULL: all wipe before releasing
	 * through jent_memset_secure(), which dereferences unconditionally,
	 * and the guard-page backends compute their base as ptr - page_size.
	 */
	if (!ptr)
		return;

#ifdef LIBGCRYPT

	/*
	 * Wiped here whatever kind of memory it is. gcry_free() wipes what
	 * came from its own secure pool, but neither the ordinary memory
	 * jent_zalloc() falls back to when the pool is unavailable, nor
	 * memory of allocation handlers an application registered with
	 * gcry_set_allocation_handler(), whose is_secure_func may well answer
	 * yes for it. gcry_free() releases either kind.
	 */
	jent_memset_secure(ptr, len);
	gcry_free(ptr);

#elif defined(AWSLC)

	/*
	 * Wiped here: OPENSSL_free() cleanses its own allocations, but hands
	 * the memory unwiped to the functions an application installed with
	 * CRYPTO_set_mem_functions() or the OPENSSL_memory_free() hook.
	 */
	jent_memset_secure(ptr, len);
	OPENSSL_free(ptr);

#elif defined(OPENSSL)

	/*
	 * OPENSSL_secure_free() clears what it takes back into the secure
	 * heap, so only the ordinary memory jent_zalloc() falls back to when
	 * the heap is unavailable is wiped here - the same pointer that
	 * OPENSSL_secure_free() forwards to the regular free().
	 */
	if (!CRYPTO_secure_allocated(ptr))
		jent_memset_secure(ptr, len);
	OPENSSL_secure_free(ptr);

#elif defined(JENT_ARCH_MEM_WINDOWS)

	SecureZeroMemory(ptr, len);
	{
		/*
		 * Mirror the guard-page layout of jent_zalloc(): the region
		 * starts one page before the returned pointer.
		 *
		 * VirtualUnlock() on a region that was never locked (the
		 * allocation was made without JENT_FORCE_SECURE_MEM and the
		 * lock failed) reports ERROR_NOT_LOCKED and does nothing else,
		 * which is why the free path does not need the flag.
		 */
		size_t page_size = jent_pagesize();
		size_t payload = (len + page_size - 1) & ~(page_size - 1);
		uint8_t *base = (uint8_t *)ptr - page_size;

		VirtualUnlock(ptr, payload);
		VirtualFree(base, 0, MEM_RELEASE);
	}

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

#endif /* JENT_ARCH_MEM_LINUX_KERNEL / JENT_ARCH_MEM_FREEBSD_KERNEL */
