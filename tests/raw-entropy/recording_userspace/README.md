# Linux Entropy Recording and Validation

The test provided here is split into two aspects: the recording of the raw
entropy data and the validation of the data. Both aspects are implemented
with the code in the respective directories.

The idea is that you give the recording directory to the customer
so that he obtains the data. Once you receive the data, you process it
with the code in the validation directory.

## Getting Started

When standard testing shall be performed, the collection of raw entropy is
performed with the script `invoke_testing.sh`.

This test tool uses the Jitter RNG source code from the `src/` directory. If you
need to test a different code tree, please pull the respective code.

The results are stored in `../results-measurements` which then needs to be
processed with the `validation-runtime` and `validation-restart` logic.

For analyzing different aspects of the Jitter RNG, different flavors of the
test script are provided as follows which all obtain the raw unconditioned
noise data to be analyzed with the tool set given in `validation-runtime`:

* `invoke_testing.sh`: This test tool invokes the default behavior of the
  Jitter RNG. Its analysis tool is `validation-runtime/processdata.sh`
  
* `invoke_testing_fips.sh`: This test tool initializes the Jitter RNG with
  `JENT_FIPS` to obtain the FIPS 140 behavior. Its analysis tool is
  `validation-runtime/processdata.sh`
  
* `invoke_testing_ntg1.sh`: This test tool initializes the Jitter RNG with
  `JENT_NTG1` to obtain the BSI NTG.1 behavior. Its analysis tool is `validation-runtime/processdata_ntg1.sh`
  to obtain the entropy rate for each data stream. See [NTG.1 Recording] for
  details.
  
* `invoke_testing_memloop.sh`: This test tool initializes the Jitter RNG with
  `JENT_NTG1` to obtain the BSI NTG.1 behavior. Its analysis tool is
  `validation-runtime/processdata_memloop.sh`. See [NTG.1 Raw Noise Sources] for
  details. NOTE: This tool may need to be invoked with root permissions as it
  attempts to allocate up to 512MB of mlock'ed memory (which typically exceeds
  the ulimit for a normal user).
  
* `invoke_testing_hashloop.sh`: This test tool initializes the Jitter RNG with
  `JENT_NTG1` to obtain the BSI NTG.1 behavior. Its analysis tool is
  `validation-runtime/processdata_hashloop.sh`. See [NTG.1 Raw Noise Sources] for
  details.
    
* `invoke_testing_commonop.sh`: This test tool initializes the Jitter RNG with
  `JENT_NTG1` to obtain the BSI NTG.1 / SP800-90B behavior. It analyzes,
  however, the Jitter RNG common operation with the different hashloop / memory
  size options. Its analysis tools are
  `validation-runtime/processdata_hashloop.sh` and
  `validation-runtime/processdata_memloop.sh`. The goal of the test is to
  analyze the common runtime behavior depending on the selected parameters for
  the hashloop and memory size. NOTE: This tool may need to be invoked with root
  permissions as it attempts to allocate up to 512MB of mlock'ed memory (which
  typically exceeds the ulimit for a normal user).

The `JENT_NTG1` and `JENT_FORCE_FIPS` modes require the memory of the entropy
collector to be locked into RAM, i.e. the allocation fails when the operating
system refuses the lock. How much memory may be locked is not set by the library
but bounded per process by the operating system: `RLIMIT_MEMLOCK` on POSIX
systems and the process working set quota on Windows. For these two modes the
recording tools raise that limit as far as the process is allowed to - see
`tests/jitterentropy-memlock.h`. Raising the `RLIMIT_MEMLOCK` *hard* limit requires
privileges, so the large memory sizes (see the notes on root permissions above)
still need the tool to be invoked as root, whereas the smaller ones now work as
a normal user; where the limit is not sufficient, the tool reports that the
Jitter RNG handle cannot be allocated.

## Core Selection on Hybrid CPUs

