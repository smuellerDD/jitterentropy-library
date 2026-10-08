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

#ifndef _JITTERENTROPY_H
#define _JITTERENTROPY_H

#ifdef LINUX_KERNEL

/*
 * Deliberately avoid <linux/module.h> here: it transitively pulls in almost the
 * entire kernel header tree (sched.h, slab.h, rwsem.h, ...), and this header is
 * included by the entropy-collection core which must be compiled with -O0 (see
 * the __OPTIMIZE__ guard in src/jitterentropy-base.c). Several of those headers
 * (e.g. the asm_inline in <linux/rwsem.h>) do not compile at -O0 on modern
 * kernels. Only the lightweight, -O0-safe headers providing the types and
 * helpers used by the core are pulled in. The kernel interface glue
 * (jitterentropy_kcapi.c and friends) includes <linux/module.h> itself.
 */
#include <linux/limits.h>
#include <linux/minmax.h>	/* min()/max()/min_t()/max_t() */
#include <linux/types.h>	/* uintN_t, size_t, ssize_t, bool, NULL */

#elif defined(_KERNEL) && defined(__FreeBSD__)

/*
 * The FreeBSD kernel. It is freestanding like the Linux kernel above - built
 * with -ffreestanding -nostdinc, so none of the C library headers below
 * exist - and it is not JENT_BAREMETAL either (see there): the arch/ backends
 * select the kernel's own interfaces on _KERNEL && __FreeBSD__. What the
 * library needs of it is what the hosted headers below provide elsewhere, and
 * <sys/systm.h> brings in the libkern string and memory helpers.
 */
#include <sys/param.h>
#include <sys/types.h>		/* uintN_t, size_t, ssize_t */
#include <sys/systm.h>		/* memcpy(), memset(), strlen(), snprintf() */
#include <sys/errno.h>
#include <sys/limits.h>
#include <sys/stdint.h>		/* UINTn_MAX, UINTn_C() */

#else /* LINUX_KERNEL */

/*
 * Set the following defines as needed for your environment
 * Compilation for AWS-LC     #define AWSLC
 * Compilation for libgcrypt  #define LIBGCRYPT
 * Compilation for OpenSSL    #define OPENSSL
 */

#include <limits.h>
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(_MSC_VER) || defined(__MINGW32__)
/*
 * Note there is deliberately no <windows.h> here, for the same reason as the
 * Mach/Apple note below: this is the installed public header, so everything it
 * includes becomes part of every consumer's translation unit - and <windows.h>
 * brings in the whole Win32 API along with its min()/max() macros, which
 * collide with ordinary C++ and C identifiers unless the consumer thinks to
 * define NOMINMAX first. Nothing in the API declared below needs it. The
 * places that do call Win32 (the sources under arch/) include it themselves,
 * each after selecting the API level it requires.
 *
 * ssize_t is the signed counterpart of size_t and has to match its width:
 * intptr_t is that type on Windows (the same choice the SDK makes for its own
 * SSIZE_T alias of LONG_PTR). A hard-coded int64_t was 64 bits wide even in
 * 32-bit builds, where it made jent_read_entropy() derive its clamp by
 * shifting a 32-bit size_t by 63 - undefined behavior, diagnosed by MSVC as
 * C4293 and silently reduced to a shift by 31.
 *
 * The guard leaves an existing definition alone: MinGW's <sys/types.h>
 * provides ssize_t as long / long long, which is a different type to the
 * compiler even where it is the same width, so redefining it is an error.
 */
# ifndef _SSIZE_T_DEFINED
#  define _SSIZE_T_DEFINED
typedef intptr_t ssize_t;
# endif
#else
# include <sys/types.h>
# include <sys/stat.h>
# include <fcntl.h>
# include <unistd.h>
#endif

/*
 * Note there is deliberately no Mach/Apple block here. This is the installed
 * public header, so anything pulled in becomes part of every consumer's
 * translation unit. The Mach headers this used to include are not needed by
 * the API declared below, <unistd.h> is already covered above, and
 * <CoreServices/CoreServices.h> in particular is a large umbrella framework
 * that no part of the library references - and one that does not exist in the
 * iOS/tvOS/watchOS SDKs, all of which define __MACH__. The one place that
 * does need Mach interfaces (arch/jitterentropy-arch-thread.c) includes
 * exactly what it uses itself.
 */

