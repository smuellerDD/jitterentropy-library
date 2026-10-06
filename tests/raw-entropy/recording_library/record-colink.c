/*
 * libjitterentropy and libjitterentropy-record linked into one program
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
 * An app collecting with libjitterentropy and recording with
 * libjitterentropy-record: that the two link at all is half the test - a copy
 * of the library the recording exported, or left global in its archive, would
 * clash with libjitterentropy here. The other half is that a collector of the
 * one keeps working around a recording of the other, through
 * jent_record_raw() and through jitterentropy-hashtime run from code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jitterentropy.h"
#include "jitterentropy-record.h"

#define ROUNDS 1000

int main(int argc, char *argv[])
{
	struct jent_record_result result;
	struct rand_data *ec;
	char out[32], path[4096], line[64], status[JENT_RECORD_STATUS_LEN];
	unsigned int lines = 0;
	FILE *f;
	int ret;

	if (argc != 2) {
		fprintf(stderr, "%s <file name prefix>\n", argv[0]);
		return 1;
	}

	ret = jent_entropy_init_ex(0, 0);
	if (ret) {
		printf("SKIP: jent_entropy_init_ex failed on this machine: %d\n",
		       ret);
		return 77;
	}
	ec = jent_entropy_collector_alloc(0, 0);
	if (!ec) {
		printf("FAIL: jent_entropy_collector_alloc\n");
		return 1;
	}

	snprintf(path, sizeof(path), "%s-0001.data", argv[1]);
	ret = jent_record_raw(path, ROUNDS, 0, 0, JENT_RECORD_COMMON, 0,
			      &result, status, sizeof(status));
	if (ret) {
		printf("FAIL: jent_record_raw: %d\n", ret);
		return 1;
	}

	f = fopen(path, "r");
	if (!f) {
		printf("FAIL: %s not written\n", path);
		return 1;
	}
	while (fgets(line, sizeof(line), f))
		lines++;
	fclose(f);
	remove(path);
	if (lines != ROUNDS) {
		printf("FAIL: %u time deltas recorded, not %u\n", lines, ROUNDS);
		return 1;
	}
	if (!strstr(status, "\"version\"")) {
		printf("FAIL: no status document of the recording collector\n");
		return 1;
	}

	/* jitterentropy-hashtime <ROUNDS> 2 <prefix>-run, reported, not printed */
	{
		struct jent_record_config config = {
			.rounds = ROUNDS, .repeats = 2, .cpu = -1,
			.source = JENT_RECORD_HASHLOOP,
		};
		char file[4096];
		size_t len;
		char *report;
		unsigned int i;

		snprintf(file, sizeof(file), "%s-run", argv[1]);
		config.file = file;
		len = jent_record_report_len(&config);
		report = malloc(len);
		if (!report) {
			printf("FAIL: no memory for the report\n");
			return 1;
		}
		ret = jent_record_run(&config, report, len);
		if (ret || !strstr(report, "Processing") ||
		    !strstr(report, "-run-0002.data")) {
			printf("FAIL: jent_record_run: %d, report:\n%s", ret,
			       report);
			return 1;
		}
		free(report);

		for (i = 1; i <= 2; i++) {
			snprintf(path, sizeof(path), "%s-%04u.data", file, i);
			if (remove(path)) {
				printf("FAIL: %s not written\n", path);
				return 1;
			}
		}
	}

	/* Sample by sample, as the kernel test interface records. */
	{
		struct jent_record *rec;
		uint64_t nonzero = 0;
		unsigned int i;

		if (jent_record_alloc(&rec, 0, JENT_DISABLE_MEMORY_ACCESS,
				      JENT_RECORD_MEMACCESS) !=
			    JENT_RECORD_EINVAL ||
		    jent_record_alloc(&rec, JENT_MAX_OSR + 1, 0,
				      JENT_RECORD_COMMON) !=
			    JENT_RECORD_EINVAL) {
			printf("FAIL: jent_record_alloc took bad arguments\n");
			return 1;
		}

		ret = jent_record_alloc(&rec, 0, 0, JENT_RECORD_COMMON);
		if (ret) {
			printf("FAIL: jent_record_alloc: %d\n", ret);
			return 1;
		}
		jent_record_prime(rec, 0);
		for (i = 0; i < ROUNDS; i++)
			nonzero |= jent_record_sample(rec, 0);
		jent_record_free(rec);
		if (!nonzero) {
			printf("FAIL: jent_record_sample gave only zeroes\n");
			return 1;
		}
	}

	/* A health test failure is the machine's, not a link failure. */
	ret = (int)jent_read_entropy_safe(&ec, out, sizeof(out));
	if (ret < 0 && ret != JENT_ERR_RCT && ret != JENT_ERR_APT &&
	    ret != JENT_ERR_LAG && ret != JENT_ERR_RCT_MEM) {
		printf("FAIL: jent_read_entropy_safe after the recording: %d\n",
		       ret);
		return 1;
	}
	jent_entropy_collector_free(ec);

	printf("PASS: %u time deltas recorded beside a collector\n", lines);
	return 0;
}
