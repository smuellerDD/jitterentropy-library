/*
 * The shared libjitterentropy-kernel exports the API of jitterentropy-kernel.h
 * and nothing else.
 *
 * An anonymous version tag, not a named node, as version.lds: this restricts
 * the exports without versioning the symbols. CMakeLists.txt checks the list
 * against the header at configure time.
 */
{
global:
	jent_kernel_close;
	jent_kernel_flags;
	jent_kernel_hashloops;
	jent_kernel_health_failure;
	jent_kernel_memsize;
	jent_kernel_open;
	jent_kernel_osr;
	jent_kernel_output;
	jent_kernel_proc;
	jent_kernel_read;
	jent_kernel_reinitializations;
	jent_kernel_selftest;
	jent_kernel_status;
	jent_kernel_uuid;
	jent_kernel_version;

local:
	*;
};