#endif /* LINUX_KERNEL */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * API / ABI incompatible changes, functional changes that require consumer to
 * be updated (as long as this number is zero, the API is not considered stable
 * and can change without a bump of the major version).
 */
#define JENT_MAJVERSION 3

/*
 * API compatible, ABI may change, functional enhancements only, consumer can be
 * left unchanged if enhancements are not considered.
 */
#define JENT_MINVERSION 8

/*
 * API / ABI compatible, no functional changes, no enhancements, bug fixes only.
 * Also, the entropy collection is not changed in any way that would necessitate
 * a re-assessment.
 */
#define JENT_PATCHLEVEL 0

#define JENT_VERSION (JENT_MAJVERSION * 1000000 + \
		      JENT_MINVERSION * 10000 + \
		      JENT_PATCHLEVEL * 100)

/* -- BEGIN Main interface functions -- */
/* Flags that can be used to initialize the RNG */
#define JENT_DISABLE_STIR (1<<0) 	/* UNUSED */
#define JENT_DISABLE_UNBIAS (1<<1) 	/* UNUSED */
#define JENT_DISABLE_MEMORY_ACCESS (1<<2) /* Disable memory access for more
					     entropy, saves MEMORY_SIZE RAM for
					     entropy collector */
#define JENT_FORCE_INTERNAL_TIMER (1<<3)  /* Force the use of the internal
					     timer */
#define JENT_DISABLE_INTERNAL_TIMER (1<<4)  /* Disable the potential use of
					       the internal timer. */
#define JENT_FORCE_FIPS (1<<5)		  /* Force FIPS compliant mode
					     including full SP800-90B
					     compliance. */
#define JENT_NTG1 (1<<6) /* AIS 20/31 NTG.1 compliance */
#define JENT_CACHE_ALL (1<<7) /* Shall size of all caches be used to
				 automatically determine the memory size for the
				 memory access? By default it is only the L1
				 cache size. */
#define JENT_FORCE_SECURE_MEM (1<<8) /* Secure memory - zeroized on free -
				   is what every build provides. Locking it
				   into RAM and excluding it from core dumps
				   are extras some platforms add. This flag
				   requires those extras for the state of the
				   entropy collector - all of it but the
				   memory access region, which is never
				   locked: fail the allocation when the
				   platform offers them but does not grant
				   them - a memory lock the operating system
				   refuses, or a secure memory arena that the
				   application did not configure (libgcrypt,
				   OpenSSL) - instead of continuing with memory
				   that may be written to swap. The extras are
				   always attempted; this flag only turns a
				   refusal into an error. Without effect where
				   the platform offers no extras (AWS-LC, no
				   memory lock). It is implied by JENT_NTG1 and
				   JENT_FORCE_FIPS, and by the system's FIPS
				   mode. */

#if defined(LINUX_KERNEL) && !defined(UINT32_C)
#define UINT32_C(c)	c ## U
#endif

/* Flags field limiting the amount of memory to be used for memory access */
#define JENT_FLAGS_TO_MEMSIZE_SHIFT	27
#define JENT_FLAGS_TO_MAX_MEMSIZE(val)	((val) >> JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_TO_FLAGS(val)	((val) << JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_1kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 1))
#define JENT_MAX_MEMSIZE_2kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 2))
#define JENT_MAX_MEMSIZE_4kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 3))
#define JENT_MAX_MEMSIZE_8kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 4))
#define JENT_MAX_MEMSIZE_16kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 5))
#define JENT_MAX_MEMSIZE_32kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 6))
#define JENT_MAX_MEMSIZE_64kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 7))
#define JENT_MAX_MEMSIZE_128kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 8))
#define JENT_MAX_MEMSIZE_256kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C( 9))
#define JENT_MAX_MEMSIZE_512kB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(10))
#define JENT_MAX_MEMSIZE_1MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(11))
#define JENT_MAX_MEMSIZE_2MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(12))
#define JENT_MAX_MEMSIZE_4MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(13))
#define JENT_MAX_MEMSIZE_8MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(14))
#define JENT_MAX_MEMSIZE_16MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(15))
#define JENT_MAX_MEMSIZE_32MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(16))
#define JENT_MAX_MEMSIZE_64MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(17))
#define JENT_MAX_MEMSIZE_128MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(18))
#define JENT_MAX_MEMSIZE_256MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(19))
#define JENT_MAX_MEMSIZE_512MB		JENT_MAX_MEMSIZE_TO_FLAGS(UINT32_C(20))
#define JENT_MAX_MEMSIZE_MAX		JENT_MAX_MEMSIZE_512MB
#define JENT_MAX_MEMSIZE_MASK		JENT_MAX_MEMSIZE_TO_FLAGS(0xffffffff)
/*
 * We start at 1kB -> offset is log2(1024) - 1 as the flag value above is added
 * to this offset.
 */