On hybrid CPUs - Intel P/E cores, ARM big.LITTLE - the core types differ in
their micro-architecture and their caches. The Jitter RNG therefore shows a
different timing behavior on each of them. A raw noise recording thus only
characterizes the core type it was taken on, which means each core type has to
be recorded and analyzed separately.

The tool `jitterentropy-cpuinfo` lists the cores to choose from:

	make -f Makefile.cpuinfo
	./jitterentropy-cpuinfo

For every CPU it reports the vendor and the model, the core type as reported
by the system, the topology and the data cache sizes together with the number
of CPUs sharing each cache. The sharing separates the core types as well - on
hybrid Intel CPUs, each P-core has an L2 cache of its own whereas a cluster of
E-cores shares one - and it is also what identifies the SMT siblings, which
are two CPU numbers for one physical core rather than two cores to measure.

The core type is whatever the system offers: the hybrid information of CPUID
on Intel, the efficiency class on Windows and the performance level on macOS.
ARM reports no core type at all; what is shown there instead is the compute
capacity the scheduler works with, normalized so that the most capable core of
the system is 1024 - it is what separates the big from the LITTLE cores, and on
a uniform system every core reports 1024 and the value says only that they are
equivalent.

AMD reports no core type either - its dense cores are the same
micro-architecture with a smaller cache and a lower clock, which the cache and
frequency columns show - so where the amd-pstate driver is in use, the per-core
performance ranking of the firmware is reported instead.

Intel's low-power efficiency cores (Meteor Lake and later) are reported as
`LP-E-core`. Note that this is derived rather than read: CPUID knows the two
core types Atom and Core only, and an LP E-core is an Atom core like any other
efficiency core. What identifies it is that it sits outside the L3 domain, so
an efficiency core seeing no L3 on a system whose other cores do is taken to be
one. All three types have to be recorded separately.

How much of this is available differs per operating system. Linux, Windows and
FreeBSD describe every CPU individually. macOS has no per-CPU interface and
describes the cores per performance level, which is how Apple Silicon exposes
its P and E cores. The remaining systems have neither that nor a way to place
the tool on a chosen CPU, so only the CPU it happens to run on is described -
run it repeatedly to see the other core types.

The nominal base frequency and the rate of the counter the Jitter RNG takes
its timings from are reported as well. The base frequency is the second value
that separates the core types - on the hybrid parts it differs where the model
name cannot - and the counter rate is the resolution every measurement is
bounded by. That counter is the timestamp counter on x86 and the architected
generic timer on ARM, where it commonly runs at a far lower rate.

Its properties are stated below the table, in the terms Linux uses for them:
`invariant` is a counter that ticks at a constant rate whatever the P-state,
`nonstop` one that keeps ticking in the deep C-states, and `known rate` says
that the rate above was enumerated rather than calibrated by the operating
system against another timer. On Linux these come
from what the kernel concluded (the `constant_tsc`, `nonstop_tsc` and
`tsc_known_freq` flags of `/proc/cpuinfo`), elsewhere from CPUID or, on ARM,
from what the architecture guarantees for the generic timer.

Where a value is unknown, the reason is given below the table rather than left
to a dash. AMD enumerates neither its base frequency nor its counter rate in
CPUID: the base frequency is taken from the nominal frequency of the CPPC
tables there, and the counter rate stays unknown. On a machine that has
neither cpufreq nor those tables - a virtual machine, typically - no frequency
is reported at all.

With `--summary` the listing holds one row per kind of CPU rather than one per
CPU, naming the CPUs of each kind. On a machine with a hundred CPUs that says
in a few lines what the full listing repeats a hundred times, which is what
the question "how many core types are there, and which CPU do I record for
each" wants:

	 CPUs Type      BaseMHz  MaxMHz  TmrMHz        L1d ...
	    4 P-core       1700    5000    2611      48K/2 ...
	      CPUs 0-3
	    8 E-core       1200    3700    2611      32K/1 ...
	      CPUs 4-11

