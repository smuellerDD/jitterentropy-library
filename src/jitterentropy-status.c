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

/*
 * Always validate the output with something like "jq -e .", when doing changes here.
 *
 * No JSON library is used here to keep dependencies slim and this is serialized only
 * here.
 */
int jent_status(const struct rand_data *ec, char *buf, size_t buflen)
{
	size_t used;

	#define jent_add_to_status(...)					\
	{								\
		used = strlen(buf);					\
		snprintf(buf + used, buflen - used, __VA_ARGS__);	\
	}

	/* needed as plain snprintf to make jent_add_to_status len calculation usable */
	snprintf(buf, buflen, "{\n");

	jent_add_to_status("\t\"version\": \"%u.%u.%u\",\n",
			   JENT_MAJVERSION, JENT_MINVERSION, JENT_PATCHLEVEL)

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
	 * runtime environment
	 */
	jent_add_to_status( "\t\"runtimeEnvironment\": {\n");

	jent_add_to_status("\t\t\"cpuCores\": %ld,\n", jent_ncpu());

	jent_add_to_status("\t\t\"cpuCache\": {\n");
	jent_add_to_status("\t\t\t\"l1Bytes\": %u,\n", jent_cache_size_roundup(0));
	jent_add_to_status("\t\t\t\"allBytes\": %u\n", jent_cache_size_roundup(1));
	jent_add_to_status("\t\t}\n");
	jent_add_to_status("\t},\n");

	/*
	 * configuration
	 */
	jent_add_to_status( "\t\"configuration\": {\n");

	jent_add_to_status( "\t\t\"osr\": %u,\n", ec->osr);
	jent_add_to_status( "\t\t\"memoryBlockSizeBytes\": %u,\n", jent_memsize(ec->flags));

	jent_add_to_status("\t\t\"hashLoopCount\": {\n");
	jent_add_to_status("\t\t\t\"runtime\": %u,\n", jent_hashloop_cnt(ec->flags));
	jent_add_to_status("\t\t\t\"initialization\": %u\n", jent_hashloop_cnt(ec->flags) * JENT_HASH_LOOP_INIT);
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"memoryLoopCount\": {\n");
	jent_add_to_status("\t\t\t\"runtime\": %u,\n", ec->memaccessloops);
	jent_add_to_status("\t\t\t\"initialization\": %u\n", ec->memaccessloops * JENT_MEM_ACC_LOOP_INIT);
	jent_add_to_status("\t\t},\n");

	jent_add_to_status("\t\t\"secureMemory\": %s,\n", jent_secure_memory_supported() ? "true" : "false");
	jent_add_to_status("\t\t\"internalTimer\": %s,\n", ec->enable_notime ? "true" : "false");
	jent_add_to_status("\t\t\"fipsMode\": %s,\n", ec->fips_enabled ? "true" : "false");
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
	jent_add_to_status("\t\t\t\"JENT_CACHE_ALL\": %s\n",
		 !!(ec->flags & JENT_CACHE_ALL) ? "true" : "false");
	jent_add_to_status("\t\t}\n");
	jent_add_to_status("\t}\n");

	jent_add_to_status("}\n");

	return 0;
#undef jent_add_to_status
}