#define JENT_MAX_MEMSIZE_OFFSET		9

/*
 * Flags field defining the hash loop: field value n selects 2^(n - 1) loops,
 * and 0 - no JENT_HASHLOOP_* flag - the built-in default. JENT_HASHLOOP_1 is
 * one loop, not the absence of the flag, which is why the field starts at 1.
 */
#define JENT_FLAGS_TO_HASHLOOP_SHIFT	23
#define JENT_HASHLOOP_TO_FLAGS(val)	((val) << JENT_FLAGS_TO_HASHLOOP_SHIFT)
#define JENT_MAX_HASHLOOP_MASK		JENT_HASHLOOP_TO_FLAGS(0xf)
#define JENT_FLAGS_TO_HASHLOOP(val)	(((val) >> JENT_FLAGS_TO_HASHLOOP_SHIFT)\
					 & 0xf)
#define JENT_HASHLOOP_1			JENT_HASHLOOP_TO_FLAGS(UINT32_C(1))
#define JENT_HASHLOOP_2			JENT_HASHLOOP_TO_FLAGS(UINT32_C(2))
#define JENT_HASHLOOP_4			JENT_HASHLOOP_TO_FLAGS(UINT32_C(3))
#define JENT_HASHLOOP_8			JENT_HASHLOOP_TO_FLAGS(UINT32_C(4))
#define JENT_HASHLOOP_16		JENT_HASHLOOP_TO_FLAGS(UINT32_C(5))
#define JENT_HASHLOOP_32		JENT_HASHLOOP_TO_FLAGS(UINT32_C(6))
#define JENT_HASHLOOP_64		JENT_HASHLOOP_TO_FLAGS(UINT32_C(7))
#define JENT_HASHLOOP_128		JENT_HASHLOOP_TO_FLAGS(UINT32_C(8))
#define JENT_MAX_HASHLOOP		JENT_HASHLOOP_128

/*
 * Oversampling rate bounds. JENT_MIN_OSR is a floor, not a default: an
 * instance asked for a lower OSR - 0 included - runs at this one.
 *
 * During initial health tests or jent_read_entropy_safe, the RNG instance
 * may re-initialize with an incremented OSR, which stops at JENT_MAX_OSR
 * and returns a failure condition. Otherwise this would run "forever".
 * Not configurable: the health test cutoff tables hold one entry per OSR up
 * to JENT_MAX_OSR (tests/health/cutoffs.py).
 */
#define JENT_MIN_OSR	3
#define JENT_MAX_OSR	20

/*
 * JENT_PRIVATE_COMPILE:the sources are compiled as one translation unit into
 * a program or library of their own - a private copy - and every function of
 * the library is static there, the API through JENT_PRIVATE_STATIC and the
 * internals one source file calls in another through JENT_INTERNAL. Nothing of
 * the copy is then visible outside that translation unit, so it links beside
 * another copy, or beside libjitterentropy, without either seeing the other.
 * Marked unused, as a copy calls only part of the API.
 */
#ifdef JENT_PRIVATE_COMPILE
# if defined(__GNUC__)
#  define JENT_PRIVATE_STATIC static __attribute__((unused))
# else
#  define JENT_PRIVATE_STATIC static
# endif
#elif defined(LINUX_KERNEL)
# define JENT_PRIVATE_STATIC
#else /* JENT_PRIVATE_COMPILE */
#if defined(_WIN32)
/*
 * Windows has no visibility attribute; the linkage is chosen per translation
 * unit instead. This header is installed and therefore also read by consumers,
 * so it must not unconditionally say "dllexport": that is only correct while
 * the DLL itself is being compiled. A consumer that saw dllexport declared the
 * imported functions as if it were defining them, which makes MSVC fall back to
 * a thunked auto-import and makes MinGW warn outright.
 *
 * The build system defines JENT_BUILDING_DLL for the library's own translation
 * units (see CMakeLists.txt); consumers of a shared build get dllimport, and
 * consumers of a static build define JENT_STATIC_LIB to get neither.
 */
