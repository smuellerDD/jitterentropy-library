/*
 * Copyright (C) 2026, Stephan Mueller <smueller@chronox.de>
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

#include "jitterentropy.h"
#include "jitterentropy-base.h"
#include "jitterentropy-internal.h"

#ifdef LINUX_KERNEL
#include <linux/kernel.h>
#include <linux/string.h>
#elif defined(_KERNEL) && defined(__FreeBSD__)
#include <sys/param.h>
#include <sys/systm.h>
#else
#include <stdio.h>
#endif

/*
 * Always validate the output with something like "jq -e .", when doing changes here.
 *
 * No JSON library is used here to keep dependencies slim and this is serialized only
 * here.
 */
int jent_status(const struct rand_data *ec, char *buf, size_t buflen)
{
	size_t used;
	int written, truncated = 0;

	if (!buf || buflen == 0)
		return -1;

	/* Truncation is taken from snprintf(), the length cannot tell it. */
	#define jent_add_to_status(...)					\
	{								\
		used = strlen(buf);					\
		if (used < buflen) {					\
			written = snprintf(buf + used, buflen - used,	\
					   __VA_ARGS__);		\
			if (written < 0 ||				\
			    (size_t)written >= buflen - used)		\
				truncated = 1;				\
		}							\
	}

	/* needed as plain snprintf to make jent_add_to_status len calculation usable */
	snprintf(buf, buflen, "{\n");

	jent_add_to_status("\t\"version\": \"%d.%d.%d\"",
			   JENT_MAJVERSION, JENT_MINVERSION, JENT_PATCHLEVEL)

	if (!ec) {
		/* No trailing comma before the closing brace. */
		jent_add_to_status("\n")
		goto out;
	}

	jent_add_to_status(",\n\t\"uuid\": \"%s\",\n", ec->uuid);

	jent_add_to_status("\t\"reinitializations\": %u,\n", ec->reinit_count);

	jent_add_to_status("\t\"output\": {\n");
	jent_add_to_status("\t\t\"invocations\": %llu,\n",
			   (unsigned long long)ec->read_invocations);
	jent_add_to_status("\t\t\"bytes\": %llu,\n",
			   (unsigned long long)ec->bytes_output);
	jent_add_to_status("\t\t\"bits\": %llu\n",
			   (unsigned long long)ec->bytes_output * 8);
	jent_add_to_status("\t},\n");

	/*
	 * health
	 */
	jent_add_to_status("\t\"healthFailure\": {\n");

	jent_add_to_status("\t\t\"apt\": {\n");
	jent_add_to_status("\t\t\t\"intermittent\": %s,\n",
			   ec->health_failure & JENT_APT_FAILURE ? "true" : "false");
	jent_add_to_status("\t\t\t\"permanent\": %s\n",
			   ec->health_failure & JENT_APT_FAILURE_PERMANENT ? "true" : "false");
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"rct\": {\n");
	jent_add_to_status("\t\t\t\"intermittent\": %s,\n",
			   ec->health_failure & JENT_RCT_FAILURE ? "true" : "false");
	jent_add_to_status("\t\t\t\"permanent\": %s\n",
			   ec->health_failure & JENT_RCT_FAILURE_PERMANENT ? "true" : "false");
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"rctMemory\": {\n");
	jent_add_to_status("\t\t\t\"intermittent\": %s,\n",
			   ec->health_failure & JENT_RCT_MEM_FAILURE ? "true" : "false");
	jent_add_to_status("\t\t\t\"permanent\": %s\n",
			   ec->health_failure & JENT_RCT_MEM_FAILURE_PERMANENT ? "true" : "false");
	jent_add_to_status("\t\t}");

#ifdef JENT_HEALTH_LAG_PREDICTOR
	jent_add_to_status(",\n");

	jent_add_to_status("\t\t\"lag\": {\n");
	jent_add_to_status("\t\t\t\"intermittent\": %s,\n",
			   ec->health_failure & JENT_LAG_FAILURE ? "true" : "false");
	jent_add_to_status("\t\t\t\"permanent\": %s\n",
			   ec->health_failure & JENT_LAG_FAILURE_PERMANENT ? "true" : "false");
	jent_add_to_status("\t\t}\n");
#else
	jent_add_to_status("\n");
