/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific online-CPU count.
 *
 * Definition of jent_ncpu() (declared in arch/jitterentropy-arch-ncpu.h). See
 * that header for the dispatch rationale.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 */

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

#define JENT_ARCH_NCPU_LINUX_KERNEL

#else /* LINUX_KERNEL */

#include <errno.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# define JENT_ARCH_NCPU_WINDOWS
#elif defined(__unix__) || defined(__APPLE__) || defined(_AIX) || \
      defined(__sun) || defined(__HAIKU__) || defined(__CYGWIN__)
# include <unistd.h>
# define JENT_ARCH_NCPU_POSIX
# ifdef __linux__
#  include <sched.h>
#  define JENT_ARCH_NCPU_LINUX_AFFINITY
#  ifndef __GLIBC__
#   include <fcntl.h>
#   include <stdlib.h>
#   define JENT_ARCH_NCPU_LINUX_SYSFS
#  endif
# endif
#endif

#endif /* LINUX_KERNEL */

#ifdef JENT_ARCH_NCPU_LINUX_SYSFS
/*
 * Parse /sys/devices/system/cpu/online and return the number of online
 * logical CPUs, or a negative errno on failure. The file holds a comma-
 * separated list of CPU index ranges, e.g. "0-3" on a 4-CPU system or
 * "0,2-5,8" when CPUs have been offlined. The kernel always exposes
 * this file when sysfs is mounted, including on UP systems (where it
 * reads "0").
 */
static long jent_ncpu_sysfs(void)
{
	char buf[256];
	int fd;
	ssize_t rlen;
	long count = 0;
	const char *p;

	fd = open("/sys/devices/system/cpu/online", O_RDONLY);
	if (fd < 0)
		return -errno;
	do {
		rlen = read(fd, buf, sizeof(buf) - 1);
	} while (rlen < 0 && errno == EINTR);
	close(fd);
	if (rlen <= 0)
		return -EIO;
	buf[rlen] = '\0';

	/*
	 * A read that fills the whole buffer without reaching the trailing
	 * newline was truncated (a system with many discontiguous ranges can
	 * exceed the buffer). Parsing the fragment would miscount the final
	 * range, so report an error and let the caller fall back.
	 */
	if ((size_t)rlen == sizeof(buf) - 1 && buf[rlen - 1] != '\n')
		return -EINVAL;

	p = buf;
	while (*p && *p != '\n') {
		char *endp;
		long start, end;

		errno = 0;
		start = strtol(p, &endp, 10);
		if (endp == p || errno != 0 || start < 0)
			return -EINVAL;
		p = endp;
		if (*p == '-') {
			p++;
			errno = 0;
			end = strtol(p, &endp, 10);
			if (endp == p || errno != 0 || end < start)
				return -EINVAL;
			p = endp;
		} else {
			end = start;
		}
		count += end - start + 1;
		if (*p == ',')
			p++;
		else
			break;
	}

	if (count <= 0)
		return -EINVAL;
	return count;
}
#endif

long jent_ncpu(void)
{
#if defined(JENT_ARCH_NCPU_WINDOWS)
	return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#elif defined(JENT_ARCH_NCPU_POSIX)
# ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
	{
		size_t cpu_set_alloc_size = CPU_ALLOC_SIZE(CPU_SETSIZE);
		cpu_set_t *cpu_set = CPU_ALLOC(CPU_SETSIZE);
		long count = 0;

		/* only get affinity if allocation was successful */
		if (cpu_set &&
		    sched_getaffinity(0,
				      cpu_set_alloc_size,
				      cpu_set) == 0) {
			count = CPU_COUNT_S(cpu_set_alloc_size, cpu_set);
		}

		if (cpu_set) {
			CPU_FREE(cpu_set);
		}

		if (count > 0) {
			return count;
		}
		/* fall through to sysfs / sysconf */
	}
# endif
# ifdef JENT_ARCH_NCPU_LINUX_SYSFS
	{
		long count = jent_ncpu_sysfs();

		if (count > 0)
			return count;
		/* fall through to sysconf */
	}
# endif
	{
		long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

		if (ncpu == -1)
			return -errno;

		if (ncpu == 0)
			return -EFAULT;

		return ncpu;
	}

#elif defined(JENT_ARCH_NCPU_LINUX_KERNEL)
#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
#error "Linux kernel does not support internal timer"
#endif
	return 1;

#else
	/*
	 * TODO: return number of available CPUs -
	 * this code disables timer thread as only one CPU is "detected".
	 */
	return 1;
#endif
}
