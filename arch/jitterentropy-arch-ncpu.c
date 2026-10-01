/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Architecture / OS-specific online-CPU count.
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
 * The Windows SDK version that declares GetActiveProcessorCount(),
 * ALL_PROCESSOR_GROUPS, GetThreadGroupAffinity(),
 * GetLogicalProcessorInformationEx() and the GROUP_AFFINITY /
 * GROUP_RELATIONSHIP structs, and the feature-test macros that make glibc
 * declare O_CLOEXEC. Must be the first line: both have to precede every system
 * header, the <windows.h> included below among them.
 */
#include "jitterentropy-arch-compat.h"

/*
 * _GNU_SOURCE exposes the Linux CPU-affinity interfaces used below on glibc
 * (sched_getaffinity(), the CPU_* set macros). Only this translation unit and
 * jitterentropy-arch-thread.c want it, so it stays here rather than joining
 * the shared block above - and it is not in the public jitterentropy.h either,
 * so the installed header imposes no feature-test macro on consumers. Like
 * that block it must precede every system header, which it does: the header
 * above includes none.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

#include "jitterentropy.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL

#include <linux/cpumask.h>	/* num_online_cpus() */

#define JENT_ARCH_NCPU_LINUX_KERNEL

#elif defined(_KERNEL) && defined(__FreeBSD__)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/smp.h>		/* mp_ncpus */

#define JENT_ARCH_NCPU_FREEBSD_KERNEL

#else /* LINUX_KERNEL */

#include <errno.h>

#if defined(JENT_BAREMETAL)
/*
 * Nothing to ask. The generic answer below is one CPU, which is also the
 * honest one: an EFI application runs on the boot processor with the others
 * parked, and the internal timer that a second CPU would be wanted for is not
 * available here anyway.
 */
#elif defined(_MSC_VER) || defined(__MINGW32__)
# include <windows.h>
# include <stddef.h>	/* offsetof() */
# include <stdlib.h>	/* malloc(), free() */
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
#   include "jitterentropy-arch-cloexec.h"
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
 * CPU_SETSIZE. A fixed set would answer neither question there, which is why
 * both callers read the mask through here.
 */
