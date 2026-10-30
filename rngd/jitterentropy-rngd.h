/*
 * jitterentropy-rngd: the interface between the daemon and its operating
 * systems
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
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
 * jitterentropy-rngd.c is the daemon on every system. How the kernel takes the
 * data is in jitterentropy-rngd-linux.c or jitterentropy-rngd-bsd.c, which
 * implement the rngd_os_*() below.
 */

#ifndef JITTERENTROPY_RNGD_H
#define JITTERENTROPY_RNGD_H

#include <getopt.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>

#include "jitterentropy.h"

#define JENT_LOG_DEBUG	3
#define JENT_LOG_VERBOSE	2
#define JENT_LOG_WARN	1
#define JENT_LOG_ERR		0
/* Logged at any verbosity: what was asked for, as by SIGUSR1 */
#define JENT_LOG_NOTICE	(-1)

/* Not errnos, which differ per system and reach into a shell's 128+n */
#define RNGD_EXIT_OK		0
#define RNGD_EXIT_ERROR		1	/* any other failure */
#define RNGD_EXIT_PERMANENT	2	/* permanent health test failure */
#define RNGD_EXIT_EIO		3
#define RNGD_EXIT_EINVAL	4
#define RNGD_EXIT_EPERM		5	/* EPERM or EACCES */

#define ENTROPYBYTES 32
#define OVERSAMPLINGFACTOR 2
/* Bytes handed to the kernel in one injection */
#define INJECT_BYTES	(ENTROPYBYTES * OVERSAMPLINGFACTOR)

struct kernel_rng {
	int fd;
	struct rand_data *ec;
	const char *dev;
};

/* /dev/random, opened for writing by the daemon */
extern struct kernel_rng Random;

extern unsigned int jent_flags;
extern int exit_on_error;
extern int oneshot;
extern volatile sig_atomic_t Term_pending;

void dolog(int severity, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
void dolog_exit(int exit_status, const char *fmt, ...)
	__attribute__((format(printf, 2, 3), noreturn));
int rngd_exit_status(int err);
void usage(void) __attribute__((noreturn));
unsigned int parse_uint_until(const char *str, char term, const char **rest);
unsigned int parse_uint(const char *str);
int install_handler(int sig, void (*handler)(int), int flags);
void dealloc(void);
ssize_t gather_entropy_retry(struct kernel_rng *rng);
void process_term(void) __attribute__((noreturn));
void process_status(void);

/* A wipe the compiler cannot drop, before a free() or an exit() included */
static inline void memset_secure(void *s, int c, size_t n)
{
	memset(s, c, n);
	__asm__ __volatile__("" : : "r" (s) : "memory");
}

/*
 * The operating system's part. Its options extend the daemon's: short ones in
 * rngd_os_optstring, long ones in rngd_os_options, ended by a nameless entry. A
 * long option without a short one takes a val from RNGD_OS_OPT on.
 */
#define RNGD_OS_OPT	512

extern const char rngd_os_description[];
extern const char rngd_os_optstring[];
extern const struct option rngd_os_options[];
void rngd_os_parse(int c, const char *arg);
void rngd_os_usage(void);

/* After the options are parsed, before any random number exists. */
void rngd_os_harden(void);

/* The daemon's start, past what --generate takes. */
void rngd_os_start(void);

/* After Random is open: what else the injections need. 0 or a -errno. */
int rngd_os_alloc(struct kernel_rng *rng);

/* Inject @len bytes with @entropy_bytes of entropy: bytes taken or -errno */
ssize_t rngd_os_inject(struct kernel_rng *rng, char *buf, size_t len,
		       size_t entropy_bytes);

/* Keep only what the run needs, @seeding if that includes injecting */
void rngd_os_drop_privileges(int seeding);

/* After each injection the kernel took in full. */
void rngd_os_seeded(void);

/* Once detached: give up what seeding and re-initializing do not need. */
void rngd_os_sandbox(struct kernel_rng *rng);

/*
 * The daemon from the first seed on, detached unless --foreground: wait for
 * the occasions to seed, and seed. Ends by process_term() or an error.
 */
void rngd_os_run(struct kernel_rng *rng) __attribute__((noreturn));

/* Release what rngd_os_alloc() set up. Called on every exit. */
void rngd_os_dealloc(struct kernel_rng *rng);

#endif /* JITTERENTROPY_RNGD_H */
