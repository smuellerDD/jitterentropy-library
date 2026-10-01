# Recording library

`libjitterentropy-record` records the raw noise data of the SP800-90B and
NTG.1 assessments from within a program, for a process no recording tool can
run in, such as an Android or iOS app. `jitterentropy-record.h` declares it:

- `jent_record_raw()` writes one file of one set, as
  `jitterentropy-hashtime <rounds> 1 <name>` does, and can hand back the
  `jent_status()` document of the collector it recorded with.
- `jent_record_run()` is `jitterentropy-hashtime` itself, driven by a
  `struct jent_record_config` instead of a command line: CPU pinning, the
  repeats, `--status`. What the tool prints it writes into a report for the
  caller; `jitterentropy-hashtime` is its command line and prints that report.
  The memory lock limit and the secure memory of the compliance modes stay
  with the caller, as the tool sets them up before calling it.
- `jent_record_alloc()`, `jent_record_prime()`, `jent_record_sample()` and
  `jent_record_free()` hand the caller one delta at a time and write no file.
  `jitterentropy-record-session.c` holds them and needs no libc, so the
  kernel module compiles it for its debugfs test interface
  (`linux_kernel/jitterentropy_testing.c`).

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

## Recording in the Android and iOS example apps

The recording scripts of `recording_userspace` need a shell, a compiler and a
file system the recording tool can be started in. A phone has none of these
for an app: iOS runs no command-line programs at all, and on Android a program
started through `adb shell` runs as another user, in another SELinux domain
and under another scheduler policy than an app does. The example apps in
`tests/android` and `tests/ios` therefore record the raw noise data
themselves, on their *Record* tab, in the process an app embedding the library
runs in.

The apps link `libjitterentropy-record` beside `libjitterentropy` and record
file by file through `jent_record_raw()`, which lets them show progress and
stop between files.
`jent_record_raw()` allocates the collector without the startup, so that the
data is that of exactly the configuration asked for, after the self tests of
the conditioning, and it records the raw counter ticks - not divided by the
common divisor an app's power-on tests determined. Another program recording
through the library would record the sets the table below lists.

### What is recorded

The three buttons of the *Record* tab record what the corresponding script
records, with the same file names:

| Button  | Script                   | Files                                                         |
|---------|--------------------------|---------------------------------------------------------------|
| Default | `invoke_testing.sh`      | `jent-raw-noise-0001.data`: 1,000,000 time deltas             |
|         |                          | `jent-raw-noise-restart-0001.data` to `-1000.data`: 1,000 each |
| FIPS    | `invoke_testing_fips.sh` | the same, with `JENT_FORCE_FIPS`                              |
| NTG.1   | `invoke_testing_ntg1.sh` | the same, with `JENT_NTG1`, and in addition:                  |
|         |                          | `jent-raw-noise_hashloop-0001.data`, `jent-raw-noise_memaccloop-0001.data` |
|         |                          | `jent-raw-noise-hashloop-restart-*.data`, `jent-raw-noise-memaccloop-restart-*.data` |

The timer thread switch, the oversampling rate, the memory size and the hash
loop count of the *Collector* tab apply as they do to a new collector; they
correspond to `--force-internal-timer`, `--osr`, `--max-mem` and
`--hloopcnt` of `jitterentropy-hashtime`. Leave them at their defaults (timer
thread off, 3, automatic, 1) for the data of the library's default
configuration.

Each file holds one decimal time delta per line. `recording.txt` beside them
names the device, the operating system, the library version, the
configuration and flags, and the health test failures seen per set - as with
the scripts, those do not stop a recording, but a set that fails its health
tests will hardly pass the validation.

A recording takes minutes - the default one two and a quarter on a Nexus 5X -
and writes about 9 MB, three times that for NTG.1.
The screen stays on while it runs; keep the app in the foreground, as a
suspended app is not measuring what the library measures in use. *Cancel*
stops it before the next file, which does not interrupt the one-million-delta
sets. The data goes to a directory `results-measurements.partial` first, which
replaces `results-measurements` only once the recording is complete, so an
incomplete recording never looks like data.

Data recorded in the Android emulator or the iOS simulator is that of the host
CPU, not the phone's, and of no use for an assessment of the phone.

### Obtaining the data

#### Android

The app writes to its internal storage, which `adb` reads through `run-as`, as
the example app is a debug build. `tar` copies the directory in one go:

```
adb exec-out run-as de.chronox.jitterentropy.example \
    tar -cf - -C files results-measurements | tar -xf - -C tests/raw-entropy
```

Uninstalling the app deletes the recording with it. The external files
directory, which `adb pull` reads directly, is not used: on a Nexus 5X with
Android 8.1, its emulated file system failed to create files in a directory
holding between two and three thousand, as an NTG.1 recording does, at a
different count on every try.

#### iOS

The app writes to `Documents/results-measurements`, which it shares: it is in
the Files app under *On My iPhone > Jitter RNG*, and in the Finder's file
sharing of the connected phone. On the command line, with the device name or
identifier `xcrun devicectl list devices` shows, and the app's bundle
identifier:

```
xcrun devicectl device copy from --device <device> \
    --domain-type appDataContainer --domain-identifier <bundle id> \
    --source Documents/results-measurements \
    --destination tests/raw-entropy/results-measurements
```

From the simulator, for trying the app out:

```
cp -R "$(xcrun simctl get_app_container booted <bundle id> data)/Documents/results-measurements" \
    tests/raw-entropy/
```

### Analyzing the data

The directory is what the recording scripts leave in
`tests/raw-entropy/results-measurements`, and needs no conversion. With it
there, the validation runs as for data recorded on a host (see
`../validation-runtime/README.md` and `../validation-restart/README.md` for
the SP800-90B tool they need):

```
cd tests/raw-entropy/validation-runtime && ./processdata.sh
cd tests/raw-entropy/validation-restart && ./processdata.sh
```

For an NTG.1 recording, `processdata_ntg1.sh` in both directories instead.
The results are in `tests/raw-entropy/results-analysis-runtime` and
`results-analysis-restart`; `../README.md` explains how to read them. Compare
them with the entropy rate of the configuration recorded: 1/OSR, and 8/OSR
for NTG.1, with the OSR in `recording.txt`.

To keep the recordings of several phones or configurations apart, copy each
into a directory of its own and name it to the scripts:

```
mkdir tests/raw-entropy/results-measurements-pixel8
adb exec-out run-as de.chronox.jitterentropy.example \
    tar -cf - -C files/results-measurements . | \
    tar -xf - -C tests/raw-entropy/results-measurements-pixel8
cd tests/raw-entropy/validation-runtime && \
    ./processdata.sh ../results-measurements-pixel8 ../results-analysis-runtime-pixel8
cd tests/raw-entropy/validation-restart && \
    ENTROPYDATA_DIR=../results-measurements-pixel8 \
    RESULTS_DIR=../results-analysis-restart-pixel8 ./processdata.sh
```

`analyze_options.sh` in both validation directories compares memory sizes: it
reads the recordings named `results-measurements-maxmem<N>`. Record *Default*
once per memory size and copy each into the directory of its `<N>`, the
position of the memory size setting - 1 for 1 kB up to 20 for 512 MB.