static int jent_affinity_mask(long *count, long *highest)
{
	unsigned int ncpu_set;
	cpu_set_t fixed;

	/*
	 * A set of the default size first, and without allocating it: the
	 * counting thread asks this on every start (jent_notime_sample_timer()),
	 * and the growing sets below are for machines few run on.
	 */
	if (!sched_getaffinity(0, sizeof(fixed), &fixed)) {
		/*
		 * The bits of the type, not CPU_SETSIZE: musl declares 128
		 * there but a cpu_set_t of 1024 bits.
		 */
		size_t i = sizeof(fixed) * 8;

		*count = CPU_COUNT_S(sizeof(fixed), &fixed);
		*highest = -1;
		while (i-- > 0) {
			if (CPU_ISSET_S(i, sizeof(fixed), &fixed)) {
				*highest = (long)i;
				break;
			}
		}
		return 0;
	}
	if (errno != EINVAL)
		return -errno;

	for (ncpu_set = (unsigned int)sizeof(fixed) * 8 * 2;
	     ncpu_set <= JENT_NCPU_SET_MAX;
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

#ifdef JENT_ARCH_NCPU_WINDOWS
/*
 * The flat Windows CPU numbering - of jent_cpu_highest(),
 * jent_thread_pin_to_cpu() and jent_entropy_set_notime_cpu() - is the active
 * processors of all groups concatenated in group order. CPU n is the n-th set
 * bit of its group's ActiveProcessorMask, not bit n: a parked or disabled
 * processor leaves a gap.
 *
 * Fetch the group layout, a single RelationGroup record with the per-group
 * entries as a trailing array, and validate header and array before use. On
 * success the caller frees *@buffer; *@groups points into it.
 */
static int jent_ncpu_groups(BYTE **buffer, GROUP_RELATIONSHIP **groups)
{
	/* Bytes that must be readable before Relationship and Size are read. */
	const size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
				    Group);
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX rec;
	DWORD len = 0;
	size_t need;
	BYTE *buf;

	if (!GetLogicalProcessorInformationEx(RelationGroup, NULL, &len) &&
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return -EFAULT;

	buf = (BYTE *)malloc(len);
	if (!buf)
		return -ENOMEM;

	if (!GetLogicalProcessorInformationEx(
			RelationGroup,
			(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len))
		goto err;

	rec = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf;
	need = hdr + offsetof(GROUP_RELATIONSHIP, GroupInfo);
	if ((size_t)len < hdr || (size_t)len < rec->Size ||
	    rec->Relationship != RelationGroup || rec->Size < need)
		goto err;

	need += (size_t)rec->Group.ActiveGroupCount *
		sizeof(PROCESSOR_GROUP_INFO);
	if (rec->Size < need)
		goto err;

	*buffer = buf;
	*groups = &rec->Group;
	return 0;

err:
	free(buf);
	return -EFAULT;
}

static unsigned int jent_ncpu_popcount(KAFFINITY mask)
{
	unsigned int n = 0;

	for (; mask; mask &= mask - 1)
		n++;
	return n;
}

int jent_cpu_to_group(unsigned long cpu, unsigned short *group,
		      unsigned int *bit)
{
	GROUP_RELATIONSHIP *groups;
	BYTE *buffer;
	WORD g;
	int ret = jent_ncpu_groups(&buffer, &groups);

	if (ret)
		return ret;

	ret = -EINVAL;
	for (g = 0; g < groups->ActiveGroupCount; g++) {
		const PROCESSOR_GROUP_INFO *gi = &groups->GroupInfo[g];
		unsigned long seen = 0;
		unsigned int b;

		if (cpu >= (unsigned long)gi->ActiveProcessorCount) {
			cpu -= gi->ActiveProcessorCount;
			continue;
		}

		/* The cpu-th set bit of this group's active mask. */
		for (b = 0; b < (unsigned int)(sizeof(KAFFINITY) * 8); b++) {
			if (!((gi->ActiveProcessorMask >> b) & (KAFFINITY)1))
				continue;
			if (seen++ != cpu)
				continue;

			*group = g;
			*bit = b;
			ret = 0;
			break;
		}
		break;
	}

	free(buffer);
	return ret;
}

/*
 * The Windows counterpart of jent_affinity_mask(): from the calling thread's
 * affinity, which the process mask and any job limit bound, report @count
 * CPUs and @highest the flat number of the largest (-1 for an empty mask).
 * Returns 0 or a negative errno.
 *
 * A thread's affinity covers its own processor group only, so beyond 64
 * logical CPUs this reports that group - which is also where the counting
 * thread can be pinned.
 */
static int jent_ncpu_thread_affinity(long *count, long *highest)
{
	const PROCESSOR_GROUP_INFO *gi;
	GROUP_RELATIONSHIP *groups;
	GROUP_AFFINITY ga;
	KAFFINITY mask;
	unsigned long base = 0;
	unsigned int bit;
	BYTE *buffer;
	WORD g;
	int ret;

	if (!GetThreadGroupAffinity(GetCurrentThread(), &ga))
		return -EFAULT;

	ret = jent_ncpu_groups(&buffer, &groups);
	if (ret)
		return ret;

	if (ga.Group >= groups->ActiveGroupCount) {
		free(buffer);
		return -EFAULT;
	}

	/* The flat number of the first CPU of the thread's group. */
	for (g = 0; g < ga.Group; g++)
		base += groups->GroupInfo[g].ActiveProcessorCount;

	gi = &groups->GroupInfo[ga.Group];
	mask = ga.Mask & gi->ActiveProcessorMask;

	*count = (long)jent_ncpu_popcount(mask);
	*highest = -1;
	for (bit = (unsigned int)(sizeof(KAFFINITY) * 8); bit-- > 0; ) {
		KAFFINITY below;

		if (!((mask >> bit) & (KAFFINITY)1))
			continue;

		/* Its position among the active processors of the group. */
		below = bit ? (gi->ActiveProcessorMask &
			       (((KAFFINITY)1 << bit) - 1)) : 0;
		*highest = (long)(base + jent_ncpu_popcount(below));
		break;
	}

	free(buffer);
	return 0;
}
#endif /* JENT_ARCH_NCPU_WINDOWS */

#ifdef JENT_ARCH_NCPU_LINUX_SYSFS
/*
 * Parse /sys/devices/system/cpu/online and return the number of online
 * logical CPUs, or a negative errno on failure. The file holds a comma-
 * separated list of CPU index ranges, e.g. "0-3" on a 4-CPU system or
 * "0,2-5,8" when CPUs have been offlined. The kernel always exposes
 * this file when sysfs is mounted, including on UP systems (where it
 * reads "0").
 */
/*
 * Count the CPUs named by an "online" list, e.g. "0-3" or "0,2-5,8".
 * Returns the count, or a negative errno when the list does not parse.
 *
 * Split from the reading of the file so the lists it has to handle - and the
 * malformed ones it must reject - can be passed in directly; a given machine
 * presents exactly one of them.
 */
static long jent_ncpu_parse_online(const char *p)
{
	long count = 0;

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
		/*
		 * Bounded before the addition: an "online" list naming a range
		 * up to LONG_MAX parses without setting errno, and adding its
		 * width to the running count would overflow a signed long,
		 * which is undefined. No kernel presents such a list, and no
		 * CPU above the ceiling the affinity paths grow to could be
		 * pinned to anyway.
		 */
		if (end >= (long)JENT_NCPU_SET_MAX)
			return -EINVAL;
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

/* @path is a parameter for the same reason as in jent_ncpu_parse_online(). */
static long jent_ncpu_sysfs_file(const char *path)
{
	char buf[256];
	int fd;
	ssize_t rlen;

	fd = open(path, O_RDONLY | JENT_O_CLOEXEC);
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

	return jent_ncpu_parse_online(buf);
}

static long jent_ncpu_sysfs(void)
{
	return jent_ncpu_sysfs_file("/sys/devices/system/cpu/online");
}
#endif

long jent_ncpu(void)
{
#if defined(JENT_ARCH_NCPU_WINDOWS)
	{
		long count = 0, highest = -1;

		if (!jent_ncpu_thread_affinity(&count, &highest) && count > 0)
			return count;
		/* fall through to the processors of the machine */
	}
	{
		/* No affinity to read: the machine's count. Zero is failure. */
		DWORD ncpu = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);

		return ncpu ? (long)ncpu : -EFAULT;
	}
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
		long ncpu;

		/*
		 * sysconf() returns -1 without touching errno for a name it
		 * does not know, which is how an indeterminate value is
		 * reported. errno would then be 0 - a success to the caller -
		 * or whatever an earlier call left there.
		 */
		errno = 0;
		ncpu = sysconf(_SC_NPROCESSORS_ONLN);
		if (ncpu == -1)
			return errno ? -errno : -EFAULT;

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

#elif defined(JENT_ARCH_NCPU_FREEBSD_KERNEL)
	/*
	 * The CPUs the kernel has started. The builtin timer thread has no
	 * backend here (see jitterentropy-arch-thread.c) and is refused
	 * regardless of this count (jent_notime_init_flags() in
	 * jitterentropy-timer.c), and a handler registered through
	 * jent_entropy_switch_notime_impl() brings its own CPU rule, so the
	 * count only feeds the cpuCores field of jent_status().
	 */
	return (mp_ncpus > 0) ? (long)mp_ncpus : 1;

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
#elif defined(JENT_ARCH_NCPU_WINDOWS)
	/* The same question of the thread's group affinity. */
	{
		long count = 0, highest = -1;

		if (!jent_ncpu_thread_affinity(&count, &highest) &&
		    highest >= 0)
			return highest;
		/* fall through to the count below */
	}
#endif

	/*
	 * Everywhere else the count is all there is, and the CPU numbers are
	 * taken to be the dense range it describes, as is the flat Windows
	 * numbering - unavoidable without an affinity API.
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
