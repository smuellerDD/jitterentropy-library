# The test script of mkVmTest in ../vm.nix.

machine.wait_for_unit("multi-user.target")

machine.succeed("grep -q '^jitter_rng ' /proc/modules")
machine.succeed("test -c /dev/jitterentropy")

print(machine.succeed("cat /proc/jitterentropy/statistics"))
print(machine.succeed("cat /proc/jitterentropy/hwrng_status"))

# The size the machine configuration pins reached the module.
out = machine.succeed("cat /proc/jitterentropy/config/flags")
assert "max memory size: 32 MB" in " ".join(out.split()), out

# Reading opens an instance; its UUID-named status file appears.
machine.succeed(
    "exec 3</dev/jitterentropy; "
    "test \"$(ls /proc/jitterentropy/instances | wc -l)\" -ge 1; "
    "head -c 32 /proc/jitterentropy/instances/* >/dev/null; "
    "exec 3<&-"
)
machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")

# JENT_IOCSTATUS on the chardev: the instance's JSON status.
print(machine.succeed(
    "jitterentropy-chardev-status | jq -e .uuid"
))

# The single-field ioctls agree with that document.
print(machine.succeed("jitterentropy-chardev-fields"))

# O_NONBLOCK reads: short-read cap and EAGAIN on contention.
print(machine.succeed("python3 /etc/jitterentropy-nonblock-test.py"))

# The debugfs test interface delivers the raw time deltas.
machine.succeed("dmesg --clear")
machine.succeed(
    "test \"$(head -c 64 /sys/kernel/debug/jitter_rng/jent_raw_hires"
    " | wc -c)\" = 64"
)

# With verbose=1 that open logged the instance's JSON status.
# printk truncates records at about 1 kB, so it is emitted line by
# line: the complete document must be in the log.
import json
import re

kernel_log = machine.succeed("dmesg")
# Strip the timestamps and undo dmesg's escaping of the tabs.
msgs = [
    re.sub(r"^\[[^\]]*\] ?", "", line).replace("\\x09", "\t")
    for line in kernel_log.splitlines()
]
start = msgs.index("{")
end = msgs.index("}", start)
doc = "\n".join(msgs[start:end + 1])
print(doc)
json.loads(doc)

# JENT_IOCSTATUS on the debugfs interface. Raw instances skip the
# startup and carry no UUID, hence the version field.
print(machine.succeed(
    "jitterentropy-chardev-status"
    " /sys/kernel/debug/jitter_rng/jent_raw_hires"
    " | jq -e .version"
))

# The single-field ioctls there; the tool asserts that JENT_IOCUUID
# reports ENODATA.
print(machine.succeed(
    "jitterentropy-chardev-fields"
    " /sys/kernel/debug/jitter_rng/jent_raw_hires"
))

# getrawentropy end to end: --samples N yields exactly N values.
machine.succeed(
    "test \"$(getrawentropy --samples 100 --osr 3 | wc -l)\" = 100"
)

# --loopcnt drives JENT_IOCLOOPCNT.
machine.succeed(
    "test \"$(getrawentropy --samples 100 --loopcnt 4 | wc -l)\""
    " = 100"
)

# --status fetches the JSON status and records nothing.
print(machine.succeed(
    "getrawentropy --samples 1 --status | jq -e .version"
))

# The CMake-built userspace tools are on PATH.
for tool in ("jitterentropy-rng", "jitterentropy-osr",
             "jitterentropy-hashtime", "jitterentropy-cpuinfo",
             "jitterentropy-gcd", "extractlsb", "getrawentropy",
             "jitterentropy-chardev-status",
             "jitterentropy-chardev-fields", "jitter_rng"):
    machine.succeed(f"command -v {tool}")

# jitter_rng, through libjitterentropy-kernel: the module's information,
# the status document and random bytes, binary and as hexadecimal digits.
out = machine.succeed("jitter_rng")
print(out)
for line in ("version:", "oversampling rate:", "uuid:", "memory size:",
             "hash loops:", "output:"):
    assert line in out, f"jitter_rng --info lacks {line}"