Two CPUs count as of one kind when everything a recording depends on matches:
the core type, the frequencies, the counter and the caches. The package and
core numbers are not part of that - they name a CPU rather than describe it.

With `--json` the same data is written as JSON, one entry per CPU, for scripts
that drive one recording per core type. Values the system does not report are `null`,
`backend` names where the data comes from (`linux`, `windows`, `macos` or
`generic`, which is what says how complete the listing is), and `pinning`
states whether the `--cpu` option below can be used at all:

	./jitterentropy-cpuinfo --json |
		jq -r '.processors[] | select(.coreType == "E-core") | .cpu'

A `macos` backend reports `"pinning": false` although a core type can still be
selected there - with `--e-cores` rather than with `--cpu`, as described below.

The recording is confined to one of the listed CPUs with the `--cpu` option of
`jitterentropy-hashtime`:

	./jitterentropy-hashtime 1000000 1 jent-raw-noise --cpu 4

The configuration a recording is made with is shown with:

	./jitterentropy-hashtime 1 1 unused --cpu 4 --status

The same effect is achieved for the recording scripts listed above by invoking
them under `taskset -c <CPU>`.

The size of the memory block used for the memory access loop does not follow
the selected core: the library derives it from the largest data cache found in
the system, which is the one of the performance cores. To record an efficiency
core with a memory size that matches its own cache instead, pass the size with
`--max-mem` - the cache sizes of that core are reported by
`jitterentropy-cpuinfo`.

Note that the internal timer cannot be used together with `--cpu`: its
counting thread requires a CPU of its own, whereas `--cpu` leaves a single CPU
in the affinity mask. OpenBSD offers no thread affinity API at all, so `--cpu`
is unavailable there; `jitterentropy-cpuinfo` says so in its output.

`jitterentropy-hashtime` offers `--cpu` only where it can place the
measurement, which is Linux, Windows, FreeBSD and NetBSD - and Linux alone when
the library is built without its internal timer, whose pinning primitive the
option uses. Passing it elsewhere is answered with the reason it cannot be
honored rather than with an unknown-option error.

### Selecting a core type on macOS

macOS offers no thread-to-CPU pinning either, so `--cpu` cannot be used there.
The efficiency cores can still be recorded on their own: macOS schedules the
lowest quality-of-service class on them alone, which is what `--e-cores` asks
for:

	./jitterentropy-hashtime 1000000 1 jent-raw-noise --e-cores

Without that option the recording is made on the performance cores, as that is
where the system runs a thread that asks for nothing. `--cpu`, `--e-cores` and
`--p-cores` are mutually exclusive, and the last two are rejected on every
other system.

The counterpart, `--p-cores`, asks for the performance cores. A command started
from a shell already carries the class those are given first, so it changes
nothing there. What it is for is a recording made from a process that was put
in the background - `taskpolicy -b`, a launchd job marked as such - which macOS
holds on the efficiency cores through a policy of the task that the class of a
thread does not lift; `--p-cores` clears that policy. Measured on an M1 Pro, the
same recording under `taskpolicy -b` takes 3.02 s of CPU time and shows a mean
timing delta of 795 ticks - an efficiency-core recording, unasked for - and with
`--p-cores` 0.54 s and 130 ticks, which are the values of a recording made in
the foreground. It remains a request: a process started through `posix_spawn()`
with a background QoS attribute is not brought back by it.

The efficiency cores are recorded as macOS runs background work on them, which
includes the lower clock it gives that class - not the same core at its own
maximum frequency, a combination no interface exposes. The note on `--max-mem`
above applies here as well.

## Recording of Raw Entropy Data

If the `invoke_testing.sh` is not helpful for performing the test, the following
explanation outlines the specific test steps to be invoked manually.