# if defined(JENT_STATIC_LIB)
#  define JENT_PRIVATE_STATIC
# elif defined(JENT_BUILDING_DLL)
#  define JENT_PRIVATE_STATIC __declspec(dllexport)
# else
#  define JENT_PRIVATE_STATIC __declspec(dllimport)
# endif
#else
#define JENT_PRIVATE_STATIC __attribute__((visibility("default")))
#endif
#endif

/* Internal, called across the library's source files; see above. */
#ifdef JENT_PRIVATE_COMPILE
# define JENT_INTERNAL JENT_PRIVATE_STATIC
#else
# define JENT_INTERNAL
#endif

/*
 * A build with no operating system behind it: an EFI application, a bootloader,
 * a firmware image. The Jitter RNG needs no OS - it reads a counter and hashes
 * what it measures - but every arch/ backend has to be told, because a
 * freestanding target is normally compiled by the host's own compiler and so
 * still announces the host: an EFI application built on Linux with
 * -ffreestanding defines __linux__ and __unix__ throughout, and without this
 * the backends would reach for mmap(), mlock(), sysconf(), sched_getaffinity()
 * and getrandom() on a machine that has none of them.
 *
 * -ffreestanding is what says so, and both GCC and Clang report it by setting
 * __STDC_HOSTED__ to zero, which is what the C standard defines the macro to
 * mean. Definable directly for a toolchain that does not, or to force the port
 * on a hosted one.
 *
 * The kernels are excluded because they are freestanding as well and have
 * backends of their own, which they select on their own macros.
 *
 * What such a build still expects from its integrator is six functions - the
 * ones a freestanding C implementation does not provide and the compiler may
 * emit calls to regardless: memcpy(), memset(), malloc(), free(), strlen() and
 * snprintf(). tests/efi supplies exactly those, on top of the EFI boot
 * services, and is what keeps this path building and running.
 *
 * One flag goes with them on aarch64: -mno-outline-atomics. GCC 10 and later
 * default to the opposite, which turns an atomic read-modify-write into a call
 * to a libgcc helper that a freestanding link does not have. The library needs
 * the flag: every startup sets the common timer divisor with
 * jent_atomic_cmpxchg_u32(), and jent_uuid_from_counter() increments the
 * process-wide counter it derives an instance identifier from with
 * jent_atomic_inc_u32() - precisely the path taken where no CSPRNG answers,
 * the normal EFI and baremetal case. Without the flag such a build fails to
 * link on an undefined __aarch64_cas4_* or __aarch64_ldadd4_*. The kernel
 * passes the same flag for the same reason.
 *
 * A core with no atomic instructions at all - ARMv6-M, RISC-V without the A
 * extension - is not supported: the library requires lock-free 32-bit atomics,
 * and arch/jitterentropy-arch-atomic.c fails to compile without them.
 *
 * The time stamp is read from a counter instruction on x86, aarch64, PowerPC,
 * s390x, SPARC64, RISC-V and LoongArch (arch/jitterentropy-arch-timer.c). Any
 * other architecture has no clock the library could read without an operating
 * system, and fails to compile unless JENT_CONF_ENABLE_INTERNAL_TIMER is set:
 * the internal timer then takes over, and the counting thread it needs comes
 * from a handler the integrator registers with
 * jent_entropy_switch_notime_impl().
 */
#if !defined(JENT_BAREMETAL) &&						       \
    !defined(LINUX_KERNEL) && !defined(__KERNEL__) &&			       \
    !(defined(_KERNEL) && defined(__FreeBSD__)) &&			       \
    defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0)
# define JENT_BAREMETAL
#endif

/*
 * Threading back-end for the internal timer.
 */
/*
 * Only for a hosted build. The environments with a threading back-end of their
 * own have to be excluded here, or this picks one they do not have and the
 * jent_notime_start_routine typedef below - which keys off these same macros -
 * ends up disagreeing with the back-end that is actually compiled: the
 * freestanding one takes int (*)(void *), the pthread one void *(*)(void *),
 * and struct jent_notime_thread then declares a start member of the wrong
 * type. That is a build failure for the library and, worse, the wrong
 * signature in the public struct for the consumer registering a handler
 * through jent_entropy_switch_notime_impl() - on exactly the target where the
 * builtin back-end always refuses, so registering one is mandatory.
 *
 * JENT_BAREMETAL is defined a few lines above, so it is already known here;
 * the FreeBSD kernel is spelled out as its own case, as it is elsewhere - the
 * arch/ backends each carry a _KERNEL && __FreeBSD__ branch of their own.
 */
