# Tests of Entropy during early boot

This test collects the first 1,001 entropy event values generated during boot
of the Linux kernel. The collection of raw entropy after reboot is compliant
to SP800-90B section 3.1.4.

The test infrastructure sets the Linux system up to reboot the system
some 1,000 times to collect these 1,001 event values.

# Test procedure

See boottime_test_record.sh. The test applies to the Jitter RNG of the vanilla
kernel with `CONFIG_CRYPTO_JITTERENTROPY_TESTINTERFACE`, booted with
`jitterentropy_testing.boot_raw_hires_test=1`.

`install.sh` installs the script and the service and enables it. It expects
getrawentropy in `/usr/local/sbin/getrawentropy`, where the script calls it.

The script reads `/sys/kernel/debug/jitterentropy_testing/jent_raw_hires` and
drives the RNG with `kcapi-rng -n "jitterentropy_rng"`. For the out-of-tree
module of `linux_kernel/`, loaded at boot with its test interface, change them
to `/sys/kernel/debug/jitter_rng/jent_raw_hires` and `jitter_rng` in the
script, and build getrawentropy with `-DRAW_DATATYPE_U64`. That module has no boot time buffer: each boot records the first 1,001
time deltas of the raw-noise instance its debugfs file allocates when the
script opens it.

The result is one file per boot operation,
`/root/results-measurements/jent-raw-noise-restart.<run>.data`, holding the
1,001 successive values getrawentropy reads from the test interface, one per
line. The script calls getrawentropy without `--timestamps`, which the
interface of the vanilla kernel needs for time deltas (see
`../recording_runtime_kernelspace/README.md`).

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
