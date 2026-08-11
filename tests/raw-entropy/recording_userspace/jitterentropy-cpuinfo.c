/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Test tool listing the identification and cache layout of every CPU.
 *
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
 *
 * On hybrid CPUs (Intel P/E cores, ARM big.LITTLE) the timing of both noise
 * sources depends on the micro-architecture and the caches of the core the
 * Jitter RNG runs on, so a recording only characterizes that core type. This
 * tool names the CPUs and their caches, and the measurement is then pinned to
 * one of them with "jitterentropy-hashtime --cpu <CPU>".
 *
 * Coverage per system:
 *
 *   Linux    Complete, from sysfs plus CPUID/MIDR read on each core.
 *   Windows  Complete, from GetLogicalProcessorInformationEx() and the registry.
 *   macOS    Complete, but per performance level (hw.perflevel<N>.*) rather
 *            than per CPU - which is how Apple Silicon exposes P and E cores.
 *   Others   Only the CPU this tool runs on, its caches only on x86: nothing
 *            there enumerates the caches of the other CPUs.
 *
 * Usage: jitterentropy-cpuinfo [--summary] [--json]
 */

/*
 * Both of these must precede every system header, so they are stated here
 * rather than next to the backend that needs them: _GNU_SOURCE exposes the
 * CPU-affinity interfaces of glibc, and _WIN32_WINNT the Windows 7 APIs a
 * toolchain targeting an older Windows hides - the same guard the library's
 * own Win32 backends carry. An externally supplied, higher value is left
 * alone.
 */
#ifdef __linux__
# define _GNU_SOURCE
#endif
#if (defined(_MSC_VER) || defined(__MINGW32__)) && !defined(_WIN32_WINNT)
# define _WIN32_WINNT 0x0601
#endif

/* Backend selection */
#if defined(_WIN32) || defined(_WIN64)
# define JENT_CPUINFO_WINDOWS
#elif defined(__linux__)
# define JENT_CPUINFO_LINUX
#elif defined(__APPLE__)
# define JENT_CPUINFO_MACOS
#elif defined(__unix__) || defined(__unix) || defined(__HAIKU__) || \
      defined(_AIX)
# define JENT_CPUINFO_GENERIC
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__x86_64__) || defined(__i386__) || \
    defined(_M_X64)     || defined(_M_IX86)
# define JENT_CPUINFO_X86
#elif defined(__aarch64__) || defined(_M_ARM64)
# define JENT_CPUINFO_ARM64
#endif

#ifdef JENT_CPUINFO_X86
# ifdef _MSC_VER
#  include <intrin.h>
# else
#  include <cpuid.h>
# endif
#endif

#define JENT_MAX_CPUS		1024
#define JENT_IDENT_LEN		160
#define JENT_TYPE_LEN		16

struct jent_cache_info {
	unsigned long size;	/* cache size in bytes, 0 if unknown */
	unsigned long shared;	/* number of CPUs sharing this cache */
};

struct jent_cpu_info {
	unsigned long cpu;
	int cpu_valid;		/* is the CPU number known? */
	long pkg;		/* physical package ID, -1 if unknown */
	long core;		/* core ID, -1 if unknown */
	unsigned long max_khz;	/* maximum CPU frequency, 0 if unknown */
	unsigned long base_khz;	/* nominal base frequency, 0 if unknown */
	unsigned long tsc_khz;	/* nominal timestamp counter rate, 0 if unknown */
	/* Timestamp counter properties, each 1 yes, 0 no, -1 not reported */
	int tsc_invariant;	/* constant rate across P-states (constant_tsc) */
	int tsc_nonstop;	/* keeps ticking in deep C-states (nonstop_tsc) */
	int tsc_known_freq;	/* rate is enumerated, not calibrated */
	struct jent_cache_info l1d, l1i, l2, l3;
	/* Vendor and model as one string, empty if the CPU is not identified */
	char ident[JENT_IDENT_LEN];
	/*
	 * Core type as reported by the system ("P-core"/"E-core") or the
	 * relative compute capacity known to the scheduler ("cap <N>").
	 */
	char type[JENT_TYPE_LEN];
};

struct jent_cpu_list {
	struct jent_cpu_info cpu[JENT_MAX_CPUS];
	long entries;		/* number of CPUs described below */
	long ncpu;		/* number of CPUs in the system */
	int pinning;		/* does the system offer CPU pinning? */
	/* Source of the data, and what a JSON consumer branches on. */
	const char *backend;
	/* The same in prose, printed as a note below the table; NULL if none. */
	const char *note;
	/*
	 * How a recording is confined to one core type without CPU pinning,
	 * NULL where there is no way to. Printed in place of the --cpu line.
	 */
	const char *select;
};

/*
 * Set the model of @info to "<vendor> <model>", padding removed: CPUID pads
 * the AMD brand string with blanks that /proc/cpuinfo and the Windows registry
 * do not, and left in, the two spellings list one machine as two models.
 */
static void jent_set_ident(struct jent_cpu_info *info, const char *vendor,
			   const char *model)
{
	size_t len;

	if (!vendor)
		vendor = "";
	if (!model)
		model = "";

	while (*vendor == ' ' || *vendor == '\t')
		vendor++;
	while (*model == ' ' || *model == '\t')
		model++;

	snprintf(info->ident, sizeof(info->ident), "%s%s%s", vendor,
		 (*vendor && *model) ? " " : "", model);

	len = strlen(info->ident);
	while (len && (info->ident[len - 1] == ' ' ||
		       info->ident[len - 1] == '\t'))
		info->ident[--len] = '\0';
}

/* Set the fields whose "unknown" value is not zero. */
static void jent_cpu_info_init(struct jent_cpu_info *info)
{
	info->pkg = -1;
	info->core = -1;
	info->tsc_invariant = -1;
	info->tsc_nonstop = -1;
	info->tsc_known_freq = -1;
}

/* Defined by the backend for the operating system in use. */
static int jent_get_cpus(struct jent_cpu_list *list);

/***************************************************************************
 * x86 identification via CPUID
 *
 * - leaf 0 holds the vendor string in EBX, EDX, ECX,
 * - leaves 0x80000002 - 0x80000004 hold the processor brand string,
 * - leaf 0x1A (Intel SDM Vol. 2A, "Hybrid Information") reports the core type
 *   in EAX[31:24]: 0x20 Atom (efficiency), 0x40 Core (performance), zero on a
 *   non-hybrid CPU. Those two are all it knows - the low-power E-cores are
 *   Atoms as well, so jent_mark_lp_cores() tells them apart afterwards.
 *
 * CPUID describes the core executing it, so the caller has to pin itself to
 * the CPU it wants identified first.
 ***************************************************************************/

#if defined(JENT_CPUINFO_X86) && !defined(JENT_CPUINFO_MACOS)

/*
 * Execute CPUID for @leaf/@subleaf into @regs (EAX, EBX, ECX, EDX). Returns
 * zero when the leaf is beyond what the CPU implements - the check the GCC and
 * clang helper does, spelled out for the MSVC intrinsics, which do not.
 */
static int jent_cpuid(unsigned int leaf, unsigned int subleaf,
		      unsigned int regs[4]);

/* Issue CPUID without asking whether the leaf is implemented. */
static void jent_cpuid_raw(unsigned int leaf, unsigned int subleaf,
			   unsigned int regs[4])
{
#ifdef _MSC_VER
	int out[4];

	__cpuidex(out, (int)leaf, (int)subleaf);
	regs[0] = (unsigned int)out[0];
	regs[1] = (unsigned int)out[1];
	regs[2] = (unsigned int)out[2];
	regs[3] = (unsigned int)out[3];
#else
	__cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
}

/*
 * The leaves at 0x40000000 and up fall under neither the standard nor the
 * extended maximum, so they need two checks of their own: CPUID.1 ECX[31],
 * which only a hypervisor sets, and the leaf range its own 0x40000000 EAX
 * states. Unchecked, an unimplemented leaf answers with the highest standard
 * one instead of zeroes - plausible-looking nonsense.
 */
static int jent_cpuid_hypervisor(unsigned int leaf, unsigned int subleaf,
				 unsigned int regs[4])
{
	unsigned int r[4];

	if (!jent_cpuid(1, 0, r) || !(r[2] & (1U << 31)))
		return 0;

	jent_cpuid_raw(0x40000000, 0, r);
	if (leaf > r[0])
		return 0;

	jent_cpuid_raw(leaf, subleaf, regs);

	return 1;
}

static int jent_cpuid(unsigned int leaf, unsigned int subleaf,
		      unsigned int regs[4])
{
	unsigned int max;

	if ((leaf & 0xFF000000U) == 0x40000000U)
		return jent_cpuid_hypervisor(leaf, subleaf, regs);

	/* The standard and the extended leaves have a maximum of their own. */
#ifdef _MSC_VER
	{
		int out[4];

		__cpuid(out, (int)(leaf & 0x80000000U));
		max = (unsigned int)out[0];
	}
#else
	/*
	 * Not __get_cpuid_count(): it appeared in GCC 4.9, while the
	 * distributions this is built on still carry 4.8. The two calls it is
	 * made of have been there all along.
	 */
	max = __get_cpuid_max(leaf & 0x80000000U, NULL);
#endif

	if (leaf > max)
		return 0;

	jent_cpuid_raw(leaf, subleaf, regs);

	return 1;
}

/* Windows names its CPUs from the registry and needs none of this. */
#if defined(JENT_CPUINFO_LINUX) || defined(JENT_CPUINFO_GENERIC)

static void jent_ident_x86(struct jent_cpu_info *info)
{
	unsigned int r[4];
	char vendor[13] = { 0 }, brand[49] = { 0 };
	const char *model = brand;

	if (!jent_cpuid(0, 0, r))
		return;
	memcpy(vendor,     &r[1], 4);
	memcpy(vendor + 4, &r[3], 4);
	memcpy(vendor + 8, &r[2], 4);

	{
		unsigned int regs[12], i;

		for (i = 0; i < 3; i++) {
			if (!jent_cpuid(0x80000002 + i, 0, &regs[i * 4]))
				break;
		}
		if (i == 3) {
			memcpy(brand, regs, sizeof(regs));
			/* The brand string is padded with leading blanks. */
			while (*model == ' ')
				model++;
		}
	}

	jent_set_ident(info, vendor, model);

	if (jent_cpuid(0x1A, 0, r) && r[0]) {
		switch (r[0] >> 24) {
		case 0x20:
			snprintf(info->type, sizeof(info->type), "E-core");
			break;
		case 0x40:
			snprintf(info->type, sizeof(info->type), "P-core");
			break;
		default:
			snprintf(info->type, sizeof(info->type), "0x%02x",
				 r[0] >> 24);
			break;
		}
	}
}

#endif /* JENT_CPUINFO_LINUX || JENT_CPUINFO_GENERIC */

/*
 * The nominal frequency some brand strings end in ("... CPU @ 2.40GHz"), in
 * kHz, or 0 when there is none. Only a trailing "@ <number><unit>Hz" is taken,
 * so that a model number containing a digit cannot pass for a frequency.
 */