#if !defined(JENT_PTHREAD) && !defined(JENT_WIN_THREADS) &&		       \
    !defined(LINUX_KERNEL) && !defined(__KERNEL__) &&			       \
    !(defined(_KERNEL) && defined(__FreeBSD__)) &&			       \
    !defined(JENT_BAREMETAL)
# if defined(_MSC_VER) || defined(__MINGW32__)
#  define JENT_WIN_THREADS
# else
#  define JENT_PTHREAD
# endif
#endif

#ifdef JENT_CONF_ENABLE_INTERNAL_TIMER
#if defined(__KERNEL__) || defined(LINUX_KERNEL)
	/*
	 * Match both the kernel's own __KERNEL__ and the build-system macro
	 * LINUX_KERNEL used by every other arch file, so a TU compiled with
	 * only one of them cannot pair the kernel memory backend with the
	 * hosted thread backend.
	 */
# define JENT_ARCH_THREAD_LINUX_KERNEL
#elif defined(_KERNEL) && defined(__FreeBSD__)
# define JENT_ARCH_THREAD_FREEBSD_KERNEL
#elif defined(JENT_BAREMETAL)
# define JENT_ARCH_THREAD_BAREMETAL
#else
# define JENT_ARCH_THREAD_HOSTED
#endif

#if defined(JENT_ARCH_THREAD_HOSTED)

#if defined(JENT_PTHREAD)
# include <pthread.h>
typedef void *(*jent_notime_start_routine)(void *);
#elif defined(JENT_WIN_THREADS)
typedef int (*jent_notime_start_routine)(void *);
#else
# error "no threading back-end selected: build with -DJENT_PTHREAD or -DJENT_WIN_THREADS"
#endif

struct jent_notime_ctx {
#if defined(JENT_PTHREAD)
	pthread_attr_t notime_pthread_attr;	/* pthreads library */
	pthread_t notime_thread_id;		/* pthreads thread ID */
#else /* JENT_WIN_THREADS */
	void *notime_thread_id;			/* Win32 thread HANDLE */
	jent_notime_start_routine notime_routine; /* what the thread runs */
	void *notime_arg;			/* its argument */
#endif
	unsigned long notime_cpu;		/* CPU the thread pins to */
	int notime_thread_started;		/* thread successfully created? */
};

#else /* freestanding: LINUX_KERNEL / FREEBSD_KERNEL / BAREMETAL */

struct jent_notime_ctx {
	unsigned long notime_cpu;		/* CPU the thread pins to */
};

typedef int (*jent_notime_start_routine)(void *);

#endif /* JENT_ARCH_THREAD_HOSTED */
#endif /* JENT_CONF_ENABLE_INTERNAL_TIMER */

/* Forward declaration of opaque value */
struct rand_data;

/*
 * Thread safety - the library takes no locks.
 *
 * - One entropy collector belongs to one thread at a time: jent_read_entropy,
 *   jent_read_entropy_safe and jent_status access its state unsynchronized.
 *   Separate collectors are independent.
 * - jent_entropy_set_notime_cpu, jent_entropy_switch_notime_impl and
 *   jent_set_fips_failure_callback must be called before the first
 *   jent_entropy_init* and before any thread generates; afterwards they
 *   return -EAGAIN.
 * - jent_entropy_init and jent_entropy_init_ex may run on several threads at
 *   once.
 * - jent_selftest is reentrant and may run in parallel with jent_read_entropy,
 *   but not with jent_read_entropy_safe on the same collector: its recovery
 *   frees the collector the verdict would be written to. See below.
 */

/* Number of low bits of the time value that we want to consider */
/* get raw entropy */
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy(struct rand_data *ec, char *data, size_t len);
JENT_PRIVATE_STATIC
ssize_t jent_read_entropy_safe(struct rand_data **ec, char *data, size_t len);
/* initialize an instance of the entropy collector */
JENT_PRIVATE_STATIC
struct rand_data *jent_entropy_collector_alloc(unsigned int osr,
	       				       unsigned int flags);
/* clearing of entropy collector */
JENT_PRIVATE_STATIC
void jent_entropy_collector_free(struct rand_data *entropy_collector);

/* initialization of entropy collector */
JENT_PRIVATE_STATIC
int jent_entropy_init(void);
JENT_PRIVATE_STATIC
int jent_entropy_init_ex(unsigned int osr, unsigned int flags);