For recoding the raw entropic data, the user has to compile the code.
To do that, he has to copy the following files into the recording directory
prior compilation. These files are taken from his Jitter RNG implementation
that he uses:

	* jitterentropy-base.c

	* jitterentropy.h

Depending on the version of the Jitter RNG, the following commands have to
be invoked for compiling the test tool:

	* Jitter RNG 3.x: make -f Makefile.hashtime

The test is now invoked with the following command:

	* Jitter RNG 3.x:

		./jitterentropy-hashtime > /dev/shm/jent-raw.data

In addition, the collection of output data from the Jitter RNG must be
compiled with the following command:

	make -f Makefile.rng

To generate output data from the Jitter RNG for validation, invoke:

	./jitterentropy-rng 2> /dev/shm/jent.rngout

The program is compiled to collect a sample of 10000000 events each (see 
the ROUNDS parameter in Makefile).

## NTG.1 Recording

The NTG.1 raw data recording is provided with the shell script
`invoke_testing_ntg1.sh`. This script generates the following sets of data with
1,000,000 samples each:

* raw data of the "common" runtime operation and behavior (this data applies
to the random data generated by the Jitter RNG intended for reseeding a
deterministic RNG),

* raw data of the "hash loop" operation (this is the first of the two noise
sources used to generate the first 256 bit output block intended to initialize
a deterministic RNG), and

* raw data for the "memory access loop" operation (this is the second noise
source used to generate the first 256 bit output block intended to initialize
a deterministic RNG).

The generated data is intended to be processed with the
`validation-runtime/processdata_ntg1.sh` tool which delivers the staticial
data individually for the aforementioned data.

## NTG.1 Raw Noise Sources

BSI is interested in answering the following question: For NTG.1, two separate
noise sources must be used. The Jitter RNG offers the memory access and the
hash loop as raw noise sources. The question is how much entropy is delivered by
each of the noise source individually.

This question is relatively easy to answer for the hash loop, as the measurement
of the Keccak execution timing hardly requires any memory access (and thus
hardly has any interference with the memory access noise source), because
on bigger CPUs like Intel, the Keccak state fits into registers and thus the
"hash loop" measurement outlined in [NTG.1 Recording] measures this operation.
This is due to the fact that the "hash loop" measurement exclusively times
the Keccak operation. Nonetheless, if the hash loop provides insufficient
data, the analysis script `invoke_testing_hashloop.sh` can be invoked. The
data is analyzed with `validation-runtime/processdata_hashloop.sh`.

However, for the memory access, only the access to L2 or higher caches or the
main RAM are considered relevant, because the L1 at least to some degree used by
the hash loop operation and the remainder of the Jitter RNG logic as well
(e.g. the initial fetches). To achieve that testing of mainly the L2 cache, the
script `invoke_testing_memloop.sh` invokes the memory access loop with the
"deterministic" access pattern. This ensures the following:

* the decision on which byte in the memory array to access is determined with
  one ADD operation (as opposed to several instructions with the "quasi-random"
  access pattern). This ensures that access to the L1 instruction cache is very
  limited.
  
* apart from the calculation of the next byte to be accessed, only the byte
  itself is read, modified with an ADD and AND operation and written back again.
  This implies that again, the use of the L1 instruction cache is very
  limited and therefore only the memory access is truly measured. When the
  memory block is larger than the L1 cache, the memory access operation will
  require at least L2 acesses. 
  
With this testing approach, the L1 cache impact is reduced to become irrelevant
for the timing variations when selecting a memory block size that is larger than
the L1 data cache size. This approach now allows to almost exclusively
measure the timing variations derived from L2 or higher caches, or RAM itself.
  
The script `invoke_testing_memloop.sh` performs the testing with all commonly
supported memory sizes. Along with the analysis script 
`validation-runtime/processdata_memloop.sh` the entropy rate for each memory
block size is calculated. When now the memory sizes that are larger than the
tested CPU's L1 data cache are considered, the entropy rate that almost entirelyis derived from L2 is visible.
