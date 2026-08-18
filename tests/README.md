# Jitter RNG Tests

The following Jitter RNG tests are available in the following
directories:

* `raw-entropy`: Gathering of the raw unprocessed entropy data and restart test
  entropy data required for the SP800-90B analysis

* `gcd`: Self test of the GCD analysis applied to the time deltas

* `health`: Induced failure test of the SP800-90B health tests

* `unit`: Unit tests for the modules in `src/` and the platform backends in
  `arch/`

* `chardev`: Status reporting of the Linux kernel module character device

## Unit tests

`tests/unit` covers the library module by module, one program per area:

| Program | Covers |
| --- | --- |
| `unit-sha3` | `src/jitterentropy-sha3.c`: the library's own known answer tests, the FIPS 202 SHA3-256 vectors, incremental absorb, SHAKE256 / XDRBG block generation, state allocation |
| `unit-gcd` | `src/jitterentropy-gcd.c`: the Euclidean GCD, the delta history analysis and each condition it reports, and the establish-once semantics of the common timer GCD |
| `unit-arch` | `arch/`: the time source, CPU count, cache size discovery, FIPS mode query, the (secure) allocator, the OS CSPRNG and thread placement |
| `unit-uuid` | `src/jitterentropy-uuid.c`: the RFC 4122 version 4 layout, the version and variant bits, and what is emitted when the platform has no CSPRNG to ask |
| `unit-base` | `src/jitterentropy-base.c` and `src/jitterentropy-status.c`: the decoding of every memory size and hash loop flag, oversampling rate clamping, collector allocation, the `jent_read_entropy*` error contract, the JSON status and UUID output, the startup self tests, the compliance modes and the internal timer |
| `unit-fault` | The failure paths, by fault injection: the allocator, `mmap`/`mprotect`/`mlock`, `sysconf`, the CPU affinity query, `getrandom()`, the FIPS indicator and the time source itself are each made to fail so the code behind them runs |
| `unit-mock` | The mocked time source and `jent_health_insert_timestamp()`: registering a time source, replaying stamps through the health tests, and the collector reallocation that only happens when the startup measurements are bad |
| `unit-notime` | The replaceable timer-less back end: registering an implementation, the guards on an incomplete one, and the thread backend when no thread can be created |
| `unit-concurrency` | Several instances at once: the whole life cycle - `jent_entropy_init_ex()`, collector allocation, generation, `jent_status()`/`jent_uuid()` and the free - run in parallel threads, checking that the process-wide startup verdict is the same for every thread and that no two instances share their output or their identity |
| `unit-zeroize` | The wipe on release: that `jent_zfree()` clears what it is given before the memory leaves the library, and that neither the entropy pool nor the SHAKE state nor `struct rand_data` still carries anything when `jent_entropy_collector_free()` releases it. The release call is interposed, as the memory cannot be read after it |
| `unit-error` | The health failure reporting above the health tests: which `JENT_ERR_*` code each failure bit is reported as, that a permanent failure outranks an intermittent one, that `jent_read_entropy_safe()` recovers from intermittent failures and gives up above `JENT_MAX_OSR`, that the health test state survives the reallocation, and the FIPS failure callback |

Each program absorbs the sources it exercises rather than linking the library:
most of what is under test is internal and a shared build exports none of it,
and some of it is static to its own translation unit. They therefore build
without the library having been built first.

Together with `tests/health` and the GCD self test they cover about 96% of the
library's lines, 90% of its branches and every one of its functions.

## Replaying time stamps

Two entry points judge time stamps the library did not measure itself.

`jent_health_insert_timestamp()` runs the health tests over stamps handed to
it, forming the delta exactly as the noise source does. That is what a raw
entropy recording is replayed through: the same code that will judge the noise
source at runtime, reaching the same verdict on the same numbers. It is part of
the API and needs no special build. Note that the first stamp is a delta
against whatever the collector last measured, so a replay should either discard
its first result or insert the first stamp of the recording twice.

