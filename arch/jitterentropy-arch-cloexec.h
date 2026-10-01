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
 * JENT_O_CLOEXEC, the open() flag that keeps a descriptor from leaking into a
 * child that a concurrent fork() and exec() starts. O_CLOEXEC is POSIX.1-2008
 * and absent from older systems, where the window stays open rather than the
 * build failing.
 *
 * It is O_CLOEXEC out of <fcntl.h> that is tested, so a backend includes this
 * header right after its <fcntl.h>; like every arch header it includes nothing
 * itself. Included too early, it settles on 0 without a word, which is why
 * it is kept apart from jitterentropy-arch-compat.h, the one that has to come
 * before every system header.
 */

#ifndef _JITTERENTROPY_ARCH_CLOEXEC_H
#define _JITTERENTROPY_ARCH_CLOEXEC_H

#ifndef JENT_O_CLOEXEC
# ifdef O_CLOEXEC
#  define JENT_O_CLOEXEC O_CLOEXEC
# else
#  define JENT_O_CLOEXEC 0
# endif
#endif

#endif /* _JITTERENTROPY_ARCH_CLOEXEC_H */
