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
 * Architecture / OS-specific thread handling for the internal timer.
 *
 * The internal ("notime") timer spawns a helper thread that does nothing
 * but increment a counter. This header declares the two pieces of code
 * that differ between platforms; the definitions live in
 * arch/jitterentropy-arch-thread.c.
 *
 *   1. Thread creation / joining. jent_notime_thread_create() /
 *      jent_notime_thread_join() hide the back-end behind a single
 *      signature and a single context struct (struct jent_notime_ctx,
 *      defined in jitterentropy.h).
 *
 *   2. Pinning the calling thread to a single logical CPU via
 *      jent_thread_pin_to_cpu(). Keeping the counting thread on one CPU
 *      avoids inter-core migration of the running counter.
 *
 * Both come in a hosted userspace flavour (JENT_ARCH_THREAD_HOSTED, POSIX or
 * Win32 threads plus the native affinity API) and a freestanding one for the
 * Linux kernel / FreeBSD kernel / baremetal targets (stub handler, pinning is
 * a no-op). Pinning is best-effort: callers treat a negative return as "not
 * pinned" and continue.
 */

#ifndef _JITTERENTROPY_ARCH_THREAD_H
#define _JITTERENTROPY_ARCH_THREAD_H

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER

/*
 * Include after jitterentropy.h, which is where the threading back-end and the
 * execution environment are selected (JENT_PTHREAD / JENT_WIN_THREADS and the
 * JENT_ARCH_THREAD_* macros) and where struct jent_notime_ctx and
 * jent_notime_start_routine are defined - both are part of the public
 * interface, since a consumer installing its own thread handler is handed that
 * context. This header only declares what arch/jitterentropy-arch-thread.c
 * implements on top of them.
 */

/* Definitions in arch/jitterentropy-arch-thread.c. */
int jent_thread_pin_to_cpu(unsigned long cpu);
int jent_notime_thread_create(struct jent_notime_ctx *ctx,
			      jent_notime_start_routine routine,
			      void *arg);
void jent_notime_thread_join(struct jent_notime_ctx *ctx);

#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

#endif /* _JITTERENTROPY_ARCH_THREAD_H */
