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
	snprintf(buf + used, buflen - used, "Configuration:\n");

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
		 "  JENT_DISABLE_MEMORY_ACCESS %u - Memory access %s\n",
		 !!(ec->flags & JENT_DISABLE_MEMORY_ACCESS),
		 ec->flags & JENT_DISABLE_MEMORY_ACCESS ? "disabled" :
							  "enabled");

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_FORCE_INTERNAL_TIMER %u - Internal timer %s\n",
		 !!(ec->flags & JENT_FORCE_INTERNAL_TIMER),
		 ec->flags & JENT_FORCE_INTERNAL_TIMER ? "forced" :
							 "not forced");

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_DISABLE_INTERNAL_TIMER %u - Internal timer %s\n",
		 !!(ec->flags & JENT_DISABLE_INTERNAL_TIMER),
		 ec->flags & JENT_DISABLE_INTERNAL_TIMER ? "disabled" :
							   "enabled");

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_FORCE_FIPS %u - FIPS modus %s\n",
		 !!(ec->flags & JENT_FORCE_FIPS),
		 ec->flags & JENT_FORCE_FIPS ? "forced" : "not forced");

	used = strlen(buf);
	snprintf(buf + used, buflen - used, "  JENT_NTG1 %u - NTG.1 modus %s\n",
		 !!(ec->flags & JENT_NTG1),
		 ec->flags & JENT_NTG1 ? "enabled" : "disabled");

	used = strlen(buf);
	snprintf(buf + used, buflen - used,
		 "  JENT_CACHE_ALL %u - Full cache size %sapplied\n",
		 !!(ec->flags & JENT_CACHE_ALL),
		 ec->flags & JENT_CACHE_ALL ? "" : "not ");

	return 0;
}
