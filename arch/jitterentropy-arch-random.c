/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific access to the operating system's CSPRNG.
 *
 * Definition of jent_os_random_bytes() (declared in
 * arch/jitterentropy-arch-random.h), which is the one place in the library
 * that asks the platform for random bytes rather than measuring for them.
 *
 * See that header for the dispatch, and for what these bytes are and are not
 * to be used for.
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
 * <bcrypt.h> and BCryptGenRandom() are declared by the Windows SDK only from
 * Windows Vista onwards. mingw-w64 has defaulted to older values across its
 * releases, so the minimum is stated here rather than left to the toolchain; it
 * must precede every system header, including the <windows.h> included below.
 * An externally supplied, higher value is left alone.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

#include <linux/random.h>	/* get_random_bytes() */
#include <linux/string.h>	/* memset() */
#include <linux/types.h>
# define JENT_RANDOM_LINUX_KERNEL

#else /* LINUX_KERNEL */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * No CSPRNG to ask on a baremetal target: none of the branches is selected and
 * jent_os_random_supported() reports so. That is not a shortfall in the noise
 * source - the OS random pool is used for the instance identifier and for the
 * startup work-scale plan, both of which fall back to what the collector
 * itself has measured.
 */
#if defined(JENT_BAREMETAL)
#elif defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# include <bcrypt.h>
# if defined(_MSC_VER)
   /* MSVC auto-links; MinGW builds must add -lbcrypt to the link. */
#  pragma comment(lib, "bcrypt.lib")
# endif
# define JENT_RANDOM_WINDOWS
#elif defined(__APPLE__)  || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
# include <stdlib.h>
# define JENT_RANDOM_ARC4RANDOM
#elif defined(__linux__)
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
/*
 * <sys/random.h> and the getrandom() wrapper are glibc 2.25 and newer; RHEL 7
 * ships 2.17 and has neither, which is a missing-header build failure rather
 * than a link error. Its kernel would not answer the syscall either - that is
 * Linux 3.17 - so nothing is lost by taking the /dev/urandom path there, which
 * is where the getrandom() branch below falls back to anyway.
 *
 * Bionic is checked the same way and for the same reason, only against an API
 * level rather than a libc version. Its <sys/random.h> is always present - the
 * NDK ships one set of headers for every level - but the declaration inside it
 * carries __INTRODUCED_IN(28), so below that the header is a no-op and the call
 * is an implicit declaration, which current NDK clang rejects outright. The
 * bound is the wrapper's alone and not the kernel's - API 21 is already Linux
 * 3.4 and up, where the syscall may or may not be there - which is exactly the
 * case the runtime fallback below covers. This is what lets the NDK build
 * target the whole range its toolchain supports (API 21 and up, see flake.nix)
 * rather than only the levels that happen to declare getrandom().
 *
 * musl (>= 1.1.20) publishes the header with no version macro to test and is
 * current enough to have it wherever this library is built.
 */
# if defined(__ANDROID__) && __ANDROID_API__ < 28
#  define JENT_RANDOM_DEVURANDOM
# elif defined(__GLIBC__) && \
       (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 25))
#  define JENT_RANDOM_DEVURANDOM
# else
#  include <sys/random.h>
#  define JENT_RANDOM_GETRANDOM
# endif
#elif defined(__unix__) || defined(__sun) || defined(_AIX) || \
      defined(__HAIKU__) || defined(__CYGWIN__)
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
# define JENT_RANDOM_DEVURANDOM
#endif

#endif /* LINUX_KERNEL */

/*
 * Whether this build selected a backend at all. Not a promise that a call
 * succeeds - a /dev/urandom that is absent from a chroot still fails - only
 * that there is something to ask.
 */
#if defined(JENT_RANDOM_LINUX_KERNEL) || defined(JENT_RANDOM_WINDOWS) || \
    defined(JENT_RANDOM_ARC4RANDOM) || defined(JENT_RANDOM_GETRANDOM) || \
    defined(JENT_RANDOM_DEVURANDOM)
# define JENT_RANDOM_AVAILABLE
#endif

#if defined(JENT_RANDOM_GETRANDOM) || defined(JENT_RANDOM_DEVURANDOM)
/*
 * Blocking read of @len bytes from @path. Returns 0 on success. The path is a
 * parameter so the outcomes this has to survive - the device missing, a short
 * read, an end of file before @len bytes - can be produced from a file the
 * caller made; /dev/urandom presents none of them.
 */
static int jent_random_read_file(const char *path, uint8_t *buf, size_t len)
{
	size_t i = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return -1;

	while (i < len) {
		ssize_t r = read(fd, buf + i, len - i);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0)
			break;
		i += (size_t)r;
	}

	close(fd);
	return (i == len) ? 0 : -1;
}

static int jent_random_dev_urandom(uint8_t *buf, size_t len)
{
	return jent_random_read_file("/dev/urandom", buf, len);
}
#endif

/* Does this build have a CSPRNG backend? */
int jent_os_random_supported(void)
{
#ifdef JENT_RANDOM_AVAILABLE
	return 1;
#else
	return 0;
#endif
}

/* Fill @buf with @len CSPRNG bytes. Returns 0 on success, -1 if unavailable. */
int jent_os_random_bytes(uint8_t *buf, size_t len)
{
#if defined(JENT_RANDOM_LINUX_KERNEL)
	get_random_bytes(buf, len);
	return 0;
#elif defined(JENT_RANDOM_WINDOWS)
	if (BCryptGenRandom(NULL, buf, (ULONG)len,
			    BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return -1;
	return 0;
#elif defined(JENT_RANDOM_ARC4RANDOM)
	arc4random_buf(buf, len);
	return 0;
#elif defined(JENT_RANDOM_GETRANDOM)
	size_t i = 0;

	while (i < len) {
		ssize_t r = getrandom(buf + i, len - i, 0);

		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;	/* fall back to /dev/urandom */
		}
		i += (size_t)r;
	}
	if (i == len)
		return 0;
	return jent_random_dev_urandom(buf, len);
#elif defined(JENT_RANDOM_DEVURANDOM)
	return jent_random_dev_urandom(buf, len);
#else
	(void)buf;
	(void)len;
	return -1;
#endif
}

/* Write the canonical hex representation of @b (16 bytes) into @out. */
