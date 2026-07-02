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
 * Architecture / OS-specific UUID generation.
 *
 * Provides jent_uuid_generate() which formats an RFC 4122 version 4 (random)
 * UUID string. The 16 underlying bytes are obtained from the platform's native
 * cryptographically secure RNG:
 *
 *   - Linux kernel              -> get_random_bytes()
 *   - Windows (MSVC / MinGW)    -> BCryptGenRandom()
 *   - Apple / *BSD              -> arc4random_buf()
 *   - Linux userspace           -> getrandom(), /dev/urandom fallback
 *   - other Unix-like           -> /dev/urandom
 *   - anything else (baremetal) -> no CSPRNG, emits the nil UUID
 *
 * The RFC 4122 version and variant bits are forced here for every backend, so
 * the result is a well-formed v4 UUID regardless of the byte source.
 */

#ifndef _JITTERENTROPY_ARCH_UUID_H
#define _JITTERENTROPY_ARCH_UUID_H

/* Length of the canonical UUID string "8-4-4-4-12" including the NUL. */
#ifndef JENT_UUID_STRLEN
# define JENT_UUID_STRLEN 37
#endif

#ifdef LINUX_KERNEL

#include <linux/random.h>	/* get_random_bytes() */
#include <linux/string.h>	/* memset() */
#include <linux/types.h>
# define JENT_UUID_LINUX_KERNEL

#else /* LINUX_KERNEL */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# include <bcrypt.h>
# if defined(_MSC_VER)
   /* MSVC auto-links; MinGW builds must add -lbcrypt to the link. */
#  pragma comment(lib, "bcrypt.lib")
# endif
# define JENT_UUID_WINDOWS
#elif defined(__APPLE__)  || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__) || defined(__DragonFly__)
# include <stdlib.h>
# define JENT_UUID_ARC4RANDOM
#elif defined(__linux__)
# include <sys/random.h>
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
# define JENT_UUID_GETRANDOM
#elif defined(__unix__) || defined(__sun) || defined(_AIX) || \
      defined(__HAIKU__) || defined(__CYGWIN__)
# include <errno.h>
# include <fcntl.h>
# include <unistd.h>
# define JENT_UUID_DEVURANDOM
#endif

#endif /* LINUX_KERNEL */

#if defined(JENT_UUID_GETRANDOM) || defined(JENT_UUID_DEVURANDOM)
/* Blocking read of @len bytes from /dev/urandom. Returns 0 on success. */
static inline int jent_uuid_dev_urandom(uint8_t *buf, size_t len)
{
	size_t i = 0;
	int fd = open("/dev/urandom", O_RDONLY);

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
#endif

/* Fill @buf with @len CSPRNG bytes. Returns 0 on success, -1 if unavailable. */
static inline int jent_uuid_random_bytes(uint8_t *buf, size_t len)
{
#if defined(JENT_UUID_LINUX_KERNEL)
	get_random_bytes(buf, len);
	return 0;
#elif defined(JENT_UUID_WINDOWS)
	if (BCryptGenRandom(NULL, buf, (ULONG)len,
			    BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
		return -1;
	return 0;
#elif defined(JENT_UUID_ARC4RANDOM)
	arc4random_buf(buf, len);
	return 0;
#elif defined(JENT_UUID_GETRANDOM)
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
	return jent_uuid_dev_urandom(buf, len);
#elif defined(JENT_UUID_DEVURANDOM)
	return jent_uuid_dev_urandom(buf, len);
#else
	(void)buf;
	(void)len;
	return -1;
#endif
}

/* Write the canonical hex representation of @b (16 bytes) into @out. */
static inline void jent_uuid_format(const uint8_t b[16], char *out)
{
	static const char hex[] = "0123456789abcdef";
	int i, j = 0;

	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[j++] = '-';
		out[j++] = hex[b[i] >> 4];
		out[j++] = hex[b[i] & 0x0f];
	}
	out[j] = '\0';
}

/*
 * Generate an RFC 4122 version 4 UUID string into @out, which must hold at
 * least JENT_UUID_STRLEN bytes. If no platform CSPRNG is available the nil UUID
 * (all zeroes) is produced.
 */
static inline void jent_uuid_generate(char *out)
{
	uint8_t b[16];

	if (jent_uuid_random_bytes(b, sizeof(b))) {
		memset(b, 0, sizeof(b));
	} else {
		/* Force the version (4) and variant (10xx) bits. */
		b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
		b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
	}

	jent_uuid_format(b, out);
}

#endif /* _JITTERENTROPY_ARCH_UUID_H */
