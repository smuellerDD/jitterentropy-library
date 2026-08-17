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
 * iOS/tvOS/watchOS SDKs, all of which define __MACH__. The few places that do
 * need Mach interfaces (arch/jitterentropy-arch-thread.c,
 * arch/jitterentropy-arch-timer.c) include exactly what they use themselves.
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
#define JENT_MINVERSION 7

/*
 * API / ABI compatible, no functional changes, no enhancements, bug fixes only.
 * Also, the entropy collection is not changed in any way that would necessitate
 * a re-assessment.
 */
#define JENT_PATCHLEVEL 1

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
#define JENT_FORCE_SECURE_MEM (1<<8) /* Require the memory of the entropy
				   collector to be secure memory: fail the
				   allocation when the platform does not grant
				   it - a memory lock the operating system
				   refuses, or a secure memory arena that the
				   application did not configure (libgcrypt,
				   OpenSSL) - instead of continuing with memory
				   that may be written to swap. Secure memory
				   is always attempted; this flag only turns a
				   refusal into an error. It is implied by
				   JENT_NTG1 and JENT_FORCE_FIPS. */

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

/* Flags field defining the hash loop */
#define JENT_FLAGS_TO_HASHLOOP_SHIFT	24
#define JENT_HASHLOOP_TO_FLAGS(val)	((val) << JENT_FLAGS_TO_HASHLOOP_SHIFT)
#define JENT_MAX_HASHLOOP_MASK		JENT_HASHLOOP_TO_FLAGS(0x7)
#define JENT_FLAGS_TO_HASHLOOP(val)	(((val) >> JENT_FLAGS_TO_HASHLOOP_SHIFT)\
					 & 0x7)
#define JENT_HASHLOOP_1			JENT_HASHLOOP_TO_FLAGS(UINT32_C(0))
#define JENT_HASHLOOP_2			JENT_HASHLOOP_TO_FLAGS(UINT32_C(1))
#define JENT_HASHLOOP_4			JENT_HASHLOOP_TO_FLAGS(UINT32_C(2))
#define JENT_HASHLOOP_8			JENT_HASHLOOP_TO_FLAGS(UINT32_C(3))
#define JENT_HASHLOOP_16		JENT_HASHLOOP_TO_FLAGS(UINT32_C(4))
#define JENT_HASHLOOP_32		JENT_HASHLOOP_TO_FLAGS(UINT32_C(5))
#define JENT_HASHLOOP_64		JENT_HASHLOOP_TO_FLAGS(UINT32_C(6))
#define JENT_HASHLOOP_128		JENT_HASHLOOP_TO_FLAGS(UINT32_C(7))
#define JENT_MAX_HASHLOOP		JENT_HASHLOOP_128

#ifdef JENT_PRIVATE_COMPILE
# define JENT_PRIVATE_STATIC static
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

/*
 * Threading back-end for the internal timer.
 */
#if !defined(JENT_PTHREAD) && !defined(JENT_WIN_THREADS) && \
    !defined(LINUX_KERNEL)
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
#elif defined(JENT_BAREMETAL) || \
      (defined(__STDC_HOSTED__) && (__STDC_HOSTED__ == 0))
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
 * Run the known answer tests of the conditioning component: SHA3-256 and
 * XDRBG-256. jent_entropy_init* performs them before anything else; they are
 * offered separately for callers that must repeat them over the lifetime of a
 * long-running process.
 *
 * They run on stack-local state alone: callable at any time, from any thread,
 * in parallel with entropy collection, allocating nothing and never blocking.
 * Returns 0, or EHASH on failure as jent_entropy_init* does.
 */
JENT_PRIVATE_STATIC
int jent_crypto_selftest(void);

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
 * Copy the instance UUID string (RFC 4122 version 4, JENT_UUID_STRLEN bytes
 * including the terminating NUL) into buf. Returns 0 on success, -1 on error.
 */
JENT_PRIVATE_STATIC
int jent_uuid(const struct rand_data *ec, char *buf, size_t buflen);

/* return secure memory support, must be done
 * in jitterentropy itself, as users may not define
 * a crypto library and so the define in arch/jitterentropy-arch-memory.h
 * is not set for them. */
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
 *	threading operation. It must also dispose of the ctx memory.
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
 *	the thread frequently.
 *
 * An example implementation is found in the Jitter RNG itself with its
 * default thread handler of jent_notime_thread_builtin.
 *
 * If the caller wants to register its own thread handler, it must be done
 * with the API call jent_entropy_switch_notime_impl as the first
 * call to interact with the Jitter RNG, even before jent_entropy_init.
 * After jent_entropy_init is called, changing of the threading implementation
 * is not allowed.
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
 * When unset, the counting thread defaults to the highest-numbered online
 * CPU. Pinning itself is best-effort: an out-of-range index or a platform
 * without affinity support does not stop the internal timer from working.
 *
 * Not every platform can honour the CPU index. OpenBSD exposes no
 * thread-to-CPU affinity API, and macOS only has affinity *tags*, which hint
 * that threads should share an L2 cache rather than naming a CPU - on Apple
 * Silicon even those are rejected by the kernel. On such systems the index is
 * accepted and recorded but has no effect on placement.
 *
 * Returns 0 on success or a negative errno on failure.
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
#define EPROGERR	7 /* UNUSED - Programming error */
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
