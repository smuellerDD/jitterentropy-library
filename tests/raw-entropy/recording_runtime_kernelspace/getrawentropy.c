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

/*
 * Compile for older kernels (< 6.13):
 * gcc -Wall -pedantic -Wextra -I../../ -o getrawentropy getrawentropy.c
 *
 * Compile for newer kernels (>= 6.13):
 * gcc -Wall -pedantic -Wextra -I../../ -DRAW_DATATYPE_U64 -o getrawentropy getrawentropy.c
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "jitterentropy.h"

#define RAWENTROPY_SAMPLES	1001
#define DEBUGFS_INTERFACE	"/sys/kernel/debug/jitter_rng/jent_raw_hires"
#define SYSFS_PARAM_DIR		"/sys/module/jitter_rng/parameters"

#define JENT_TEST_HASHLOOP (1<<15)
#define JENT_TEST_MEMACCLOOP (1<<16)

/*
 * Starting with Linux kernel version 6.13, the data size changed from u32 to
 * u64 (see crypto/jitterentropy-testing.c:jent_testing_rb). Therefore, starting
 * from this kernel onwards, this tool MUST be compiled with -DRAW_DATATYPE_U64.
 */
#ifdef RAW_DATATYPE_U64

#define DATASIZE		sizeof(uint64_t)
#define PR_DATATYPE		PRIu64
typedef uint64_t		lrngval_t;

#else /* RAW_DATATYPE_U64 */

#define DATASIZE		sizeof(uint32_t)
#define PR_DATATYPE		PRIu32
typedef uint32_t		lrngval_t;

#endif /* RAW_DATATYPE_U64 */

struct opts {
	size_t samples;
	unsigned int flags;
	unsigned int osr;
	unsigned int timestamps;
	const char *debugfs_file;
	const char *sysfs_dir;
};

static int write_config(const struct opts *opts, const char *file,
			unsigned int val)
{
	FILE *config = NULL;
	char filename[FILENAME_MAX];
	size_t written, len;
	int ret = 0;

	snprintf(filename, sizeof(filename), "%s/%s", opts->sysfs_dir, file);
	config = fopen(filename, "r+");
	/*
	 * If we cannot open the file, we silently ignore that (e.g. for older)
	 * variants of the in-kernel Jitter RNG.
	 */
	if (!config)
		return 0;

	/* Create string to write and write it */
	snprintf(filename, sizeof(filename), "%u", val);
	len = strlen(filename);
	written = fwrite(filename, 1, len, config);
	if (written != len) {
		printf("Configuration parameters writing to %s failed (%zu written, %zu expected to write)\n",
		       file, written, len);
		ret = -EINVAL;
		goto out;
	}

out:
	if (config)
		fclose(config);
	return ret;
}

static int getrawentropy(const struct opts *opts)
{
#define BUFFER_SIZE		(RAWENTROPY_SAMPLES * DATASIZE)
	/*
	 * Use size_t to avoid truncating the byte count for large --samples
	 * values (the previous uint32_t wrapped, e.g. silently yielding 0).
	 *
	 * requested counts the output samples still to be printed; --samples N
	 * yields exactly N output lines in both operation modes.
	 */
	size_t requested = opts->samples * DATASIZE;
	lrngval_t leftover = 0;
	uint8_t leftover_present = 0;
	uint8_t *buffer_p, buffer[BUFFER_SIZE];
	ssize_t ret;
	int fd = -1;

	ret = write_config(opts, "testing_osr", opts->osr);
	if (ret < 0)
		return ret;
	ret = write_config(opts, "testing_flags", opts->flags);
	if (ret < 0)
		return ret;

	fd = open(opts->debugfs_file, O_RDONLY);
	if (fd < 0)
		return errno;

	while (requested) {
		unsigned int i;
		size_t need = requested;
		size_t gather;

		/*
		 * In time stamp mode, the first input value only serves as the
		 * baseline of the delta computation and produces no output, so
		 * one extra input value is required.
		 */
		if (opts->timestamps && !leftover_present)
			need += DATASIZE;

		gather = (need > BUFFER_SIZE) ? BUFFER_SIZE : need;

		buffer_p = buffer;

		ret = read(fd, buffer_p, gather);
		if (ret < 0) {
			ret = -errno;
			goto out;
		}
		if (ret == 0) {
			/*
			 * Unexpected EOF (e.g. --debugfs-file points at an
			 * empty/short regular file). Without this the outer
			 * loop would spin forever as 'requested' never reaches
			 * zero.
			 */
			fprintf(stderr, "Unexpected EOF reading %s\n",
				opts->debugfs_file);
			ret = -EIO;
			goto out;
		}

		for (i = 0; i < ret / (DATASIZE); i++) {
			lrngval_t val;

			memcpy(&val, buffer_p, DATASIZE);
			buffer_p += DATASIZE;

			if (opts->timestamps) {
				/*
				 * Time stamp mode: the interface delivers raw
				 * time stamps, output the deltas of successive
				 * values.
				 */
				if (!leftover_present) {
					leftover = val;
					leftover_present = 1;
					continue;
				}

				printf("%"PR_DATATYPE"\n", val - leftover);
				leftover = val;
			} else {
				/*
				 * Delta mode (default): the interface delivers
				 * the raw noise time deltas as consumed by the
				 * Jitter RNG, output them unmodified.
				 */
				printf("%"PR_DATATYPE"\n", val);
			}

			requested -= DATASIZE;

			if (requested == 0)
				break;
		}
	}

	ret = 0;

out:
	if (fd >= 0)
		close(fd);

	return (int)ret;
}

