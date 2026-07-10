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
 * The tool extracts the 4 or 8 LSB of the high-res time stamp and
 * concatenates them to form a binary data stream.
 *
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>

#ifdef _MSC_VER
#include <io.h>
#define open   _open
#define close  _close
#define unlink _unlink
#else
#include <unistd.h>
#endif

#define BITS_PER_SAMPLE 64

/*
	Extract bits from sample based on significant bit mask
*/

static unsigned char extract(uint64_t sample, uint64_t mask)
{
	unsigned char byte = 0;
	int i, j = 0;

	for (i = 0; i < BITS_PER_SAMPLE && mask; i++) {
		if (mask & 1) {
			byte |= (sample & 1) << j;
			j++;
		}
		mask >>= 1;
		sample >>= 1;
	}
	return (byte);
}

/*
	Convert mask in hexadecimal format to binary
*/

static int hextolong(char *p_strmask, uint64_t *p_mask)
{

	uint64_t mask = 0;
	int count = 0;

	while (*p_strmask) {
		count++;
		mask <<= 4;

		if ((*p_strmask >= '0') && (*p_strmask <= '9'))
			mask |= *p_strmask - '0';
		else if ((*p_strmask >= 'A') && (*p_strmask <= 'F'))
			mask |= *p_strmask - 'A' + 10;
		else if ((*p_strmask >= 'a') && (*p_strmask <= 'f'))
			mask |= *p_strmask - 'a' + 10;
		else
			return -1;

		p_strmask++;
	}

	if (count > 16)
		return(-1);

	*p_mask = mask;
	return(0);
}

/*
	Count the number of bits on
*/

static int bitcount(uint64_t mask)
{
	int i, j = 0;

	for (i = 0; i < BITS_PER_SAMPLE && mask; i++) {
		if (mask & 1) {
			j++;
		}
		mask >>= 1;
	}
	return (j);
}

/*
	Print 64 bits of long word masking with '-' those not matching value
*/

static char *printbits(uint64_t sample, int value)
{
	static char buf[BITS_PER_SAMPLE + 9];
	char *p_buf = buf + sizeof(buf) - 1;
	int i;
	*p_buf-- = '\0';
	for (i = 0; i < BITS_PER_SAMPLE; i++) {
		if (i % 8 == 0)
			*p_buf-- = ' ';

		if ((sample & 1) ^ value)
			*p_buf = '-';
		else
			*p_buf = '0' + value;
		p_buf--;
		sample >>= 1;
	}

	return buf;
}


int main(int argc, char *argv[])
{
	FILE *f = NULL;
	char buf[64];
	char *endptr = NULL;
	int fd = -1;
	uint32_t count;
	uint32_t i = 0;
	unsigned long val;

	uint64_t mask;
	uint64_t unchanged0s, unchanged1s;
	int rc;

	if (argc != 5) {
		fprintf(stderr, "Usage: %s inputfile outfile maxevents mask\n",
			argv[0]);
		return 1;
	}

	/*
	 * Validate all parameters before the output file is created: the
	 * output file is created with O_EXCL, so an early error exit must not
	 * leave an (empty) output file behind that would make every
	 * subsequent invocation fail.
	 */
	errno = 0;
	val = strtoul(argv[3], &endptr, 10);
	if (errno || !endptr || *endptr != '\0' || val == 0 ||
	    val > UINT32_MAX) {
		fprintf(stderr, "maxevents value is incorrect [%s]\n", argv[3]);
		return 1;
	}
	count = (uint32_t)val;

	rc = hextolong(argv[4], &mask);
	if (rc) {
		fprintf(stderr,
			"Mask value is incorrect [%s], use up to 16 hexadecimal characters\n",
			argv[4]);
		return 1;
	}

	if (bitcount(mask) > 8) {
		fprintf(stderr,
			"SP800-90B tool only supports up to 8 bits. Check the mask value\n");
		return 1;
	}

	f = fopen(argv[1], "r");
	if (!f) {
		fprintf(stderr, "File %s cannot be opened for read: %s\n",
			argv[1], strerror(errno));
		return 1;
	}

	fd = open(argv[2], O_CREAT|O_WRONLY|O_EXCL, 0777);
	if (fd < 0) {
		fprintf(stderr, "File %s cannot be opened for write: %s\n",
			argv[2], strerror(errno));
		fclose(f);
		return 1;
	}

	unchanged0s = 0;
	unchanged1s = ~(uint64_t)0;

	while (i < count && fgets(buf, sizeof(buf), f)) {
		uint64_t sample;
		unsigned char var;
		char *saveptr = NULL;
	 	char *res = NULL;

		i++;

#if defined(_MSC_VER)
		res = strtok_s(buf, " ", &saveptr);
#else
		res = strtok_r(buf, " ", &saveptr);
#endif
		if (!res) {
			fprintf(stderr, "strtok_r/s error (%s)\n", buf);
			goto err;
		}

		sample = strtoull(res, NULL, 10);
		unchanged0s |= sample;
		unchanged1s &= sample;

		var = extract(sample, mask);
		if (write(fd, &var, sizeof(var)) != (int)sizeof(var)) {
			fprintf(stderr, "write error: %s\n", strerror(errno));
			goto err;
		}
	}

	if (ferror(f)) {
		fprintf(stderr, "Read error on %s\n", argv[1]);
		goto err;
	}

	if (i < count) {
		fprintf(stderr,
			"Premature end of input: only %" PRIu32 " of %" PRIu32 " samples processed from %s\n",
			i, count, argv[1]);
		goto err;
	}

	printf("Processed %" PRIu32 " items from %s samples with mask [0x%016llx] significant bits [%d]\n", i, argv[1], (unsigned long long)mask, bitcount(mask));

	printf("Constant 0s in sample: \n%s\n", printbits(unchanged0s, 0));
	printf("Constant 1s in sample: \n%s\n", printbits(unchanged1s, 1));

	fclose(f);
	close(fd);
	return 0;

err:
	fclose(f);
	close(fd);
	/*
	 * Do not leave a partial output file behind: it would be analyzed as
	 * if it were complete and, due to O_EXCL, block any re-run.
	 */
	unlink(argv[2]);
	return 1;
}
