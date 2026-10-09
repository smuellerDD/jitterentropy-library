# Tests of Entropy during early boot

This test collects the first 1,000 time deltas the Jitter RNG measures during
boot of the Linux kernel. The collection of raw entropy after reboot is
compliant to SP800-90B section 3.1.4.

The test infrastructure sets the Linux system up to reboot the system
some 1,000 times to collect these 1,000 time deltas each time.

# Test procedure

See boottime_test_record.sh. The test applies to the Jitter RNG of the vanilla
kernel with `CONFIG_CRYPTO_JITTERENTROPY_TESTINTERFACE`, booted with
`jitterentropy_testing.boot_raw_hires_test=1`.

`install.sh` installs the script and the service and enables it. It expects
getrawentropy in `/usr/local/sbin/getrawentropy`, where the script calls it.
Build it for the kernel it runs on: from Linux 6.13 on, the vanilla test
interface stores its time stamps as `u64` (check the type of `jent_testing_rb`
in `crypto/jitterentropy-testing.c`), and getrawentropy MUST then be built
with `-DRAW_DATATYPE_U64`. Built without it, it reads each stamp as two `u32`
halves, and every boot records nonsense deltas without any error.

The script reads `/sys/kernel/debug/jitterentropy_testing/jent_raw_hires` and
drives the RNG with `kcapi-rng -n "jitterentropy_rng"`. For the out-of-tree
module of `linux_kernel/`, loaded at boot with its test interface, set
`DEBUGFS_FILE=/sys/kernel/debug/jitter_rng/jent_raw_hires`, `KCAPI_NAME=jitter_rng`
and an empty `RAW_OPTS=` (dropping `--timestamps`), and build getrawentropy
with `-DRAW_DATATYPE_U64`. That module has no boot time buffer: each boot
records the first 1,000 time deltas of the raw-noise instance its debugfs file
allocates when the script opens it.

These, as well as `OUTDIR`, `TESTS`, `KCAPIRNG` and `GETRAWENTROPY`, are read
from the environment, e.g. from an `EnvironmentFile=` added to the service;
the values above are the defaults.

The result is one file per boot operation,
`/root/results-measurements/jent-raw-noise-restart.<run>.data`, holding 1,000
time deltas, one per line. The interface of the vanilla kernel delivers the raw
time stamps it read at boot - its buffer holds 1,024 - so the script calls
getrawentropy with `--timestamps`, which prints the deltas of 1,001 successive
time stamps (see `../recording_runtime_kernelspace/README.md`).

The number of files equals the number of reboots. Once `TESTS` (1000) runs are
recorded, the script disables the service and writes the platform details to
`/root/results-measurements/platform.txt`. To start the test over, clean out
`/root/results-measurements`, including its state file `jent_state`, and
enable the service again (see boottime_test_record.sh). Without
`/usr/bin/kcapi-rng` the script writes that error into the data file of the
run and ends the test at once.

# Test analysis

Copy the obtained files into `results-measurements` and process them by
invoking `validation-restart/processdata.sh`.
