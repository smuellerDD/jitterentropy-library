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
 * The per-instance UUID: jent_uuid_generate() formats an RFC 4122 version 4
 * UUID string over 16 bytes asked of jent_os_random_bytes(), which facility
 * answers being arch/'s business.
 *
 * What is decided here - version and variant bits, the 8-4-4-4-12 layout, what
 * to emit when there are no bytes to be had - is the same on every target,
 * which is why it does not live in arch/ beside the CSPRNG it calls.
 */
#ifndef _JITTERENTROPY_UUID_H
#define _JITTERENTROPY_UUID_H

/* Length of the canonical UUID string "8-4-4-4-12" including the NUL. */
#ifndef JENT_UUID_STRLEN
# define JENT_UUID_STRLEN 37
#endif

/*
 * Generate an RFC 4122 version 4 UUID string into @out, which must hold at
 * least JENT_UUID_STRLEN bytes. Where the platform offers no CSPRNG the nil
 * UUID (all zeroes) is produced. Defined in src/jitterentropy-uuid.c.
 */
void jent_uuid_generate(char *out);

#endif /* _JITTERENTROPY_UUID_H */
