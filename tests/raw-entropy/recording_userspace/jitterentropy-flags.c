/*
 * Encode and decode the flags word of jent_entropy_init_ex() and
 * jent_entropy_collector_alloc()
 *
 * Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
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
 * Uses the JENT_* macros of jitterentropy.h only and links nothing, so the
 * names and values printed are those of the header it was built with.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "jitterentropy.h"
#include "jitterentropy-options.h"

#define F(name, note) { #name, name, note }

struct jent_flag {
	const char *name;
	unsigned int value;
	const char *note;
};

static const struct jent_flag jent_bits[] = {
	F(JENT_DISABLE_STIR, "unused"),
	F(JENT_DISABLE_UNBIAS, "unused"),
	F(JENT_DISABLE_MEMORY_ACCESS, NULL),
	F(JENT_FORCE_INTERNAL_TIMER, NULL),
	F(JENT_DISABLE_INTERNAL_TIMER, NULL),
	F(JENT_FORCE_FIPS, NULL),
	F(JENT_NTG1, NULL),
	F(JENT_CACHE_ALL, NULL),
	F(JENT_FORCE_SECURE_MEM, NULL),
};

/* Indexed by the field value minus one */
static const struct jent_flag jent_memsizes[] = {
	F(JENT_MAX_MEMSIZE_1kB, NULL),	 F(JENT_MAX_MEMSIZE_2kB, NULL),
	F(JENT_MAX_MEMSIZE_4kB, NULL),	 F(JENT_MAX_MEMSIZE_8kB, NULL),
	F(JENT_MAX_MEMSIZE_16kB, NULL),	 F(JENT_MAX_MEMSIZE_32kB, NULL),
	F(JENT_MAX_MEMSIZE_64kB, NULL),	 F(JENT_MAX_MEMSIZE_128kB, NULL),
	F(JENT_MAX_MEMSIZE_256kB, NULL), F(JENT_MAX_MEMSIZE_512kB, NULL),
	F(JENT_MAX_MEMSIZE_1MB, NULL),	 F(JENT_MAX_MEMSIZE_2MB, NULL),
	F(JENT_MAX_MEMSIZE_4MB, NULL),	 F(JENT_MAX_MEMSIZE_8MB, NULL),
	F(JENT_MAX_MEMSIZE_16MB, NULL),	 F(JENT_MAX_MEMSIZE_32MB, NULL),
	F(JENT_MAX_MEMSIZE_64MB, NULL),	 F(JENT_MAX_MEMSIZE_128MB, NULL),
	F(JENT_MAX_MEMSIZE_256MB, NULL), F(JENT_MAX_MEMSIZE_512MB, NULL),
};

static const struct jent_flag jent_hashloops[] = {
	F(JENT_HASHLOOP_1, NULL),  F(JENT_HASHLOOP_2, NULL),
	F(JENT_HASHLOOP_4, NULL),  F(JENT_HASHLOOP_8, NULL),
	F(JENT_HASHLOOP_16, NULL), F(JENT_HASHLOOP_32, NULL),
	F(JENT_HASHLOOP_64, NULL), F(JENT_HASHLOOP_128, NULL),
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* A new entry in the header has to be added to the tables above. */
typedef char jent_memsizes_complete[
	ARRAY_SIZE(jent_memsizes) ==
	JENT_FLAGS_TO_MAX_MEMSIZE(JENT_MAX_MEMSIZE_MAX) ? 1 : -1];
typedef char jent_hashloops_complete[
	ARRAY_SIZE(jent_hashloops) ==
	JENT_FLAGS_TO_HASHLOOP(JENT_MAX_HASHLOOP) ? 1 : -1];

static unsigned int jent_bits_mask(void)
{
	unsigned int mask = 0;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(jent_bits); i++)
		mask |= jent_bits[i].value;
	return mask;
}

/* The bits below the hash loop field that the header gives no meaning */
static unsigned int jent_reserved_mask(void)
{
	return (JENT_HASHLOOP_TO_FLAGS(1U) - 1) & ~jent_bits_mask();
}

