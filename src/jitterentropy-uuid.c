/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * The per-instance UUID.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 * Copyright Markus Theil <theil.markus@gmail.com>, 2026
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
#include "jitterentropy.h"
#include "jitterentropy-internal.h"
#include "jitterentropy-sha3.h"

#ifdef LINUX_KERNEL
#include <linux/string.h>	/* memcpy() */
#include <linux/types.h>
#elif defined(_KERNEL) && defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/systm.h>		/* memcpy() */
#include <sys/stdint.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

static void jent_uuid_format(const uint8_t b[16], char *out)
{
	static const char hex[] = "0123456789abcdef";
	int i, j = 0;

	JENT_BUILD_BUG_ON(JENT_UUID_STRLEN != 16 * 2 + 4 + 1);

	for (i = 0; i < 16; i++) {
		if (i == 4 || i == 6 || i == 8 || i == 10)
			out[j++] = '-';
		out[j++] = hex[b[i] >> 4];
		out[j++] = hex[b[i] & 0x0f];
	}
	out[j] = '\0';
}

/* Process-wide, so that no two instances derive the same identifier. */
static uint32_t jent_uuid_counter = 0;

/* Without a CSPRNG: SHA3-256 of the counter and the time as a version 8 UUID. */
static void jent_uuid_from_counter(uint8_t b[16])
{
	HASH_CTX_ON_STACK(ctx);
	uint8_t digest[JENT_SHA3_256_SIZE_DIGEST], in[12];
	uint32_t val = jent_atomic_inc_u32(&jent_uuid_counter);
	uint64_t now = 0;
	unsigned int i;

	jent_get_nstime(&now);

	for (i = 0; i < 4; i++)
		in[i] = (uint8_t)(val >> (8 * i));
	for (i = 0; i < 8; i++)
		in[4 + i] = (uint8_t)(now >> (8 * i));

	jent_sha3_256_init(&ctx);
	jent_sha3_update(&ctx, in, sizeof(in));
	jent_sha3_final(&ctx, digest);
	memcpy(b, digest, 16);

	b[6] = (uint8_t)((b[6] & 0x0f) | 0x80);

	jent_memset_secure(&ctx, JENT_SHA_MAX_CTX_SIZE);
	jent_memset_secure(digest, sizeof(digest));
}

void jent_uuid_generate(char *out)
{
	uint8_t b[16];

	if (jent_os_random_bytes(b, sizeof(b)))
		jent_uuid_from_counter(b);
	else
		b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);

	/* Force the variant (10xx) bits. */
	b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);

	jent_uuid_format(b, out);
}
