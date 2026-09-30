/*
 * The shared library exports the API of jitterentropy.h and nothing else.
 * -fvisibility=hidden cannot express "visible to our own test programs but not
 * to the world", so jent_gcd_* used to be exported for no consumer's benefit.
 *
 * An anonymous version tag, not a named node: this restricts the exports
 * without versioning the symbols. CMakeLists.txt checks the list against the
 * header at configure time.
 */
{
global:
	jent_entropy_collector_alloc;
	jent_entropy_collector_bytes_output;
	jent_entropy_collector_flags;
	jent_entropy_collector_free;
	jent_entropy_collector_hashloops;
	jent_entropy_collector_health_failure;
	jent_entropy_collector_memsize;
	jent_entropy_collector_osr;
	jent_entropy_collector_read_invocations;
	jent_entropy_collector_reinitializations;
	jent_entropy_collector_uuid;
	jent_entropy_init;
	jent_entropy_init_ex;
	jent_entropy_set_notime_cpu;
	jent_entropy_switch_notime_impl;
	jent_notime_fini;
	jent_notime_init;
	jent_read_entropy;
	jent_read_entropy_safe;
	jent_secure_memory_supported;
	jent_selftest;
	jent_set_fips_failure_callback;
	jent_status;
	jent_version;

local:
	*;
};
