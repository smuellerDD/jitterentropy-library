/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific UUID generation.
 *
 * Definition of jent_uuid_generate() (declared in
 * arch/jitterentropy-arch-uuid.h). The 16 underlying bytes come from the
 * platform's native CSPRNG; the RFC 4122 version/variant bits are forced here.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

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
static int jent_uuid_dev_urandom(uint8_t *buf, size_t len)
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
static int jent_uuid_random_bytes(uint8_t *buf, size_t len)
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
static void jent_uuid_format(const uint8_t b[16], char *out)
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

void jent_uuid_generate(char *out)
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
