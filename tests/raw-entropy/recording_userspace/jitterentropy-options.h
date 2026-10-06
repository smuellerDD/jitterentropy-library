/*
 * The command line options the recording tools share
 *
 * Copyright (C) 2019 - 2026, Stephan Mueller <smueller@chronox.de>
 *
 * License: see LICENSE file in root directory
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
 * jitterentropy-rng, jitterentropy-osr and jitterentropy-hashtime take the
 * same flags; each used to parse them itself. Header only, as each tool is a
 * single translation unit built by its own Makefile as well.
 *
 * Messages go to stderr: jitterentropy-rng writes its data to stdout, which is
 * redirected into the file that is analyzed.
 */

#ifndef _JITTERENTROPY_OPTIONS_H
#define _JITTERENTROPY_OPTIONS_H

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jitterentropy.h"

/* The shared options, for the usage line; --osr only where it is offered. */
#define JENT_OPTIONS_USAGE						\
	"--ntg1|--force-fips|--disable-memory-access|"			\
	"--disable-internal-timer|--force-internal-timer|--all-caches|"	\
	"--max-mem <NUM>|--hloopcnt <NUM>"
#define JENT_OPTIONS_USAGE_OSR	"--osr <OSR>"

/*
 * Parse a complete numeric option value. A plain strtoul(str, NULL, 10) turns
 * a typo (or a follow-up option consumed as value) into 0 and the tool would
 * silently record with a configuration different from what was requested.
 */
static inline int parse_ulong(const char *str, unsigned long *val)
{
	char *endptr;

	errno = 0;
	*val = strtoul(str, &endptr, 10);
	if (endptr == str || *endptr != '\0' || errno != 0) {
		fprintf(stderr, "Invalid numeric value \"%s\"\n", str);
		return 1;
	}
	return 0;
}

/*
 * The value of the option at (*argv)[1], from (*argv)[2]: *argc and *argv
 * move on by one, as the caller's loop does past the option itself.
 */
static inline int jent_option_value(int *argc, char ***argv,
				    const char *what, unsigned long *val)
{
	(*argc)--;
	(*argv)++;
	if (*argc <= 1) {
		fprintf(stderr, "%s value missing\n", what);
		return 1;
	}

	return parse_ulong((*argv)[1], val);
}

/*
 * Parse the option at (*argv)[1] if it is one the tools share, into *flags
 * and, where the tool offers --osr (@osr not NULL), *osr. Returns 1 when it
 * was one - its value, if it takes one, consumed as jent_option_value()
 * describes - 0 when it was not, and -1 when it was one with an invalid value,
 * which is reported.
 *
 * Matched by prefix, as the tools always have.
 */
static inline int jent_parse_option(int *argc, char ***argv,
				    unsigned int *flags, unsigned int *osr)
{
	const char *opt = (*argv)[1];
	unsigned long val;

	if (!strncmp(opt, "--ntg1", 6))
		*flags |= JENT_NTG1;
	else if (!strncmp(opt, "--force-fips", 12))
		*flags |= JENT_FORCE_FIPS;
	else if (!strncmp(opt, "--disable-memory-access", 23))
		*flags |= JENT_DISABLE_MEMORY_ACCESS;
	else if (!strncmp(opt, "--disable-internal-timer", 24))
		*flags |= JENT_DISABLE_INTERNAL_TIMER;
	else if (!strncmp(opt, "--force-internal-timer", 22))
		*flags |= JENT_FORCE_INTERNAL_TIMER;
	else if (!strncmp(opt, "--all-caches", 12))
		*flags |= JENT_CACHE_ALL;
	else if (osr && !strncmp(opt, "--osr", 5)) {
		if (jent_option_value(argc, argv, "OSR", &val) ||
		    val >= UINT_MAX)
			return -1;
		*osr = (unsigned int)val;
	} else if (!strncmp(opt, "--max-mem", 9)) {
		/* 0 sets no size; n is JENT_MAX_MEMSIZE_1kB and up, 2^(n + 9) */
		if (jent_option_value(argc, argv, "Maximum memory", &val))
			return -1;
		if (val > 20) {
			fprintf(stderr, "Unknown maximum memory value\n");
			return -1;
		}
		/* A repeated option replaces the field rather than or-ing in. */
		*flags &= ~(unsigned int)JENT_MAX_MEMSIZE_MASK;
		*flags |= JENT_MAX_MEMSIZE_TO_FLAGS((unsigned int)val);
	} else if (!strncmp(opt, "--hloopcnt", 10)) {
		/* n is JENT_HASHLOOP_1 and up: 2^n loops */
		if (jent_option_value(argc, argv, "Hash loop count", &val))
			return -1;
		if (val > 7) {
			fprintf(stderr, "Unknown hashloop value\n");
			return -1;
		}
		*flags &= ~(unsigned int)JENT_MAX_HASHLOOP_MASK;
		*flags |= JENT_HASHLOOP_TO_FLAGS((unsigned int)val + 1);
	} else {
		return 0;
	}

	return 1;
}

#endif /* _JITTERENTROPY_OPTIONS_H */