#endif

	jent_add_to_status("\t},\n");

	/*
	 * Counters, window positions and cutoffs of the health tests. Nothing
	 * derived from a time delta - the APT base, the stuck test's deltas,
	 * the lag history - is reported.
	 */
	jent_add_to_status("\t\"healthTests\": {\n");

	jent_add_to_status("\t\t\"rct\": {\n");
	jent_add_to_status("\t\t\t\"count\": %u,\n", ec->rct_count);
	jent_add_to_status("\t\t\t\"cutoff\": %u,\n",
			   (unsigned int)ec->rct_cutoff);
	jent_add_to_status("\t\t\t\"cutoffPermanent\": %u\n",
			   (unsigned int)ec->rct_cutoff_permanent);
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"rctMemory\": {\n");
	jent_add_to_status("\t\t\t\"count\": %u,\n",
			   (unsigned int)ec->rct_mem_count);
	jent_add_to_status("\t\t\t\"observations\": %u,\n",
			   (unsigned int)ec->rct_mem_ctr);
	jent_add_to_status("\t\t\t\"windowSize\": %u,\n",
			   (unsigned int)ec->rct_mem_nosr);
	jent_add_to_status("\t\t\t\"cutoff\": %u,\n",
			   (unsigned int)ec->rct_mem_cutoff);
	jent_add_to_status("\t\t\t\"cutoffPermanent\": %u\n",
			   (unsigned int)ec->rct_mem_cutoff_permanent);
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"apt\": {\n");
	jent_add_to_status("\t\t\t\"count\": %u,\n", ec->apt_count);
	jent_add_to_status("\t\t\t\"observations\": %u,\n",
			   ec->apt_observations);
	jent_add_to_status("\t\t\t\"windowSize\": %u,\n",
			   (unsigned int)JENT_APT_WINDOW_SIZE);
	jent_add_to_status("\t\t\t\"cutoff\": %u,\n", ec->apt_cutoff);
	jent_add_to_status("\t\t\t\"cutoffPermanent\": %u\n",
			   ec->apt_cutoff_permanent);
	jent_add_to_status("\t\t}");

#ifdef JENT_HEALTH_LAG_PREDICTOR
	jent_add_to_status(",\n");

	jent_add_to_status("\t\t\"lag\": {\n");
	jent_add_to_status("\t\t\t\"successCount\": %u,\n",
			   ec->lag_prediction_success_count);
	jent_add_to_status("\t\t\t\"successRun\": %u,\n",
			   ec->lag_prediction_success_run);
	jent_add_to_status("\t\t\t\"observations\": %u,\n",
			   ec->lag_observations);
	jent_add_to_status("\t\t\t\"windowSize\": %u,\n",
			   (unsigned int)JENT_LAG_WINDOW_SIZE);
	jent_add_to_status("\t\t\t\"globalCutoff\": %u,\n",
			   ec->lag_global_cutoff);
	jent_add_to_status("\t\t\t\"globalCutoffPermanent\": %u,\n",
			   ec->lag_global_cutoff_permanent);
	jent_add_to_status("\t\t\t\"localCutoff\": %u,\n",
			   ec->lag_local_cutoff);
	jent_add_to_status("\t\t\t\"localCutoffPermanent\": %u\n",
			   ec->lag_local_cutoff_permanent);
	jent_add_to_status("\t\t}\n");
#else
	jent_add_to_status("\n");
#endif

	jent_add_to_status("\t},\n");

	jent_add_to_status("\t\"selftestFailed\": %s,\n",
			   jent_atomic_load_int(&ec->selftest_failed) ?
			   "true" : "false");

	/*
	 * runtime environment
	 */
	jent_add_to_status( "\t\"runtimeEnvironment\": {\n");

	jent_add_to_status("\t\t\"cpuCores\": %ld,\n", jent_ncpu());

	jent_add_to_status("\t\t\"cpuCache\": {\n");
	jent_add_to_status("\t\t\t\"l1Bytes\": %llu,\n",
			   (unsigned long long)jent_cache_size_roundup(0));
	jent_add_to_status("\t\t\t\"allBytes\": %llu\n",
			   (unsigned long long)jent_cache_size_roundup(1));
	jent_add_to_status("\t\t}\n");
	jent_add_to_status("\t},\n");

	/*
	 * configuration
	 */
	jent_add_to_status( "\t\"configuration\": {\n");

	jent_add_to_status( "\t\t\"osr\": %u,\n", ec->osr);
	jent_add_to_status( "\t\t\"osrMin\": %u,\n",
			   (unsigned int)JENT_MIN_OSR);
	jent_add_to_status( "\t\t\"osrMax\": %u,\n",
			   (unsigned int)JENT_MAX_OSR);
	jent_add_to_status( "\t\t\"memoryBlockSizeBytes\": %u,\n",
			   ec->memmask ? (unsigned int)(ec->memmask + 1) : 0);

	jent_add_to_status("\t\t\"hashLoopCount\": {\n");
	jent_add_to_status("\t\t\t\"runtime\": %u,\n", ec->hashloopcnt);
	jent_add_to_status("\t\t\t\"initialization\": %u\n", ec->hashloopcnt * JENT_HASH_LOOP_INIT);
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"memoryLoopCount\": {\n");
	jent_add_to_status("\t\t\t\"runtime\": %u,\n", ec->memaccessloops);
	jent_add_to_status("\t\t\t\"initialization\": %u\n", ec->memaccessloops * JENT_MEM_ACC_LOOP_INIT);
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"secureMemory\": %s,\n", jent_memory_is_secure(ec->flags) ? "true" : "false");
	jent_add_to_status("\t\t\"secureMemoryBackend\": \"%s\",\n", jent_memory_backend_name());
	jent_add_to_status("\t\t\"internalTimer\": %s,\n", ec->enable_notime ? "true" : "false");
