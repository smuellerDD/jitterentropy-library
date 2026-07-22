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
 * gcc -Wall -pedantic -Wextra -I../../.. -I../../../linux_kernel -o getrawentropy getrawentropy.c
 *
 * Compile for newer kernels (>= 6.13):
 * gcc -Wall -pedantic -Wextra -I../../.. -I../../../linux_kernel -DRAW_DATATYPE_U64 -o getrawentropy getrawentropy.c
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
#include <sys/ioctl.h>
#include <unistd.h>

#include "jitterentropy.h"
#include "jitterentropy_uapi.h"

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
	unsigned int status;
	uint64_t loopcnt;
	const char *debugfs_file;
	const char *sysfs_dir;
};

/*
 * Parse a complete numeric option value. A plain strtoul(str, NULL, 10) turns
 * a typo (or a follow-up option consumed as value due to the prefix matching
 * below) into 0 and the tool would silently record with a configuration
 * different from what the operator requested.
 */
static int parse_ulong(const char *str, unsigned long *val)
{
	char *endptr;

	errno = 0;
	*val = strtoul(str, &endptr, 10);
	if (endptr == str || *endptr != '\0' || errno != 0) {
		printf("Invalid numeric value \"%s\"\n", str);
		return -EINVAL;
	}
	return 0;
}

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
	/*
	 * The value leaves the stdio buffer only at fclose() time (it is far
	 * smaller than the buffer), so a kernel rejecting it (e.g. EINVAL)
	 * surfaces only here. Without this check the recording would silently
	 * run with a different osr/flags configuration than requested.
	 */
	if (config && fclose(config) != 0 && !ret) {
		printf("Configuration parameters writing to %s failed: %s\n",
		       file, strerror(errno));
		ret = -EINVAL;
	}
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
	/* Bytes of a partial record carried over from the previous read(). */
	size_t fill = 0;
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

	/*
	 * A non-default loop count requires the JENT_IOCLOOPCNT ioctl of the
	 * out-of-tree module's test interface. Fail hard when it is absent
	 * (e.g. the test interface of the vanilla kernel) instead of silently
	 * recording with the configured loop count.
	 */
	if (opts->loopcnt) {
		uint64_t loopcnt = opts->loopcnt;

		if (ioctl(fd, JENT_IOCLOOPCNT, &loopcnt) < 0) {
			perror("JENT_IOCLOOPCNT");
			ret = -errno;
			goto out;
		}
	}

	/*
	 * Early exit when the status is requested: print the JSON status of
	 * the freshly opened recording instance without recording any data,
	 * mirroring jitterentropy-hashtime --status. Can be used to compare
	 * the configuration of past measurements with the runtime.
	 */
	if (opts->status) {
		struct jent_status_ioctl status;
		char status_str[JENT_STATUS_MAX_LEN];

		status.buf = (uintptr_t)status_str;
		status.length = sizeof(status_str);

		if (ioctl(fd, JENT_IOCSTATUS, &status) < 0) {
			perror("JENT_IOCSTATUS");
			ret = -errno;
			goto out;
		}
		printf("%s", status_str);
		ret = 0;
		goto out;
	}

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

		/*
		 * Append to any partial record carried over from the previous
		 * iteration: a read() delivering a byte count that is not a
		 * record multiple (a pipe, or --debugfs-file pointing at an
		 * odd-sized regular file) would otherwise drop the trailing
		 * bytes and misalign - and thereby garble - every subsequently
		 * decoded value.
		 */
		ret = read(fd, buffer + fill, gather - fill);
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

		fill += (size_t)ret;

		for (i = 0; i < fill / (DATASIZE); i++) {
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

		/* Carry a trailing partial record to the next iteration. */
		fill -= (size_t)(buffer_p - buffer);
		memmove(buffer, buffer_p, fill);
	}

	ret = 0;

out:
	if (fd >= 0)
		close(fd);

	/*
	 * The samples were written through stdout's buffer; a write error on
	 * the redirect target (e.g. ENOSPC) may surface only at flush time.
	 * Report it so a truncated data file cannot pass as a success.
	 */
	if (!ret && (fflush(stdout) != 0 || ferror(stdout))) {
		fprintf(stderr, "Failed to write output\n");
		ret = -EIO;
	}

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
 *	     to the respecive used noise source(s)) - requires the
 *	     JENT_IOCLOOPCNT ioctl of the out-of-tree module's test interface
 * --max-mem Set the memory size of the memory block used for the memory access
 *	     loop
 * --hashloop Perform the measurement of the hash loop only
 * --memaccess Perform the measurement of the memory access loop only
 * --hloopcnt Number of hashloop operations at runtime
 * --status Print the JSON status of the recording instance and exit without
 *	    recording - requires the JENT_IOCSTATUS ioctl of the out-of-tree
 *	    module's test interface
 */
int main(int argc, char * argv[])
{
	struct opts opts;

	opts.samples = RAWENTROPY_SAMPLES;
	opts.debugfs_file = DEBUGFS_INTERFACE;
	opts.sysfs_dir = SYSFS_PARAM_DIR;
	opts.osr = 0;
	opts.flags = 0;
	opts.timestamps = 0;
	opts.status = 0;
	opts.loopcnt = 0;

	/*
	 * Every option has a default, so any single option (e.g. --samples N
	 * or --status) is a valid invocation.
	 */
	if (argc < 2) {
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

			if (parse_ulong(argv[1], &val))
				return 1;
			/*
			 * Bound the sample count so that the byte total
			 * (samples + 1) * DATASIZE cannot overflow size_t. A
			 * zero count would record an empty data file with exit
			 * status 0.
			 */
			if (!val || val >= UINT_MAX ||
			    val + 1 > SIZE_MAX / DATASIZE)
				return 1;
			opts.samples = (size_t)val;
		} else if (!strncmp(argv[1], "--debugfs-file", 14) ||
			   !strncmp(argv[1], "-f", 6)) {
			argc--;
			argv++;
			if (argc <= 1) {
				printf("debugfs file path missing\n");
				return 1;
			}

			opts.debugfs_file = argv[1];

		} else if (!strncmp(argv[1], "--param-dir", 11)) {
			argc--;
			argv++;
			if (argc <= 1) {
				printf("sysfs parameter directory path missing\n");
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

			if (parse_ulong(argv[1], &val) || val >= UINT_MAX)
				return 1;
			opts.osr = (unsigned int)val;
		} else if (!strncmp(argv[1], "--loopcnt", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Loop count value missing\n");
				return 1;
			}

			/*
			 * Mirror the bound of the userspace recording tool
			 * jitterentropy-hashtime (also enforced by the
			 * JENT_IOCLOOPCNT ioctl).
			 */
			if (parse_ulong(argv[1], &val) || val >= UINT_MAX)
				return 1;
			opts.loopcnt = val;
		} else if (!strncmp(argv[1], "--max-mem", 9)) {
			unsigned long val;

			argc--;
			argv++;
			if (argc <= 1) {
				printf("Maximum memory value missing\n");
				return 1;
			}

			if (parse_ulong(argv[1], &val))
				return 1;
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

			if (parse_ulong(argv[1], &val))
				return 1;
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
		} else if (!strncmp(argv[1], "--status", 8)) {
			opts.status = 1;
		} else {
			printf("Unknown option %s\n", argv[1]);
			return 1;
		}

		argc--;
		argv++;
	}

	return getrawentropy(&opts);
}