/*
 * Run the known answer tests of the conditioning component: SHA3-256,
 * SHAKE-256 and XDRBG-256. jent_entropy_init* performs them before anything
 * else; they are offered separately for callers that must repeat them over the
 * lifetime of a long-running process.
 *
 * They run on stack-local state alone: callable at any time, from any thread,
 * in parallel with jent_read_entropy, allocating nothing and never blocking.
 * Not in parallel with jent_read_entropy_safe on the same instance, though:
 * its recovery frees the instance and replaces it, and a verdict bound to the
 * old pointer would then be written into freed memory.
 *
 * ec binds the verdict to an instance: on failure that instance stops
 * producing output - jent_read_entropy and jent_read_entropy_safe return
 * JENT_ERR_SELFTEST, in every mode, not only under FIPS - until a later run
 * bound to it passes. ec may be NULL to obtain the verdict without binding it
 * to an instance.
 * Returns 0, or EHASH on failure as jent_entropy_init* does.
 */
JENT_PRIVATE_STATIC
int jent_selftest(struct rand_data *ec);

/*
 * Set a callback to run on health failure in FIPS mode.
 * This function will take an action determined by the caller.
 */
typedef void (*jent_fips_failure_cb)(struct rand_data *ec,
				     unsigned int health_failure);
JENT_PRIVATE_STATIC
int jent_set_fips_failure_callback(jent_fips_failure_cb cb);

/* return version number of core library */
JENT_PRIVATE_STATIC
unsigned int jent_version(void);

/* print out human-readable status of the Jitter RNG (JSON) */
JENT_PRIVATE_STATIC
int jent_status(const struct rand_data *ec, char *buf, size_t buflen);

/* Length of the canonical UUID string "8-4-4-4-12" including the NUL. */
#ifndef JENT_UUID_STRLEN
# define JENT_UUID_STRLEN 37
#endif

/*
 * Copy the instance UUID string (RFC 9562 version 4, or 8 without a CSPRNG;
 * JENT_UUID_STRLEN bytes including the terminating NUL) into buf. Returns 0 on
 * success, -1 on error.
 */
JENT_PRIVATE_STATIC
int jent_entropy_collector_uuid(const struct rand_data *ec, char *buf,
				size_t buflen);

/*
 * Read-only accessors of an instance's settings and state, the numbers
 * jent_status reports without its JSON. They follow the instance through the
 * reallocations after a health test failure: the osr is at least JENT_MIN_OSR
 * and raised on each, as is the memory size in effect where it is derived -
 * one the caller set stays - and the hash loop count, whether set by the
 * caller or not, up to JENT_MAX_HASHLOOP (128); a compile-time default above
 * that stays as it is. Same rules as jent_status: the thread that owns the
 * instance, or with generation stopped.
 *
 * Each returns 0 for a NULL ec and sets errno to EINVAL, except in a kernel or
 * a JENT_BAREMETAL build, which have no errno; errno is left alone otherwise.
 * No instance reports 0 as its osr.
 *
 * jent_entropy_collector_osr: the oversampling rate.
 * jent_entropy_collector_flags: the JENT_* flags in effect: the memory size
 *	and hash loop fields set to the values in effect (the memory size field
 *	as given with JENT_DISABLE_MEMORY_ACCESS), JENT_FORCE_SECURE_MEM added
 *	where implied, JENT_DISABLE_INTERNAL_TIMER under JENT_NTG1, and after a
 *	reallocation in FIPS mode the timer flag of the clock in use.
 * jent_entropy_collector_memsize: the bytes of the memory access region, 0
 *	without.
 * jent_entropy_collector_health_failure: the JENT_*_FAILURE bits standing, 0
 *	if none. Set in every mode, but only in FIPS mode (JENT_NTG1 implies
 *	it) does a failure stop the output; otherwise they are informational.
 * jent_entropy_collector_reinitializations: reallocations after an intermittent
 *	health test failure, each replacement counted - those of
 *	jent_read_entropy_safe and of the startup every allocation runs.
 * jent_entropy_collector_read_invocations: the successful reads of the
 *	instance.
 * jent_entropy_collector_bytes_output: the bytes those reads delivered.
 * jent_entropy_collector_hashloops: the hash loop count per generated block.
 *
 * The read and byte counters carry over into a recovery's replacement, as the
 * UUID does.
 */
