# The NixOS VM tests of the kernel module.
ctx:
let
  inherit (ctx) kernelSetsFor lib machineFor mkVmTest moduleFor toolsFor
    vmTestsFor;
in
{
  # Shared by the VM tests and the live images: the chosen kernel with
  # jitter_rng loaded, the tools, and the testing conveniences. Both are
  # test environments, hence the debugfs test interface.
  machineFor = kernelPackages:
    { config, lib, pkgs, ... }: {
      boot.kernelPackages = kernelPackages;
      boot.extraModulePackages = [
        ((moduleFor pkgs config.boot.kernelPackages.kernel).override {
          withTestInterface = true;
        })
      ];
      boot.kernelModules = [ "jitter_rng" ];
      # Per-instance JSON status to the kernel log; test systems only.
      boot.extraModprobeConfig = ''
        options jitter_rng verbose=1 ntg1=1 cache_all=1 selftest_interval=15
      '';
      # libjitterentropy-kernel and its jitter_rng tool as well, which
      # need the module: this is the only place they meet a real one.
      environment.systemPackages = [
        ((toolsFor pkgs).overrideAttrs (old: {
          cmakeFlags = (old.cmakeFlags or [ ]) ++ [ "-DENABLE_KERNEL_LIB=ON" ];
        }))
      ] ++ (with pkgs; [
        fx
        htop
        jq
        libkcapi
        python3
        sp800-90b-entropyassessment
        tmux
        vim
        xxd
      ]);
      # The chardev O_NONBLOCK semantics: reads capped at one 32-byte
      # buffer, and EAGAIN rather than waiting on a concurrent reader.
      environment.etc."jitterentropy-nonblock-test.py".text = ''
          import os
          import threading
          import time

          fd = os.open("/dev/jitterentropy", os.O_RDONLY | os.O_NONBLOCK)

          data = os.read(fd, 4096)
          assert len(data) == 32, f"nonblocking read returned {len(data)} bytes"

          # A large blocking read holds the instance lock per 32-byte
          # chunk, so a nonblocking read usually sees EAGAIN; the loop
          # below tolerates winning the gap between chunks. O_NONBLOCK is
          # checked on entry, hence the sleep before flipping it back.
          os.set_blocking(fd, True)
          t = threading.Thread(target=os.read, args=(fd, 4 * 1024 * 1024),
                               daemon=True)
          t.start()
          time.sleep(0.5)
          os.set_blocking(fd, False)

          deadline = time.monotonic() + 10
          while True:
              try:
                  os.read(fd, 16)
              except BlockingIOError:
                  break
              assert time.monotonic() < deadline, "no EAGAIN observed"
              time.sleep(0.01)
          print("OK")
      '';
      # The ISO profile autologs in "nixos"; these are test images.
      services.getty.autologinUser = lib.mkForce "root";
      console.keyMap = "de";
      environment.shellAliases = {
        "sample_kernel" = "getrawentropy --ntg1 --samples 1000000 --debugfs-file /sys/kernel/debug/jitter_rng/jent_raw_hires";
        "clock_rdtsc" = "echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource";
        "jitter_hwrng" = "echo jitterentropy > /sys/class/misc/hw_random/rng_current";
        "show_hwrng" = "cat /sys/class/misc/hw_random/rng_current";
        "kcapi_read" = "kcapi-rng -n jitter_rng -b 32 --hex";
      };
    };

  # Boots the shared machine configuration on the chosen kernel: a flake
  # check, and a `nix run` target through its interactive driver.
  mkVmTest = pkgs: name: kernelPackages:
    pkgs.testers.runNixOSTest {
      name = "jitterentropy-${name}";

      nodes.machine = {
        imports = [ (machineFor kernelPackages) ];
        boot.kernelParams = [ "clocksource=tsc" "tsc=reliable" ];
        virtualisation.qemu.options = [ "-cpu" "host" ];
        # The O_NONBLOCK test needs a poller running while a reader holds
        # the instance lock. On one vCPU a non-preemptible build (5.10,
        # PREEMPT_VOLUNTARY) only reschedules after unlock, so the poller
        # would never see EAGAIN. A second vCPU makes it real
        # concurrency.
        virtualisation.cores = 2;
      };

      testScript = ''
        machine.wait_for_unit("multi-user.target")

        # The module is loaded and its interfaces are present.
        machine.succeed("lsmod | grep -q '^jitter_rng'")
        machine.succeed("test -c /dev/jitterentropy")

        # procfs exports, including the per-instance status directory.
        print(machine.succeed("cat /proc/jitterentropy/statistics"))
        print(machine.succeed("cat /proc/jitterentropy/hwrng_status"))

        # Reading opens an instance; its UUID-named status file appears.
        machine.succeed(
            "exec 3</dev/jitterentropy; "
            "test \"$(ls /proc/jitterentropy/instances | wc -l)\" -ge 1; "
            "head -c 32 /proc/jitterentropy/instances/* >/dev/null; "
            "exec 3<&-"
        )
        machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")

        # The chardev JENT_IOCSTATUS ioctl delivers the instance's JSON
        # status (the tool also probes the EOVERFLOW length-report path).
        print(machine.succeed(
            "jitterentropy-chardev-status | jq -e .uuid"
        ))

        # And the single-field ioctls agree with that document, field by
        # field: UUID, version, osr, flags, health failure state, output
        # counters and reinitialization count.
        print(machine.succeed("jitterentropy-chardev-fields"))

        # O_NONBLOCK reads: short-read cap and EAGAIN on contention.
        print(machine.succeed("python3 /etc/jitterentropy-nonblock-test.py"))

        # libjitterentropy-kernel through its tool: the module and an
        # instance, the status document, random bytes in both forms, and
        # the self test, which needs CAP_SYS_ADMIN - the test runs as root.
        print(machine.succeed("jitter_rng --info"))
        machine.succeed("jitter_rng --status | jq -e .uuid")
        machine.succeed("test \"$(jitter_rng --random 64 | wc -c)\" = 64")
        machine.succeed(
            "jitter_rng --random 16 --hex | grep -Eqx '[0-9a-f]{32}'"
        )
        print(machine.succeed("jitter_rng --selftest"))

        # The debugfs raw entropy test interface delivers the raw noise
        # time deltas of the measure_jitter operation.
        machine.succeed("dmesg --clear")
        machine.succeed(
            "test \"$(head -c 64 /sys/kernel/debug/jitter_rng/jent_raw_hires"
            " | wc -c)\" = 64"
        )

        # With verbose=1 (set via modprobe.d in the machine
        # configuration), the open of the test interface logged the
        # recording instance's JSON status to the kernel log. printk
        # truncates records at about 1 kB, so the status is emitted line
        # by line; verify that the complete document landed in the log
        # buffer.
        import json
        import re

        kernel_log = machine.succeed("dmesg")
        # Strip the timestamp prefix and undo dmesg's escaping of the
        # tab indentation.
        msgs = [
            re.sub(r"^\[[^\]]*\] ?", "", line).replace("\\x09", "\t")
            for line in kernel_log.splitlines()
        ]
        start = msgs.index("{")
        end = msgs.index("}", start)
        doc = "\n".join(msgs[start:end + 1])
        print(doc)
        json.loads(doc)

        # The JENT_IOCSTATUS ioctl is also exposed on the debugfs test
        # interface, reporting the status of the per-open raw-noise
        # recording instance (the tool takes the file to query as
        # argument). Raw instances skip the startup sequence and thus
        # carry no UUID, so assert on the version field instead.
        print(machine.succeed(
            "jitterentropy-chardev-status"
            " /sys/kernel/debug/jitter_rng/jent_raw_hires"
            " | jq -e .version"
        ))

        # The same single-field ioctls, on the same interface. A raw
        # instance carries no UUID, so JENT_IOCUUID must report ENODATA
        # there rather than an empty string; the tool asserts that.
        print(machine.succeed(
            "jitterentropy-chardev-fields"
            " /sys/kernel/debug/jitter_rng/jent_raw_hires"
        ))

        # getrawentropy drives the same interface end-to-end: it sets the
        # testing_osr module parameter and prints the raw time delta
        # samples unmodified. --samples N yields exactly N values.
        machine.succeed(
            "test \"$(getrawentropy --samples 100 --osr 3 | wc -l)\" = 100"
        )

        # --loopcnt drives the JENT_IOCLOOPCNT ioctl: a fixed loop count
        # overrides the instance's configured hash and memory access loop
        # counts for the recorded measurements.
        machine.succeed(
            "test \"$(getrawentropy --samples 100 --loopcnt 4 | wc -l)\""
            " = 100"
        )

        # --status fetches the recording instance's JSON status via
        # JENT_IOCSTATUS and records nothing.
        print(machine.succeed(
            "getrawentropy --samples 1 --status | jq -e .version"
        ))

        # The CMake-built userspace tools are on PATH.
        for tool in ("jitterentropy-rng", "jitterentropy-osr",
                     "jitterentropy-hashtime", "jitterentropy-cpuinfo",
                     "gcd", "extractlsb", "getrawentropy",
                     "jitterentropy-chardev-status",
                     "jitterentropy-chardev-fields", "jitter_rng"):
            machine.succeed(f"command -v {tool}")
      '';
    };

  # One VM test per nixpkgs kernel, plus the default and latest kernels.
  vmTestsFor = pkgs:
    (lib.mapAttrs'
      (name: ps: lib.nameValuePair "vm-${name}" (mkVmTest pkgs "vm-${name}" ps))
      (kernelSetsFor pkgs)) // {
        vm = mkVmTest pkgs "vm" pkgs.linuxPackages;
        vm-latest = mkVmTest pkgs "vm-latest" pkgs.linuxPackages_latest;
      };
}
