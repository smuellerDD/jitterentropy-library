/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2026
 *
 * License
 * =======
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
 * OS-specific online CPU count discovery.
 *
 * Provides jent_ncpu() returning the number of online logical CPUs, or
 * a negative errno on failure. The dispatch is:
 *   - Windows                        -> GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
 *   - Linux (glibc)                  -> sched_getaffinity(2), with
 *                                       sysconf(_SC_NPROCESSORS_ONLN) as fallback
 *   - Linux (musl / non-glibc)       -> sched_getaffinity(2), with
 *                                       /sys/devices/system/cpu/online and
 *                                       sysconf(_SC_NPROCESSORS_ONLN) as
 *                                       fallbacks
 *   - hosted Unix-like (BSDs, Apple, -> sysconf(_SC_NPROCESSORS_ONLN)
 *     AIX, Solaris/illumos, Haiku,
 *     Cygwin)
 *   - Linux Kernel                   -> 1 (we do not need a timer thread)
 *   - other (e.g. baremetal)         -> 1 (timer thread will be disabled)
 *
 * The Unix-like detection uses macros that are pre-defined by the compiler
 * (not feature-test macros), so it does not depend on header include order
 * or libc-specific side effects.
 *
 * On Linux, the sched_getaffinity(2) syscall is preferred over sysconf()
 * because the count it reports also reflects taskset/cgroup cpuset
 * restrictions imposed on the calling process.
 *
 * On non-glibc Linux libcs (musl, bionic, ...) the syscall fallback path
 * additionally parses /sys/devices/system/cpu/online directly. Their
 * implementations of _SC_NPROCESSORS_ONLN consult /proc/stat or similar
 * sources that may be missing in minimal containers, chroots, or early
 * boot environments. glibc routes _SC_NPROCESSORS_ONLN through
 * sched_getaffinity itself, so the extra sysfs step is unnecessary there.
 * Detection: glibc (and uClibc in glibc-compat mode) defines __GLIBC__ via
 * <features.h>, which <errno.h> already pulls in; musl deliberately does
 * not, so the negative test below is the conventional way to spot it.
 */

#ifndef _JITTERENTROPY_ARCH_NCPU_H
#define _JITTERENTROPY_ARCH_NCPU_H

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
static inline long jent_ncpu_sysfs(void)
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

static inline long jent_ncpu(void)
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

#endif /* _JITTERENTROPY_ARCH_NCPU_H */