static unsigned long jent_brand_khz(const char *brand)
{
	const char *at = strrchr(brand, '@');
	unsigned long value = 0, scale = 0, frac_digits = 0;
	int seen_digit = 0, in_frac = 0;

	if (!at)
		return 0;

	for (at++; *at == ' '; at++)
		;

	for (; *at; at++) {
		if (*at >= '0' && *at <= '9') {
			value = value * 10 + (unsigned long)(*at - '0');
			if (in_frac)
				frac_digits++;
			seen_digit = 1;
		} else if (*at == '.' && !in_frac) {
			in_frac = 1;
		} else {
			break;
		}
	}

	if (!seen_digit)
		return 0;

	if (*at == 'G')
		scale = 1000000;	/* GHz -> kHz */
	else if (*at == 'M')
		scale = 1000;		/* MHz -> kHz */
	else if (*at == 'k' || *at == 'K')
		scale = 1;
	else
		return 0;

	if (strncmp(at + 1, "Hz", 2))
		return 0;

	/* Undo the decimal point: "2.40GHz" parsed 240 with two digits. */
	for (; frac_digits; frac_digits--) {
		if (scale < 10)
			return 0;
		scale /= 10;
	}

	return value * scale;
}

/*
 * Frequencies and timestamp counter of the core this runs on.
 *
 * Leaf 0x16 (Intel SDM Vol. 2A, "Processor Frequency Information") gives the
 * base in EAX[15:0] and the maximum in EBX[15:0], in MHz. Only EBX follows the
 * core executing it; EAX stays package-wide even where the P and E cores differ
 * - so Linux and Windows prefer their per-CPU base frequency and reach this
 * leaf only where that is unavailable.
 *
 * Leaf 0x15 gives the TSC rate as ECX (crystal clock in Hz) * EBX / EAX. That
 * counter is what the Jitter RNG times with, so its rate bounds the resolution
 * of every measurement. 0x80000007 EDX[8] marks it invariant - constant across
 * P-states, not stopping in deep C-states - which is what makes it usable.
 *
 * AMD implements neither 0x15 nor 0x16, so there the values stay unknown and
 * the rate comes from cpufreq or the registry instead.
 */
static void jent_freq_x86(struct jent_cpu_info *info)
{
	unsigned int r[4];

	if (jent_cpuid(0x16, 0, r)) {
		if (!info->base_khz && (r[0] & 0xFFFF))
			info->base_khz = (unsigned long)(r[0] & 0xFFFF) * 1000;
		if (!info->max_khz && (r[1] & 0xFFFF))
			info->max_khz = (unsigned long)(r[1] & 0xFFFF) * 1000;
	}

	/*
	 * Last resort: the frequency the brand string ends in is the base one.
	 * Without 0x16 and without cpufreq, it is all there is.
	 */
	if (!info->base_khz)
		info->base_khz = jent_brand_khz(info->ident);

	if (jent_cpuid(0x15, 0, r) && r[0] && r[1] && r[2])
		info->tsc_khz = (unsigned long)((uint64_t)r[2] * r[1] /
						r[0] / 1000);

	/*
	 * Leaf 0x40000010, in kHz: the timing leaf VMware defined and others
	 * adopted. On an AMD host it is one of the few places a guest can learn
	 * the rate at all. Not every hypervisor has it, and the leaf-range check
	 * in jent_cpuid() then leaves the rate unknown rather than guessing.
	 */
	if (!info->tsc_khz && jent_cpuid(0x40000010, 0, r) && r[0])
		info->tsc_khz = r[0];

	/*
	 * An enumerated rate is a known one; without it the operating system
	 * calibrates against another timer and arrives at its own number.
	 */
	info->tsc_known_freq = info->tsc_khz ? 1 : 0;

	if (jent_cpuid(0x80000007, 0, r)) {
		/*
		 * The one invariant-TSC bit covers both properties. Linux
		 * splits them into constant_tsc and nonstop_tsc, which it also
		 * sets from model checks on the parts predating the bit - hence
		 * the Linux backend overriding these afterwards.
		 */
		info->tsc_invariant = (r[3] & (1U << 8)) ? 1 : 0;
		info->tsc_nonstop = info->tsc_invariant;
	}
}

#endif /* JENT_CPUINFO_X86 && !JENT_CPUINFO_MACOS */

/*
 * The data cache geometry from CPUID, needed only where the operating system
 * has no interface of its own for it.
 *
 * Intel leaf 4 and the identically laid out AMD leaf 0x8000001D enumerate one
 * cache per sub-leaf (Intel SDM Vol. 2A, "Deterministic Cache Parameters";
 * AMD APM Vol. 3, "Cache Topology Information"):
 *
 *   EAX[ 4: 0]  cache type (0 = none/end, 1 = data, 2 = instruction, 3 = unified)
 *   EAX[ 7: 5]  cache level
 *   EBX[11: 0]  system coherency line size - 1
 *   EBX[21:12]  physical line partitions - 1
 *   EBX[31:22]  ways of associativity - 1
 *   ECX         number of sets - 1
 * Total size = (ways + 1) * (partitions + 1) * (line size + 1) * (sets + 1).
 *
 * The sharing count is deliberately not taken from EAX[25:14]: that field is
 * rounded up to a power of two and describes what the encoding allows, not the
 * topology - on a 12-CPU part it claims 64 CPUs share the L3. The column stays
 * empty rather than carrying a wrong number.
 */
#if defined(JENT_CPUINFO_X86) && defined(JENT_CPUINFO_GENERIC)

/* Walk the sub-leaves of @leaf; returns non-zero if any cache was found. */
static int jent_caches_x86_leaf(struct jent_cpu_info *info, unsigned int leaf)
{
	unsigned int sub;
	int found = 0;

	for (sub = 0; sub < 16; sub++) {
		unsigned int type, level, ways, partitions, line, sets;
		struct jent_cache_info *cache;
		unsigned int r[4], eax, ebx, ecx;

		if (!jent_cpuid(leaf, sub, r))
			break;
		eax = r[0];
		ebx = r[1];
		ecx = r[2];

		type = eax & 0x1F;
		if (type == 0)
			break;

		level      = (eax >> 5) & 0x7;
		ways       = ((ebx >> 22) & 0x3FF) + 1;
		partitions = ((ebx >> 12) & 0x3FF) + 1;
		line       = (ebx & 0xFFF) + 1;
		sets       = ecx + 1;

		if (level == 1 && type == 2)
			cache = &info->l1i;
		else if (level == 1 && (type == 1 || type == 3))
			cache = &info->l1d;
		else if (level == 2 && type != 2)
			cache = &info->l2;
		else if (level == 3 && type != 2)
			cache = &info->l3;
		else
			continue;

		cache->size = (unsigned long)ways * partitions * line * sets;
		found = 1;
	}

	return found;
}

static void jent_caches_x86(struct jent_cpu_info *info)
{
	/*
	 * Leaf 4 is Intel's; AMD and Hygon leave it empty (cache type 0) and
	 * carry the identical structure in the extended leaf. Both are probed
	 * rather than dispatching on the vendor ID, as the library itself does
	 * in arch/jitterentropy-arch-cache.c.
	 */
	if (jent_caches_x86_leaf(info, 0x00000004))
		return;

	jent_caches_x86_leaf(info, 0x8000001D);
}

#endif /* JENT_CPUINFO_X86 && JENT_CPUINFO_GENERIC */

/***************************************************************************
 * AArch64 timer
 *
 * Shared by the Linux and the macOS backend. The BSDs are left out
 * deliberately: nothing there is verified to enable the EL0 access this needs,
 * and a read that traps would take the tool down with SIGILL.
 ***************************************************************************/

#if defined(JENT_CPUINFO_ARM64) && \
    (defined(JENT_CPUINFO_LINUX) || defined(JENT_CPUINFO_MACOS))

/*
 * The rate of the architected generic timer: the Jitter RNG times with
 * CNTVCT_EL0 here, and CNTFRQ_EL0 states its frequency. The architecture
 * requires that counter to be constant, independent of the CPU clock and
 * always on (Arm ARM (DDI 0487)), so the three properties below follow from
 * the architecture rather than a feature bit. Rates are commonly tens of MHz -
 * far coarser than an x86 TSC, and that is what bounds the measurements.
 */
static void jent_timer_arm64(struct jent_cpu_info *info)
{
	uint64_t freq;

	__asm__ __volatile__("mrs %0, cntfrq_el0" : "=r" (freq));

	/* Firmware that leaves the register at zero has not set it up. */
	if (!freq)
		return;

	info->tsc_khz = (unsigned long)(freq / 1000);
	info->tsc_invariant = 1;
	info->tsc_nonstop = 1;
	info->tsc_known_freq = 1;
}

#endif /* JENT_CPUINFO_ARM64 && (JENT_CPUINFO_LINUX || JENT_CPUINFO_MACOS) */

/***************************************************************************
 * Linux backend
 *
 * sysfs describes the caches and the topology of every CPU without any
 * privileges. Only the identification has to be read on the CPU itself, which
 * the affinity API allows.
 ***************************************************************************/

#ifdef JENT_CPUINFO_LINUX

#include <sched.h>

#define JENT_SYSFS_CPU		"/sys/devices/system/cpu"

/* Number of cache levels exposed per CPU in sysfs that are looked at. */
#define JENT_MAX_CACHE_INDEX	10

static int read_file_str(const char *path, char *buf, size_t buflen)
{
	FILE *f = fopen(path, "r");
	size_t len;

	if (!f)
		return -errno;

	if (!fgets(buf, (int)buflen, f)) {
		fclose(f);
		return -EIO;
	}
	fclose(f);

	/* Strip the trailing newline sysfs adds to every attribute. */
	len = strlen(buf);
	while (len && (buf[len - 1] == '\n' || buf[len - 1] == ' '))
		buf[--len] = '\0';

	return len ? 0 : -ENODATA;
}

static int read_cpu_str(unsigned long cpu, const char *attr,
			char *buf, size_t buflen)
{
	char path[256];

	snprintf(path, sizeof(path), JENT_SYSFS_CPU "/cpu%lu/%s", cpu, attr);
	return read_file_str(path, buf, buflen);
}

/* Numeric sysfs attribute. Base 0, as midr_el1 is given as 0x... */
static int read_cpu_val(unsigned long cpu, const char *attr,
			unsigned long *val)
{
	char buf[64], *endptr;
	int ret = read_cpu_str(cpu, attr, buf, sizeof(buf));

	if (ret)
		return ret;

	errno = 0;
	*val = strtoul(buf, &endptr, 0);
	if (endptr == buf || errno != 0)
		return -EINVAL;

	return 0;
}

static int read_cpu_signed(unsigned long cpu, const char *attr, long *val)
{
	char buf[64], *endptr;
	int ret = read_cpu_str(cpu, attr, buf, sizeof(buf));

	if (ret)
		return ret;

	errno = 0;
	*val = strtol(buf, &endptr, 10);
	if (endptr == buf || errno != 0)
		return -EINVAL;

	return 0;
}

/*
 * Parse a kernel CPU list like "0-3,8" (/sys/.../online, shared_cpu_list) into
 * @cpus, at most @max of them. Returns the length of the list or a negative
 * errno - a count above @max reports the truncation to the caller.
 */