JENT_PRIVATE_STATIC
unsigned int jent_entropy_collector_osr(const struct rand_data *ec);
JENT_PRIVATE_STATIC
unsigned int jent_entropy_collector_flags(const struct rand_data *ec);
JENT_PRIVATE_STATIC
size_t jent_entropy_collector_memsize(const struct rand_data *ec);
JENT_PRIVATE_STATIC
unsigned int jent_entropy_collector_health_failure(const struct rand_data *ec);
JENT_PRIVATE_STATIC
unsigned int jent_entropy_collector_reinitializations(
	const struct rand_data *ec);
JENT_PRIVATE_STATIC
uint64_t jent_entropy_collector_read_invocations(const struct rand_data *ec);
JENT_PRIVATE_STATIC
uint64_t jent_entropy_collector_bytes_output(const struct rand_data *ec);
JENT_PRIVATE_STATIC
unsigned int jent_entropy_collector_hashloops(const struct rand_data *ec);

/* return secure memory support - memory zeroized on free, which every build
 * provides, so this returns 1; locking and core dump exclusion are extras it
 * does not report. Must be done in jitterentropy itself, as users may not
 * define a crypto library and so the define in
 * arch/jitterentropy-arch-memory.h is not set for them. */
JENT_PRIVATE_STATIC
int jent_secure_memory_supported(void);

/**
 * Function pointer data structure to register an external thread handler
 * used for the timer-less mode of the Jitter RNG.
 *
 * The external caller provides these function pointers to handle the
 * management of the timer thread that is spawned by the Jitter RNG.
 *
 * @var jent_notime_init This function is intended to initialize the threading
 *	support. All data that is required by the threading code must be
 *	held in the data structure ctx. The Jitter RNG maintains the
 *	data structure and uses it for every invocation of the following calls.
 *
 * @var jent_notime_fini This function shall terminate the threading support.
 *	The function must dispose of all memory and resources used for the
 *	threading operation. It must also dispose of the ctx memory. It is
 *	only called with a non-NULL ctx, the one a successful init stored: a
 *	collector that never enabled the timer-less mode, or whose init
 *	failed, is released without it - as is one whose init succeeded but
 *	left ctx NULL, so such an init must not hold resources fini would
 *	release.
 *
 * @var jent_notime_start This function is called when the Jitter RNG wants
 *	to start a thread. Besides providing a pointer to the ctx
 *	allocated during initialization time, the Jitter RNG provides a
 *	pointer to the function the thread shall execute and the argument
 *	the function shall be invoked with. These two parameters have the
 *	same purpose as the trailing two parameters of pthread_create(3).
 *
 * @var jent_notime_stop This function is invoked by the Jitter RNG when the
 *	thread should be stopped. Note, the Jitter RNG intends to start/stop
 *	the thread frequently. It is called exactly once for each start that
 *	returned success, and never for one that failed, so it may assume the
 *	thread its start recorded exists - it is not called to clean up after
 *	a start that did not create one.
 *
 * An example implementation is found in the Jitter RNG itself with its
 * default thread handler of jent_notime_thread_builtin.
 *
 * If the caller wants to register its own thread handler, it must be done
 * with the API call jent_entropy_switch_notime_impl as the first
 * call to interact with the Jitter RNG, even before jent_entropy_init.
 * After jent_entropy_init is called, changing of the threading implementation
 * is not allowed. The handler is copied at registration: the struct passed
 * need not outlive the call, and later changes to it have no effect.
 */
struct jent_notime_thread {
	int (*jent_notime_init)(void **ctx);
	void (*jent_notime_fini)(void *ctx);
	int (*jent_notime_start)(void *ctx,
#ifdef JENT_PTHREAD
		void *(*start_routine) (void *), void *arg);
#else
		int (*start_routine)(void *), void *arg);
#endif
	void (*jent_notime_stop)(void *ctx);
};

/* Set a different thread handling logic for the notimer support */
JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread);