static void usage(const char *name)
{
	fprintf(stderr, "Usage: %s [--hex] FLAG ...\n", name);
	fprintf(stderr, "       %s --decode VALUE\n\n", name);
	fprintf(stderr,
		"FLAG is a JENT_* name, the JENT_ prefix and case optional,\n"
		"or one of " JENT_OPTIONS_USAGE ".\n"
		"VALUE is decimal, or hexadecimal with a 0x prefix.\n");
}

/* Whether the first n characters of a and b match, ignoring case */
static int jent_case_equal(const char *a, const char *b, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (tolower((unsigned char)a[i]) !=
		    tolower((unsigned char)b[i]))
			return 0;
		if (!a[i])
			break;
	}
	return 1;
}

/* Case-insensitive, and the JENT_ prefix of the table's name optional */
static int jent_name_matches(const char *arg, const char *name)
{
	if (!jent_case_equal(arg, name, 5))
		name += 5;
	return jent_case_equal(arg, name, strlen(name) + 1);
}

static const struct jent_flag *jent_lookup(const struct jent_flag *table,
					   size_t n, const char *arg)
{
	size_t i;

	for (i = 0; i < n; i++) {
		if (jent_name_matches(arg, table[i].name))
			return &table[i];
	}
	return NULL;
}

/*
 * Add value to *flags. A second, different value for the memory size or
 * hash loop field would be ORed into a third, so it is refused.
 */
static int jent_add(unsigned int *flags, unsigned int value, const char *arg)
{
	static const unsigned int fields[] = {
		JENT_MAX_MEMSIZE_MASK, JENT_MAX_HASHLOOP_MASK
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fields); i++) {
		unsigned int have = *flags & fields[i];
		unsigned int add = value & fields[i];

		if (have && add && have != add) {
			fprintf(stderr, "%s: field already set\n", arg);
			return 1;
		}
	}
	*flags |= value;
	return 0;
}

/*
 * Report what jent_entropy_init_ex() and jent_entropy_collector_alloc()
 * refuse. Returns the number of reasons.
 */
static int jent_refusals(unsigned int flags)
{
	unsigned int memsize = JENT_FLAGS_TO_MAX_MEMSIZE(flags);
	unsigned int hashloop = JENT_FLAGS_TO_HASHLOOP(flags);
	int n = 0;

	if (flags & jent_reserved_mask()) {
		fprintf(stderr, "refused: reserved bits 0x%08x set\n",
			flags & jent_reserved_mask());
		n++;
	}
	if (memsize > ARRAY_SIZE(jent_memsizes)) {
		fprintf(stderr, "refused: memory size field %u above %u\n",
			memsize, (unsigned int)ARRAY_SIZE(jent_memsizes));
		n++;
	}
	if (hashloop > ARRAY_SIZE(jent_hashloops)) {
		fprintf(stderr, "refused: hash loop field %u above %u\n",
			hashloop, (unsigned int)ARRAY_SIZE(jent_hashloops));
		n++;
	}
	if ((flags & JENT_FORCE_INTERNAL_TIMER) &&
	    (flags & JENT_DISABLE_INTERNAL_TIMER)) {
		fprintf(stderr, "refused: JENT_FORCE_INTERNAL_TIMER with "
			"JENT_DISABLE_INTERNAL_TIMER\n");
		n++;
	}
	if ((flags & JENT_FORCE_INTERNAL_TIMER) && (flags & JENT_NTG1)) {
		fprintf(stderr,
			"refused: JENT_FORCE_INTERNAL_TIMER with JENT_NTG1\n");
		n++;
	}
	/* Also in the system's FIPS mode, which is not known here */
	if ((flags & JENT_DISABLE_MEMORY_ACCESS) &&
	    (flags & (JENT_FORCE_FIPS | JENT_NTG1))) {
		fprintf(stderr, "refused: JENT_DISABLE_MEMORY_ACCESS with "
			"JENT_FORCE_FIPS or JENT_NTG1\n");
		n++;
	}
	return n;
}

static void jent_print_field(const char *what, unsigned int mask,
			     unsigned int field, const struct jent_flag *table,
			     size_t n)
{
	printf("%-28s 0x%08x  ", what, mask);
	if (!field)
		printf("default\n");
	else if (field <= n)
		printf("%s (0x%08x)\n", table[field - 1].name,
		       table[field - 1].value);
	else
		printf("invalid value %u\n", field);
}