static long parse_cpu_list(const char *str, unsigned long *cpus, size_t max)
{
	const char *p = str;
	long count = 0;

	while (*p && *p != '\n') {
		char *endptr;
		unsigned long start, end, i;

		errno = 0;
		start = strtoul(p, &endptr, 10);
		if (endptr == p || errno != 0)
			return -EINVAL;
		p = endptr;

		if (*p == '-') {
			p++;
			errno = 0;
			end = strtoul(p, &endptr, 10);
			if (endptr == p || errno != 0 || end < start)
				return -EINVAL;
			p = endptr;
		} else {
			end = start;
		}

		for (i = start; i <= end; i++, count++) {
			if (cpus && (size_t)count < max)
				cpus[count] = i;
		}

		if (*p == ',')
			p++;
		else
			break;
	}

	return count ? count : -EINVAL;
}

/* Convert a sysfs cache size like "48K" or "32M" into bytes. */
static unsigned long parse_cache_size(const char *str)
{
	char *endptr;
	unsigned long val;

	errno = 0;
	val = strtoul(str, &endptr, 10);
	if (endptr == str || errno != 0)
		return 0;

	switch (*endptr) {
	case 'K':
		return val * 1024;
	case 'M':
		return val * 1024 * 1024;
	case 'G':
		return val * 1024 * 1024 * 1024;
	default:
		return val;
	}
}

static void jent_caches_linux(struct jent_cpu_info *info)
{
	unsigned int idx;

	for (idx = 0; idx < JENT_MAX_CACHE_INDEX; idx++) {
		char attr[64], type[32], size[32], list[512];
		struct jent_cache_info *cache;
		unsigned long level;
		long shared;

		snprintf(attr, sizeof(attr), "cache/index%u/level", idx);
		if (read_cpu_val(info->cpu, attr, &level))
			break;

		snprintf(attr, sizeof(attr), "cache/index%u/type", idx);
		if (read_cpu_str(info->cpu, attr, type, sizeof(type)))
			continue;

		/*
		 * The Jitter RNG only accesses data; the L1i is kept just to
		 * document the split. L2 and L3 are commonly unified.
		 */
		if (level == 1 && !strncmp(type, "Instruction", 11))
			cache = &info->l1i;
		else if (level == 1)
			cache = &info->l1d;
		else if (level == 2 && strncmp(type, "Instruction", 11))
			cache = &info->l2;
		else if (level == 3 && strncmp(type, "Instruction", 11))
			cache = &info->l3;
		else
			continue;

		snprintf(attr, sizeof(attr), "cache/index%u/size", idx);
		if (read_cpu_str(info->cpu, attr, size, sizeof(size)))
			continue;
		cache->size = parse_cache_size(size);

		/*
		 * The sharing count separates P-cores from E-core clusters: a
		 * P-core owns its L2, a group of E-cores shares one.
		 */
		snprintf(attr, sizeof(attr), "cache/index%u/shared_cpu_list",
			 idx);
		if (read_cpu_str(info->cpu, attr, list, sizeof(list)))
			continue;
		shared = parse_cpu_list(list, NULL, 0);
		if (shared > 0)
			cache->shared = (unsigned long)shared;
	}
}

#ifdef JENT_CPUINFO_ARM64

static const struct {
	unsigned long id;
	const char *name;
} arm_implementers[] = {
	{ 0x41, "ARM" },	{ 0x42, "Broadcom" },	{ 0x43, "Cavium" },
	{ 0x46, "Fujitsu" },	{ 0x48, "HiSilicon" },	{ 0x4e, "NVIDIA" },
	{ 0x50, "APM" },	{ 0x51, "Qualcomm" },	{ 0x53, "Samsung" },
	{ 0x56, "Marvell" },	{ 0x61, "Apple" },	{ 0x69, "Intel" },
	{ 0x6d, "Microsoft" },	{ 0x70, "Phytium" },	{ 0xc0, "Ampere" },
}, arm_parts[] = {
	/* Parts of the ARM implementer (0x41) */
	{ 0xd03, "Cortex-A53" },	{ 0xd04, "Cortex-A35" },
	{ 0xd05, "Cortex-A55" },	{ 0xd06, "Cortex-A65" },
	{ 0xd07, "Cortex-A57" },	{ 0xd08, "Cortex-A72" },
	{ 0xd09, "Cortex-A73" },	{ 0xd0a, "Cortex-A75" },
	{ 0xd0b, "Cortex-A76" },	{ 0xd0c, "Neoverse-N1" },
	{ 0xd0d, "Cortex-A77" },	{ 0xd40, "Neoverse-V1" },
	{ 0xd41, "Cortex-A78" },	{ 0xd44, "Cortex-X1" },
	{ 0xd46, "Cortex-A510" },	{ 0xd47, "Cortex-A710" },
	{ 0xd48, "Cortex-X2" },		{ 0xd49, "Neoverse-N2" },
	{ 0xd4a, "Neoverse-E1" },	{ 0xd4d, "Cortex-A715" },
	{ 0xd4e, "Cortex-X3" },		{ 0xd4f, "Neoverse-V2" },
	{ 0xd80, "Cortex-A520" },	{ 0xd81, "Cortex-A720" },
	{ 0xd82, "Cortex-X4" },		{ 0xd84, "Neoverse-V3" },
	{ 0xd85, "Cortex-X925" },	{ 0xd87, "Cortex-A725" },
	{ 0xd8e, "Neoverse-N3" },
};

static const char *arm_lookup(unsigned long id, int implementer)
{
	size_t i, entries = implementer ?
		sizeof(arm_implementers) / sizeof(arm_implementers[0]) :
		sizeof(arm_parts) / sizeof(arm_parts[0]);
	const char *name = NULL;

	for (i = 0; i < entries; i++) {
		if (implementer && arm_implementers[i].id == id) {
			name = arm_implementers[i].name;
			break;
		} else if (!implementer && arm_parts[i].id == id) {
			name = arm_parts[i].name;
			break;
		}
	}

	return name;
}

/* What has to be read on the CPU itself - see jent_ident_sysfs() below. */
static void jent_ident_local(struct jent_cpu_info *info)
{
	jent_timer_arm64(info);
}

/*
 * AArch64 identification from MIDR_EL1, which sysfs exposes per CPU: the
 * implementer in bits[31:24] and the part number in bits[15:4] (Arm ARM
 * (DDI 0487)). The part number is what tells big from LITTLE.
 */
static void jent_ident_sysfs(struct jent_cpu_info *info)
{
	unsigned long midr, impl, part, variant, revision, capacity;
	const char *impl_name, *part_name;
	char part_buf[16];

	if (read_cpu_val(info->cpu, "regs/identification/midr_el1", &midr))
		return;

	impl     = (midr >> 24) & 0xff;
	part     = (midr >>  4) & 0xfff;
	variant  = (midr >> 20) & 0xf;
	revision =  midr        & 0xf;

	impl_name = arm_lookup(impl, 1);
	/* The part number space is implementer-specific. */
	part_name = (impl == 0x41) ? arm_lookup(part, 0) : NULL;
	if (!part_name) {
		snprintf(part_buf, sizeof(part_buf), "part 0x%03lx", part);
		part_name = part_buf;
	}

	if (impl_name)
		snprintf(info->ident, sizeof(info->ident), "%s %s r%lup%lu",
			 impl_name, part_name, variant, revision);
	else
		snprintf(info->ident, sizeof(info->ident),
			 "implementer 0x%02lx %s r%lup%lu", impl, part_name,
			 variant, revision);

	/*
	 * ARM reports no core type; what stands in for it is the scheduler's
	 * capacity, normalized to 1024 for the most capable core. It separates
	 * big from LITTLE - on a uniform system every core reports 1024 and the
	 * value says only that they are equivalent.
	 */
	if (!read_cpu_val(info->cpu, "cpu_capacity", &capacity))
		snprintf(info->type, sizeof(info->type), "cap %lu", capacity);
}

#elif defined(JENT_CPUINFO_X86)

/* CPUID answers for the core executing it, so this has to run on that core. */
static void jent_ident_local(struct jent_cpu_info *info)
{
	jent_ident_x86(info);
	jent_freq_x86(info);
}

static void jent_ident_sysfs(struct jent_cpu_info *info)
{
	unsigned long perf;

	/*
	 * AMD reports no core type - its dense cores are the same
	 * micro-architecture, and CPUID has no hybrid leaf. Under amd-pstate
	 * the firmware's highest performance level ranks the cores instead.
	 * Read from sysfs, so it covers the CPUs this tool cannot visit too.
	 */
	if (!info->type[0] &&
	    !read_cpu_val(info->cpu, "cpufreq/amd_pstate_highest_perf", &perf))
		snprintf(info->type, sizeof(info->type), "perf %lu", perf);
}

#else /* neither x86 nor AArch64 */

/* Nothing is read from the CPU itself; /proc/cpuinfo names it below. */
static void jent_ident_local(struct jent_cpu_info *info)
{
	(void)info;
}

static void jent_ident_sysfs(struct jent_cpu_info *info)
{
	(void)info;
}

#endif /* JENT_CPUINFO_ARM64 */

/* The CPU the kernel numbers @cpu, or NULL if the listing has no such entry. */
static struct jent_cpu_info *jent_cpu_by_id(struct jent_cpu_list *list,
					    long cpu)
{
	long i;

	if (cpu < 0)
		return NULL;

	for (i = 0; i < list->entries; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		if (info->cpu_valid && info->cpu == (unsigned long)cpu)
			return info;
	}

	return NULL;
}

#ifdef JENT_CPUINFO_X86

/* Is @name one of the space separated words of @flags? */
static int has_flag(const char *flags, const char *name)
{
	size_t len = strlen(name);
	const char *p = flags;

	while ((p = strstr(p, name))) {
		if ((p == flags || p[-1] == ' ') &&
		    (p[len] == '\0' || p[len] == ' '))
			return 1;
		p += len;
	}

	return 0;
}

#endif /* JENT_CPUINFO_X86 */

/*
 * What the kernel publishes per CPU in /proc/cpuinfo: names for the CPUs still
 * unnamed, and on x86 its own view of the timestamp counter.
 *
 * The names cover the CPUs the tool could not visit, and are the only
 * identification where there is neither CPUID nor a MIDR - the key differs per
 * architecture (RISC-V "uarch", POWER "cpu", s390 "machine"). On x86 the vendor
 * is prepended so that a name from here and one from CPUID match.
 *
 * The flags line says more about the counter than CPUID does: Linux sets
 * constant_tsc and nonstop_tsc from model checks as well, and tsc_known_freq
 * states that the rate was enumerated rather than calibrated.
 *
 * The file holds a block per CPU and is walked once for both - reading it per
 * CPU would be quadratic on exactly the machines where that hurts.
 */
