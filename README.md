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

That header is the whole of the API: on the platforms whose linker takes a
version script the shared library exports exactly the functions declared in
it, listed in `version.lds`, and nothing else. The list is checked against the
header at configure time, so the two cannot drift apart unnoticed.

# Build Instructions

To generate the shared library `make` followed by `make install`.

Besides the Makefile based build system, CMake support is also provided.
This may eases cross compiling or setting the relevant options for BSI's
functionality class NTG.1, like:

```sh
cmake -S . -B build -DINTERNAL_TIMER=off -DEXTERNAL_CRYPTO=OPENSSL
cmake --build build
```
CMake may also be used on platforms like Windows or MacOS to ease compilation.

On Linux you may omit the `EXTERNAL_CRYPTO` setting, as the default
memory handling implementation already implements secure erase and swap
prevention.

## Build Options

| Option | Default | Effect |
| --- | --- | --- |
| `BUILD_SHARED_LIBS` | `OFF` | Build a shared library instead of a static one |
| `INTERNAL_TIMER` | `ON` | Compile the thread based internal timer, used where no high-resolution timer is available |
| `EXTERNAL_CRYPTO` | unset | Use an external libcrypto for hashing and secure memory: `AWSLC`, `OPENSSL` or `LIBGCRYPT` |
| `STACK_PROTECTOR` | `ON` | Compile with the stack protector enabled |
| `AARCH64_NSTIME_REGISTER` | unset | Name of the register `jent_get_nstime()` should read on AArch64 |
| `ENABLE_SANITIZERS` | `OFF` | Address and undefined behavior sanitizers (development only) |
| `ENABLE_COVERAGE` | `OFF` | Instrument for code coverage and add the `coverage` target (development only) |
| `ENABLE_FUZZING` | `OFF` | Instrument for libFuzzer and build the coverage-guided harness under `tests/fuzz` (Clang only, development) |
| `MOCK_TIMER` | `OFF` | Let the caller replace the time source with a callback (testing only - such a build produces no entropy of its own) |
| `ENABLE_TOOLS` | `ON` | Build and install the recording and validation tools, which live under `tests/` |
| `BUILD_TESTING` | `ON` | Build the test programs and register the CTest suite |

The two are independent. `ENABLE_TOOLS` decides what is installed for a user
to run - `jitterentropy-rng`, `jitterentropy-osr`, `jitterentropy-hashtime`,
`getrawentropy`, `extractlsb`, `gcd`, `jitterentropy-health` and the
`jitterentropy-chardev-*` tools - and `BUILD_TESTING`, the name CMake projects
conventionally use, decides what CTest is given. Three of the tools are also
what the suite drives, so they are built whenever either option is on and
installed only for the first:

```sh
cmake -S . -B build -DENABLE_TOOLS=OFF    # the suite, nothing installed but the library
cmake -S . -B build -DBUILD_TESTING=OFF   # the tools, no test programs built
cmake -S . -B build -DENABLE_TOOLS=OFF -DBUILD_TESTING=OFF   # the library alone
```

With both off the install tree is the library, its header, the pkg-config and
CMake package files and the man page.

Packaging commonly passes `-DBUILD_TESTING=OFF` on its own - the nixpkgs cmake
hook does - and that is now exactly what it says: the tools keep being built
and installed. The option is defined directly rather than through
`include(CTest)`, which would also pull in the CDash submission machinery.

## Running the Test Suite

The CMake build registers the test programs with CTest:

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

The suite has two halves. The deterministic tests - the GCD self test, the unit
tests for `src/` and `arch/`, and the induced failure tests of the health tests
- compute over fixed inputs and answer the same everywhere. The entropy
generation tests exercise the real noise source and can fail for reasons that
are properties of the machine rather than defects in the code: a memory lock
limit lower than the collector needs, or a startup whose health tests do not
converge on the block size derived from this CPU's caches. They carry the
`unreliable` label so that a gating run can leave them out:

```sh
ctest --test-dir build --output-on-failure -LE unreliable   # gating set
ctest --test-dir build --output-on-failure -L unreliable    # only those
```

This is what the CI workflow gates on. Useful selections:

```sh
ctest --test-dir build -N              # list the tests without running them
ctest --test-dir build -R unit-        # only the src/ and arch/ unit tests
ctest --test-dir build -R health-      # only the induced failure tests
ctest --test-dir build -R unit-sha3 -V # one test with its full output
ctest --test-dir build -j "$(nproc)"   # in parallel
```

On a multi-configuration generator such as Visual Studio, name the
configuration that was built: `ctest --test-dir build -C Release`.

Each test directory also carries a standalone Makefile for trees without CMake:
`make -C tests/unit check` and `make -C tests/health check`.

See `tests/README.md` for what the individual test programs cover.

## Code Coverage

Configuring with `ENABLE_COVERAGE` instruments the build and adds a `coverage`
target that runs the suite and writes a browsable HTML report to
`<build>/coverage/index.html`. It needs either `gcovr` or `lcov` together with
`genhtml`, and a separate build directory - the instrumentation forces `-O0`:

```sh
cmake -S . -B build-coverage -DENABLE_COVERAGE=ON
cmake --build build-coverage
cmake --build build-coverage --target coverage
```

The target runs the whole suite, the `unreliable` tests included, since they
are what reaches the noise source and the health tests at runtime; their result
is not propagated, so a machine that cannot run them still gets a report.

## Fuzzing

The public API has a fuzzing harness under `tests/fuzz`. Its standalone runner
is part of the CTest suite on every platform; the coverage-guided one needs
Clang with the libFuzzer runtime:

```sh
cmake -S . -B build-fuzz -DENABLE_FUZZING=ON -DENABLE_SANITIZERS=ON
cmake --build build-fuzz
./build-fuzz/tests/fuzz/fuzz-api -max_total_time=300 corpus/
```

See `tests/README.md` for what the harness drives and asserts.

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

If the high-resolution timer needed by jent_get_nstime is not available
on your target, add a new branch to arch/jitterentropy-arch-timer.h
guarded by the appropriate architecture macros.

# Testing and Entropy Rate Validation

See `tests/README.md` for the test suite and how to run it, and
`tests/raw-entropy/README.md` for the raw entropy gathering and the SP800-90B
entropy rate analysis.

Induced failure testing of the health tests, as FIPS 140-3 / SP800-90B
validations require, is provided by `tests/health` - see `tests/README.md`.

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

No special considerations.

### Initialization

The Jitter RNG must be initialized with the following flag settings:

- `JENT_FORCE_FIPS` may be set (i.e. the NTG.1 configuration and the [SP800-90B configuration](#nist-sp800-90b-compliance) can be jointly enabled).

- `JENT_NTG1` must be set.

- `JENT_FORCE_INTERNAL_TIMER` must not be set.

- `JENT_DISABLE_MEMORY_ACCESS` must not be set.

- All other flags may be set at the caller's discretion.

### Runtime

The Jitter RNG must not be used if the health test returns a permanent error.

### Status

The status returned by the jent_status API must show the following information among others:

- AIS 20/31 NTG.1 mode enabled

- Memory Block Size equal or larger than four times L1 cache

- Internal timer disabled

### Testing

The following test evidence must be provided to the German BSI for proving the compliance to NTG.1:

- Measured entropy rate must show rate 8/OSR or higher (see the file tests/raw-entropy/README.md given in the source code distribution for the test approach as well as the analysis to increase the entropy rate):

	* Hash loop (SP800-90B restart + runtime tests)

	* Memory access loop (SP800-90B restart + runtime tests)

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