/*
 * Invoke the application.
 *
 * The options allowed for this application are as follows:
 *
 * --samples Number of raw values generated for one instance
 * --debugfs-file DebugFS file to access
 * --param-dir SysFS parameter directory
 * --timestamps The interface delivers raw time stamps instead of time deltas:
 *	        print the deltas of successive values. This is required for the
 *	        test interface of the vanilla Linux kernel
 *	        (crypto/jitterentropy-testing.c). The out-of-tree module of this
 *	        code base delivers the measure_jitter output time deltas
 *	        directly, which are printed unmodified (default).
 * --ntg1 Enable flag JENT_NTG1
 * --force-fips Enable flag JENT_FORCE_FIPS
 * --disable-memory-access Enable flag JENT_DISABLE_MEMORY_ACCESS
 * --disable-internal-timer Enable flag JENT_FORCE_INTERNAL_TIMER
 * --force-internal-timer Enable flag JENT_FORCE_INTERNAL_TIMER
 * --osr Apply the given OSR value
 * --loopcnt Apply the given loop count value for the operation (i.e. apply it
 *	     to the respecive used noise source(s))
 * --max-mem Set the memory size of the memory block used for the memory access
 *	     loop
 * --hashloop Perform the measurement of the hash loop only
 * --memaccess Perform the measurement of the memory access loop only
 * --hloopcnt Number of hashloop operations at runtime
 */