static void jent_cpuinfo_linux(struct jent_cpu_list *list)
{
	static const char *keys[] = { "model name", "uarch", "cpu model",
				      "cpu", "machine" };
	FILE *f = fopen("/proc/cpuinfo", "r");
	/* Sized for the flags line, which is by far the longest one here. */
	char line[4096], vendor[64] = "";
	long cur = -1;

	if (!f)
		return;

	while (fgets(line, sizeof(line), f)) {
		struct jent_cpu_info *info;
		char *val = strchr(line, ':');
		size_t k, keylen;

		if (!val)
			continue;
		*val++ = '\0';
		while (*val == ' ' || *val == '\t')
			val++;
		val[strcspn(val, "\n")] = '\0';

		/* Strip the padding the file uses between key and colon. */
		keylen = strlen(line);
		while (keylen && (line[keylen - 1] == ' ' ||
				  line[keylen - 1] == '\t'))
			line[--keylen] = '\0';

		/* Opens the block of a CPU; everything below belongs to it. */
		if (!strcmp(line, "processor")) {
			cur = strtol(val, NULL, 10);
			vendor[0] = '\0';
			continue;
		}

		info = jent_cpu_by_id(list, cur);
		if (!info)
			continue;

		/* Precedes the model name in the block. */
		if (!strcmp(line, "vendor_id")) {
			snprintf(vendor, sizeof(vendor), "%s", val);
			continue;
		}

#ifdef JENT_CPUINFO_X86
		if (!strcmp(line, "flags")) {
			info->tsc_invariant = has_flag(val, "constant_tsc");
			info->tsc_nonstop = has_flag(val, "nonstop_tsc");
			info->tsc_known_freq = has_flag(val, "tsc_known_freq");
			continue;
		}
#endif

		/* A CPU named on the core itself keeps that name. */
		if (info->ident[0])
			continue;

		for (k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
			if (!strcmp(line, keys[k])) {
				jent_set_ident(info, vendor, val);
				break;
			}
		}
	}

	fclose(f);
}

/*
 * Move the calling thread to @cpu: on a hybrid CPU the identification is only
 * meaningful when read on the core in question.
 */
