/*
 * The shared libjitterentropy-record exports the API of jitterentropy-record.h
 * and nothing else - none of jitterentropy.h, whose functions it carries only
 * as its own static copy.
 *
 * An anonymous version tag, not a named node, as version.lds: this restricts
 * the exports without versioning the symbols. CMakeLists.txt checks the list
 * against the header at configure time.
 */
{
global:
	jent_record_alloc;
	jent_record_free;
	jent_record_health_names;
	jent_record_prime;
	jent_record_raw;
	jent_record_report_len;
	jent_record_run;
	jent_record_sample;

local:
	*;
};
