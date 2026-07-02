/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific FIPS mode detection.
 *
 * Definition of jent_fips_enabled() (declared in
 * arch/jitterentropy-arch-fips.h). The crypto-library branches include the
 * relevant headers (gcrypt.h / openssl) themselves; only .c files include
 * these headers (the arch headers intentionally include nothing).
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

#include <linux/fips.h>

int jent_fips_enabled(void)
{
	return fips_enabled;
}

#else /* LINUX_KERNEL */

#ifdef LIBGCRYPT
# include <gcrypt.h>
#endif
#ifdef AWSLC
# include <openssl/crypto.h>
#endif
#ifdef OPENSSL
# include <openssl/evp.h>
#endif

#if !defined(LIBGCRYPT) && !defined(AWSLC) && !defined(OPENSSL) && \
    !defined(_MSC_VER) && !defined(__MINGW32__)
# include <errno.h>
# include <fcntl.h>
# include <sys/types.h>
# include <unistd.h>
#endif

int jent_fips_enabled(void)
{
#ifdef LIBGCRYPT
	return gcry_fips_mode_active();
#elif defined(AWSLC)
	return FIPS_mode();
#elif defined(OPENSSL)
	return EVP_default_properties_is_fips_enabled(NULL);
#elif defined(_MSC_VER) || defined(__MINGW32__)
	return 0;
#else
#define FIPS_MODE_SWITCH_FILE "/proc/sys/crypto/fips_enabled"
	char buf[2] = "0";
	int fd = 0;
	ssize_t rlen;

	if ((fd = open(FIPS_MODE_SWITCH_FILE, O_RDONLY)) >= 0) {
		do {
			rlen = read(fd, buf, sizeof(buf));
		} while (rlen < 0 && errno == EINTR);
		close(fd);
		if (rlen <= 0)
			return 0;
	}
	if (buf[0] == '1')
		return 1;
	else
		return 0;
#undef FIPS_MODE_SWITCH_FILE
#endif
}

#endif /* LINUX_KERNEL */
