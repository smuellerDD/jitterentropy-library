/*
 * Jitter RNG: unit tests for libjitterentropy-kernel
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
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
 * The user space library of the kernel module, without the module: what it
 * does with its arguments, with a file that ends, and with a descriptor that
 * is no Jitter RNG device. The module itself answers in the VM tests, through
 * the jitter_rng tool.
 */

/* Ahead of the first system header, for the absorbed source's O_CLOEXEC. */
#define _GNU_SOURCE

#include "unit.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "../../linux_kernel/userspace/jitterentropy-kernel.c"

/* A temporary file holding @len bytes of @data; the path in @path. */
static int ut_file(char *path, const void *data, size_t len)
{
	int fd;

	strcpy(path, "/tmp/jent-kernel-XXXXXX");
	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	if (len && write(fd, data, len) != (ssize_t)len) {
		close(fd);
		remove(path);
		return -1;
	}
	close(fd);
	return 0;
}

static void test_open_read(void)
{
	char path[32];
	uint8_t data[100], buf[100];
	size_t i;
	int fd;

	jent_ut_group("opening and reading");

	JENT_UT_EQ(jent_kernel_open("/nonexistent/jent/device"), -ENOENT,
		   "a device that is not there is reported");

	for (i = 0; i < sizeof(data); i++)
		data[i] = (uint8_t)i;
	if (ut_file(path, data, sizeof(data))) {
		JENT_UT_SKIP("reading", "no temporary file");
		return;
	}

	fd = jent_kernel_open(path);
	JENT_UT_TRUE(fd >= 0, "a file opens");

	JENT_UT_EQ(jent_kernel_read(fd, buf, 40), 40,
		   "a read delivers what was asked for");
	JENT_UT_MEM_EQ(buf, data, 40, "and it is the data");
	JENT_UT_EQ(jent_kernel_read(fd, buf, sizeof(buf)), 60,
		   "a read across the end is short");
	JENT_UT_MEM_EQ(buf, data + 40, 60, "and carries what was left");
	JENT_UT_EQ(jent_kernel_read(fd, buf, 10), 0,
		   "a read at the end delivers nothing");

	JENT_UT_EQ(jent_kernel_read(fd, NULL, 10), -EINVAL,
		   "no buffer for a length is refused");
	JENT_UT_EQ(jent_kernel_read(fd, NULL, 0), 0,
		   "no buffer for no length is nothing to do");
	JENT_UT_EQ(jent_kernel_read(-1, buf, 10), -EBADF,
		   "an error before the first byte is returned");

	JENT_UT_EQ(jent_kernel_close(fd), 0, "the descriptor closes");
	JENT_UT_EQ(jent_kernel_close(fd), -EBADF, "and only once");

	remove(path);
}

/* A regular file is no Jitter RNG device: every ioctl says so. */
static void test_ioctls(void)
{
	char path[32], buf[JENT_KERNEL_STATUS_LEN];
	uint64_t a, b;
	unsigned int val;
	size_t size;
	int fd;

	jent_ut_group("the ioctls");

	if (ut_file(path, "x", 1)) {
		JENT_UT_SKIP("the ioctls", "no temporary file");
		return;
	}
	fd = jent_kernel_open(path);
	JENT_UT_TRUE(fd >= 0, "a file opens");

	JENT_UT_EQ(jent_kernel_status(fd, buf, sizeof(buf)), -ENOTTY,
		   "the status of a file that is no device");
	JENT_UT_EQ(jent_kernel_uuid(fd, buf, sizeof(buf)), -ENOTTY,
		   "its identifier");
	JENT_UT_EQ(jent_kernel_version(fd, &val), -ENOTTY, "its version");
	JENT_UT_EQ(jent_kernel_osr(fd, &val), -ENOTTY,
		   "its oversampling rate");
	JENT_UT_EQ(jent_kernel_flags(fd, &val), -ENOTTY, "its flags");
	JENT_UT_EQ(jent_kernel_health_failure(fd, &val), -ENOTTY,
		   "its health failures");
	JENT_UT_EQ(jent_kernel_reinitializations(fd, &val), -ENOTTY,
		   "its reinitializations");
	JENT_UT_EQ(jent_kernel_memsize(fd, &size), -ENOTTY,
		   "its memory size");
	JENT_UT_EQ(jent_kernel_hashloops(fd, &val), -ENOTTY,
		   "its hash loop count");
	JENT_UT_EQ(jent_kernel_output(fd, &a, &b), -ENOTTY, "its output");
	JENT_UT_EQ(jent_kernel_selftest(fd), -ENOTTY, "its self test");

	JENT_UT_EQ(jent_kernel_status(fd, NULL, sizeof(buf)), -EINVAL,
		   "a status without a buffer is refused");
	JENT_UT_EQ(jent_kernel_status(fd, buf, 0), -EINVAL,
		   "and one without a length");
	JENT_UT_EQ(jent_kernel_uuid(fd, buf, JENT_KERNEL_UUID_STRLEN - 1),
		   -EINVAL, "a buffer too small for a UUID is refused");
	JENT_UT_EQ(jent_kernel_uuid(fd, NULL, sizeof(buf)), -EINVAL,
		   "and none at all");
	JENT_UT_EQ(jent_kernel_version(fd, NULL), -EINVAL,
		   "a value with nowhere to go is refused");
	JENT_UT_EQ(jent_kernel_memsize(fd, NULL), -EINVAL,
		   "whatever its width");

	jent_kernel_close(fd);
	remove(path);
}

