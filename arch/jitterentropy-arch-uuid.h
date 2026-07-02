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

/*
 * Generate an RFC 4122 version 4 UUID string into @out, which must hold at
 * least JENT_UUID_STRLEN bytes. If no platform CSPRNG is available the nil UUID
 * (all zeroes) is produced. Defined in arch/jitterentropy-arch-uuid.c.
 */
void jent_uuid_generate(char *out);

#endif /* _JITTERENTROPY_ARCH_UUID_H */
