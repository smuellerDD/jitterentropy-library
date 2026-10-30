# jitterentropy-rngd under its systemd unit, then in the foreground at -vvv.
# Prepended by nix/rngd.nix: lrng, whether the kernel has the LRNG.

# Type=notify: active once the kernel took the first injection.
machine.wait_for_unit("jitterentropy.service")

usage = machine.succeed("jitterentropy-rngd -h 2>&1 || true")
assert ("LRNG presence detected" in usage) == lrng, usage
if lrng:
    print(machine.succeed("cat /proc/lrng_type"))
else:
    machine.fail("test -e /proc/lrng_type")

# The daemon opens both at the start.
print(machine.succeed(
    "cat /proc/sys/kernel/random/entropy_avail"
    " /proc/sys/kernel/random/write_wakeup_threshold"))

# SIGUSR1: the status in the journal, at the unit's default verbosity.
machine.succeed("systemctl kill -s USR1 jitterentropy.service")
machine.wait_until_succeeds(
    "journalctl -u jitterentropy.service"
    " | grep -q 'Status of the Jitter RNG on SIGUSR1'", timeout=30)
machine.succeed("systemctl is-active jitterentropy.service")

# SIGUSR2: a reseed now, its outcome in the journal.
seeded = ("Injected 64 bytes into the LRNG" if lrng
          else "Reseeded the kernel with 64 bytes")
machine.succeed("systemctl kill -s USR2 jitterentropy.service")
machine.wait_until_succeeds(
    "journalctl -u jitterentropy.service"
    f" | grep -q '{seeded}, as SIGUSR2 asked'",
    timeout=60)
machine.succeed("systemctl is-active jitterentropy.service")

# Hardened under the unit, which narrows the capabilities further.
assert machine.succeed(
    "systemctl show -p LimitCORE --value jitterentropy.service").strip() == "0"
unit_status = machine.succeed(
    "cat /proc/$(systemctl show -p MainPID --value jitterentropy.service)"
    "/status")
assert "NoNewPrivs:\t1\n" in unit_status, unit_status
assert "CapEff:\t0000000000200000\n" in unit_status, unit_status

machine.succeed("systemctl stop jitterentropy.service")


def hardened(launch, caps="0000000000204000"):
    """Run the daemon by launch until it holds caps, return its PID."""
    machine.succeed(f"{launch} > /dev/null 2>&1 & echo $! > /tmp/rngd.pid")
    machine.wait_until_succeeds(
        f"grep -q '^CapEff:.{caps}$' /proc/$(cat /tmp/rngd.pid)/status",
        timeout=30)
    return machine.succeed("cat /tmp/rngd.pid").strip()


# As root by hand: the bounding set goes too.
pid = hardened("jitterentropy-rngd -F")
machine.succeed(f"grep -q '^CapBnd:.0000000000204000$' /proc/{pid}/status")
machine.succeed(f"kill {pid}")

# --generate needs less, but only once its file is open: root reaches this one
# through CAP_DAC_OVERRIDE alone.
pid = hardened("jitterentropy-rngd --generate 4000000000", "0000000000004000")
machine.succeed(f"kill {pid}")
machine.succeed(
    "install -d -o nobody -m 0700 /tmp/nobody &&"
    " jitterentropy-rngd --generate 32 --output /tmp/nobody/seed &&"
    " test $(stat -c %s /tmp/nobody/seed) = 32")

# Not dumpable: the kernel gives such a process's /proc files to root, whoever
# runs it. --generate takes no root, and these bytes outlast the test.
machine.succeed(
    "setpriv --reuid=65534 --regid=65534 --clear-groups"
    " jitterentropy-rngd --generate 4000000000 > /dev/null 2>&1 &"
    " echo $! > /tmp/rngd.pid")
machine.wait_until_succeeds(
    "test $(stat -c %u /proc/$(cat /tmp/rngd.pid)/fd) = 0", timeout=30)
machine.succeed("kill $(cat /tmp/rngd.pid)")


def run(args, secs):
    """The daemon in the foreground for secs seconds, ended by SIGTERM."""
    # The driver's shell stops at a failing command: the status is caught.
    out = machine.succeed(
        f"rc=0; timeout -s TERM {secs} jitterentropy-rngd -F -vvv {args}"
        " > /tmp/rngd.log 2>&1 || rc=$?; echo $rc; cat /tmp/rngd.log")
    status, _, log = out.partition("\n")
    print(log)
    # timeout reports a command it ended as 124, whatever the command did.
    assert status == "124", f"exit {status}"
    assert "Shutting down cleanly" in log, "no clean shutdown"
    for bad in ("- Error]", "- Warning]"):
        assert bad not in log, f"{bad} in the log"
    assert "Status of the Jitter RNG after its initialization:" in log
    assert ("Kernel RNG: the LRNG" if lrng
            else "RNDRESEEDCRNG works") in log
    assert "/proc/sys/crypto/fips_enabled: " in log
    return log, log.count("Injected 64 bytes")


# The default schedule.
log, injections = run("", 8)
print(f"default schedule: {injections} injections in 8 s")
if lrng:
    assert "LRNG: Inject 512 bits of data with 512 bits of entropy" in log
    assert "Alarm every" not in log, "an alarm with the LRNG"
    assert "Reseeding of kernel DRNG triggered" not in log
    assert injections >= 1
else:
    assert "Linux kernel >= 5.18: Inject 512 bits" in log
    assert ("Alarm every 5 s, forced reseed after 120 wakeups; after the"
            " first every 50 s and 12 wakeups") in log
    assert "Reseeding of kernel DRNG triggered" in log
    # A seeded kernel: no low estimate, /dev/random never writable.
    assert injections == 1, injections

# The LRNG has no schedule to set: the daemon refuses one at its start.
if lrng:
    for args in ("-t 2", "--phase1 5:120", "--phase2 50:12"):
        out = machine.fail(f"jitterentropy-rngd -F {args} 2>&1")
        assert "has no effect on a kernel with the LRNG" in out, out
else:
    log, injections = run("-t 2", 9)
    print(f"-t 2: {injections} injections in 9 s")
    assert ("Alarm every 2 s, forced reseed after 1 wakeups") in log
    assert injections >= 4, injections

# --oneshot and --generate.
machine.succeed("jitterentropy-rngd --oneshot")
machine.succeed(
    "test \"$(jitterentropy-rngd --generate 32 --hex | wc -c)\""
    " = 64")
