/*
 * Raw noise recording of the Jitter RNG, sample by sample
 *
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
 * The part of the recording that writes no file and needs no libc: included
 * by jitterentropy-record.c, and compiled on its own into the kernel module
 * for its debugfs test interface (linux_kernel/Kbuild.source).
 */

#include "jitterentropy-internal.h"
#include "jitterentropy-noise.h"
#include "jitterentropy-timer.h"

#include "jitterentropy-record.h"

typedef unsigned int (*jent_record_measure_t)(struct rand_data *ec,
					      uint64_t loop_cnt,
					      uint64_t *ret_current_delta,
					      int health);

struct jent_record {
	struct rand_data *ec;
	jent_record_measure_t measure_jitter;
};

/*
 * The measurement of @source, or NULL: no such source, or the memory access
 * loop without a memory region to walk. That one returns before it writes its
 * delta, and the recording would be all zeroes that look like a complete one
 * to an SP800-90B assessment.
 */
static jent_record_measure_t jent_record_measure(unsigned int source,
						 unsigned int flags)
{
	switch (source) {
	case JENT_RECORD_COMMON:
		return jent_measure_jitter;
	case JENT_RECORD_HASHLOOP:
		return jent_measure_jitter_ntg1_sha3;
	case JENT_RECORD_MEMACCESS:
		if (flags & JENT_DISABLE_MEMORY_ACCESS)
			return NULL;
		return jent_measure_jitter_ntg1_memaccess;
	default:
		return NULL;
	}
}

JENT_RECORD_API
int jent_record_alloc(struct jent_record **rec, unsigned int osr,
		      unsigned int flags, unsigned int source)
{
	jent_record_measure_t measure = jent_record_measure(source, flags);
	struct jent_record *r;
	int ret;

	if (!rec)
		return JENT_RECORD_EINVAL;
	*rec = NULL;

	/* Refused by the allocation as well, but as a NULL like no memory. */
	if (!measure || osr > JENT_MAX_OSR)
		return JENT_RECORD_EINVAL;

	r = jent_zalloc_unlocked(sizeof(*r));
	if (!r)
		return JENT_RECORD_ENOMEM;

	r->ec = jent_entropy_collector_alloc_raw(osr, flags);
	if (!r->ec) {
		/*
		 * Run the self tests again only to tell them from memory: the
		 * GCD self test allocates, and reports EMEM when it cannot.
		 */
		ret = jent_raw_selftest(flags);
		jent_zfree(r, sizeof(*r));
		return (ret && ret != EMEM) ? JENT_RECORD_ESELFTEST :
					      JENT_RECORD_ECOLLECTOR;
	}

	/* A timer thread that did not start would record a counter at rest. */
	if (r->ec->enable_notime && jent_notime_settick(r->ec)) {
		jent_entropy_collector_free(r->ec);
		jent_zfree(r, sizeof(*r));
		return JENT_RECORD_ECOLLECTOR;
	}

	/* Enable full SP800-90B health test handling */
	r->ec->is_fips_enabled = 1;
	r->measure_jitter = measure;
	*rec = r;

	return JENT_RECORD_OK;
}

JENT_RECORD_API
void jent_record_free(struct jent_record *rec)
{
	if (!rec)
		return;

	/* checks internally if timer was used, maybe NOOP */
	jent_notime_unsettick(rec->ec);
	jent_entropy_collector_free(rec->ec);
	jent_zfree(rec, sizeof(*rec));
}

JENT_RECORD_API
void jent_record_prime(struct jent_record *rec, unsigned int loopcnt)
{
	/* The NTG.1 hash-loop and memory-access variants prime themselves. */
	if (rec->measure_jitter == jent_measure_jitter)
		jent_measure_jitter(rec->ec, loopcnt, NULL, 0);
}

JENT_RECORD_API
uint64_t jent_record_sample(struct jent_record *rec, unsigned int loopcnt)
{
	uint64_t delta = 0;

	/* Disregard stuck indicator */
	rec->measure_jitter(rec->ec, loopcnt, &delta, 1);

	return delta;
}

#ifdef LINUX_KERNEL
struct rand_data *jent_record_collector(struct jent_record *rec)
{
	return rec->ec;
}
#endif