static int pin_to_cpu(unsigned long cpu)
{
	cpu_set_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;

	CPU_ZERO(&set);
	CPU_SET((size_t)cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		return -errno;

	/*
	 * sched_setaffinity() migrates before it returns, but a CPU that went
	 * offline meanwhile leaves the thread elsewhere - and the
	 * identification would then describe the wrong core.
	 */
	if (sched_getcpu() != (int)cpu)
		return -EAGAIN;

	return 0;
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	static char unreachable_note[512];
	unsigned long cpu_ids[JENT_MAX_CPUS];
	char online[1024];
	long ncpu, i, unreachable = 0;
	cpu_set_t *previous;
	size_t setsize;
	int ret;

	ret = read_file_str(JENT_SYSFS_CPU "/online", online, sizeof(online));
	if (ret) {
		fprintf(stderr, "Cannot read " JENT_SYSFS_CPU "/online: %s\n",
			strerror(-ret));
		return ret;
	}

	ncpu = parse_cpu_list(online, cpu_ids, JENT_MAX_CPUS);
	if (ncpu < 0) {
		fprintf(stderr, "Cannot parse the list of online CPUs \"%s\"\n",
			online);
		return (int)ncpu;
	}

	list->ncpu = ncpu;
	list->pinning = 1;
	list->backend = "linux";

	/* Before cpu_ids is read below: only that many of them were stored. */
	if (ncpu > JENT_MAX_CPUS) {
		fprintf(stderr, "Only the first %d of %ld online CPUs are "
			"reported\n", JENT_MAX_CPUS, ncpu);
		ncpu = JENT_MAX_CPUS;
	}

	/*
	 * The affinity of this thread, restored once every CPU has been
	 * visited. Allocated for the highest CPU number, which on a large
	 * machine exceeds the CPU_SETSIZE a plain cpu_set_t covers.
	 */
	{
		unsigned int ncpu_set = (unsigned int)(cpu_ids[ncpu - 1] + 1);

		previous = CPU_ALLOC(ncpu_set);
		setsize = previous ? CPU_ALLOC_SIZE(ncpu_set) : 0;
		if (previous && sched_getaffinity(0, setsize, previous)) {
			CPU_FREE(previous);
			previous = NULL;
			setsize = 0;
		}
	}

	for (i = 0; i < ncpu; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		jent_cpu_info_init(info);
		info->cpu = cpu_ids[i];
		info->cpu_valid = 1;

		if (read_cpu_signed(info->cpu, "topology/physical_package_id",
				    &info->pkg))
			info->pkg = -1;
		if (read_cpu_signed(info->cpu, "topology/core_id", &info->core))
			info->core = -1;
		if (read_cpu_val(info->cpu, "cpufreq/cpuinfo_max_freq",
				 &info->max_khz))
			info->max_khz = 0;
		/* Exported by the intel-pstate driver, per CPU. */
		if (read_cpu_val(info->cpu, "cpufreq/base_frequency",
				 &info->base_khz))
			info->base_khz = 0;

		/*
		 * AMD exports no base_frequency and CPUID carries none, which
		 * leaves the CPPC nominal frequency - the same quantity by
		 * another name. Second, not first: it is package-wide, and on a
		 * hybrid Intel part it reports the P-core value for the E-cores
		 * as well.
		 */
		if (!info->base_khz) {
			unsigned long nominal;

			if (!read_cpu_val(info->cpu, "acpi_cppc/nominal_freq",
					  &nominal) && nominal)
				info->base_khz = nominal * 1000;
		}

		/* Likewise where amd-pstate is the driver but cpufreq is not. */
		if (!info->max_khz &&
		    read_cpu_val(info->cpu, "cpufreq/amd_pstate_max_freq",
				 &info->max_khz))
			info->max_khz = 0;

		jent_caches_linux(info);
		jent_ident_sysfs(info);

		/*
		 * The rest has to be read on the CPU it describes. Tried for
		 * every CPU, not just those of the current affinity mask: a
		 * mask narrowed with taskset can be widened again. A cpuset
		 * cgroup cannot, and those CPUs keep what sysfs reported alone.
		 */
		if (pin_to_cpu(info->cpu)) {
			unreachable++;
			continue;
		}

		jent_ident_local(info);
	}

	if (previous) {
		sched_setaffinity(0, setsize, previous);
		CPU_FREE(previous);
	}

	list->entries = ncpu;

	/* Names the CPUs the loop above could not visit, among others. */
	jent_cpuinfo_linux(list);

	if (unreachable) {
		/* One note, not a line per CPU - that would bury the listing. */
		snprintf(unreachable_note, sizeof(unreachable_note),
			 "%ld of the %ld CPUs could not be visited: this "
			 "process is confined to a cpuset\nthat does not "
			 "include them - a container or a cgroup - so what only "
			 "the CPU\nitself reports is missing for them, its "
			 "core type on Intel among it. Run outside\nthat "
			 "confinement to describe every CPU.",
			 unreachable, ncpu);
		list->note = unreachable_note;
	}

	return 0;
}

#endif /* JENT_CPUINFO_LINUX */

/***************************************************************************
 * macOS backend
 *
 * macOS offers no thread-to-CPU pinning and no per-CPU description, but it
 * groups the cores into performance levels - hw.perflevel0 the P cores,
 * hw.perflevel1 the E cores - and reports the caches per level, which is
 * exactly the distinction this tool exists for. Systems with a single core
 * type report no levels and are described by the flat hw.* names.
 ***************************************************************************/

#ifdef JENT_CPUINFO_MACOS

#include <sys/types.h>
#include <sys/sysctl.h>

static int sysctl_str(const char *name, char *buf, size_t buflen)
{
	size_t len = buflen;

	if (!buflen)
		return -EINVAL;

	/*
	 * Terminated before the call as well: a name the kernel does not know
	 * leaves the buffer untouched, and the callers read it regardless of
	 * what this returns.
	 */
	buf[0] = '\0';
	if (sysctlbyname(name, buf, &len, NULL, 0))
		return -errno;
	buf[buflen - 1] = '\0';

	return 0;
}

/* Numeric sysctl of either width - macOS reports both, so ask the node. */
static int sysctl_num(const char *name, unsigned long long *val)
{
	size_t len = 0;

	if (sysctlbyname(name, NULL, &len, NULL, 0))
		return -errno;

	if (len == sizeof(uint32_t)) {
		uint32_t v = 0;

		len = sizeof(v);
		if (sysctlbyname(name, &v, &len, NULL, 0))
			return -errno;
		*val = v;
		return 0;
	}
	if (len == sizeof(uint64_t)) {
		uint64_t v = 0;

		len = sizeof(v);
		if (sysctlbyname(name, &v, &len, NULL, 0))
			return -errno;
		*val = v;
		return 0;
	}

	return -EINVAL;
}

/* Read hw.perflevel<level>.<attr>, or hw.<attr> when there are no levels. */
static int sysctl_level_num(int level, const char *attr,
			    unsigned long long *val)
{
	char name[64];

	if (level < 0)
		snprintf(name, sizeof(name), "hw.%s", attr);
	else
		snprintf(name, sizeof(name), "hw.perflevel%d.%s", level, attr);

	return sysctl_num(name, val);
}

/*
 * The counter the Jitter RNG times with: the generic timer on Apple Silicon,
 * the TSC on the Intel Macs. The kernel publishes the TSC rate but not how it
 * arrived at it, so the counter properties stay unknown rather than assumed.
 */
static void jent_timer_macos(struct jent_cpu_info *info)
{
#ifdef JENT_CPUINFO_ARM64
	jent_timer_arm64(info);
#elif defined(JENT_CPUINFO_X86)
	unsigned long long freq = 0;

	if (!sysctl_num("machdep.tsc.frequency", &freq) && freq)
		info->tsc_khz = (unsigned long)(freq / 1000);
#else
	(void)info;
#endif
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	unsigned long long nlevels = 0, freq = 0, ncpu = 0, packages = 0;
	/* Sized so that vendor, blank and model always fit into ident. */
	char ident[JENT_IDENT_LEN - 64] = "", vendor[63] = "";
	int level, levels;
	long n = 0;

	if (sysctl_num("hw.logicalcpu", &ncpu) || !ncpu)
		return -ENOENT;
	list->ncpu = (long)ncpu;

	/*
	 * The affinity tags macOS offers only hint that threads belong
	 * together, and Apple Silicon does not implement them at all.
	 */
	list->pinning = 0;
	list->backend = "macos";
	list->note =
		"macOS describes the cores per performance level, not per "
		"CPU: the CPU\ncolumn is the position in this listing, "
		"fastest level first, and Core\ncounts within a level - equal "
		"Core numbers of different Types are\ndifferent cores.";
	/*
	 * The E-cores are still reachable: macOS schedules the lowest
	 * quality-of-service class on them alone, which is what --e-cores asks
	 * for. The P-cores are where a measurement runs by default anyway.
	 */
	list->select =
		"  jitterentropy-hashtime <rounds> <repeats> <file> --e-cores\n"
		"  No thread-to-CPU pinning here: --e-cores selects the "
		"efficiency cores and\n  --p-cores asks for the performance "
		"ones, which a recording uses anyway\n  unless the process was "
		"put in the background.";

	/* Present on Intel Macs only; Apple Silicon reports the model alone. */
	sysctl_str("machdep.cpu.vendor", vendor, sizeof(vendor));
	sysctl_str("machdep.cpu.brand_string", ident, sizeof(ident));
	if (sysctl_num("hw.cpufrequency_max", &freq))
		freq = 0;

	/* Which CPU sits in which package is not reported, so a single one
	 * is all that can be known - every Mac but the two-socket Pros. */
	if (sysctl_num("hw.packages", &packages) || packages != 1)
		packages = 0;

	if (sysctl_num("hw.nperflevels", &nlevels) || nlevels < 2)
		nlevels = 0;
	levels = nlevels ? (int)nlevels : 1;

	for (level = 0; level < levels && n < JENT_MAX_CPUS; level++) {
		unsigned long long logical = 0, physical = 0, l1d = 0, l1i = 0;
		unsigned long long l2 = 0, l3 = 0, per_l2 = 0, i;
		/* Negative selects the flat hw.* names for a uniform system. */
		int sel = nlevels ? level : -1;
		char name[64], type[64] = "", core_type[JENT_TYPE_LEN] = "";

		if (sysctl_level_num(sel, "logicalcpu", &logical) || !logical)
			continue;
		/*
		 * More cores than threads is not a machine that exists, and
		 * the ratio below is a divisor: take the level as one thread
		 * per core rather than divide by zero.
		 */
		if (sysctl_level_num(sel, "physicalcpu", &physical) ||
		    !physical || physical > logical)
			physical = logical;

		sysctl_level_num(sel, "l1dcachesize", &l1d);
		sysctl_level_num(sel, "l1icachesize", &l1i);
		sysctl_level_num(sel, "l2cachesize", &l2);
		if (sysctl_level_num(sel, "cpusperl2", &per_l2))
			per_l2 = 0;
		/* No L3 per level; the flat node covers the Macs with one. */
		if (sysctl_num("hw.l3cachesize", &l3))
			l3 = 0;

		/*
		 * One performance level means one core type, and the type
		 * stays empty as it does in the other backends.
		 */
		if (nlevels) {
			snprintf(name, sizeof(name), "hw.perflevel%d.name",
				 level);
			if (sysctl_str(name, type, sizeof(type)))
				type[0] = '\0';

			/*
			 * The level names are "Performance" and "Efficiency";
			 * fall back to the order, documented as fastest first.
			 */
			if (type[0] == 'P' || (!type[0] && level == 0))
				snprintf(core_type, sizeof(core_type),
					 "P-core");
			else if (type[0] == 'E' || !type[0])
				snprintf(core_type, sizeof(core_type),
					 "E-core");
			else
				snprintf(core_type, sizeof(core_type), "%s",
					 type);
		}

		for (i = 0; i < logical && n < JENT_MAX_CPUS; i++, n++) {
			struct jent_cpu_info *info = &list->cpu[n];

			jent_cpu_info_init(info);
			info->cpu = (unsigned long)n;
			info->cpu_valid = 1;
			if (packages)
				info->pkg = 0;
			/*
			 * SMT siblings are consecutive on the Intel Macs. The
			 * count restarts per performance level, as macOS has
			 * no numbering spanning them - so equal numbers in
			 * different levels are different cores, as the note
			 * below says.
			 */
			info->core = (long)(i / (logical / physical));
			info->max_khz = (unsigned long)(freq / 1000);
			jent_timer_macos(info);

			info->l1d.size = (unsigned long)l1d;
			info->l1d.shared = 1;
			info->l1i.size = (unsigned long)l1i;
			info->l1i.shared = 1;
			info->l2.size = (unsigned long)l2;
			info->l2.shared = (unsigned long)per_l2;
			info->l3.size = (unsigned long)l3;
			info->l3.shared = (unsigned long)ncpu;

			jent_set_ident(info, vendor, ident);

			snprintf(info->type, sizeof(info->type), "%s",
				 core_type);
		}
	}

	if (!n)
		return -ENOENT;

	list->entries = n;

	return 0;
}

#endif /* JENT_CPUINFO_MACOS */

/***************************************************************************
 * Windows backend
 *
 * GetLogicalProcessorInformationEx() describes the caches, cores and packages
 * with the group affinity mask of the CPUs each covers, plus the efficiency
 * class Windows tells a P-core from an E-core by. The model is not part of it
 * and comes from the registry, which covers the ARM64 machines as well.
 ***************************************************************************/

#ifdef JENT_CPUINFO_WINDOWS

#include <windows.h>

/* Windows has never defined more than this many processor groups. */
#define JENT_MAX_GROUPS		64

/* Number of CPUs in a group affinity mask. */
static unsigned long affinity_count(KAFFINITY mask)
{
	unsigned long count = 0;

	while (mask) {
		count += mask & 1;
		mask >>= 1;
	}

	return count;
}

/* One processor group, and where its CPUs start in the flat numbering. */
struct jent_group {
	unsigned long base;	/* flat number of the first active CPU */
	KAFFINITY active;	/* which bits of the group hold an active CPU */
};

/*
 * Flat number of the CPU at bit @bit of group @group, or -1 where that bit
 * holds no active CPU.
 *
 * The flat numbering is the one jitterentropy-hashtime --cpu takes, so it has
 * to be the one jent_thread_pin_to_cpu() resolves: the CPUs of the preceding
 * groups, then the n-th *set* bit of this group's ActiveProcessorMask. The bit
 * position itself is not that number - it is only equal to it while the active
 * CPUs of a group occupy its lowest bits without a gap, which stops holding as
 * soon as one is parked or disabled. Numbering by bit position there would
 * name a different CPU than --cpu pins to, leaving one row of the listing
 * unwritten and another written twice.
 */
static long flat_cpu(const struct jent_group *groups, WORD ngroups,
		     WORD group, unsigned long bit)
{
	KAFFINITY below;

	if (group >= ngroups || bit >= sizeof(KAFFINITY) * 8)
		return -1;
	if (!((groups[group].active >> bit) & (KAFFINITY)1))
		return -1;

	/* The active CPUs of the group sitting below @bit. */
	below = groups[group].active & (((KAFFINITY)1 << bit) - 1);

	return (long)(groups[group].base + affinity_count(below));
}

/* Apply @fn to every CPU covered by @mask. */
static void for_each_cpu(struct jent_cpu_list *list,
			 const struct jent_group *groups, WORD ngroups,
			 const GROUP_AFFINITY *mask,
			 void (*fn)(struct jent_cpu_info *, void *), void *ctx)
{
	unsigned long bit;

	for (bit = 0; bit < sizeof(KAFFINITY) * 8; bit++) {
		long cpu;

		if (!((mask->Mask >> bit) & 1))
			continue;

		cpu = flat_cpu(groups, ngroups, mask->Group, bit);
		if (cpu < 0 || cpu >= list->entries)
			continue;

		fn(&list->cpu[cpu], ctx);
	}
}

/*
 * Describe the processor groups in @out, up to @max of them, and return the
 * number of CPUs across those described or a negative errno.
 *
 * The groups are enumerated rather than counted with
 * GetActiveProcessorGroupCount() / GetActiveProcessorCount(): those report how
 * many CPUs are active, and only ActiveProcessorMask says which bit positions
 * they are - what the flat numbering above is built from. This is the source
 * jent_thread_pin_to_cpu() reads as well.
 */
static long jent_groups_windows(struct jent_group *out, WORD max,
				WORD *ngroups)
{
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *rec;
	const GROUP_RELATIONSHIP *rel;
	/* Bytes that must be readable before Relationship and Size are read. */
	const size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
				    Group);
	size_t need;
	DWORD len = 0;
	BYTE *buf;
	long ncpu = 0;
	WORD group;

	if (GetLogicalProcessorInformationEx(RelationGroup, NULL, &len) ||
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return -EFAULT;

	buf = (BYTE *)malloc(len);
	if (!buf)
		return -ENOMEM;

	if (!GetLogicalProcessorInformationEx(
			RelationGroup,
			(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf, &len)) {
		free(buf);
		return -EFAULT;
	}

	/*
	 * RelationGroup is reported as a single record covering every group,
	 * with the per-group entries as a trailing array. Validate the header,
	 * then the array the announced ActiveGroupCount implies, before either
	 * is dereferenced.
	 */
	rec = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)buf;
	need = hdr + offsetof(GROUP_RELATIONSHIP, GroupInfo);
	if ((size_t)len < hdr || (size_t)len < rec->Size ||
	    rec->Relationship != RelationGroup || rec->Size < need) {
		free(buf);
		return -EFAULT;
	}

	rel = &rec->Group;
	need += (size_t)rel->ActiveGroupCount * sizeof(PROCESSOR_GROUP_INFO);
	if (rec->Size < need) {
		free(buf);
		return -EFAULT;
	}

	*ngroups = rel->ActiveGroupCount < max ? rel->ActiveGroupCount : max;

	/* Groups number from zero, so one starts at the count before it. */
	for (group = 0; group < *ngroups; group++) {
		const PROCESSOR_GROUP_INFO *gi = &rel->GroupInfo[group];

		out[group].base = (unsigned long)ncpu;
		out[group].active = gi->ActiveProcessorMask;
		ncpu += (long)gi->ActiveProcessorCount;
	}

	free(buf);

	return ncpu;
}

struct cache_ctx {
	struct jent_cache_info cache;
	BYTE level;
	int instruction;
};

static void set_cache(struct jent_cpu_info *info, void *arg)
{
	const struct cache_ctx *ctx = (const struct cache_ctx *)arg;
	struct jent_cache_info *cache;

	if (ctx->level == 1 && ctx->instruction)
		cache = &info->l1i;
	else if (ctx->level == 1)
		cache = &info->l1d;
	else if (ctx->level == 2 && !ctx->instruction)
		cache = &info->l2;
	else if (ctx->level == 3 && !ctx->instruction)
		cache = &info->l3;
	else
		return;

	*cache = ctx->cache;
}

struct core_ctx {
	long index;
	BYTE efficiency_class;
};

static void set_core(struct jent_cpu_info *info, void *arg)
{
	const struct core_ctx *ctx = (const struct core_ctx *)arg;

	info->core = ctx->index;
	/*
	 * Held here until the highest class in the system is known, one above
	 * the class itself: zero then marks a CPU no core record covered, which
	 * class 0 is otherwise indistinguishable from.
	 */
	info->max_khz = (unsigned long)ctx->efficiency_class + 1;
}

static void set_package(struct jent_cpu_info *info, void *arg)
{
	info->pkg = *(const long *)arg;
}

/* Model, vendor and nominal frequency as published by the kernel per CPU. */
static void jent_ident_windows(struct jent_cpu_info *info)
{
	/* Sized so that vendor, blank and model always fit into ident. */
	char key[128], name[JENT_IDENT_LEN - 64] = "", vendor[63] = "";
	DWORD len, mhz = 0;

	/*
	 * The subkeys carry the numbering the system assigned the processors,
	 * which is the flat numbering used here as long as all of them are
	 * active. Where one is not, the model is read from a neighbouring CPU -
	 * a different string only on a machine mixing models across packages.
	 */
	snprintf(key, sizeof(key),
		 "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%lu",
		 info->cpu);

	len = sizeof(vendor);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "VendorIdentifier",
			 RRF_RT_REG_SZ, NULL, vendor, &len) != ERROR_SUCCESS)
		vendor[0] = '\0';

	len = sizeof(name);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "ProcessorNameString",
			 RRF_RT_REG_SZ, NULL, name, &len) != ERROR_SUCCESS)
		name[0] = '\0';

	jent_set_ident(info, vendor, name);

	/*
	 * "~MHz" is the base speed, not a maximum, and Windows publishes no
	 * boost figure anywhere - the MaxMhz of CallNtPowerInformation() is
	 * this same value. The key is per CPU, so as on Linux it wins over the
	 * package-wide CPUID leaf 0x16, which only fills in what is left.
	 */
	len = sizeof(mhz);
	if (RegGetValueA(HKEY_LOCAL_MACHINE, key, "~MHz", RRF_RT_REG_DWORD,
			 NULL, &mhz, &len) == ERROR_SUCCESS)
		info->base_khz = mhz * 1000;
}

