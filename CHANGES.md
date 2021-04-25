3.0.3
 * Add link call to pthreads library as suggested by Mikhail Novosyolov
 * Add ENTROPY_SAFETY_FACTOR to apply consideration of asymptotically reaching
   full entropy following SP800-90C suggested by Joshua Hill

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