static int jent_decode(const char *arg)
{
	unsigned long val;
	char *endptr;
	unsigned int flags;
	size_t i;
	int base = 10;

	if (!strncmp(arg, "0x", 2) || !strncmp(arg, "0X", 2)) {
		arg += 2;
		base = 16;
	}
	/* strtoul() would take a sign and leading blanks */
	if (!isxdigit((unsigned char)*arg)) {
		fprintf(stderr, "Invalid value\n");
		return 1;
	}
	errno = 0;
	val = strtoul(arg, &endptr, base);
	if (errno || *endptr || val > 0xffffffffUL) {
		fprintf(stderr, "Invalid value\n");
		return 1;
	}
	flags = (unsigned int)val;

	printf("0x%08x = %u\n", flags, flags);
	for (i = 0; i < ARRAY_SIZE(jent_bits); i++) {
		const struct jent_flag *f = &jent_bits[i];
		const char *state = (flags & f->value) ? "set" : "-";

		if (f->value == JENT_FORCE_SECURE_MEM &&
		    !(flags & f->value) &&
		    (flags & (JENT_FORCE_FIPS | JENT_NTG1)))
			state = "implied";
		printf("%-28s 0x%08x  %s", f->name, f->value, state);
		if (f->note)
			printf(" (%s)", f->note);
		printf("\n");
	}
	printf("%-28s 0x%08x  %s\n", "reserved", jent_reserved_mask(),
	       (flags & jent_reserved_mask()) ? "set" : "-");
	jent_print_field("hash loop field", JENT_MAX_HASHLOOP_MASK,
			 JENT_FLAGS_TO_HASHLOOP(flags), jent_hashloops,
			 ARRAY_SIZE(jent_hashloops));
	jent_print_field("memory size field", JENT_MAX_MEMSIZE_MASK,
			 JENT_FLAGS_TO_MAX_MEMSIZE(flags), jent_memsizes,
			 ARRAY_SIZE(jent_memsizes));

	/* The refusals on stderr after the table, also when stdout is a pipe */
	fflush(stdout);
	return jent_refusals(flags) ? 1 : 0;
}

int main(int argc, char *argv[])
{
	const char *name = argv[0];
	unsigned int flags = 0;
	int hex = 0;

	if (argc > 1 && !strcmp(argv[1], "--decode")) {
		if (argc != 3) {
			usage(name);
			return 1;
		}
		return jent_decode(argv[2]);
	}
	if (argc < 2) {
		usage(name);
		return 1;
	}

	/* The loop looks at argv[1], as jent_parse_option() does */
	for (; argc > 1; argc--, argv++) {
		const char *arg = argv[1];
		const struct jent_flag *f;
		unsigned int opt = 0;
		int ret;

		if (!strcmp(arg, "--hex")) {
			hex = 1;
			continue;
		}
		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(name);
			return 0;
		}
		/* Only as the first argument, and with nothing else */
		if (!strcmp(arg, "--decode")) {
			usage(name);
			return 1;
		}

		ret = jent_parse_option(&argc, &argv, &opt, NULL);
		if (ret < 0)
			return 1;
		if (ret > 0) {
			if (jent_add(&flags, opt, arg))
				return 1;
			continue;
		}

		f = jent_lookup(jent_bits, ARRAY_SIZE(jent_bits), arg);
		if (!f)
			f = jent_lookup(jent_memsizes,
					ARRAY_SIZE(jent_memsizes), arg);
		if (!f)
			f = jent_lookup(jent_hashloops,
					ARRAY_SIZE(jent_hashloops), arg);
		if (!f) {
			fprintf(stderr, "Unknown flag %s\n", arg);
			usage(name);
			return 1;
		}
		if (jent_add(&flags, f->value, arg))
			return 1;
	}

	if (hex)
		printf("0x%08x\n", flags);
	else
		printf("%u\n", flags);

	fflush(stdout);
	return jent_refusals(flags) ? 1 : 0;
}