/*
 * Does @p hold the member its Relationship names, in the space it announces?
 * The records are variable-length and the trailing GroupMask array is as long
 * as GroupCount says, so the fixed part and that array both have to fit into
 * Size before either is read. @hdr is what the walk has already checked.
 */
static int record_fits(const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p,
		       size_t hdr)
{
	size_t need;

	if (p->Relationship == RelationCache)
		return p->Size >= hdr + sizeof(CACHE_RELATIONSHIP);

	if (p->Relationship != RelationProcessorCore &&
	    p->Relationship != RelationProcessorPackage)
		return 1;	/* not read below */

	/* GroupCount sits before the array, so the fixed part comes first. */
	need = hdr + offsetof(PROCESSOR_RELATIONSHIP, GroupMask);
	if (p->Size < need)
		return 0;

	return (size_t)(p->Size - need) >=
	       (size_t)p->Processor.GroupCount * sizeof(GROUP_AFFINITY);
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *buf;
	struct jent_group groups[JENT_MAX_GROUPS];
	/* Efficiency class plus one, zero where no core record was seen. */
	unsigned int classes[JENT_MAX_CPUS];
	unsigned int max_class = 0;
	/* Bytes that must be readable before Relationship and Size are read. */
	const size_t hdr = offsetof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX,
				    Processor);
	WORD ngroups = 0;
	DWORD len = 0;
	BYTE *pos, *end;
	long ncpu, cores = 0, packages = 0, i;

	ncpu = jent_groups_windows(groups, JENT_MAX_GROUPS, &ngroups);
	if (ncpu < 0)
		return (int)ncpu;
	if (!ngroups || !ncpu)
		return -ENOENT;

	list->ncpu = ncpu;
	list->pinning = 1;
	list->backend = "windows";
	if (ncpu > JENT_MAX_CPUS) {
		fprintf(stderr, "Only the first %d of %ld CPUs are reported\n",
			JENT_MAX_CPUS, ncpu);
		ncpu = JENT_MAX_CPUS;
	}
	list->entries = ncpu;

	for (i = 0; i < ncpu; i++) {
		jent_cpu_info_init(&list->cpu[i]);
		list->cpu[i].cpu = (unsigned long)i;
		list->cpu[i].cpu_valid = 1;
	}

	if (GetLogicalProcessorInformationEx(RelationAll, NULL, &len) ||
	    GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return -EFAULT;

	buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)malloc(len);
	if (!buf)
		return -ENOMEM;

	if (!GetLogicalProcessorInformationEx(RelationAll, buf, &len)) {
		free(buf);
		return -EFAULT;
	}

	pos = (BYTE *)buf;
	end = pos + len;
	while ((size_t)(end - pos) >= hdr) {
		SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *p =
			(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)pos;
		WORD n;

		/*
		 * A record shorter than the header would make the walk spin,
		 * one longer than what is left would be read past the buffer,
		 * and one too small for what it claims to describe would be
		 * read past its own end. None of the data after such a record
		 * can be located, so the walk stops rather than skipping it.
		 */
		if (p->Size < hdr || (size_t)(end - pos) < p->Size ||
		    !record_fits(p, hdr))
			break;
		pos += p->Size;

		/*
		 * Not a switch: -Wswitch-enum would ask for every value of the
		 * enumeration although only three matter. Trace caches hold no
		 * data and are skipped as well.
		 */
		if (p->Relationship == RelationCache &&
		    p->Cache.Type != CacheTrace) {
			struct cache_ctx ctx;

			ctx.level = p->Cache.Level;
			ctx.instruction = (p->Cache.Type == CacheInstruction);
			ctx.cache.size = (unsigned long)p->Cache.CacheSize;
			ctx.cache.shared =
				affinity_count(p->Cache.GroupMask.Mask);
			for_each_cpu(list, groups, ngroups,
				     &p->Cache.GroupMask, set_cache, &ctx);
		} else if (p->Relationship == RelationProcessorCore) {
			struct core_ctx ctx;

			ctx.index = cores++;
			ctx.efficiency_class = p->Processor.EfficiencyClass;
			if (ctx.efficiency_class > max_class)
				max_class = ctx.efficiency_class;
			for (n = 0; n < p->Processor.GroupCount; n++)
				for_each_cpu(list, groups, ngroups,
					     &p->Processor.GroupMask[n],
					     set_core, &ctx);
		} else if (p->Relationship == RelationProcessorPackage) {
			long pkg = packages++;

			for (n = 0; n < p->Processor.GroupCount; n++)
				for_each_cpu(list, groups, ngroups,
					     &p->Processor.GroupMask[n],
					     set_package, &pkg);
		}
	}

	free(buf);

	/*
	 * The efficiency class is a ranking without a fixed scale, so it names
	 * a core type only once the highest class is known: that one is the
	 * P-cores, everything below it an E-core. There can be several such
	 * classes, which jent_mark_lp_cores() separates afterwards.
	 */
	for (i = 0; i < ncpu; i++) {
		classes[i] = (unsigned int)list->cpu[i].max_khz;
		list->cpu[i].max_khz = 0;
	}

	for (i = 0; i < ncpu; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		/*
		 * A CPU no core record covered - the CPU numbering and the
		 * records disagreeing about which one exists, say - is left
		 * without a type instead of taking the lower one by default.
		 */
		if (max_class && classes[i])
			snprintf(info->type, sizeof(info->type), "%s",
				 classes[i] - 1 == max_class ?
					"P-core" : "E-core");

		jent_ident_windows(info);
	}

#ifdef JENT_CPUINFO_X86
	/*
	 * The base frequency and the counter are only in CPUID, which answers
	 * for the core executing it - so unlike everything above, this visits
	 * each CPU. The first call reports the affinity restored afterwards.
	 */
	{
		GROUP_AFFINITY previous;
		int restore = 0;
		WORD group;

		for (group = 0; group < ngroups; group++) {
			unsigned long bit;

			for (bit = 0; bit < sizeof(KAFFINITY) * 8; bit++) {
				GROUP_AFFINITY affinity, old;
				PROCESSOR_NUMBER current;
				long cpu = flat_cpu(groups, ngroups, group,
						    bit);

				if (cpu < 0 || cpu >= ncpu)
					continue;

				memset(&affinity, 0, sizeof(affinity));
				affinity.Group = group;
				affinity.Mask = (KAFFINITY)1 << bit;
				if (!SetThreadGroupAffinity(GetCurrentThread(),
							    &affinity, &old))
					continue;
				if (!restore) {
					previous = old;
					restore = 1;
				}

				/*
				 * A parked or offline CPU leaves the thread
				 * where it was, and CPUID would then describe
				 * the wrong core.
				 */
				GetCurrentProcessorNumberEx(&current);
				if (current.Group != group ||
				    current.Number != (BYTE)bit)
					continue;

				jent_freq_x86(&list->cpu[cpu]);
			}
		}

		if (restore)
			SetThreadGroupAffinity(GetCurrentThread(), &previous,
					       NULL);
	}
#endif /* JENT_CPUINFO_X86 */

	return 0;
}

#endif /* JENT_CPUINFO_WINDOWS */

/***************************************************************************
 * Generic backend for the BSDs and everything else
 *
 * None of these enumerates the caches of the individual CPUs, and OpenBSD has
 * no thread affinity API at all. What is left is the CPU this tool runs on:
 * CPUID for its caches and core type on x86, hw.model for its name on the BSDs.
 *
 * The numeric sysctl MIB is preferred over sysctlbyname(), which OpenBSD lacks.
 * Solaris, Haiku and Cygwin have no sysctl at all - hence
 * JENT_CPUINFO_HAVE_SYSCTL - and are left with the CPU count and CPUID.
 ***************************************************************************/

#ifdef JENT_CPUINFO_GENERIC

#include <unistd.h>

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || \
    defined(__DragonFly__)
# include <sys/types.h>
# include <sys/param.h>
# include <sys/sysctl.h>
# define JENT_CPUINFO_HAVE_SYSCTL
/*
 * Every BSD but OpenBSD has sysctlbyname(). The MIB is used where it covers
 * the node, the name interface for those without a portable MIB constant.
 */
# ifndef __OpenBSD__
#  define JENT_CPUINFO_HAVE_SYSCTLBYNAME
# endif
#endif

/*
 * FreeBSD is the one system here that can place a thread on a chosen CPU, so
 * the one whose CPUs can be described individually. Same call the library
 * pins its counting thread with (arch/jitterentropy-arch-thread.c).
 */
#ifdef __FreeBSD__
# include <sys/cpuset.h>
# define JENT_CPUINFO_BSD_AFFINITY

static int pin_to_cpu(unsigned long cpu)
{
	cpuset_t set;

	if (cpu >= (unsigned long)CPU_SETSIZE)
		return -EINVAL;

	CPU_ZERO(&set);
	CPU_SET((int)cpu, &set);
	if (cpuset_setaffinity(CPU_LEVEL_WHICH, CPU_WHICH_TID, -1, sizeof(set),
			       &set))
		return -errno;

	return 0;
}
#endif /* __FreeBSD__ */

#ifdef JENT_CPUINFO_HAVE_SYSCTL

static int sysctl_hw_str(int node, char *buf, size_t buflen)
{
	int mib[2] = { CTL_HW, node };
	size_t len = buflen;

	if (sysctl(mib, 2, buf, &len, NULL, 0))
		return -errno;
	buf[buflen - 1] = '\0';

	return 0;
}

#endif /* JENT_CPUINFO_HAVE_SYSCTL */

/* The clock rate in kHz, or 0 where the system does not report one. */
static unsigned long jent_clockrate(void)
{
#if defined(__OpenBSD__) && defined(HW_CPUSPEED)
	int mib[2] = { CTL_HW, HW_CPUSPEED };
	int speed = 0;
	size_t len = sizeof(speed);

	if (sysctl(mib, 2, &speed, &len, NULL, 0) || speed <= 0)
		return 0;

	return (unsigned long)speed * 1000;		/* MHz -> kHz */
#elif defined(JENT_CPUINFO_HAVE_SYSCTLBYNAME)
	/* hw.clockrate is FreeBSD and DragonFly, in MHz. */
	int speed = 0;
	size_t len = sizeof(speed);

	if (!sysctlbyname("hw.clockrate", &speed, &len, NULL, 0) && speed > 0)
		return (unsigned long)speed * 1000;

	/* NetBSD states the counter rate instead, in Hz. */
	{
		uint64_t freq = 0;

		len = sizeof(freq);
		if (!sysctlbyname("machdep.tsc_freq", &freq, &len, NULL, 0) &&
		    freq)
			return (unsigned long)(freq / 1000);
	}

	return 0;
#else
	return 0;
#endif
}

