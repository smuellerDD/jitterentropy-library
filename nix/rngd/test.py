# The test script of rngdVmFor in ../rngd.nix.

import json

machine.wait_for_unit("jitterentropy.service")

# Type=notify: the unit is active once the daemon sent READY=1, which it
# does after the kernel accepted its first injection.
out = machine.succeed(
    "systemctl show -p Type,ActiveState,SubState,NRestarts"
    " jitterentropy.service"
)
for prop in ("Type=notify", "ActiveState=active", "SubState=running",
             "NRestarts=0"):
    assert prop in out, out

# The unit runs without -v, where only errors are logged: there are none.
journal = machine.succeed("journalctl -b -u jitterentropy.service -o cat")
assert "Error]" not in journal, journal

# The daemon is released with the library and reports its version.
out = machine.succeed("jitterentropy-rngd --version 2>&1")
assert "jitterentropy-rngd" in out and "Jitterentropy Core" in out, out

# --status prints the status document and exits without daemonizing.
_, out = machine.execute("jitterentropy-rngd --status 2>&1")
doc = json.loads(out[out.index("{"):out.rindex("}") + 1])
assert doc["version"], doc
assert not doc["configuration"]["ntg1Mode"], doc
assert not doc["configuration"]["fipsMode"], doc

# --fips and --ntg1 survive a later --flags.
_, out = machine.execute(
    "jitterentropy-rngd --fips --ntg1 --flags 0 --status 2>&1"
)
doc = json.loads(out[out.index("{"):out.rindex("}") + 1])
assert doc["configuration"]["ntg1Mode"], doc
assert doc["configuration"]["fipsMode"], doc

_, out = machine.execute("jitterentropy-rngd -i --status 2>&1")
doc = json.loads(out[out.index("{"):out.rindex("}") + 1])
assert doc["configuration"]["internalTimer"], doc

# Conflicting timer requests are refused by the library.
rc, out = machine.execute("jitterentropy-rngd -n -i --status 2>&1")
assert rc != 0 and "{" not in out, out

# A second instance in the foreground: it injects, and leaves on SIGTERM.
_, out = machine.execute(
    "timeout -s TERM 3 jitterentropy-rngd -F -vvv 2>&1"
)
print(out)
assert "Injected 64 bytes" in out, out
assert "Reseeding of kernel DRNG triggered" in out, out
assert "Warning]" not in out and "Error]" not in out, out
assert "Shutting down cleanly" in out, out

# A forced reseed after every wakeup, and invalid phases are refused.
_, out = machine.execute(
    "timeout -s TERM 5 jitterentropy-rngd -F -vvv --phase1 1:1 "
    "--phase2 1:1 2>&1"
)
assert out.count("Force reseed") >= 2, out
rc, out = machine.execute("jitterentropy-rngd --phase2 1:0 2>&1")
assert rc == 1 and "Usage" in out, out

# --oneshot seeds the kernel and exits on its own.
rc, out = machine.execute("timeout 60 jitterentropy-rngd --oneshot -vvv 2>&1")
assert rc == 0, out
assert "Injected 64 bytes" in out, out

# The service stops cleanly as well.
machine.succeed("systemctl stop jitterentropy.service")
out = machine.succeed("systemctl show -p Result jitterentropy.service")
assert "Result=success" in out, out
