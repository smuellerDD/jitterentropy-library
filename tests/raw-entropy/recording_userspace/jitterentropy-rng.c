/*
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

#include "jitterentropy.h"
#include "jitterentropy-memlock.h"
#include "jitterentropy-options.h"

#include <errno.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <fcntl.h>
# include <io.h>
#endif

int main(int argc, char * argv[])
{
	unsigned long long size, rounds;
	int ret = 0;
	unsigned int flags = 0, osr = 0;
	struct rand_data *ec_nostir;
	char status[4096];
	int hex = 0;
	size_t i;

	if (argc < 2) {
		fprintf(stderr, "%s <number of measurements> [" JENT_OPTIONS_USAGE "|" JENT_OPTIONS_USAGE_OSR "|--hex]\n", argv[0]);
		return 1;
	}

	{
		char *endp;
		const char *p = argv[1];

		while (*p == ' ' || *p == '\t')
			p++;

		/*
		 * Reject non-numeric input instead of treating it as 0, and a
		 * sign with it: strtoull() accepts "-5" and wraps it round to
		 * ULLONG_MAX - 4 with errno clear, so "jitterentropy-rng -5"
		 * ran for what amounts to forever.
		 */
		if (*p == '-' || *p == '+') {
			fprintf(stderr, "Invalid rounds value %s\n", argv[1]);
			return 1;
		}

		errno = 0;
		rounds = strtoull(p, &endp, 10);
		if (errno || endp == p || *endp != '\0' ||
		    rounds >= ULLONG_MAX) {
			fprintf(stderr, "Invalid rounds value %s\n", argv[1]);
			return 1;
		}
	}
	argc--;
	argv++;

	while (argc > 1) {
		int ret_opt = jent_parse_option(&argc, &argv, &flags, &osr);

		if (ret_opt < 0)
			return 1;
		if (ret_opt > 0) {
			/* One of the options all tools share. */
		} else if (!strncmp(argv[1], "--hex", 5)) {
			hex = 1;
		} else {
			fprintf(stderr, "Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	/*
	 * The compliance modes require the collector memory to be locked into
	 * RAM, which the operating system permits only within a per-process
	 * limit. See jitterentropy-memlock.h.
	 */
	if (jent_raise_memlock_limit(flags))
		fprintf(stderr,
			"Cannot raise the memory lock limit, allocating the entropy collector may fail\n");

	/*
	 * Likewise the secure memory arena of the external crypto backends,
	 * which is created once for the process and is what the library
	 * allocates the collector from. See jitterentropy-memlock.h.
	 *
	 * Both precede jent_entropy_init_ex(): the initialization creates a
	 * collector of its own, with the very same flags, so it allocates from
	 * the arena and locks its memory just like the instance below.
	 */
	if (jent_init_secure_memory(flags))
		fprintf(stderr,
			"Cannot create the secure memory arena, allocating the entropy collector will fail\n");

	ret = jent_entropy_init_ex(osr, flags);
	if (ret) {
		fprintf(stderr, "The initialization failed with error code %d\n",
			ret);
		return ret;
	}

	ec_nostir = jent_entropy_collector_alloc(osr, flags);
	if (!ec_nostir) {
		fprintf(stderr, "Jitter RNG handle cannot be allocated\n");
		return 1;
	}

	if (jent_status(ec_nostir, status, sizeof(status))) {
		fprintf(stderr, "Cannot obtain status information\n");
		ret = 1;
		goto out;
	}

	fprintf(stderr, "%s", status);

	/*
	 * Windows opens stdout in text mode: every 0x0A byte of the random
	 * stream would be written out as 0x0D 0x0A, corrupting and inflating
	 * the data as soon as it is redirected into a file or a pipe - which
	 * is the only way this tool is used. The hex encoding below emits no
	 * 0x0A at all and is therefore left alone.
	 */
#if defined(_MSC_VER) || defined(__MINGW32__)
	if (!hex && _setmode(_fileno(stdout), _O_BINARY) == -1) {
		fprintf(stderr, "Cannot switch stdout to binary mode\n");
		ret = 1;
		goto out;
	}
#endif

	for (size = 0; size < rounds; size++) {
		uint8_t tmp[32];

		ssize_t rc = jent_read_entropy_safe(&ec_nostir, (char *)tmp,
						    sizeof(tmp));

		if (rc < 0) {
			fprintf(stderr, "Reading random data failed with error code %zd\n",
				rc);
			ret = 1;
			goto out;
		}
		/*
		 * Treat output errors as fatal: consumers pipe this data into
		 * files for analysis, and a silently truncated stream with
		 * exit code 0 would be processed as if complete.
		 */
		if (hex) {
			for (i = 0; i < sizeof(tmp); ++i) {
				if (fprintf(stdout, "%02X",
					    (unsigned int)tmp[i]) < 0) {
					fprintf(stderr, "Can't output data\n");
					ret = 1;
					goto out;
				}
			}
		} else {
			if (fwrite(&tmp, sizeof(tmp), 1, stdout) != 1) {
				fprintf(stderr, "Can't output data\n");
				ret = 1;
				goto out;
			}
		}
	}

	/*
	 * Most of the output above only fills the stdio buffer; the final
	 * chunk would be flushed inside exit(), where a write failure (e.g.
	 * ENOSPC) is silently discarded. Flush explicitly so a truncated
	 * stream cannot terminate with exit code 0.
	 */
	if (fflush(stdout) != 0) {
		fprintf(stderr, "Can't output data\n");
		ret = 1;
		goto out;
	}

	ret = 0;

out:
	jent_entropy_collector_free(ec_nostir);
	return ret;
}