/* Everything that can be said about the CPU this thread is running on. */
static void jent_describe_current(struct jent_cpu_info *info)
{
#ifdef JENT_CPUINFO_HAVE_SYSCTL
	char model[JENT_IDENT_LEN] = "";

	if (!sysctl_hw_str(HW_MODEL, model, sizeof(model)) && model[0])
		jent_set_ident(info, NULL, model);
#endif

	info->base_khz = jent_clockrate();

#ifdef JENT_CPUINFO_X86
	/* Overrides hw.model with the vendor and brand string of this core. */
	jent_ident_x86(info);
	jent_caches_x86(info);
	jent_freq_x86(info);
#endif
}

static int jent_get_cpus(struct jent_cpu_list *list)
{
	long ncpu, n = 0;

	ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	list->ncpu = (ncpu > 0) ? ncpu : 1;
	list->backend = "generic";

	/* No affinity API on OpenBSD, so --cpu is unavailable there. */
#ifdef __OpenBSD__
	list->pinning = 0;
#else
	list->pinning = 1;
#endif

#ifdef JENT_CPUINFO_BSD_AFFINITY
	for (n = 0; n < list->ncpu && n < JENT_MAX_CPUS; n++) {
		struct jent_cpu_info *info = &list->cpu[n];

		/* Whatever was reached is kept; the note below says so. */
		if (pin_to_cpu((unsigned long)n))
			break;

		jent_cpu_info_init(info);
		info->cpu = (unsigned long)n;
		info->cpu_valid = 1;
		jent_describe_current(info);
	}
#endif

	if (!n) {
		struct jent_cpu_info *info = &list->cpu[0];

		jent_cpu_info_init(info);
		/* Without affinity, which CPU this is cannot be known. */
		info->cpu_valid = 0;
		jent_describe_current(info);
		n = 1;

		list->note =
			"This system describes no CPU but the one this tool "
			"runs on. On a\nhybrid CPU, run it repeatedly to see "
			"the other core types.";
	} else if (n < list->ncpu) {
		list->note =
			"Only the CPUs this tool could place itself on are "
			"described.";
	}

	list->entries = n;

	return 0;
}

#endif /* JENT_CPUINFO_GENERIC */

/***************************************************************************
 * Unsupported systems
 ***************************************************************************/

#if !defined(JENT_CPUINFO_LINUX) && !defined(JENT_CPUINFO_MACOS) && \
    !defined(JENT_CPUINFO_WINDOWS) && !defined(JENT_CPUINFO_GENERIC)

static int jent_get_cpus(struct jent_cpu_list *list)
{
	list->backend = "none";
	return -ENOSYS;
}

#endif

/*
 * Separate the low-power E-cores from the regular ones. Neither CPUID nor the
 * Windows efficiency class distinguishes them - what does is where they sit,
 * outside the L3 domain, so they are the E-cores seeing no L3 on a system
 * whose other cores do.
 *
 * A derivation from the cache topology rather than something the hardware
 * states, so it only refines cores already identified as E-cores, and only
 * where some CPU does report an L3.
 */
static void jent_mark_lp_cores(struct jent_cpu_list *list)
{
	int have_l3 = 0;
	long i;

	for (i = 0; i < list->entries; i++) {
		if (list->cpu[i].l3.size) {
			have_l3 = 1;
			break;
		}
	}

	if (!have_l3)
		return;

	for (i = 0; i < list->entries; i++) {
		struct jent_cpu_info *info = &list->cpu[i];

		if (!strcmp(info->type, "E-core") && !info->l3.size)
			snprintf(info->type, sizeof(info->type), "LP-E-core");
	}
}

/***************************************************************************
 * Output
 ***************************************************************************/

/* Format a cache as "<size in KiB>/<CPUs sharing it>", e.g. "1280K/2". */
static void format_cache(const struct jent_cache_info *cache, char *buf,
			 size_t buflen)
{
	if (!cache->size) {
		snprintf(buf, buflen, "-");
		return;
	}

	if (cache->shared)
		snprintf(buf, buflen, "%luK/%lu", cache->size / 1024,
			 cache->shared);
	else
		snprintf(buf, buflen, "%luK", cache->size / 1024);
}

static void format_num(long val, char *buf, size_t buflen)
{
	if (val < 0)
		snprintf(buf, buflen, "-");
	else
		snprintf(buf, buflen, "%ld", val);
}

/* Summary of one tri-state CPU property over all CPUs. */
#define JENT_FLAG_NONE	(-1)	/* no CPU reports it */
#define JENT_FLAG_MIXED	(-2)	/* the CPUs disagree */

/* What the counter of this architecture is called, named mid-sentence. */
#ifdef JENT_CPUINFO_ARM64
# define JENT_TIMER_NAME	"generic timer"
#else
# define JENT_TIMER_NAME	"timestamp counter"
#endif

enum jent_tsc_prop {
	jent_tsc_invariant,
	jent_tsc_nonstop,
	jent_tsc_known_freq,
};

static int jent_tsc_flag(const struct jent_cpu_list *list,
			 enum jent_tsc_prop prop)
{
	int value = JENT_FLAG_NONE;
	long i;

	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];
		int cur;

		switch (prop) {
		case jent_tsc_nonstop:
			cur = info->tsc_nonstop;
			break;
		case jent_tsc_known_freq:
			cur = info->tsc_known_freq;
			break;
		case jent_tsc_invariant:
		default:
			cur = info->tsc_invariant;
			break;
		}

		if (cur < 0)
			continue;
		if (value == JENT_FLAG_NONE)
			value = cur;
		else if (value != cur)
			return JENT_FLAG_MIXED;
	}

	return value;
}

/* Does any CPU report @vendor? The model string starts with the vendor ID. */
static int jent_vendor_is(const struct jent_cpu_list *list, const char *vendor)
{
	size_t len = strlen(vendor);
	long i;

	for (i = 0; i < list->entries; i++) {
		if (!strncmp(list->cpu[i].ident, vendor, len))
			return 1;
	}

	return 0;
}

static const char *jent_flag_str(int value)
{
	switch (value) {
	case 1:
		return "yes";
	case 0:
		return "no";
	case JENT_FLAG_MIXED:
		return "differs between CPUs";
	default:
		return "unknown";
	}
}

/* Does any CPU report a type starting with @prefix - "cap ", "perf "? */
static int jent_have_type(const struct jent_cpu_list *list, const char *prefix)
{
	size_t len = strlen(prefix);
	long i;

	for (i = 0; i < list->entries; i++) {
		if (!strncmp(list->cpu[i].type, prefix, len))
			return 1;
	}

	return 0;
}

/*
 * One note below the table, printed as a bullet with its wrapped lines
 * indented to match. The heading is emitted before the first note, so a system
 * with nothing to report gets no empty section.
 */
static void print_note(int *heading, const char *text)
{
	const char *p;

	if (!*heading) {
		printf("\nNotes:\n");
		*heading = 1;
	}

	printf("  - ");
	for (p = text; *p; p++) {
		putchar(*p);
		if (*p == '\n')
			printf("    ");
	}
	putchar('\n');
}

/*
 * The model of @info as "#<N>", numbering the distinct ones in the order met
 * and collecting them in @idents for the list below the table. On a hybrid x86
 * CPU every core reports the same brand string, so one entry commonly stands
 * for the whole machine.
 */
static void format_ident(const struct jent_cpu_info *info, const char **idents,
			 int *nidents, char *buf, size_t buflen)
{
	int j;

	if (!info->ident[0]) {
		snprintf(buf, buflen, "-");
		return;
	}

	for (j = 0; j < *nidents; j++) {
		if (!strcmp(idents[j], info->ident))
			break;
	}
	if (j == *nidents && *nidents < JENT_MAX_CPUS)
		idents[(*nidents)++] = info->ident;

	snprintf(buf, buflen, "#%d", j + 1);
}

/* Everything of a CPU that is not the CPU, package and core number. */
#define JENT_ROW_FMT	"%-9s %7s %7s %7s %10s %10s %10s %11s %6s\n"

static void format_row(const struct jent_cpu_info *info, const char **idents,
		       int *nidents, char *buf, size_t buflen)
{
	char base[16], mhz[16], tsc[16], ident[16];
	char l1d[24], l1i[24], l2[24], l3[24];

	format_num(info->base_khz ? (long)(info->base_khz / 1000) : -1,
		   base, sizeof(base));
	format_num(info->max_khz ? (long)(info->max_khz / 1000) : -1,
		   mhz, sizeof(mhz));
	format_num(info->tsc_khz ? (long)(info->tsc_khz / 1000) : -1,
		   tsc, sizeof(tsc));
	format_cache(&info->l1d, l1d, sizeof(l1d));
	format_cache(&info->l1i, l1i, sizeof(l1i));
	format_cache(&info->l2, l2, sizeof(l2));
	format_cache(&info->l3, l3, sizeof(l3));
	format_ident(info, idents, nidents, ident, sizeof(ident));

	snprintf(buf, buflen, JENT_ROW_FMT, info->type[0] ? info->type : "-",
		 base, mhz, tsc, l1d, l1i, l2, l3, ident);
}

static void print_table(const struct jent_cpu_list *list, const char **idents,
			int *nidents)
{
	long i;

	printf("%4s %4s %5s " JENT_ROW_FMT,
	       "CPU", "Pkg", "Core", "Type", "BaseMHz", "MaxMHz", "TmrMHz",
	       "L1d", "L1i", "L2", "L3", "Model");

	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];
		char cpu[16], pkg[16], core[16], row[256];

		if (info->cpu_valid)
			snprintf(cpu, sizeof(cpu), "%lu", info->cpu);
		else
			snprintf(cpu, sizeof(cpu), "?");
		format_num(info->pkg, pkg, sizeof(pkg));
		format_num(info->core, core, sizeof(core));
		format_row(info, idents, nidents, row, sizeof(row));

		printf("%4s %4s %5s %s", cpu, pkg, core, row);
	}
}

/*
 * Do two CPUs need to be measured separately? Package and core numbers are
 * left out: they name a CPU rather than describe it. What remains is what a
 * recording depends on.
 */
static int same_kind(const struct jent_cpu_info *a,
		     const struct jent_cpu_info *b)
{
	return !strcmp(a->type, b->type) && !strcmp(a->ident, b->ident) &&
	       a->base_khz == b->base_khz && a->max_khz == b->max_khz &&
	       a->tsc_khz == b->tsc_khz &&
	       !memcmp(&a->l1d, &b->l1d, sizeof(a->l1d)) &&
	       !memcmp(&a->l1i, &b->l1i, sizeof(a->l1i)) &&
	       !memcmp(&a->l2, &b->l2, sizeof(a->l2)) &&
	       !memcmp(&a->l3, &b->l3, sizeof(a->l3));
}

/* Append @cpu to a list of ranges, "0-3,8,10-11". */
static void append_range(char *buf, size_t buflen, unsigned long first,
			 unsigned long last)
{
	size_t len = strlen(buf);

	if (len && len + 1 < buflen)
		buf[len++] = ',';

	if (first == last)
		snprintf(buf + len, buflen - len, "%lu", first);
	else
		snprintf(buf + len, buflen - len, "%lu-%lu", first, last);
}

/*
 * One row per kind of CPU instead of one per CPU: on a server with a hundred
 * alike, the full listing says the same thing a hundred times.
 */
