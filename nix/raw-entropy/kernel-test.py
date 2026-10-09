# The test script of rawEntropyKernelVmFor in ../raw-entropy.nix, which sets
# src, restarts and samples ahead of it.

raw = "/root/jent/tests/raw-entropy"
# Where boottime_test_record.sh keeps its runs.
out = "/root/results-measurements"
ea = (
    "EATOOL_NONIID=$(command -v ea_non_iid) "
    "EATOOL=$(command -v ea_restart) "
)

def run(d, cmd):
    machine.succeed(f"cd {raw}/{d} && {ea} {cmd} >&2")

# The recorder reboots after each run but the last, and QEMU exits on each
# reboot.
def boot_through_restarts():
    for _ in range(restarts - 1):
        machine.wait_for_shutdown()
        machine.start()
    machine.wait_for_unit("multi-user.target")

# One file of 1000 time deltas per run, the run count stored, the platform
# written by the last run, and the unit through without an error.
def restarts_recorded():
    machine.succeed(f"test \"$(cat {out}/jent_state)\" = {restarts}")
    machine.succeed(f"test -s {out}/platform.txt")
    files = machine.succeed(
        f"ls {out}/jent-raw-noise-restart.*.data"
    ).split()
    assert len(files) == restarts, files
    for f in files:
        machine.succeed(
            f"test \"$(wc -l < {f})\" = 1000"
            # Time deltas vary; a stuck or constant source not.
            f" && test \"$(sort -u {f} | wc -l)\" -gt 10"
        )
    machine.succeed(
        "test \"$(systemctl show -P ExecMainStatus boottime_test_record)\""
        " = 0"
    )

# validation-runtime on a recording of jent-raw-noise-0001.data in data.
def validate_runtime(data, results):
    run("validation-runtime",
        f"MAX_EVENTS={samples} ./processdata.sh {data} {results}")
    print(machine.succeed(f"grep -h 'min(' {results}/*.minentropy_*.txt"))

# validation-restart on the 1000 restarts in data. A good source fails
# ea_restart's sanity check in about 1% of the runs, so a failure gets one
# more try on a new recording.
def validate_restart(data, results):
    for attempt in (1, 2):
        status, _ = machine.execute(
            f"cd {raw}/validation-restart && {ea}"
            f" ENTROPYDATA_DIR={data} RESULTS_DIR={results}"
            " ./processdata.sh >&2"
        )
        if status == 0:
            break
        assert attempt == 1, "restart validation failed twice"
        machine.succeed(f"rm -rf {results} {data}/jent-raw-noise-restart-*")
        run("recording_runtime_kernelspace",
            f"OUTDIR={data} bash -c '. ./invoke_testing_helper.sh;"
            " raw_entropy_restart'")
    print(machine.succeed(f"grep -h 'min(' {results}/*.minentropy_*.txt"))

machine.start()

with subtest("restart recording, vanilla kernel"):
    boot_through_restarts()
    restarts_recorded()
    kmsg = machine.succeed("journalctl -k -b")
    assert "One time data collection test enabled" in kmsg

machine.succeed(f"cp -r {src} /root/jent && chmod -R u+w /root/jent")
# With RESULTS_DIR given, the caller builds extractlsb.
run("validation-restart", "make")

with subtest("runtime recording and validation, vanilla kernel"):
    # As recording_runtime_kernelspace/README.md. The boot buffer is off,
    # so the recording starts afresh, and kcapi-rng drives the RNG, which
    # records only while it is used.
    data = "/root/vanilla-measurements"
    machine.succeed(
        "echo 0 > /sys/module/jitterentropy_testing/parameters/"
        "boot_raw_hires_test"
    )
    machine.succeed(
        f"mkdir -p {data} &&"
        # Bounded: without kcapi-rng the reader waits forever.
        " { timeout 1800 getrawentropy --timestamps"
        " -f /sys/kernel/debug/jitterentropy_testing/jent_raw_hires"
        f" -s {samples} > {data}/jent-raw-noise-0001.data"
        " & rec=$!;"
        " while kill -0 $rec 2>/dev/null; do"
        " kcapi-rng -n jitterentropy_rng -b 65536 > /dev/null; done;"
        " wait $rec; }"
    )
    machine.succeed(
        f"test \"$(wc -l < {data}/jent-raw-noise-0001.data)\" = {samples}"
    )
    validate_runtime(data, "/root/vanilla-runtime")

with subtest("runtime and restart recording and validation, jitter_rng"):
    data = "/root/jitter_rng-measurements"
    run("recording_runtime_kernelspace",
        f"OUTDIR={data} NUM_EVENTS={samples} ./invoke_testing.sh")
    machine.succeed(
        f"test \"$(ls {data}/jent-raw-noise-restart-*.data | wc -l)\""
        " = 1000"
    )
    validate_runtime(data, "/root/jitter_rng-runtime")
    validate_restart(data, "/root/jitter_rng-restart")

with subtest("restart recording, jitter_rng"):
    # As recording_restart_kernelspace/README.md has it for the module. The
    # recorder cannot disable a NixOS unit, so it starts again on the next
    # boot; without the results of the last series it records afresh.
    machine.succeed(
        f"rm -rf {out} && mkdir -p /etc/default && printf '%s\\n'"
        " DEBUGFS_FILE=/sys/kernel/debug/jitter_rng/jent_raw_hires"
        " RAW_OPTS= KCAPI_NAME=jitter_rng"
        " > /etc/default/boottime_test_record"
    )
    machine.shutdown()
    machine.start()
    boot_through_restarts()
    restarts_recorded()
