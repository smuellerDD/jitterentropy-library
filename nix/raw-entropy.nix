# The raw entropy recording and validation tools, run in NixOS VMs.
ctx:
let
  inherit (ctx) fipsKernelFor lib machineFor machineWith moduleFor
    moduleSigningKey self toolsFor;
in
{
  # The tests/raw-entropy scripts as the README drives them: record from the
  # userspace library and the kernel module, then validate with the nixpkgs
  # NIST tools. The scripts build their own helpers, hence the compiler in
  # the VM. What is under test is the tooling, not the entropy rate.
  rawEntropyVmFor = pkgs:
    pkgs.testers.runNixOSTest {
      name = "jitterentropy-raw-entropy";

      nodes.machine = { pkgs, ... }: {
        imports = [ (machineFor pkgs.linuxPackages) ];
        boot.kernelParams = [ "clocksource=tsc" "tsc=reliable" ];
        virtualisation.qemu.options = [ "-cpu" "host" ];
        virtualisation.cores = 2;
        virtualisation.memorySize = 2048;
        virtualisation.diskSize = 4096;
        environment.systemPackages = with pkgs; [ gcc gnumake ];
      };

      # The Python lives in files of its own; what it takes from here is set
      # ahead of it.
      testScript = ''
        src = "${self}"
      '' + builtins.readFile ./raw-entropy/lib.py
         + builtins.readFile ./raw-entropy/test.py;
    };

  # The kernel-space recordings in FIPS mode, on the fipsKernelFor kernel
  # booted with fips=1: with the kernel's own Jitter RNG through its test
  # interface, and with jitter_rng.ko of this tree, signed with the key the
  # kernel trusts. The restart recorder is the shipped
  # boottime_test_record.sh: it records early in each boot and restarts the
  # VM, whose QEMU exits on that, so the test starts it again.
  rawEntropyFipsVmFor = pkgs:
    let
      tools = toolsFor pkgs;
      kernelPackages = fipsKernelFor pkgs;
      # Runs per restart recording, where the real test takes 1000.
      restarts = 1;
      unsigned = (moduleFor pkgs kernelPackages.kernel).override {
        withTestInterface = true;
      };
    in pkgs.testers.runNixOSTest {
      name = "jitterentropy-raw-entropy-fips";

      nodes.machine = { pkgs, ... }: {
        imports = [
          (machineWith kernelPackages {
            signingKey = moduleSigningKey;
            compressZstd = true;
          })
        ];
        boot.kernelParams = [
          "fips=1"
          # Buffers the first entropy events of each boot for the restart
          # recorder.
          "jitterentropy_testing.boot_raw_hires_test=1"
          "clocksource=tsc"
          "tsc=reliable"
        ];
        # The kernel builds its drivers in and has no TPM driver.
        boot.initrd.includeDefaultModules = false;
        boot.initrd.systemd.tpm2.enable = false;
        virtualisation.qemu.options = [ "-cpu" "host" ];
        virtualisation.cores = 2;
        virtualisation.memorySize = 2048;
        virtualisation.diskSize = 4096;
        environment.systemPackages = with pkgs; [ gcc gnumake ];

        # boottime_test_record.service with the paths NixOS has.
        systemd.services.boottime_test_record = {
          description = "Boot time test for Kernel Jitter RNG";
          unitConfig.DefaultDependencies = false;
          wants = [ "sys-kernel-debug.mount" ];
          after = [ "local-fs.target" "sys-kernel-debug.mount"
                    "systemd-modules-load.service" ];
          before = [ "sysinit.target" ];
          wantedBy = [ "basic.target" ];
          path = with pkgs; [ coreutils gnugrep util-linux systemd
                              pciutils ];
          environment = {
            GETRAWENTROPY = "${tools}/bin/getrawentropy";
            KCAPIRNG = "${pkgs.libkcapi}/bin/kcapi-rng";
            TESTS = toString restarts;
          };
          serviceConfig = {
            EnvironmentFile = "-/etc/default/boottime_test_record";
            ExecStart = "${pkgs.bash}/bin/bash ${self}/tests/raw-entropy/recording_restart_kernelspace/boottime_test_record.sh";
          };
        };
      };

      testScript = ''
        src = "${self}"
        restarts = ${toString restarts}
        unsigned_ko = "${unsigned}/lib/modules/${kernelPackages.kernel.modDirVersion}/extra/jitter_rng.ko"
      '' + builtins.readFile ./raw-entropy/lib.py
         + builtins.readFile ./raw-entropy/fips-test.py;
    };
}
