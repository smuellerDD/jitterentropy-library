# The test script of rawEntropyFipsVmFor in ../raw-entropy.nix, which sets src,
# restarts and unsigned_ko and puts lib.py ahead of it.

out = "/root/results-measurements"

# The recorder restarts the VM after each run but the last, and
# QEMU exits on each restart.
def boot_through_restarts():
    for _ in range(restarts - 1):
        machine.wait_for_shutdown()
        machine.start()
    machine.wait_for_unit("multi-user.target")

# One file of 1001 time deltas per run, none failed, the run
# count stored.
def restarts_recorded():
    machine.succeed(
        f"test \"$(cat {out}/jent_state)\" = {restarts}"
        f" && test \"$(cat {out}/jent_failures)\" = 0"
        # A failure recorded again later leaves its
        # .data.failed.<n> behind.
        f" && ! ls {out}/*.failed* 2>/dev/null"
        # Written by the last run.
        f" && test -s {out}/platform.txt"
    )
    files = machine.succeed(
        f"ls {out}/jent-raw-noise-restart.*.data"
    ).split()
    assert len(files) == restarts, files
    for f in files:
        machine.succeed(
            f"test \"$(wc -l < {f})\" = 1001"
            # Time deltas vary; a stuck or constant source not.
            f" && test \"$(sort -u {f} | wc -l)\" -gt 10"
        )

machine.start()

with subtest("restart recording, vanilla kernel"):
    boot_through_restarts()
    restarts_recorded()
    # From the journal: jitter_rng's messages overrun the ring
    # buffer. Not grep -q in a pipe: it kills journalctl with
    # SIGPIPE, which the driver's pipefail makes a failure.
    kmsg = machine.succeed("journalctl -k -b")
    assert "One time data collection test enabled" in kmsg

with subtest("FIPS mode, and only signed modules"):
    machine.succeed("test \"$(cat /proc/sys/crypto/fips_enabled)\" = 1")
    # Loaded means verified, under MODULE_SIG_FORCE. The taint is
    # O, out of tree, without E, an unsigned module.
    machine.succeed(
        "test \"$(modinfo -F sig_id jitter_rng)\" = 'PKCS#7'"
    )
    taint = machine.succeed("cat /sys/module/jitter_rng/taint").strip()
    assert taint == "O", f"jitter_rng taint {taint}"
    tainted = int(machine.succeed("cat /proc/sys/kernel/tainted"))
    assert not tainted & 8192, f"tainted {tainted}: unsigned module"
    out_of_tree = machine.fail(
        f"insmod {unsigned_ko} 2>&1"
    )
    assert "Key was rejected" in out_of_tree, out_of_tree
    # The kernel's own modules: all compressed, all signed. extra/
    # holds jitter_rng's, which would hide a kernel without any.
    machine.succeed(
        "mods=$(find -L /run/booted-system/kernel-modules/lib/modules"
        " -name '*.ko.zst' ! -path '*/extra/*') && test -n \"$mods\" &&"
        " test -z \"$(find -L /run/booted-system/kernel-modules/lib/modules"
        " -path '*/kernel/*' -name '*.ko')\" &&"
        " for m in $mods; do"
        " test \"$(modinfo -F sig_id $m)\" = 'PKCS#7'"
        " || { echo $m; exit 1; }; done"
    )

machine.succeed(f"cp -r {src} /root/jent && chmod -R u+w /root/jent")

# The recordings alone: raw-entropy-vm validates them.
with subtest("runtime recording, vanilla kernel"):
    # As recording_runtime_kernelspace/README.md. The boot buffer
    # is off, so the recording starts afresh.
    machine.succeed(
        "echo 0 > /sys/module/jitterentropy_testing/parameters/"
        "boot_raw_hires_test"
    )
    d = f"{raw}/recording_runtime_kernelspace"
    machine.succeed(
        f"cd {d} && gcc -Wall -pedantic -Wextra -I../../.."
        " -I../../../linux_kernel -DRAW_DATATYPE_U64"
        " -o getrawentropy getrawentropy.c"
    )
    machine.succeed(
        f"mkdir -p {raw}/results-measurements && cd {d} &&"
        # Bounded: without kcapi-rng the reader waits forever.
        " { timeout 1800 ./getrawentropy --timestamps"
        " -f /sys/kernel/debug/jitterentropy_testing/jent_raw_hires"
        " -s 1000000 > ../results-measurements/jent-raw-noise-0001.data"
        " & rec=$!;"
        " while kill -0 $rec 2>/dev/null; do"
        " kcapi-rng -n jitterentropy_rng -b 65536 > /dev/null; done;"
        " wait $rec; }"
    )
    recorded("jent-raw-noise-0001.data", 1, 1000000)
    reset()

with subtest("runtime recording, jitter_rng"):
    run("recording_runtime_kernelspace", "./invoke_testing.sh")
    recorded("jent-raw-noise-0001.data", 1, 1000000)
    recorded("jent-raw-noise-restart-*.data", 1000, 1000)
    reset()

with subtest("restart recording, jitter_rng"):
    # The recorder's systemctl disable fails on a NixOS unit, so
    # the unit starts again on the next boot, which this series
    # relies on. Removing the results makes it record again.
    machine.succeed(
        f"rm -rf {out} && printf '%s\\n'"
        " DEBUGFS_FILE=/sys/kernel/debug/jitter_rng/jent_raw_hires"
        " RAW_OPTS= KCAPI_NAME=jitter_rng"
        " > /etc/default/boottime_test_record"
    )
    machine.shutdown()
    machine.start()
    boot_through_restarts()
    restarts_recorded()
