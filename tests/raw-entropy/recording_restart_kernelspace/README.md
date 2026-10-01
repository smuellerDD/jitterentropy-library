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

The script's settings - the tool paths, the debugfs file, the getrawentropy
options, the RNG kcapi-rng drives and the number of runs - can be given in
`/etc/default/boottime_test_record`, which the service reads. For the
out-of-tree module of `linux_kernel/`, loaded at boot with its test interface,
set

```
DEBUGFS_FILE=/sys/kernel/debug/jitter_rng/jent_raw_hires
RAW_OPTS=
KCAPI_NAME=jitter_rng
```

`install.sh` installs the script and the service and enables it. It checks
for getrawentropy where the script will look for it: `GETRAWENTROPY` from its
environment or from `/etc/default/boottime_test_record`, by default
`/usr/local/sbin/getrawentropy`, which has to be an absolute path: the service
runs from `/`. The installer reads that file as systemd does only for plain
`KEY=VALUE` lines, a value at most enclosed in quotes as a whole. Rather than
read it otherwise than the service would, it refuses the whole file if any
line ends in a backslash (comments included), holds a CR before its end, or,
outside comments, holds a backslash or a quote other than one pair around a
whole value - whichever key that line sets: to systemd an open quote carries
on into the lines below it. A path with blanks is fine unquoted. The script
takes `TESTS` as a positive decimal number only, leading zeros allowed;
anything else stops the test without a reboot.

The out-of-tree module has no boot time buffer: each boot records the first 1,001 time deltas of
the raw-noise instance its debugfs file allocates when the script opens it.

The result is one file per boot operation,
`/root/results-measurements/jent-raw-noise-restart.<run>.data`, holding the
1,001 successive time deltas recorded by the Jitter RNG, one per line.

The number of files equals the number of successful runs. A run whose
recording failed, timed out or came up short is renamed to
`jent-raw-noise-restart.<run>.data.failed.<n>`, the n-th failure of that run,
does not count, and is recorded again on the next boot; ten failures in a row
stop the test. Without kcapi-rng the test stops at once, leaving the error in
`jent-raw-noise-restart.<run>.error`, which the analysis does not pick up.

Once the test is complete, the script disables the service and writes the
platform details to `/root/results-measurements/platform.txt`; a failed disable
is reported on stderr (the journal of the service). A boot with the test
already complete - the service enabled again, or its disable failed - records
nothing and does the same again; to start the test over, clean out the
results and the state files in `/root/results-measurements` first (see
boottime_test_record.sh).

# Test analysis

Copy the obtained files into `results-measurements` and process them by
invoking `validation-restart/processdata.sh`.