static void test_proc(void)
{
	static const char version[] = "3.8.0\n";
	char path[32], buf[64], name[128];

	jent_ut_group("the files in /proc/jitterentropy");

	JENT_UT_EQ(jent_kernel_proc(NULL, buf, sizeof(buf)), -EINVAL,
		   "no name is refused");
	JENT_UT_EQ(jent_kernel_proc("", buf, sizeof(buf)), -EINVAL,
		   "an empty one");
	JENT_UT_EQ(jent_kernel_proc("/etc/passwd", buf, sizeof(buf)), -EINVAL,
		   "an absolute one");
	JENT_UT_EQ(jent_kernel_proc("../version", buf, sizeof(buf)), -EINVAL,
		   "one leading out of the directory");
	JENT_UT_EQ(jent_kernel_proc("config/../../x", buf, sizeof(buf)),
		   -EINVAL, "also from below it");
	JENT_UT_EQ(jent_kernel_proc("version", NULL, sizeof(buf)), -EINVAL,
		   "no buffer");
	JENT_UT_EQ(jent_kernel_proc("version", buf, 0), -EINVAL,
		   "and no room in it");

	memset(name, 'a', sizeof(name) - 1);
	name[sizeof(name) - 1] = '\0';
	JENT_UT_EQ(jent_kernel_proc(name, buf, sizeof(buf)), -ENAMETOOLONG,
		   "a name too long is refused");
	JENT_UT_EQ(jent_kernel_proc("no/such/file", buf, sizeof(buf)), -ENOENT,
		   "a file that is not there is reported");

	/* The reading of a file, against ones made here. */
	if (ut_file(path, version, sizeof(version) - 1)) {
		JENT_UT_SKIP("reading a file", "no temporary file");
		return;
	}
	JENT_UT_EQ(jent_kernel_file(path, buf, sizeof(buf)), 0,
		   "a file is read");
	JENT_UT_TRUE(!strcmp(buf, version), "whole, and terminated");
	JENT_UT_EQ(jent_kernel_file(path, buf, sizeof(version)), 0,
		   "a buffer it just fits is enough");
	JENT_UT_TRUE(!strcmp(buf, version), "and holds all of it");
	buf[0] = 'x';
	JENT_UT_EQ(jent_kernel_file(path, buf, sizeof(version) - 1),
		   -EOVERFLOW, "a buffer one byte short is not");
	JENT_UT_EQ(buf[0], '\0', "and is left empty rather than cut short");
	remove(path);

	if (ut_file(path, NULL, 0)) {
		JENT_UT_SKIP("reading an empty file", "no temporary file");
		return;
	}
	buf[0] = 'x';
	JENT_UT_EQ(jent_kernel_file(path, buf, sizeof(buf)), 0,
		   "an empty file is read");
	JENT_UT_EQ(buf[0], '\0', "as an empty string");
	remove(path);
}

int main(void)
{
	test_open_read();
	test_ioctls();
	test_proc();

	return jent_ut_report("unit-kernel-lib");
}
