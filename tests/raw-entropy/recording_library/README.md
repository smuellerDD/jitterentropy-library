# Recording library

`libjitterentropy-record` records the raw noise data of the SP800-90B and
NTG.1 assessments from within a program, for a process no recording tool can
run in. `jitterentropy-record.h` declares it:

- `jent_record_raw()` writes one file of one set, as
  `jitterentropy-hashtime <rounds> 1 <name>` does, and can hand back the
  `jent_status()` document of the collector it recorded with.
- `jent_record_run()` is `jitterentropy-hashtime` itself, driven by a
  `struct jent_record_config` instead of a command line: CPU pinning, the
  repeats, `--status`. What the tool prints it writes into a report for the
  caller, which `jent_record_report_len()` sizes; `jitterentropy-hashtime` is
  its command line and prints that report. The memory lock limit and the
  secure memory of the compliance modes stay with the caller, as the tool sets
  them up before calling it.
- `jent_record_alloc()`, `jent_record_prime()`, `jent_record_sample()` and
  `jent_record_free()` hand the caller one delta at a time and write no file.
  `jitterentropy-record-session.c` holds them and needs no libc, so the
  kernel module compiles it for its debugfs test interface
  (`linux_kernel/jitterentropy_testing.c`).
- `jent_record_health_names()` names the health tests of a failure mask as
  `jitterentropy-hashtime` lists them.

None prints anything. The library carries a copy of `libjitterentropy` of
its own and exports nothing but these functions, so it links beside
`libjitterentropy`, shared or static; `record-colink.c` tests that. The build
offers it only when asked to - `-DENABLE_RECORDING=ON` with CMake,
`ENABLE_RECORDING=1` with make - shared or static as the library itself.
`jitterentropy-record-lib.c` compiles the copy and the recording as one
translation unit, a private copy (`JENT_PRIVATE_COMPILE`) in which every
function of the library is static, so both the archive and the shared library
define the recording and nothing else by construction, with any toolchain.
On the ELF linkers `record.lds` limits the shared library's exports to the API
of `jitterentropy-record.h` as well; CMake checks it against the header.

`jitterentropy-hashtime` compiles the same recording code into itself rather
than linking the library, so a recording made through the library and one made
with the tool are taken the same way.

## What is recorded

`jent_record_raw()` allocates the collector without the startup, so that the
data is that of exactly the configuration asked for, after the self tests of
the conditioning, and it records the raw counter ticks - not divided by the
common divisor the startup would determine. Each file holds one decimal time
delta per line. Health test failures are reported in the result and do not
end a recording, but a set that fails its health tests will hardly pass the
validation.

To be analyzed by the scripts of `validation-runtime` and
`validation-restart`, record the sets the corresponding script of
`recording_userspace` records, with the same file names:

| Script                   | Files                                                         |
|--------------------------|---------------------------------------------------------------|
| `invoke_testing.sh`      | `jent-raw-noise-0001.data`: 1,000,000 time deltas             |
|                          | `jent-raw-noise-restart-0001.data` to `-1000.data`: 1,000 each, each from a collector of its own |
| `invoke_testing_fips.sh` | the same, with `JENT_FORCE_FIPS`                              |
| `invoke_testing_ntg1.sh` | the same, with `JENT_NTG1`, and in addition:                  |
|                          | `jent-raw-noise_hashloop-0001.data`, `jent-raw-noise_memaccloop-0001.data` |
|                          | `jent-raw-noise-hashloop-restart-*.data`, `jent-raw-noise-memaccloop-restart-*.data` |

The hash loop and memory access sets are recorded with the noise source
`JENT_RECORD_HASHLOOP` and `JENT_RECORD_MEMACCESS`, the others with
`JENT_RECORD_COMMON`. The scripts show the options each set is recorded with.

## Analyzing the data

Put the files into `tests/raw-entropy/results-measurements`, where the
recording scripts leave them; they need no conversion. The validation then runs
as for data recorded with the tools (see `../validation-runtime/README.md` and
`../validation-restart/README.md` for the SP800-90B tool they need):

```
cd tests/raw-entropy/validation-runtime && ./processdata.sh
cd tests/raw-entropy/validation-restart && ./processdata.sh
```

For an NTG.1 recording, `processdata_ntg1.sh` in both directories instead.
The results are in `tests/raw-entropy/results-analysis-runtime` and
`results-analysis-restart`; `../README.md` explains how to read them. Compare
them with the entropy rate of the configuration recorded: 1/OSR, and 8/OSR
for NTG.1.
