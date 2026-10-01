4.0.0-prerelease
 * Compatibility: 4.0.0 is neither API nor ABI compatible with 3.7.0, and the SONAME is libjitterentropy.so.4. Rebuild every caller. The incompatible changes are the renumbered JENT_HASHLOOP_* flags (see Jitter RNG core), the removal of the installed header jitterentropy-base-user.h and of the undeclared exports jent_gcd_* (see Build)
 * Jitter RNG core: add jent_selftest, which runs the known answer tests on their own and permanently stops a bound collector on failure (JENT_ERR_SELFTEST), the jent_entropy_collector_* accessors for what jent_status reports, a UUID per instance, JENT_ERR_* definitions of the existing error codes, jent_entropy_set_notime_cpu to pin the internal timer thread, and the counters, window positions and cutoffs of the health tests in jent_status (healthTests)
 * Jitter RNG core: the JENT_FORCE_SECURE_MEM flag replaces the compile-time option JENT_CONF_RELAX_MLOCK: by default a collector is created even where its state cannot be locked; with the flag - implied in FIPS and NTG.1 mode, the system's FIPS mode included - the allocation fails. The memory access region is no longer locked, which exceeded common lock quotas
 * Jitter RNG core: the JENT_HASHLOOP_* flags are renumbered - API and ABI change: JENT_HASHLOOP_1 had the same bits as no flag. The field is now bits 23-26; value n selects 2^(n - 1) loops, 0 the default. Reserved flag bits, fields and an osr out of range, and memory access disabled in FIPS / NTG.1 mode are refused (EPROGERR / NULL) instead of ignored
 * Jitter RNG core: the fallback to the internal timer is decided per collector, the startup verdict and the timer divisor are kept per clock rather than per process, and jent_entropy_init* may be called from several threads at once
 * Jitter RNG core: jent_read_entropy_safe reports a failure it cannot recover from as JENT_ERR_*_PERMANENT and carries the health test state into its replacement collector
 * Jitter RNG core: drop the backtracking operation at the end of jent_read_entropy - the XDRBG-256 generate is already one-way - keep the XDRBG state off the stack and wipe the stack in hosted builds; fixes to the startup's monotonicity check and error codes, a torn read of the internal timer on 32-bit targets, a double free with an external timer, tiny detected caches and JENT_HASH_LOOP_DEFAULT on reallocation
 * Health tests: implement the permanent failure of the lag predictor test (alpha=2^-44), extend the APT cutoff tables from osr 15 to JENT_MAX_OSR, and invoke the FIPS failure callback once per failure instead of on every check of it
 * Health tests: fixes to the recovery of the RCT with memory, which now reaches its permanent cutoff, to the lag predictor cutoffs, which fired slightly too often, to the APT and the lag predictor, which restart at each FIPS / NTG.1 startup stage and no longer see the priming measurement, and to the reallocation, which keeps the clock type of FIPS and NTG.1 instances
 * Platforms: new are freestanding builds (-ffreestanding, JENT_BAREMETAL; lock-free 32-bit atomics required), the FreeBSD kernel, and Android and iOS with example apps in tests/android and tests/ios, replacing arch/android
 * Platforms: fixes to the cache detection on Linux, the CPU count and FIPS policy on Windows, the aarch64 counter on Apple platforms, and the libgcrypt and OpenSSL secure memory backends
 * Linux kernel: add the jitter_rng module - out of tree, through DKMS or built into the kernel - with /dev/jitterentropy, /dev/hwrng, crypto API, /proc/jitterentropy, debugfs raw sample and ioctl interfaces and a periodic self test; see linux_kernel/README.md and jitter_rng(4)
 * Linux kernel: add libjitterentropy-kernel and the jitter_rng tool, user space access to the module's interfaces; CMake builds and installs them with ENABLE_KERNEL_LIB
 * Linux kernel: the module uses the library through jitterentropy.h alone, which now also carries JENT_MIN_OSR, JENT_MAX_OSR and jent_flags_invalid; its debugfs interface records through jent_record_alloc, jent_record_prime, jent_record_sample and jent_record_free, new in libjitterentropy-record
 * Daemon: jitterentropy-rngd, which feeds the Jitter RNG into the Linux /dev/random, is part of this tree (taken from https://github.com/smuellerDD/jitterentropy-rngd at 1.3.3-prerelease) and links the library; CMake builds and installs it, its systemd unit and its man page with ENABLE_RNGD, make with ENABLE_RNGD=1. New are --oneshot, --fips, --ntg1, --force-internal-timer and --disable-internal-timer, and --phase1 and --phase2 for its reseed schedule
 * Build: CMake 3.22 is the minimum version. The shared library exports the API of jitterentropy.h and nothing else (version.lds), so the undeclared jent_gcd_* exports of 3.7.0 are gone, and jitterentropy-base-user.h is no longer installed: callers of its helpers such as jent_get_nstime must provide their own
 * Build: new CMake options ENABLE_TOOLS, BUILD_TESTING, ENABLE_RECORDING (libjitterentropy-record, not for production), MOCK_TIMER and the sanitizer, coverage and fuzzing options; JENT_PRIVATE_COMPILE for a private copy exporting nothing; man pages per group of functions, tool and kernel interface (INSTALL_MAN)
 * Build: MSVC optimized builds no longer inline helpers into the timed noise loop; relocatable installs (jitterentropy.pc, the macOS install name, a $ORIGIN-relative rpath for the tools) and fixes to the static CMake exports and make install
 * Tests: improved test support - unit tests for every module of src/ and arch/ (make check without CMake), induced failure and replay tests of the health tests with their cutoffs checked against the SP800-90B derivation, fuzzing, NixOS VM tests of the kernel module, the daemon and the raw entropy recording, and CI on more platforms, including an EFI application without an operating system
 * Tests: add the tools jitterentropy-cpuinfo and jitterentropy-flags, jitterentropy-hashtime --cpu, and raw entropy recording through the kernel module's debugfs interface
 * Tests: getrawentropy prints the debugfs values unmodified by default and takes deltas of successive values, which 3.7.0 always did, only with --timestamps; fixes to the restart validation, which gave ea_restart too few samples, and to the raw entropy scripts

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