/*
 * Pin the timer-less counting thread to the given logical CPU index.
 *
 * This must be called before the library is initialized (i.e. before
 * jent_entropy_init*); afterwards it returns -EAGAIN and has no effect.
 * When unset, the counting thread defaults to the highest-numbered CPU in the
 * affinity set of the thread that starts it - the caller's, which a cpuset or
 * job object may confine - rather than the highest online CPU. Outside Linux
 * and Windows, where no affinity set can be read, it is the CPU count minus
 * one. Pinning itself is best-effort: an out-of-range index or a platform
 * without affinity support does not stop the internal timer from working.
 *
 * Not every platform can honour the CPU index. OpenBSD exposes no
 * thread-to-CPU affinity API, and macOS only has affinity *tags*, which hint
 * that threads should share an L2 cache rather than naming a CPU - on Apple
 * Silicon even those are rejected by the kernel. On such systems the index is
 * accepted and recorded but has no effect on placement.
 *
 * Returns 0 on success or a negative errno on failure, -EOPNOTSUPP as does
 * jent_entropy_switch_notime_impl() without the internal timer compiled in.
 */
JENT_PRIVATE_STATIC
int jent_entropy_set_notime_cpu(unsigned long cpu);
/* -- END of Main interface functions -- */

/* -- BEGIN timer-less threading support functions to prevent code dupes -- */
JENT_PRIVATE_STATIC
int jent_notime_init(void **ctx);

JENT_PRIVATE_STATIC
void jent_notime_fini(void *ctx);
/* -- END timer-less threading support functions to prevent code dupes -- */

/* -- BEGIN error codes for init function -- */
#define ENOTIME  	1 /* Timer service not available */
#define ECOARSETIME	2 /* Timer too coarse for RNG */
#define ENOMONOTONIC	3 /* Timer is not monotonic increasing */
#define EMINVARIATION	4 /* UNUSED - Timer variations too small for RNG */
#define EVARVAR		5 /* UNUSED - Timer does not produce variations of
			     variations (2nd derivation of time is zero) */
#define EMINVARVAR	6 /* Timer variations of variations is too small */
#define EPROGERR	7 /* Invalid argument, e.g. an osr above JENT_MAX_OSR */
#define ESTUCK		8 /* Too many stuck results during init. */
#define EHEALTH		9 /* Health test failed during initialization */
#define ERCT		10 /* RCT failed during initialization */
#define EHASH		11 /* Hash self test failed */
#define EMEM		12 /* Can't allocate memory for initialization */
#define EGCD		13 /* GCD self-test failed */
/* -- END error codes for init function -- */

/* -- BEGIN error codes for jent_read_entropy / jent_read_entropy_safe -- */
/*
 * Both functions return the number of generated bytes on success and one of
 * the following negative values on error. All health test failures leave the
 * entropy collector in an error state and produce no output data.
 */
#define JENT_ERR_EINVAL		(-1) /* API misuse: no entropy collector or
					no data buffer for a non-zero length */
#define JENT_ERR_RCT		(-2) /* Intermittent RCT failure */
#define JENT_ERR_APT		(-3) /* Intermittent APT failure */
#define JENT_ERR_NOTIME		(-4) /* The timer cannot be initialized */
#define JENT_ERR_LAG		(-5) /* Intermittent Lag predictor failure */
#define JENT_ERR_RCT_PERMANENT	(-6) /* Permanent RCT failure */
#define JENT_ERR_APT_PERMANENT	(-7) /* Permanent APT failure */
#define JENT_ERR_LAG_PERMANENT	(-8) /* Permanent Lag predictor failure */
#define JENT_ERR_RCT_MEM	(-9) /* Intermittent RCT with memory failure */
#define JENT_ERR_RCT_MEM_PERMANENT (-10) /* Permanent RCT with memory
					    failure */
#define JENT_ERR_SELFTEST	(-11) /* A jent_selftest run bound to this
					 instance failed; a later passing
					 bound run clears it */
/* -- END error codes for jent_read_entropy / jent_read_entropy_safe -- */

/* -- BEGIN error masks for health tests -- */
#define JENT_RCT_FAILURE	1 /* Failure in RCT health test. */
#define JENT_APT_FAILURE	2 /* Failure in APT health test. */
#define JENT_LAG_FAILURE	4 /* Failure in Lag predictor health test. */
#define JENT_RCT_MEM_FAILURE	8 /* Failure in RCT with memory health test. */
#define JENT_PERMANENT_FAILURE_SHIFT	16
#define JENT_PERMANENT_FAILURE(x)	((x) << JENT_PERMANENT_FAILURE_SHIFT)
#define JENT_RCT_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_RCT_FAILURE)
#define JENT_APT_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_APT_FAILURE)
#define JENT_LAG_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_LAG_FAILURE)
#define JENT_RCT_MEM_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_RCT_MEM_FAILURE)
/* -- END error masks for health tests -- */

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_H */
