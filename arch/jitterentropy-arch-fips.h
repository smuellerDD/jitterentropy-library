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
 * Crypto-library / OS-specific FIPS-mode detection.
 *
 * Provides jent_fips_enabled() returning non-zero when the runtime is in
 * FIPS mode and zero otherwise. Dispatch:
 *   - LIBGCRYPT  -> gcry_fips_mode_active()
 *   - AWSLC      -> FIPS_mode()
 *   - OPENSSL    -> EVP_default_properties_is_fips_enabled(NULL)
 *   - Windows    -> 0 (no kernel switch to query)
 *   - POSIX      -> read /proc/sys/crypto/fips_enabled (Linux); on systems
 *                   that lack the file the open() fails and we report 0.
 *   - Linux Kernel -> fips_enabled flag
 *
 * The crypto-library branches expect the consumer to have included the
 * relevant headers before this one (gcrypt.h / openssl/evp.h).
 */

#ifndef _JITTERENTROPY_ARCH_FIPS_H
#define _JITTERENTROPY_ARCH_FIPS_H

/*
 * Return non-zero when FIPS mode is active for the active backend.
 * Defined in arch/jitterentropy-arch-fips.c.
 */
int jent_fips_enabled(void);

#endif /* _JITTERENTROPY_ARCH_FIPS_H */
