# The test script of rawEntropyVmFor in ../raw-entropy.nix, which sets src and
# puts lib.py ahead of it.

machine.wait_for_unit("multi-user.target")

machine.succeed(f"cp -r {src} /root/jent && chmod -R u+w /root/jent")

with subtest("userspace"):
    run("recording_userspace", "./invoke_testing.sh")
    assess("userspace")

with subtest("userspace, FIPS"):
    run("recording_userspace", "./invoke_testing_fips.sh")
    assess("fips")

with subtest("userspace, NTG.1"):
    run("recording_userspace", "./invoke_testing_ntg1.sh")
    assess_ntg1()

# The sweeps are recorded short and not validated: the high
# settings take tens of minutes at 1000000 samples, and what is
# under test is that every setting records.
sweep = 10000

with subtest("userspace, hash loop sweep"):
    run("recording_userspace",
        f"NUM_EVENTS={sweep} ./invoke_testing_hashloop.sh")
    recorded("jent-raw-noise_hashloop_*-0001.data", 8, sweep)
    reset()

with subtest("userspace, memory access sweep"):
    run("recording_userspace",
        f"NUM_EVENTS={sweep} ./invoke_testing_memloop.sh")
    recorded("jent-raw-noise_memaccloop_deterministic*-0001.data",
             20, sweep)
    reset()

with subtest("userspace, common operation sweep"):
    run("recording_userspace",
        f"NUM_EVENTS={sweep} ./invoke_testing_commonop.sh")
    recorded("jent-raw-noise_hashloop_*-0001.data", 8, sweep)
    recorded("jent-raw-noise_memaccloop_deterministic*-0001.data",
             20, sweep)
    reset()

# The other programs the directory's Makefiles build.
with subtest("userspace, programs"):
    d = "recording_userspace"
    run(d, "make -f Makefile.rng")
    machine.succeed(
        f"cd {raw}/{d} && test"
        ' "$(./jitterentropy-rng 1000 2>/dev/null | wc -c)" = 32000'
    )
    run(d, "make -f Makefile.osr && ./jitterentropy-osr 10 1000000")
    run(d, "make -f Makefile.cpuinfo && ./jitterentropy-cpuinfo")
    machine.succeed(
        f"cd {raw}/{d} && ./jitterentropy-cpuinfo --json | jq -e ."
    )

with subtest("kernel"):
    run("recording_runtime_kernelspace", "./invoke_testing.sh")
    assess("kernel")

with subtest("kernel, NTG.1"):
    run("recording_runtime_kernelspace", "./invoke_testing_ntg1.sh")
    assess_ntg1()