static void print_summary(const struct jent_cpu_list *list, const char **idents,
			  int *nidents)
{
	long i, j;
	int printed = 0;

	printf("%5s " JENT_ROW_FMT,
	       "CPUs", "Type", "BaseMHz", "MaxMHz", "TmrMHz",
	       "L1d", "L1i", "L2", "L3", "Model");

	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];
		char row[256], cpus[512] = "";
		unsigned long first = 0, last = 0;
		int open = 0;
		long count = 0;

		/* Already covered by a kind printed earlier. */
		for (j = 0; j < i; j++) {
			if (same_kind(&list->cpu[j], info))
				break;
		}
		if (j < i)
			continue;

		for (j = i; j < list->entries; j++) {
			const struct jent_cpu_info *other = &list->cpu[j];

			if (!same_kind(other, info))
				continue;
			count++;
			if (!other->cpu_valid)
				continue;

			if (open && other->cpu == last + 1) {
				last = other->cpu;
				continue;
			}
			if (open)
				append_range(cpus, sizeof(cpus), first, last);
			first = other->cpu;
			last = other->cpu;
			open = 1;
		}
		if (open)
			append_range(cpus, sizeof(cpus), first, last);

		format_row(info, idents, nidents, row, sizeof(row));
		printf("%5ld %s", count, row);
		if (cpus[0])
			printf("      CPUs %s\n", cpus);
		printed++;
	}

	/*
	 * Only where the listing covers the machine: a backend describing the
	 * one CPU it runs on has every row alike by construction.
	 */
	if (printed == 1 && list->entries == list->ncpu)
		printf("\nEvery CPU of this machine is of the same kind: any "
		       "one of them can be\nrecorded for all.\n");
}

static void print_cpus(const struct jent_cpu_list *list, int summary)
{
	const char *idents[JENT_MAX_CPUS];
	int nidents = 0, j;
	long i;

	if (summary)
		print_summary(list, idents, &nidents);
	else
		print_table(list, idents, &nidents);

	printf("\nColumns:\n");
	if (!summary)
		printf("  Pkg, Core  package and core ID - equal in both means "
		       "SMT siblings of one core\n");
	else
		printf("  CPUs       how many CPUs are of this kind, and which "
		       "ones they are\n");
	printf("  Type       core type as the system reports it\n");
	/*
	 * Where a ranking stands in for the core type, its scale has to be
	 * said - not least that a uniform machine has every core at the top.
	 */
	if (jent_have_type(list, "cap "))
		printf("             \"cap <N>\" is the compute capacity the "
		       "scheduler works with,\n             1024 being the "
		       "most capable core of the system\n");
	if (jent_have_type(list, "perf "))
		printf("             \"perf <N>\" is the performance ranking "
		       "the firmware gives the\n             core, 255 being "
		       "the maximum - AMD reports no core type\n");
	printf("  BaseMHz    nominal base frequency, not the clock the CPU "
	       "currently runs at\n");
	printf("  TmrMHz     rate of the %s, which the Jitter RNG times "
	       "with\n", JENT_TIMER_NAME);
	printf("  L1d - L3   cache size in KiB, followed by the number of "
	       "CPUs sharing it\n");

	/*
	 * The properties of that counter belong to the part, not the core, so
	 * they are summarized here rather than taking three more columns.
	 */
	if (jent_tsc_flag(list, jent_tsc_invariant) != JENT_FLAG_NONE ||
	    jent_tsc_flag(list, jent_tsc_nonstop) != JENT_FLAG_NONE ||
	    jent_tsc_flag(list, jent_tsc_known_freq) != JENT_FLAG_NONE) {
		printf("\nTimer: invariant %s, nonstop %s, known rate %s\n",
		       jent_flag_str(jent_tsc_flag(list, jent_tsc_invariant)),
		       jent_flag_str(jent_tsc_flag(list, jent_tsc_nonstop)),
		       jent_flag_str(jent_tsc_flag(list,
						   jent_tsc_known_freq)));
	}

	{
		int have_freq = 0, have_tsc = 0, heading = 0;

		/* What the listing covers comes before what it is missing. */
		if (list->note)
			print_note(&heading, list->note);

		/* Which of the two columns are a dash for every CPU. */
		for (i = 0; i < list->entries; i++) {
			if (list->cpu[i].base_khz || list->cpu[i].max_khz)
				have_freq = 1;
			if (list->cpu[i].tsc_khz)
				have_tsc = 1;
		}

		/*
		 * A dash there is not an unread value: on AMD no CPUID leaf
		 * carries the rate, and the operating system may still know
		 * it. Spelled out per case so that each reads as one sentence.
		 */
		if (!have_tsc) {
			int amd = jent_vendor_is(list, "AuthenticAMD");
			int known = jent_tsc_flag(list, jent_tsc_known_freq);

			if (amd && known == 1)
				print_note(&heading,
					   "The counter rate is not enumerated "
					   "by this CPU: AMD implements neither\n"
					   "CPUID leaf carrying it, so only the "
					   "operating system knows it.");
			else if (amd)
				print_note(&heading,
					   "The counter rate is not enumerated "
					   "by this CPU: AMD implements neither\n"
					   "CPUID leaf carrying it, and the "
					   "operating system determines it on "
					   "its own.");
			else if (known == 1)
				print_note(&heading,
					   "The counter rate is not enumerated "
					   "by this CPU, so only the operating\n"
					   "system knows it.");
			else
				print_note(&heading,
					   "The counter rate is not enumerated "
					   "by this CPU, and the operating "
					   "system\ndetermines it on its own.");
		}

		if (!have_freq)
			print_note(&heading,
				   "No frequency is reported: neither the CPU "
				   "nor the operating system\nstates one.");
	}

	if (nidents) {
		printf("\nModels:\n");
		for (j = 0; j < nidents; j++)
			printf("  #%d: %s\n", j + 1, idents[j]);
	}

	printf("\nRecording:\n");
	if (list->pinning)
		printf("  jitterentropy-hashtime <rounds> <repeats> <file> "
		       "--cpu <CPU>\n");
	else if (list->select)
		printf("%s\n", list->select);
	else
		printf("  This system offers no thread-to-CPU pinning, so a "
		       "recording cannot be\n  confined to one core.\n");
	printf("  Add --status to print the configuration a recording is made "
	       "with.\n");
}

/*
 * JSON output: the same data as the table, for scripts driving a recording per
 * core type. Unreported values are null rather than absent, so that every CPU
 * carries the same set of keys.
 */
static void print_json_string(const char *str)
{
	putchar('"');
	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;

		switch (c) {
		case '"':
		case '\\':
			printf("\\%c", c);
			break;
		case '\n':
			printf("\\n");
			break;
		case '\t':
			printf("\\t");
			break;
		default:
			if (c < 0x20)
				printf("\\u%04x", c);
			else
				putchar(c);
			break;
		}
	}
	putchar('"');
}

/* A tri-state CPU property as a JSON literal. */
static const char *jent_json_flag(int value)
{
	if (value < 0)
		return "null";
	return value ? "true" : "false";
}

static void print_json_cache(const char *name,
			     const struct jent_cache_info *cache, int last)
{
	printf("\t\t\t\t\"%s\": ", name);

	if (!cache->size) {
		printf("null%s\n", last ? "" : ",");
		return;
	}

	printf("{ \"sizeBytes\": %lu, \"sharedCpus\": ", cache->size);
	if (cache->shared)
		printf("%lu", cache->shared);
	else
		printf("null");
	printf(" }%s\n", last ? "" : ",");
}

static void print_json(const struct jent_cpu_list *list)
{
	long i;

	printf("{\n");
	printf("\t\"cpus\": %ld,\n", list->ncpu);
	printf("\t\"pinning\": %s,\n", list->pinning ? "true" : "false");
	printf("\t\"backend\": ");
	print_json_string(list->backend ? list->backend : "none");
	printf(",\n");

	printf("\t\"processors\": [\n");
	for (i = 0; i < list->entries; i++) {
		const struct jent_cpu_info *info = &list->cpu[i];

		printf("\t\t{\n");

		printf("\t\t\t\"cpu\": ");
		if (info->cpu_valid)
			printf("%lu,\n", info->cpu);
		else
			printf("null,\n");

		printf("\t\t\t\"package\": ");
		if (info->pkg < 0)
			printf("null,\n");
		else
			printf("%ld,\n", info->pkg);

		printf("\t\t\t\"core\": ");
		if (info->core < 0)
			printf("null,\n");
		else
			printf("%ld,\n", info->core);

		printf("\t\t\t\"coreType\": ");
		if (info->type[0])
			print_json_string(info->type);
		else
			printf("null");
		printf(",\n");

		printf("\t\t\t\"baseFrequencyKHz\": ");
		if (info->base_khz)
			printf("%lu,\n", info->base_khz);
		else
			printf("null,\n");

		printf("\t\t\t\"maxFrequencyKHz\": ");
		if (info->max_khz)
			printf("%lu,\n", info->max_khz);
		else
			printf("null,\n");

		printf("\t\t\t\"timer\": { \"frequencyKHz\": ");
		if (info->tsc_khz)
			printf("%lu", info->tsc_khz);
		else
			printf("null");
		printf(", \"invariant\": %s",
		       jent_json_flag(info->tsc_invariant));
		printf(", \"nonstop\": %s", jent_json_flag(info->tsc_nonstop));
		printf(", \"knownFrequency\": %s },\n",
		       jent_json_flag(info->tsc_known_freq));

		printf("\t\t\t\"model\": ");
		if (info->ident[0])
			print_json_string(info->ident);
		else
			printf("null");
		printf(",\n");

		printf("\t\t\t\"caches\": {\n");
		print_json_cache("l1d", &info->l1d, 0);
		print_json_cache("l1i", &info->l1i, 0);
		print_json_cache("l2", &info->l2, 0);
		print_json_cache("l3", &info->l3, 1);
		printf("\t\t\t}\n");

		printf("\t\t}%s\n", (i + 1 < list->entries) ? "," : "");
	}
	printf("\t]\n");
	printf("}\n");
}

static void usage(const char *name)
{
	printf("%s [--summary] [--json]\n", name);
	printf("List the identification and the caches of all CPUs.\n\n");
	printf("  --summary  one row per kind of CPU instead of one per CPU\n");
	printf("  --json     report the same data as JSON, one entry per CPU\n");
	printf("  --help     print this text\n");
}

int main(int argc, char *argv[])
{
	static struct jent_cpu_list list;
	int json = 0, summary = 0, i, ret;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--json")) {
			json = 1;
		} else if (!strcmp(argv[i], "--summary")) {
			summary = 1;
		} else if (!strcmp(argv[i], "--help") ||
			   !strcmp(argv[i], "-h")) {
			usage(argv[0]);
			return 0;
		} else {
			fprintf(stderr, "Unknown option %s\n", argv[i]);
			usage(argv[0]);
			return 1;
		}
	}

	ret = jent_get_cpus(&list);
	if (ret) {
		fprintf(stderr, "Cannot obtain the CPU information: %s\n",
			strerror(-ret));
		return 1;
	}

	jent_mark_lp_cores(&list);

	if (json) {
		print_json(&list);
		return 0;
	}

	printf("CPUs: %ld\n\n", list.ncpu);
	print_cpus(&list, summary);

	return 0;
}
