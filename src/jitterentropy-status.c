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

#include <stdio.h>

#include "jitterentropy.h"
#include "jitterentropy-base.h"
#include "jitterentropy-internal.h"

int jent_status(const struct rand_data *ec, char *buf, size_t buflen)
{
	size_t used;

	snprintf(buf, buflen, "Jitter RNG version: %u.%u.%u\n\n",
		 JENT_MAJVERSION, JENT_MINVERSION, JENT_PATCHLEVEL);

	used = strlen(buf);
	snprintf(buf + used, buflen - used, "Health Status:\n");

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " APT:\n  intermittent: %s\n  permanent: %s\n",
		 ec->health_failure & JENT_APT_FAILURE ? "fail" : "pass",
		 ec->health_failure & JENT_APT_FAILURE_PERMANENT ? "fail" :
								   "pass");
	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " RCT:\n  intermittent: %s\n  permanent: %s\n",
		 ec->health_failure & JENT_RCT_FAILURE ? "fail" : "pass",
		 ec->health_failure & JENT_RCT_FAILURE_PERMANENT ? "fail" :
								   "pass");
#ifdef JENT_HEALTH_LAG_PREDICTOR
	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " LAG:\n  intermittent: %s\n  permanent: %s\n",
		 ec->health_failure & JENT_LAG_FAILURE ? "fail" : "pass",
		 ec->health_failure & JENT_LAG_FAILURE_PERMANENT ? "fail" :
								   "pass");
#endif

	used = strlen(buf);
	snprintf(buf + used, buflen - used, "Configuration:\n");

	used = strlen(buf);
	snprintf(buf + used, buflen - used, " OSR: %u\n", ec->osr);

	used = strlen(buf);
	snprintf(buf + used, buflen - used, " Memory Block Size: %u bytes\n",
		 jent_memsize(ec->flags));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " Detected Cache Sizes:\n  L1 %u bytes\n  all caches %u bytes\n",
		 jent_cache_size_roundup(0), jent_cache_size_roundup(1));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " Hash loop count:\n  runtime: %u\n  initialization: %u\n",
		 jent_hashloop_cnt(ec->flags),
		 jent_hashloop_cnt(ec->flags) * JENT_HASH_LOOP_INIT);

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " Memory Access loop count:\n  runtime: %u\n  initialization: %u\n",
		 ec->memaccessloops,
		 ec->memaccessloops * JENT_MEM_ACC_LOOP_INIT);

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " Secure Memory: %i\n", jent_secure_memory_supported());

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " Internal Timer: %i\n", ec->enable_notime);

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " FIPS mode: %i\n", ec->fips_enabled);

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " AIS 20/31 NTG.1 mode: %i\n", !!(ec->flags & JENT_NTG1));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 " CPU Cores: %ld\n",
		 jent_ncpu());

	used = strlen(buf);
	snprintf(buf + used, buflen - used, " Flags:\n");

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_DISABLE_MEMORY_ACCESS %u\n",
		 !!(ec->flags & JENT_DISABLE_MEMORY_ACCESS));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_FORCE_INTERNAL_TIMER %u\n",
		 !!(ec->flags & JENT_FORCE_INTERNAL_TIMER));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_DISABLE_INTERNAL_TIMER %u\n",
		 !!(ec->flags & JENT_DISABLE_INTERNAL_TIMER));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_FORCE_FIPS %u\n",
		 !!(ec->flags & JENT_FORCE_FIPS));

	used = strlen(buf);
	snprintf(buf + used, buflen - used, "  JENT_NTG1 %u\n",
		 !!(ec->flags & JENT_NTG1));

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_CACHE_ALL %u\n",
		 !!(ec->flags & JENT_CACHE_ALL));

	return 0;
}
