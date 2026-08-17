/*
 * Non-physical true random number generator based on timing jitter.
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

/*
 * Architecture / OS-specific access to the operating system's CSPRNG.
 *
 * Provides jent_os_random_bytes(). The backends are defined in
 * arch/jitterentropy-arch-random.c; the dispatch order is:
 *
 *   - Linux kernel              -> get_random_bytes()
 *   - Windows (MSVC / MinGW)    -> BCryptGenRandom()
 *   - Apple / *BSD              -> arc4random_buf()
 *   - Linux userspace           -> getrandom(), /dev/urandom fallback
 *   - other Unix-like           -> /dev/urandom
 *   - anything else (baremetal) -> none, and the call fails
 *
 * Keeping the headers those need - <windows.h>, <bcrypt.h>, <sys/random.h>,
 * the file reads - away from the callers is what this file is for, as
 * elsewhere in arch/.
 *
 * None of it is an entropy source for the Jitter RNG and must never become
 * one: the library exists to produce randomness on machines whose platform has
 * none worth having, the last line of that table being a target it still has
 * to work on. The bytes are for callers that want a value nobody outside the
 * process can anticipate and can cope with the platform offering nothing.
 */
#ifndef _JITTERENTROPY_ARCH_RANDOM_H
#define _JITTERENTROPY_ARCH_RANDOM_H

#ifdef LINUX_KERNEL
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/*
 * Fill @buf with @len bytes from whichever of the above this target has.
 * Returns 0 on success and nonzero where there is none to ask or the ask
 * failed, in which case @buf holds nothing worth using.
 */
int jent_os_random_bytes(uint8_t *buf, size_t len);

#endif /* _JITTERENTROPY_ARCH_RANDOM_H */