assert "unavailable" not in out, out
assert "interfaces: kcapi hwrng chardev testing" in " ".join(out.split()), out
machine.succeed("jitter_rng --status | jq -e .uuid")
machine.succeed("test \"$(jitter_rng --random 100 | wc -c)\" = 100")
machine.succeed("test \"$(jitter_rng -r 32 -x | tr -d '\\n' | wc -c)\" = 64")
machine.succeed("jitter_rng --selftest")

print(machine.succeed(
    "python3 /etc/jitterentropy-loopcnt-test.py"
))
# The recording tool rejects the same values itself.
out = machine.fail("getrawentropy --samples 1 --loopcnt 65537 2>&1")
assert "out of range" in out, out

# The recording takes a signal per measurement, not per batch of
# 1000. At the slowest loop count a batch takes clearly longer than
# a per-measurement check may take to answer.
def measurement_ms(loopcnt, samples=1):
    return int(machine.succeed(
        "start=$(date +%s%N); "
        f"getrawentropy --samples {samples} --loopcnt {loopcnt}"
        " >/dev/null; "
        "echo $(( ($(date +%s%N) - start) / 1000000 ))"
    ).strip())

loopcnt = 1 << 16
per_measurement = measurement_ms(loopcnt)
batch = per_measurement * 1000
limit = 2000 + 4 * per_measurement + 3000
print(f"loopcnt {loopcnt}: {per_measurement} ms per measurement, "
      f"{batch} ms per batch of 1000")
assert batch > 2 * limit, f"batch of {batch} ms too short to tell"

answered = int(machine.succeed(
    "start=$(date +%s%N); "
    f"timeout -s INT 2 getrawentropy --samples 100000"
    f" --loopcnt {loopcnt} >/dev/null || true; "
    "echo $(( ($(date +%s%N) - start) / 1000000 ))"
).strip())
print(f"SIGINT answered after {answered} ms")
assert answered < limit, \
    f"SIGINT answered after {answered} ms, batch is {batch} ms"

# Documents reporting instance activity are root only; the
# configuration files stay world readable.
machine.succeed(
    "test \"$(stat -c %a /proc/jitterentropy/hwrng_status)\" = 400"
)
machine.succeed(
    "test \"$(stat -c %a /proc/jitterentropy/statistics)\" = 400"
)
machine.succeed(
    "test \"$(stat -c %a /proc/jitterentropy/instances)\" = 500"
)
for world_readable in ("version",
                       "config/flags", "config/flags_raw",
                       "config/osr", "config/ntg1", "config/fips",
                       "interfaces/kcapi", "interfaces/hwrng",
                       "interfaces/chardev", "interfaces/testing"):
    machine.succeed(
        "test \"$(stat -c %a"
        f" /proc/jitterentropy/{world_readable})\" = 444"
    )

# An unprivileged caller is refused there but can still read the
# device. setpriv rather than runuser, which would reset PATH.
unpriv = "setpriv --reuid=65534 --regid=65534 --clear-groups"
for root_only in ("hwrng_status", "statistics"):
    out = machine.fail(
        f"{unpriv} cat /proc/jitterentropy/{root_only} 2>&1"
    )
    assert "Permission denied" in out, out
out = machine.fail(f"{unpriv} ls /proc/jitterentropy/instances 2>&1")
assert "Permission denied" in out, out
machine.succeed(
    f"test \"$({unpriv} sh -c"
    " 'head -c 32 /dev/jitterentropy | wc -c')\" = 32"
)

# The per-instance status file likewise.
machine.succeed(
    "exec 3</dev/jitterentropy; "
    "test \"$(stat -c %a /proc/jitterentropy/instances/*)\" = 400; "
    f"denied=$({unpriv} sh -c"
    " 'cat /proc/jitterentropy/instances/*' 2>&1 || true); "
    "case $denied in *'Permission denied'*) ;; "
    "*) echo \"$denied\"; exit 1;; esac; "
    "exec 3<&-"
)

# The concurrent-instance cap. Outside NTG.1: the counting is under
# test, not the compliance startup of every open.
machine.succeed("rmmod jitter_rng")
machine.succeed("modprobe jitter_rng max_instances=4 ntg1=0")
machine.wait_for_file("/dev/jitterentropy")
machine.succeed(
    "test \"$(cat /sys/module/jitter_rng/parameters/max_instances)\""
    " = 4"
)
print(machine.succeed(
    "python3 /etc/jitterentropy-maxinstances-test.py 4"
))

