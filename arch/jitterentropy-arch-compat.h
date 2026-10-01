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
 * Compilation-environment shims shared by the arch backends.
 *
 * What a system header declares is decided before it is read - the glibc
 * feature-test macros select the POSIX and BSD interfaces, _WIN32_WINNT the
 * Windows SDK version - so all of it has to be in place ahead of the first
 * system header a translation unit pulls in. That is why this header is the
 * first line of every arch backend source file that needs any of it, ahead of
 * jitterentropy.h, and why it contains no #include of its own: macro state is
 * all it is, so a translation unit remains free to define further macros of
 * its own (the _GNU_SOURCE of the two affinity backends) on either side of it.
 *
 * This is deliberately not part of the public jitterentropy.h. A consumer of
 * the library must not have a feature-test macro imposed on it by including
 * our header, which would change what its own system headers declare. Setting
 * the macros here, before jitterentropy.h rather than inside it, keeps that
 * property: they apply to this translation unit alone, and jitterentropy.h
 * still states none.
 *
 * An externally supplied value is left alone throughout - a build that already
 * names a POSIX level or a Windows version keeps the one it names.
 *
 * JENT_O_CLOEXEC, which needs <fcntl.h> read first, is in
 * jitterentropy-arch-cloexec.h.
 */

#ifndef _JITTERENTROPY_ARCH_COMPAT_H
#define _JITTERENTROPY_ARCH_COMPAT_H

/*
 * The interfaces the backends reach for are all guarded on glibc by a
 * feature-test macro that a strict -std=c11 - which the Makefile uses - turns
 * off: MAP_ANONYMOUS, MAP_ANON and madvise()/MADV_DONTDUMP are __USE_MISC,
 * clock_gettime() and the CLOCK_* identifiers are __USE_POSIX199309, and
 * O_CLOEXEC is __USE_XOPEN2K8. Without this block the first two are a
 * missing-identifier build failure and the third silently degrades to the
 * JENT_O_CLOEXEC fallback of 0.
 *
 * _DEFAULT_SOURCE is the modern spelling and restores them from glibc 2.19 on.
 * glibc 2.17 (RHEL 7) does not know that macro, and under __STRICT_ANSI__ it
 * picks no POSIX level by itself either, so _POSIX_C_SOURCE is stated as well.
 * An explicit POSIX level in turn suppresses the _BSD_SOURCE that 2.17 adds on
 * its own to a non-strict build, so that is defined too and __USE_MISC stays
 * as it was; _BSD_SOURCE is also the only spelling 2.17 understands, while
 * everything from 2.20 on warns about it unless _DEFAULT_SOURCE is present
 * too, which is why both are given.
 *
 * Nothing gets narrower elsewhere: current glibc forces _POSIX_C_SOURCE to
 * 200809L under _DEFAULT_SOURCE anyway, musl reads _DEFAULT_SOURCE as
 * _BSD_SOURCE and only ever widens with further macros, and bionic has no
 * POSIX level gating. The interfaces that are not guarded at all - glibc's
 * _SC_LEVEL*_CACHE_SIZE and the other sysconf() names, getrandom() and
 * GRND_NONBLOCK in <sys/random.h> - are unaffected either way.
 */
#if defined(__linux__)
# ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
# endif
# ifndef _BSD_SOURCE
#  define _BSD_SOURCE
# endif
# if !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
#  define _POSIX_C_SOURCE 200809L
# endif
#endif

/*
 * newlib (Cygwin) and Solaris hide POSIX under a strict -std=c11 as well -
 * O_CLOEXEC among it, which would leave JENT_O_CLOEXEC at 0. _DEFAULT_SOURCE
 * and __EXTENSIONS__ are their spellings for "everything".
 */
#if defined(__CYGWIN__) && !defined(_DEFAULT_SOURCE)
# define _DEFAULT_SOURCE
#endif
#if defined(__sun) && !defined(__EXTENSIONS__)
# define __EXTENSIONS__
#endif

/*
 * The Windows interfaces the backends use - GetLogicalProcessorInformationEx()
 * with RelationCache and RelationGroup, GetActiveProcessorCount(),
 * ALL_PROCESSOR_GROUPS, GetThreadGroupAffinity()/SetThreadGroupAffinity() and
 * the GROUP_AFFINITY / GROUP_RELATIONSHIP structs and BCryptGenRandom() - are
 * declared by the Windows SDK only when the translation unit asks for Windows 7
 * or newer. mingw-w64 in particular has defaulted to older values across its
 * releases, so the minimum is stated here rather than left to the toolchain; it
 * has to precede every system header, <windows.h> included.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

/*
 * How far the application has initialized libgcrypt. A copy compiled into
 * libgcrypt itself, which runs after the initialization, defines it as 1
 * (and cannot call the public gcry_control() from inside libgcrypt).
 *
 * JENT_GCRY_INITIALIZED(): the application declared its initialization
 * finished (GCRYCTL_INITIALIZATION_FINISHED), its secure memory pool
 * included. Before that, a secure allocation would create the process-wide
 * pool at libgcrypt's default size, which the application's own
 * GCRYCTL_INIT_SECMEM then could not change.
 */
#if defined(LIBGCRYPT) && !defined(JENT_GCRY_INITIALIZED)
# define JENT_GCRY_INITIALIZED() \
	(gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P) != 0)
#endif

#endif /* _JITTERENTROPY_ARCH_COMPAT_H */
