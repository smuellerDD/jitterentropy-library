3.7.1-prerelease
 * Jitter RNG core: add jent_selftest to the API, running the SHA3-256 and XDRBG-256 known answer tests of the conditioning component on their own. jent_entropy_init* has always run them at startup; a long-running consumer such as the ESDM has to repeat them periodically, which so far meant re-running the whole startup including its statistical tests. The call is reentrant - stack-local state only, no allocation, no blocking - so it can run in parallel with entropy collection. The verdict can be bound to an entropy collector instance: on failure that instance permanently stops producing output, jent_read_entropy* returning the new JENT_ERR_SELFTEST error code in every mode of operation, not only under FIPS
 * Jitter RNG core: add JENT_ERR_* definitions for all error codes returned by jent_read_entropy and jent_read_entropy_safe - the numeric values are unchanged
 * Jitter RNG core: drop the enhanced backtracking operation at the end of jent_read_entropy, which ran on insecure memory only. Since the XDRBG-256 conversion in 3.7.0 every generated output block consumes the state one-way - the retained successor state cannot reproduce data already returned - so the extra empty generate defended nothing the construction does not already guarantee, and secure and insecure memory now behave identically
 * Jitter RNG core: fix the monotonicity check of jent_entropy_init*, which could not detect a timer running backwards. It compared the reading a measurement ended on against prev_time - delta, a reconstruction from an unsigned delta that jent_measure_jitter has already divided by the common timer divisor - so the comparison reduced to "delta > 0", which the coarseness check had already established. It now compares the readings the two measurements actually ended on, and ENOMONOTONIC is reachable
 * Jitter RNG core: fix reporting of a permanent RCT failure during jent_entropy_init* - it was reported as EHEALTH instead of ERCT
 * Health tests: extend the APT cutoff tables from osr 15 to JENT_MAX_OSR, which every other cutoff table already covered. Above osr 15 the APT used to reuse the osr 15 entry; under NTG.1, where that entry is not yet the 512 cap, this made the test tighter than the oversampling rate calls for. A build assertion now fails the compilation of any table that does not cover the full osr range
 * Health tests: implement the permanent failure of the lag predictor test, which was defined as JENT_LAG_FAILURE_PERMANENT but never raised. The cutoffs use alpha=2^-44, the square of the intermittent alpha, following the convention of the RCT and APT
 * Jitter RNG core: extract the sysfs cache attribute parsing, the online-CPU list parsing and the FIPS indicator read into separate functions, so the shapes they have to handle can be tested without the file they normally come from
 * Jitter RNG core: reject a sysfs cache size whose unit suffix would overflow the shift rather than computing an undefined value
 * Jitter RNG core: jent_zfree() tolerates a NULL pointer, as free() does
 * Jitter RNG core: jent_status() stops appending once the buffer is full instead of walking the rest of the document one no-op snprintf at a time
 * Health tests: add jent_health_insert_timestamp to run the health tests over externally obtained time stamps, so a raw entropy recording can be judged by the very tests that judge the noise source at runtime. Internal rather than part of the API
 * Health tests: the health tests can now be tested themselves with tests/health/health.c - the induced failure testing SP800-90B validations require, driving every health test to both its intermittent and its permanent cutoff, and replaying a file of time stamps through them with --replay (issue #167)
 * Jitter RNG core: apply LLM code review -> add sanity checks
 * Jitter RNG core: add UUID generation and update status printing
 * Jitter RNG core: try to pin the timer thread to one CPU
 * Jitter RNG core: add Linux kernel support header files and conditionally compile support code that is already offered by the Linux kernel

3.7.0
 * Add secure memory implementation for Linux and {Net,Open,Free}BSD, MacOS and Windows
 * Update supported CMake version to 3.10
 * doc: use Doxygen-style comments
 * NTG.1 compliance: Modify startup such that the memory access and SHA-3 loop are treated as independent noise sources which are sampled to collect at least 240 bits each before first block of random numbers is released
 * Remove all code when JENT_CONF_DISABLE_LOOP_SHUFFLE is unset. This code is already discouraged for a long time. Now it is taken out for good.
 * If cache size cannot be detected from base system (e.g. virtualization), use the requested memory size.
 * Change the stuck test to always calculate the absolute values of the 2nd and 3rd discrete derivation of time.
 * Replace SHA3-256 output generation with XDRBG-256
 * Prune the jitterentropy.h header file of internal definitions and delcarations which are moved to src/jitterentropy-internal.h. With that, jitterentropy.h only contains the API. This modification does not alter the Jitter RNG behavior at all.
 * Update secure storage memory implementation for libgcrypt and OpenSSL
 * Add API jent_status

3.6.3
 * Correct time stamp processing on AIX
 * Use high-resolution time stamp on Apple Silicon
 * GCD power-up test: consider OSR

3.6.2
 * Fix RCT re-initialization in jent_read_entropy_safe (thanks to Joshua Hill for pointing this out)
 * simplify test code
 * improve keyword portability

3.6.1
 * Add more test code
 * Add support for SunPRO compiler
 * Fix compilation on OpenBSD by replacing sed with tr
 * internal timer: Add support for Apple
 * Various small fixes to compilation to imporve portability

3.6.0
 * Remove bi-modal behavior of conditioning function
 * Make jent_read_entropy_safe safer by retrying the health test
 * Move the version information to make them available at compile time

3.5.0
 * add distinction between intermittent and permanent health failure

 * add compile time option to allow configuring a mask to reduce the size of
   the time stamp used for the APT

3.4.1
 * add FIPS 140 hints to man page
 * simplify the test tool to search for optimal configurations
 * fix: jent_loop_shuffle: re-add setting the time that was lost with 3.4.0
 * enhancement: add ARM64 assembler code to read high-res timer

3.4.0
 * enhancement: add API call jent_set_fips_failure_callback as requested by Daniel Ojalvo
 * fix: Change the SHA-3 integration: The entropy pool is now a SHA-3 state.
It is filled with the time delta containing entropy and auxiliary data that does not contain entropy using a SHA update operation. The auxiliary data is calculated by a SHA-3 hashing of some varying state data. The time delta that contains entropy is measured about the SHA-3 hasing of the auxiliary data. This satisfies FIPS 140-3 IG D.K resolutions 4, 6, and 8.
 * enhancement: add CMake support by Andrew Hopkins

3.3.1
 * fix: bug fix in initialization logic by Vladis Dronov <vdronov@redhat.com>
 * fix: use __asm__ instead of asm to suit the C11 standard

3.3.0
 * add jent_get_cachesize if _SC_LEVEL1_DCACHE_SIZE is not defined
 * limit the memory buffer size allocated and allow caller to provide
   the means to provide a limit, too
 * fix: update man page
 * update README explaining how to handle entropy shortfall to make it consistent with the current code base

3.2.0
 * fix: add API call jent_read_entropy_safe to header file
 * enhancement: add jent_entropy_init_ex API call
 * enhancement: call jent_entropy_init_ex automatically when jent_entropy_collector_alloc_internal detects that no self test has yet been performed
 * test: provide jitterentropy-rng test tool allowing all options exported by the library to be invoked
 * fix: re-add check of time_backwards in power-on test
 * fix: silence static code analysis tool
 * test: add test for GCD
 * enhancement: add GCD selftest
 * fix: simplify memory management for SHA-3
 * enhancement: add random memory access (JENT_RANDOM_MEMACCESS)

3.1.0
 * Add link call to pthreads library as suggested by Mikhail Novosyolov
 * Add ENTROPY_SAFETY_FACTOR to apply consideration of asymptotically reaching
   full entropy following SP800-90C suggested by Joshua Hill
 * Add test for finiding more entropy by changing the memory buffer size
   used for the memory access loop
 * Increase the memory buffer size to 512 kBytes per default based on
   measurements on systems with low entropy.
 * Add jent_ncpu() detecting the number of existing CPUs. Only when more than
   one CPU is in the system, the internal timer thread is started.
 * add GCD testing and analysis suggested by Joshua Hill
 * add fixes to APT suggested by Joshua Hill
 * add lag predictor health test suggested by Joshua Hill
 * add jent_read_entropy_safe API call
 * break up jitterentropy-base.c into various smaller code files

3.0.2
 * Small fixes suggested by Joshua Hill
 * Update the invocation of SHA-3 invocation: each loop iteration defined by the loop shuffle is a self-contained SHA-3 operation. Therefore, the conditioning information is always *one* SHA-3 operation with different time duration.
 * add JENT_CONF_DISABLE_LOOP_SHUFFLE config option allowing disabling of the shuffle operation
 * Use -O0

3.0.1
 * on older GCC versions use -fstack-protector as suggested by Warszawski,
   Diego
 * prevent creating the internal timer thread if a high-res hardware timer is
   found as reported by Lonnie Abelbeck

3.0.0
 * use RDTSC on x86 directly instead of clock_gettime
 * use SHA-3 instead of LFSR
 * add internal high-resolution timer support

2.2.0
 * SP800-90B compliance: Add RCT runtime health test
 * SP800-90B compliance: Add Chi-Squared runtime health test as a replacement
   for the adaptive proportion test
 * SP800-90B compliance: Increase initial entropy test to 1024 rounds
 * SP800-90B compliance: Invoke runtime health tests during initialization
 * remove FIPS 140-2 continuous self test (RCT covers the requirement as per
   FIPS 140-2 IG 9.8)
 * SP800-90B compliance: Do not mix stuck time deltas into entropy pool

2.1.2:
 * Add static library compilation thanks to Neil Horman
 * Initialize variable ec to satisfy valgrind as suggested by Steve Grubb
 * Add cross-compilation support suggested by Lonnie Abelbeck

2.1.1:
 * Fix implementation of mathematical properties.

2.1.0:
 * Convert all __[u|s][32|64] into [uint|int][32|64]_t
 * Remove all code protected by #if defined(__KERNEL__) && !defined(MODULE)
 * Add JENT_PRIVATE_COMPILE: Enable flag during compile when
   compiling a private copy of the Jitter RNG
 * Remove unused statistical test code
 * Add FIPS 140-2 continuous self test code
 * threshold for init-time stuck test configurable with JENT_STUCK_INIT_THRES
   during compile time

2.0.1:
 * Invcation of stuck test during initalization

2.0.0:
 * Replace the XOR folding of a time delta with an LFSR -- the use of an
   LFSR is mathematically more sound for the argument to maintain entropy

1.2.0:
 * Use constant time operation of jent_stir_pool to prevent leaking
   timing information about RNG.
 * Make it compile on 32 bit archtectures

1.1.0:
 * start new numbering schema
 * update processing of bit that is deemed holding no entropy by heuristic:
   XOR it into pool without LSFR and bit rotation (reported and suggested
   by Kevin Fowler <kevpfowler@gmail.com>)

