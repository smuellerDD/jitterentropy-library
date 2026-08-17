/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * The per-instance UUID.
 *
 * Definition of jent_uuid_generate() (declared in src/jitterentropy-uuid.h).
 * The 16 underlying bytes come from the platform's CSPRNG through
 * jent_os_random_bytes(); the RFC 4122 version and variant bits and the
 * formatting are decided here.
 *
 * Nothing in this file is architecture-specific, which is why it does not
 * live in arch/ beside the CSPRNG it calls: it is byte layout and hex
 * digits, the same on every target.
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

#ifdef LINUX_KERNEL
#include <linux/string.h>	/* memset() */
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#endif

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

	if (jent_os_random_bytes(b, sizeof(b))) {
		memset(b, 0, sizeof(b));
	} else {
		/* Force the version (4) and variant (10xx) bits. */
		b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
		b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
	}

	jent_uuid_format(b, out);
}
