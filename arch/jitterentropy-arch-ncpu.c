/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific online-CPU count.
 *
 * Definition of jent_ncpu() (declared in arch/jitterentropy-arch-ncpu.h). See
 * that header for the dispatch rationale.
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
 * _GNU_SOURCE exposes the Linux CPU-affinity interfaces used below on glibc
 * (sched_getaffinity(), the CPU_* set macros). It is defined here, in the
 * translation unit that needs it, rather than in the public jitterentropy.h so
 * the installed header does not impose a feature-test macro on consumers; it
 * must precede every system header.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

/*
 * GetActiveProcessorCount() and ALL_PROCESSOR_GROUPS are declared by the
 * Windows SDK only when the translation unit asks for Windows 7 or newer.
 * mingw-w64 has defaulted to older values across its releases, so the minimum
 * is stated here rather than left to the toolchain; like _GNU_SOURCE above it
 * must precede every system header, including the <windows.h> included below.
 * An externally supplied, higher value is left alone.
 */
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

#include <linux/cpumask.h>	/* num_online_cpus() */

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

#ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
/*
 * Read the affinity mask of the calling thread and report both questions asked
 * of it: @count how many CPUs it holds, @highest the largest number among them
 * (-1 for an empty mask). Returns 0 or a negative errno.
 *
 * The set grows on retry: sched_getaffinity(2) fails with EINVAL when it is
 * smaller than the CPU mask of the kernel, as on a machine with more CPUs than
 * CPU_SETSIZE. A fixed set would not merely lose the highest CPU there, it
 * would answer neither question - which is why both callers read the mask
 * through here rather than each with a set of its own.
 */
static int jent_affinity_mask(long *count, long *highest)
{
	unsigned int ncpu_set;

	for (ncpu_set = CPU_SETSIZE; ncpu_set <= JENT_NCPU_SET_MAX;
	     ncpu_set *= 2) {
		size_t size = CPU_ALLOC_SIZE(ncpu_set);
		cpu_set_t *set = CPU_ALLOC(ncpu_set);
		int ret;

		if (!set)
			return -ENOMEM;

		ret = sched_getaffinity(0, size, set) ? errno : 0;
		if (!ret) {
			unsigned int i = ncpu_set;

			*count = CPU_COUNT_S(size, set);
			*highest = -1;
			while (i-- > 0) {
				if (CPU_ISSET_S(i, size, set)) {
					*highest = (long)i;
					break;
				}
			}
		}

		CPU_FREE(set);

		if (ret == EINVAL)
			continue;	/* set too small - try a larger one */
		if (ret)
			return -ret;

		return 0;
	}

	return -EINVAL;
}
#endif /* JENT_ARCH_NCPU_LINUX_AFFINITY */

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
		long count = 0, highest = -1;

		if (!jent_affinity_mask(&count, &highest) && count > 0)
			return count;
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
	/*
	 * Only consumed by the jent_status() JSON output in kernel builds: the
	 * sole other user, the timer-less noise-source thread setup, is
	 * compiled out (see the #error above), so reporting the real count
	 * cannot enable that path.
	 */
	return (long)num_online_cpus();

#else
	/*
	 * TODO: return number of available CPUs -
	 * this code disables timer thread as only one CPU is "detected".
	 */
	return 1;
#endif
}

long jent_cpu_highest(void)
{
#ifdef JENT_ARCH_NCPU_LINUX_AFFINITY
	/*
	 * The mask is a set, not a range - counting its members and naming the
	 * last of them are different questions, and only the latter yields a
	 * CPU a thread can be pinned to.
	 */
	{
		long count = 0, highest = -1;

		if (!jent_affinity_mask(&count, &highest) && highest >= 0)
			return highest;
		/* fall through to the count below */
	}
#endif

	/*
	 * Everywhere else the count is all there is, and the CPU numbers are
	 * taken to be the dense range it describes - true for the flat Windows
	 * numbering, which jent_thread_pin_to_cpu() resolves in the same order,
	 * and unavoidable without an affinity API.
	 */
	{
		long ncpu = jent_ncpu();

		if (ncpu < 0)
			return ncpu;
		if (ncpu == 0)
			return -EFAULT;

		return ncpu - 1;
	}
}
