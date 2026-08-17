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
 * OS-specific online CPU count discovery.
 *
 * Provides jent_ncpu() returning the number of online logical CPUs, or
 * a negative errno on failure. The dispatch (see
 * arch/jitterentropy-arch-ncpu.c) is:
 *   - Windows                        -> GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
 *   - Linux (glibc)                  -> sched_getaffinity(2), with
 *                                       sysconf(_SC_NPROCESSORS_ONLN) as fallback
 *   - Linux (musl / non-glibc)       -> sched_getaffinity(2), with
 *                                       /sys/devices/system/cpu/online and
 *                                       sysconf(_SC_NPROCESSORS_ONLN) as
 *                                       fallbacks
 *   - hosted Unix-like (BSDs, Apple, -> sysconf(_SC_NPROCESSORS_ONLN)
 *     AIX, Solaris/illumos, Haiku,
 *     Cygwin)
 *   - Linux Kernel                   -> 1 (we do not need a timer thread)
 *   - other (e.g. baremetal)         -> 1 (timer thread will be disabled)
 *
 * Provides jent_cpu_highest() returning the highest of those CPU numbers - the
 * one a thread may be pinned to - or a negative errno. Not the count minus
 * one: the CPUs a thread may run on are a set, and one confined to a cpuset
 * need not hold the numbers the count would name. Only Linux can tell the two
 * apart; elsewhere the count minus one is all there is.
 */

#ifndef _JITTERENTROPY_ARCH_NCPU_H
#define _JITTERENTROPY_ARCH_NCPU_H

/*
 * Largest CPU set the Linux affinity paths grow to, in CPUs; the highest CPU
 * number handled is therefore one below it. The bound is shared by
 * jent_cpu_highest() and jent_thread_pin_to_cpu(), so every CPU the first can
 * name is one the second can pin to - held to a fixed cpu_set_t the second
 * would refuse everything from CPU_SETSIZE (1024 on glibc) up. Far above the
 * CONFIG_NR_CPUS of any kernel built today, and a set of this size is a
 * short-lived 8 KiB allocation.
 */
#define JENT_NCPU_SET_MAX	(1U << 16)

long jent_ncpu(void);
long jent_cpu_highest(void);

#endif /* _JITTERENTROPY_ARCH_NCPU_H */
