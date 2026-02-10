/*
 * Non-physical true random number generator based on timing jitter.
 *
 * Copyright Stephan Mueller <smueller@chronox.de>, 2014 - 2025
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

#if defined(_MSC_VER) || defined(__MINGW32__)
#include "arch/jitterentropy-base-windows.h"
#else
#include "jitterentropy-base-user.h"
#endif

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

/* Flags field limiting the amount of memory to be used for memory access */
#define JENT_FLAGS_TO_MEMSIZE_SHIFT	27
#define JENT_FLAGS_TO_MAX_MEMSIZE(val)	(val >> JENT_FLAGS_TO_MEMSIZE_SHIFT)
#define JENT_MAX_MEMSIZE_TO_FLAGS(val)	(val << JENT_FLAGS_TO_MEMSIZE_SHIFT)
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

#ifdef JENT_PRIVATE_COMPILE
# define JENT_PRIVATE_STATIC static
#else /* JENT_PRIVATE_COMPILE */
#if defined(_MSC_VER)
#define JENT_PRIVATE_STATIC __declspec(dllexport)
#else
#define JENT_PRIVATE_STATIC __attribute__((visibility("default")))
#endif
#endif

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

/* return secure memory support, must be done
 * in jitterentropy itself, as users may not define
 * a crypto library and so the define in jitterentropy-base-user.h
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
#ifdef __MINGW32__
		void *(*start_routine) (void *), void *arg);
#else
		int (*start_routine)(void *), void *arg);
#endif
	void (*jent_notime_stop)(void *ctx);
};

/* Set a different thread handling logic for the notimer support */
JENT_PRIVATE_STATIC
int jent_entropy_switch_notime_impl(struct jent_notime_thread *new_thread);
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
#define EMINVARIATION	4 /* Timer variations too small for RNG */
#define EVARVAR		5 /* Timer does not produce variations of variations
			     (2nd derivation of time is zero) */
#define EMINVARVAR	6 /* Timer variations of variations is too small */
#define EPROGERR	7 /* Programming error */
#define ESTUCK		8 /* Too many stuck results during init. */
#define EHEALTH		9 /* Health test failed during initialization */
#define ERCT		10 /* RCT failed during initialization */
#define EHASH		11 /* Hash self test failed */
#define EMEM		12 /* Can't allocate memory for initialization */
#define EGCD		13 /* GCD self-test failed */
/* -- END error codes for init function -- */

/* -- BEGIN error masks for health tests -- */
#define JENT_RCT_FAILURE	1 /* Failure in RCT health test. */
#define JENT_APT_FAILURE	2 /* Failure in APT health test. */
#define JENT_LAG_FAILURE	4 /* Failure in Lag predictor health test. */
#define JENT_PERMANENT_FAILURE_SHIFT	16
#define JENT_PERMANENT_FAILURE(x)	(x << JENT_PERMANENT_FAILURE_SHIFT)
#define JENT_RCT_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_RCT_FAILURE)
#define JENT_APT_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_APT_FAILURE)
#define JENT_LAG_FAILURE_PERMANENT	JENT_PERMANENT_FAILURE(JENT_LAG_FAILURE)
/* -- END error masks for health tests -- */

#ifdef __cplusplus
}
#endif

#endif /* _JITTERENTROPY_H */