# An unprivileged caller is held to the cap. It cannot read the
# counters, so they are checked here: 4 + 1 admitted opens.
before = json.loads(
    machine.succeed("cat /proc/jitterentropy/statistics")
)["charDevice"]
print(machine.succeed(
    f"{unpriv}"
    " python3 /etc/jitterentropy-maxinstances-test.py 4"
))
after = json.loads(
    machine.succeed("cat /proc/jitterentropy/statistics")
)["charDevice"]
assert after["openInstances"] == before["openInstances"], after
assert after["cumulativeOpens"] == \
    before["cumulativeOpens"] + 5, after

# The kernel crypto API cap, reached through AF_ALG. The second
# unprivileged run finds every slot of the first released.
machine.succeed("test \"$(cat /proc/jitterentropy/interfaces/kcapi)\" = 1")
machine.succeed("rmmod jitter_rng")
machine.succeed("modprobe jitter_rng max_kcapi_instances=4 ntg1=0")
machine.wait_for_file("/dev/jitterentropy")
machine.succeed(
    "test \"$(cat"
    " /sys/module/jitter_rng/parameters/max_kcapi_instances)\" = 4"
)
# Linux 7.3 refuses every AF_ALG rng bind with ENOENT unless
# af_alg_restrict is 0; the sysctl appears with af_alg.
machine.succeed(
    "modprobe algif_rng || true;"
    " f=/proc/sys/crypto/af_alg_restrict;"
    " [ ! -e $f ] || echo 0 > $f"
)
print(machine.succeed(
    "python3 /etc/jitterentropy-maxkcapi-test.py 4"
))
for _ in range(2):
    print(machine.succeed(
        f"{unpriv} python3 /etc/jitterentropy-maxkcapi-test.py 4"
    ))

# max_memsize pins the memory access region of every instance.
machine.succeed("rmmod jitter_rng")
machine.succeed("modprobe jitter_rng max_memsize=1024")
machine.wait_for_file("/dev/jitterentropy")
machine.succeed(
    "test \"$(cat /sys/module/jitter_rng/parameters/max_memsize)\""
    " = 1024"
)
out = machine.succeed("cat /proc/jitterentropy/config/flags")
assert "max memory size: 1 MB" in " ".join(out.split()), out
machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")

# A size the field cannot hold refuses the load.
machine.succeed("rmmod jitter_rng")
machine.fail("modprobe jitter_rng max_memsize=3")
machine.fail("modprobe jitter_rng max_memsize=1048576")
# 0 overrides the machine's pinned size with the derivation.
machine.succeed("modprobe jitter_rng max_memsize=0")
machine.wait_for_file("/dev/jitterentropy")
out = machine.succeed("cat /proc/jitterentropy/config/flags")
assert "max memory size: auto" in " ".join(out.split()), out

# An oversampling rate above the maximum refuses the load; the
# NTG.1 ceiling itself loads and generates.
machine.succeed("rmmod jitter_rng")
machine.fail("modprobe jitter_rng osr=100")
machine.succeed("modprobe jitter_rng osr=20")
machine.wait_for_file("/dev/jitterentropy")
machine.succeed("test \"$(cat /proc/jitterentropy/config/osr)\" = 20")
machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")

# 0 is unbounded for both caps. 512 kB per instance, so that three
# hundred fit into the VM; the crypto API run is unprivileged,
# which the default cap would refuse. Outside NTG.1 and FIPS mode,
# where one of three hundred startups may fail on a runner's clock.
machine.succeed("rmmod jitter_rng")
machine.succeed(
    "modprobe jitter_rng max_instances=0 max_kcapi_instances=0"
    " cache_all=0 max_memsize=512 ntg1=0"
)
out = machine.succeed("cat /proc/jitterentropy/config/fips")
assert out.strip() == "0", out
machine.wait_for_file("/dev/jitterentropy")
print(machine.succeed(
    "python3 /etc/jitterentropy-maxinstances-test.py 0 300"
))
print(machine.succeed(
    f"{unpriv} python3 /etc/jitterentropy-maxkcapi-test.py 0 300"
))

# Back to the configuration the machine is set up with.
machine.succeed("rmmod jitter_rng")
machine.succeed("modprobe jitter_rng")
machine.wait_for_file("/dev/jitterentropy")
machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")