`jent_set_mock_timer()` goes further and replaces the time source for the whole
library, so the startup self test, the noise source and the health tests all
run on the supplied stamps. It exists only in a build configured with
`-DMOCK_TIMER=ON`, which is off by default: a library whose clock the caller
supplies produces no entropy of its own. `jent_status()` reports
`mockedTimerBuild` so that fact cannot be lost between a test run and its
report.

It is deliberately **not** part of the API. It is declared in
`arch/jitterentropy-arch-timer.h`, so it is reachable from inside the library
and from tests that compile these sources into themselves - as the programs
here do - and from nowhere else. The installed header does not mention it, and
a shared build exports no symbol for it even with the option on. Note that a collector using the internal timer reads that thread's
counter and never calls the time source, so a mocked clock has to be paired
with `JENT_DISABLE_INTERNAL_TIMER`.

Between them they reach what no machine offers: a clock that does not move, one
that is too coarse, and the collector reallocation that only happens when the
measurements taken during startup trip a health test.

## Fault injection

Most of what a hardened library does is handle failure, and none of it runs on
a healthy machine. `unit-fault` and `unit-notime` therefore interpose the
allocator, the kernel calls behind it (`mmap`, `mprotect`, `mlock`), the
platform queries (`sysconf`, the CPU affinity query, `getrandom()`, the FIPS
indicator), thread creation and the time source, and make each of them fail in
turn.

The interposition needs nothing from the library. These programs already
absorb its sources, so a `#define` renames the real function while the file
that defines it is compiled and the test supplies its own under the original
name - the shipped library carries no testing conditional and no test hook.

`unit-fault` denies each allocation of a collector, of the startup self test
and of a health-failure recovery one at a time, and checks that the result is
the same every time: nothing handed back, nothing left behind. Run under the
sanitizers - which the CI does - that is also what says the partially built
state is released rather than dropped.

Assertions are limited to what each module promises its callers.

Where a
property cannot be established on the machine or in the build at hand - no
discoverable cache size, no lockable memory, the lag predictor compiled out -
the case is reported as skipped rather than passed over silently.

A handful of branches are deliberately left uncovered, because reaching them
would mean breaking the thing that detects them: the arms where a known-answer
test finds its own algorithm wrong, and where the GCD self test finds the
analysis it checks broken.

## Induced failure testing of the health tests

FIPS 140-3 / SP800-90B validations require evidence that each health test
raises the error it is meant to raise. `tests/health` provides that evidence:
for every health test (RCT, APT, lag predictor, RCT with memory) and for both
its intermittent and its permanent cutoff it feeds a known-bad sample sequence
into the test and reads the result back with `jent_health_failure()`. Each case
verifies that the expected error is reported *and* that no unrelated health
test reported one, so the failure is attributed to the test under examination.

Both the common and the NTG.1 cutoff configurations are covered. The tool takes
the oversampling rate as its only argument and defaults to `JENT_MIN_OSR`:

```
tests/health/health [osr]
```

## Recomputing the cutoffs

The cutoffs the tests above are driven to are lookup tables in
`src/jitterentropy-health.c`, one entry per oversampling rate, plus the two
macros in `src/jitterentropy-health.h` that state the RCT cutoff, which is
linear in that rate. All of them come out of the window size and the false
positive rate alpha, and `tests/health/cutoffs.py` computes them with mpmath:
the RCT as `ceil(-log2(alpha)/H)`, the APT and the lag predictor's global
cutoff as an inverse binomial CDF, the lag predictor's local cutoff as the
shortest run whose probability of occurring in a window falls below alpha, and
the repetition count test with memory as a normal approximation, `tau` standard
deviations above the mean of its window.

```
python3 tests/health/cutoffs.py            # print the cutoffs as C
python3 tests/health/cutoffs.py --check    # compare against the source
```

`--check` exits non-zero on a mismatch and takes about ten seconds. It is not
part of the CTest suite, mpmath being a dependency nothing else here has.

