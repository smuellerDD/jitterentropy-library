# Hardware RNG based on CPU timing jitter

The Jitter RNG provides a noise source using the CPU execution timing jitter.
It does not depend on any system resource other than a high-resolution time
stamp. It is a small-scale, yet fast entropy source that is viable in almost
all environments and on a lot of CPU architectures.

The implementation of the Jitter RNG is independent of any operating system.
As such, it could even run on baremetal without any operating system.

The design of the RNG is given in the documentation found in at
[http://www.chronox.de/jent](http://www.chronox.de/jent). This documentation also covers the full
assessment of the SP800-90B compliance as well as all required test code.

## API

The API is documented in the man page jitterentropy.3.

To use the Jitter RNG, the header file jitterentropy.h must be included.

# Build Instructions

To generate the shared library `make` followed by `make install`.

Besides the Makefile based build system, CMake support is also provided.
This may eases cross compiling or setting the relevant options for BSI's
functionality class NTG.1, like:

```sh
mkdir build
cd build
cmake -DINTERNAL_TIMER=off -DEXTERNAL_CRYPTO=OPENSSL ..
make
```
CMake may also be used on platforms like Windows or MacOS to ease compilation.

On Linux you may omit the `EXTERNAL_CRYPTO` setting, as the default
memory handling implementation already implements secure erase and swap
prevention.

# Operational Considerations

Please keep the following aspects regarding jitterentropy's usage in mind:

* Use no multithreading on a single instace of `struct rand_data`. If multiple
  threads shall be used, allocate multiple per-thread instances via `jent_entropy_collector_alloc()`.
* Virtual Machine Monitors/Hypervisor may trap and emulate the platforms native timestamping mechanism,
  like `rdtsc`, leading to degraded entropy levels. Please check and disable emulation if possible.
* Activate the health tests (JENT_FORCE_FIPS or JENT_NTG1) if you are operating in a regulated environment
  and/or have done prior entropy estimation. Failing health tests will block the output of the RNG.
* Startup tests take a short but noticeable amount of time, you may not create a new jitter RNG instance
  whenever random bytes are needed.
* While jitterentropy is a rather fast noise source, don't expect multiple MB/s or GB/s. Use it as seed
  source for another deterministic RNG if such speeds are needed.

# Android

To compile the code on Android, use the following Makefile:

arch/android/Android.mk	-- NDK make file template that can be used to directly
			   compile the CPU Jitter RNG code into Android binaries

## Direct CPU instructions

If the function in jent_get_nstime is not available, you can replace the
jitterentropy-base-user.h with examples from the arch/ directory.

# Testing and Entropy Rate Validation

See `tests/raw-entropy/README.md`.

# Specific Configuration Requirements

In general, no specific configurations are needed to run the Jitter RNG. It is
intended to deliver sufficient entropy.

However, specific configurations are required if you want to comply with certain
rules from certain jurisdictions. The following sections outline such
configuration requirements.

Note, the configurations are given via the `flags` field to be set during
initialization of the Jitter RNG.

## NIST SP800-90B Compliance

In order for the Jitter RNG to be compliant with the requirements from SP800-90B including the FIPS 140 IG D.K, the following usage constraints must be observed:

### Compilation

No special considerations.

### Initialization

The following flags are to be considered:

- `JENT_NTG1` may be set (i.e. the SP800-90B configuration and the [AIS 20/31 NTG.1 configuration](#ais-2031-ntg1-compliance) can be jointly enabled).

- Either `JENT_FORCE_FIPS` must be set or base OS is in FIPS mode (i.e. the helper function jent_fips_enabled returns true).

- `JENT_FORCE_INTERNAL_TIMER` must not be set.

- All other flags may be set at the caller's discretion.

### Runtime

The Jitter RNG must not be used if the health test returns a permanent error.

### Status

The status returned by the jent_status API must show the following information among others:

- FIPS mode enabled

- Internal timer disabled

- No health test failing

### Testing

The following test evidence must be provided to CMVP for proving the compliance to SP800-90B:

- Apply heuristic analysis mandated by NIST on common behavior - see the
  [CMUF Entropy Working Group](https://www.cmuf.org/) for the methodology. More
  information about the working group is given at
  [NIST](https://csrc.nist.gov/presentations/2023/cmuf-entropy-working-group).

- Obtain CAVP certificate for SHAKE-256 conditioner - use
  [ACVP-Parser](https://github.com/smuellerDD/acvpparser)

## AIS 20/31 NTG.1 Compliance

In order for the Jitter RNG to be NTG.1 compliant, the following usage constraints must be observed.

### Compilation

The Jitter RNG must be compiled with the compile time options of:

- `CONFIG_CRYPTO_CPU_JITTERENTROPY_SECURE_MEMORY` must be enabled - this option is set when using specific memory allocation functions as defined in jitterentropy-base-user.h. Note, in case this define is not set for your given environment, ensure that a memory allocation is used which implements the following properties - after this is guaranteed, the flag can be set:

	* The memory must not be swapped to non-volatile store upon memory pressure by the OS.

	* The memory must not be shared in any kind with another entity beyond the Jitter RNG. E.g. a it shall not be a shared memory segment.

	* The memory must be actively zeroized before the memory is released.

### Initialization

The Jitter RNG must be initialized with the following flag settings:

- `JENT_FORCE_FIPS` may be set (i.e. the NTG.1 configuration and the [SP800-90B configuration](#nist-sp800-90b-compliance) can be jointly enabled).

- `JENT_NTG1` must be set.

- `JENT_FORCE_INTERNAL_TIMER` must not be set.

- All other flags may be set at the caller's discretion.

### Runtime

The Jitter RNG must not be used if the health test returns a permanent error.

### Status

The status returned by the jent_status API must show the following information among others:

- AIS 20/31 NTG.1 mode enabled

- Memory Block Size equal or larger than four times L1 cache

- Secure Memory enabled

- Internal timer disabled

### Testing

The following test evidence must be provided to the German BSI for proving the compliance to NTG.1:

- Measured entropy rate must show rate 8/OSR or higher (see the file tests/raw-entropy/README.md given in the source code distribution for the test approach as well as the analysis to increase the entropy rate):

	* Hash loop

	* Memory access loop

	* Common behavior (SP800-90B restart + runtime tests)

- If the selected OSR after applying the methodology is larger than 20, the Jitter RNG cannot be used on the particular system.

# Version Numbers

The version numbers for this library have the following schema:
MAJOR.MINOR.PATCHLEVEL

Changes in the major number implies API and ABI incompatible changes, or
functional changes that require consumer to be updated (as long as this
number is zero, the API is not considered stable and can change without a
bump of the major version).

Changes in the minor version are API compatible, but the ABI may change.
Functional enhancements only are added. Thus, a consumer can be left
unchanged if enhancements are not considered. The consumer only needs to
be recompiled.

Patchlevel changes are API / ABI compatible. No functional changes, no
enhancements are made. This release is a bug fixe release only. The
consumer can be left unchanged and does not need to be recompiled.

# Author

Stephan Mueller <smueller@chronox.de>