#ifdef JENT_CONF_ENABLE_MOCK_TIMER
	jent_add_to_status("\t\t\"mockedTimerBuild\": true,\n");
	jent_add_to_status("\t\t\"mockedTimerActive\": %s,\n",
			   jent_mock_timer_active() ? "true" : "false");
#else
	jent_add_to_status("\t\t\"mockedTimerBuild\": false,\n");
#endif
	jent_add_to_status("\t\t\"fipsMode\": %s,\n", ec->is_fips_enabled ? "true" : "false");
	jent_add_to_status("\t\t\"ntg1Mode\": %s,\n", !!(ec->flags & JENT_NTG1) ? "true" : "false");

	jent_add_to_status("\t\t\"flags\": {\n");
	jent_add_to_status("\t\t\t\"JENT_DISABLE_MEMORY_ACCESS\": %s,\n",
		 !!(ec->flags & JENT_DISABLE_MEMORY_ACCESS) ? "true" : "false");
	jent_add_to_status("\t\t\t\"JENT_FORCE_INTERNAL_TIMER\": %s,\n",
		 !!(ec->flags & JENT_FORCE_INTERNAL_TIMER) ? "true" : "false");
	jent_add_to_status("\t\t\t\"JENT_DISABLE_INTERNAL_TIMER\": %s,\n",
		 !!(ec->flags & JENT_DISABLE_INTERNAL_TIMER) ? "true" : "false");
	jent_add_to_status("\t\t\t\"JENT_FORCE_FIPS\": %s,\n",
		 !!(ec->flags & JENT_FORCE_FIPS) ? "true" : "false");
	jent_add_to_status("\t\t\t\"JENT_NTG1\": %s,\n",
		 !!(ec->flags & JENT_NTG1) ? "true" : "false");
	jent_add_to_status("\t\t\t\"JENT_CACHE_ALL\": %s,\n",
		 !!(ec->flags & JENT_CACHE_ALL) ? "true" : "false");
	jent_add_to_status("\t\t\t\"JENT_FORCE_SECURE_MEM\": %s\n",
		 !!(ec->flags & JENT_FORCE_SECURE_MEM) ? "true" : "false");
	jent_add_to_status("\t\t}\n");
	jent_add_to_status("\t}\n");

out:
	jent_add_to_status("}\n");

	return truncated ? -1 : 0;
#undef jent_add_to_status
}

int jent_entropy_collector_uuid(const struct rand_data *ec, char *buf,
				size_t buflen)
{
	size_t len;

	if (!ec || !buf || buflen == 0)
		return -1;

	len = strlen(ec->uuid) + 1;
	if (buflen < len)
		return -1;

	memcpy(buf, ec->uuid, len);
	return 0;
}

/*
 * The value of every accessor for a NULL collector, with errno set to EINVAL
 * where there is one: not in a kernel, and not without a C library, where
 * errno would be a symbol nothing defines.
 */
static unsigned int jent_entropy_collector_null(void)
{
#if !defined(LINUX_KERNEL) && !(defined(_KERNEL) && defined(__FreeBSD__)) && \
    !defined(JENT_BAREMETAL)
	errno = EINVAL;
#endif
	return 0;
}

unsigned int jent_entropy_collector_osr(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->osr;
}

unsigned int jent_entropy_collector_flags(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->flags;
}

/* As jent_status reports it: no region, no mask. */
size_t jent_entropy_collector_memsize(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->memmask ? (size_t)ec->memmask + 1 : 0;
}

unsigned int jent_entropy_collector_health_failure(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->health_failure;
}

unsigned int
jent_entropy_collector_reinitializations(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->reinit_count;
}

uint64_t jent_entropy_collector_read_invocations(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->read_invocations;
}

uint64_t jent_entropy_collector_bytes_output(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->bytes_output;
}

unsigned int jent_entropy_collector_hashloops(const struct rand_data *ec)
{
	if (!ec)
		return jent_entropy_collector_null();
	return ec->hashloopcnt;
}