int main(int argc, char * argv[])
{
	struct opts opts;
	//unsigned int loopcnt = 0;

	opts.samples = RAWENTROPY_SAMPLES;
	opts.debugfs_file = DEBUGFS_INTERFACE;
	opts.sysfs_dir = SYSFS_PARAM_DIR;
	opts.osr = 0;
	opts.flags = 0;
	opts.timestamps = 0;

	if (argc < 4) {
		printf("%s --samples <NUMSAMPLES> | --debugfs-file <FILE> [ --param-dir <DIR> | --timestamps | --ntg1|--force-fips|--disable-memory-access|--disable-internal-timer|--force-internal-timer|--osr <OSR>|--loopcnt <NUM>|--max-mem <NUM>|--hashloop|--memaccess|--all-caches|--hloopcnt <NUM>|--status]\n", argv[0]);
		return 1;
	}

	while (argc > 1) {
		if (!strncmp(argv[1], "--samples", 9) ||
		    !strncmp(argv[1], "-s", 2)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("OSR value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			/*
			 * Bound the sample count so that the byte total
			 * (samples + 1) * DATASIZE cannot overflow size_t.
			 */
			if (val >= UINT_MAX || val + 1 > SIZE_MAX / DATASIZE)
				return 1;
			opts.samples = (size_t)val;
		} else if (!strncmp(argv[1], "--debugfs-file", 14) ||
			   !strncmp(argv[1], "-f", 6)) {
			argc--;
			argv++;
			if (argc <= 1) {
				printf("OSR value missing\n");
				return 1;
			}

			opts.debugfs_file = argv[1];

		} else if (!strncmp(argv[1], "--param-dir", 11)) {
			argc--;
			argv++;
			if (argc <= 1) {
				printf("OSR value missing\n");
				return 1;
			}

			opts.sysfs_dir = argv[1];

		} else if (!strncmp(argv[1], "--timestamps", 12))
			opts.timestamps = 1;
		else if (!strncmp(argv[1], "--ntg1", 6))
			opts.flags |= JENT_NTG1;
		else if (!strncmp(argv[1], "--force-fips", 12))
			opts.flags |= JENT_FORCE_FIPS;
		else if (!strncmp(argv[1], "--disable-memory-access", 23))
			opts.flags |= JENT_DISABLE_MEMORY_ACCESS;
		else if (!strncmp(argv[1], "--disable-internal-timer", 24))
			opts.flags |= JENT_DISABLE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--force-internal-timer", 22))
			opts.flags |= JENT_FORCE_INTERNAL_TIMER;
		else if (!strncmp(argv[1], "--all-caches", 12))
			opts.flags |= JENT_CACHE_ALL;
		else if (!strncmp(argv[1], "--hashloop", 10))
			opts.flags |= JENT_TEST_HASHLOOP;
		else if (!strncmp(argv[1], "--memaccess", 11))
			opts.flags |= JENT_TEST_MEMACCLOOP;
		else if (!strncmp(argv[1], "--osr", 5)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("OSR value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			if (val >= UINT_MAX)
				return 1;
			opts.osr = (unsigned int)val;
		// } else if (!strncmp(argv[1], "--loopcnt", 9)) {
		// 	unsigned long val;
  //
		// 	argc--;
		// 	argv++;
		// 	if (argc <= 1) {
		// 		printf("Loop count value missing\n");
		// 		return 1;
		// 	}
  //
		// 	val = strtoul(argv[1], NULL, 10);
		// 	if (val >= UINT_MAX)
		// 		return 1;
		// 	loopcnt = (unsigned int)val;
		} else if (!strncmp(argv[1], "--max-mem", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Maximum memory value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			switch (val) {
			case 0:
				/* Allow to set no option */
				break;
			case 1:
				opts.flags |= JENT_MAX_MEMSIZE_1kB;
				break;
			case 2:
				opts.flags |= JENT_MAX_MEMSIZE_2kB;
				break;
			case 3:
				opts.flags |= JENT_MAX_MEMSIZE_4kB;
				break;
			case 4:
				opts.flags |= JENT_MAX_MEMSIZE_8kB;
				break;
			case 5:
				opts.flags |= JENT_MAX_MEMSIZE_16kB;
				break;
			case 6:
				opts.flags |= JENT_MAX_MEMSIZE_32kB;
				break;
			case 7:
				opts.flags |= JENT_MAX_MEMSIZE_64kB;
				break;
			case 8:
				opts.flags |= JENT_MAX_MEMSIZE_128kB;
				break;
			case 9:
				opts.flags |= JENT_MAX_MEMSIZE_256kB;
				break;
			case 10:
				opts.flags |= JENT_MAX_MEMSIZE_512kB;
				break;
			case 11:
				opts.flags |= JENT_MAX_MEMSIZE_1MB;
				break;
			case 12:
				opts.flags |= JENT_MAX_MEMSIZE_2MB;
				break;
			case 13:
				opts.flags |= JENT_MAX_MEMSIZE_4MB;
				break;
			case 14:
				opts.flags |= JENT_MAX_MEMSIZE_8MB;
				break;
			case 15:
				opts.flags |= JENT_MAX_MEMSIZE_16MB;
				break;
			case 16:
				opts.flags |= JENT_MAX_MEMSIZE_32MB;
				break;
			case 17:
				opts.flags |= JENT_MAX_MEMSIZE_64MB;
				break;
			case 18:
				opts.flags |= JENT_MAX_MEMSIZE_128MB;
				break;
			case 19:
				opts.flags |= JENT_MAX_MEMSIZE_256MB;
				break;
			case 20:
				opts.flags |= JENT_MAX_MEMSIZE_512MB;
				break;
			default:
				printf("Unknown maximum memory value\n");
				return 1;
			}
		} else if (!strncmp(argv[1], "--hloopcnt", 10)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Hash loop count value missing\n");
				return 1;
			}

			val = strtoul(argv[1], NULL, 10);
			switch (val) {
			case 0:
				opts.flags |= JENT_HASHLOOP_1;
				break;
			case 1:
				opts.flags |= JENT_HASHLOOP_2;
				break;
			case 2:
				opts.flags |= JENT_HASHLOOP_4;
				break;
			case 3:
				opts.flags |= JENT_HASHLOOP_8;
				break;
			case 4:
				opts.flags |= JENT_HASHLOOP_16;
				break;
			case 5:
				opts.flags |= JENT_HASHLOOP_32;
				break;
			case 6:
				opts.flags |= JENT_HASHLOOP_64;
				break;
			case 7:
				opts.flags |= JENT_HASHLOOP_128;
				break;
			default:
				printf("Unknown hashloop value\n");
				return 1;
			}
		// } else if (!strncmp(argv[1], "--status", 8)) {
		// 	status = 1;
		} else {
			printf("Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	return getrawentropy(&opts);
}