The common-case tables of the repetition count test with memory come out of
that last formula as the cap of their window, which is a cutoff the test cannot
reach - the values that disable it, as the source says beside them. Only its
NTG.1 tables, computed with an 8-fold entropy margin, are live.

## Replaying a recording through the health tests

`tests/health` also reads time stamps from a file and runs the health tests
over them, which is how a raw entropy recording is judged by the very tests
that will judge the noise source at runtime:

```
tests/health/health --replay FILE [osr] [--ntg1]
```

One decimal or `0x`-prefixed value per line; blank lines and `#` comments are
skipped so a recording can carry a header, and `-` reads standard input. It
exits 0 when no health test fired, 1 when one did, and 2 when the file could
not be read or did not parse.

The first stamp only establishes what the second is a delta against, so it is
inserted twice and the first measurement judged is the first one the recording
describes. The deltas are judged as they are: a Jitter RNG whose startup found
a common divisor greater than one would divide by it first, which changes what
counts as stuck, so a recording from a coarse counter is judged more harshly
here than it would be at runtime. The tool says so in its output.

### Test vectors

`tests/health/testdata` holds one recording per health test failure that a file
of time stamps can produce, and one that must produce none. Each carries a
header explaining the delta shape that produces it and why:

| Vector | Produces |
| --- | --- |
| `rct-intermittent.txt` | RCT intermittent, alone |
| `rct-permanent.txt` | RCT intermittent and permanent, alone |
| `lag-intermittent.txt` | Lag predictor intermittent, alone |
| `lag-permanent.txt` | Lag predictor intermittent and permanent, alone |
| `apt-intermittent.txt` | APT intermittent, alongside RCT and lag |
| `apt-permanent.txt` | APT intermittent and permanent, alongside RCT and lag |
| `no-failure.txt` | nothing |

Two shapes do the isolating. Deltas that rise by a constant amount have a
third derivative of zero - the stuck condition - while never repeating a value,
so only the RCT sees anything. Deltas repeating a cycle of four distinct values
are never stuck and never fill an APT window, but the predictor looking four
back is always right, so only the lag predictor sees anything.

The APT cannot be isolated: its cutoff is 459 recurrences of one symbol within
a window of 512, and identical back-to-back deltas are exactly what the RCT
counts and what the lag predictor predicts. Both fire long before the APT
does, and the vectors say so.

The repetition count test with memory has no vector, because a replay cannot
reach it. On hitting its intermittent cutoff it enters a recovery loop that
generates fresh random data and clears its counter, so a replay raises neither
that error nor the permanent one behind it. The induced failure mode covers
both by entering the recovery state directly.

CTest runs each vector and matches the report rather than the exit status,
which is 1 for any failure and so cannot tell them apart. The four isolating
vectors assert the whole report, so they fail if anything else fires too.

A cutoff that cannot be reached in a given configuration is reported as skipped
rather than as a failure, with the reason. Two such cases exist by design: the
permanent cutoff of the RCT with memory in the common configuration, which
`tau = 3` places one count above the largest value its window can produce, and
the intermittent APT cutoff from osr 15 on, which has grown into the maximum of
512 that FIPS 140-2 IG 7.19 resolution #16 caps it at - the same value the
permanent cutoff already has.

## Running the tests

The CMake build registers the deterministic tests with CTest, one per
oversampling rate for the induced failure tests:

```
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure -LE unreliable
```

The tests that exercise the real noise source are labelled `unreliable`: they
can fail for reasons that are properties of the machine rather than defects in
the code, so a gating run excludes them with `-LE unreliable` and
`ctest -L unreliable` runs only those.

## Coverage

Configuring with `-DENABLE_COVERAGE=ON` instruments the build and adds a
`coverage` target that runs the whole suite and writes a browsable HTML report
to `<build>/coverage/index.html`. It needs either `gcovr` or `lcov` together
with `genhtml`:

```
cmake -S . -B build -DENABLE_COVERAGE=ON && cmake --build build
cmake --build build --target coverage
```

# Author
Stephan Mueller <smueller@chronox.de>
