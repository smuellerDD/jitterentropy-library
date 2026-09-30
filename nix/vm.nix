# The NixOS VM tests of the kernel module, one per kernel.
ctx:
let
  inherit (ctx) kernelSetsFor lib machineFor machineWith mkVmTest moduleFor
    toolsFor;
in
{
  # Shared by the VM tests and the live images: the chosen kernel with
  # jitter_rng loaded, the tools, and the debugfs test interface.
  machineFor = kernelPackages: machineWith kernelPackages { };

  # moduleArgs: further moduleFor overrides, such as a signing key.
  machineWith = kernelPackages: moduleArgs:
    { config, lib, pkgs, ... }: {
      boot.kernelPackages = kernelPackages;
      boot.extraModulePackages = [
        ((moduleFor pkgs config.boot.kernelPackages.kernel).override ({
          withTestInterface = true;
        } // moduleArgs))
      ];
      boot.kernelModules = [ "jitter_rng" ];
      # verbose: per-instance JSON status to the kernel log. max_memsize
      # keeps the hundreds of instances the tests open within the VM's
      # memory.
      boot.extraModprobeConfig = ''
        options jitter_rng verbose=1 ntg1=1 cache_all=1 selftest_interval=15 max_memsize=32768
      '';
      environment.systemPackages = [
        (toolsFor pkgs)
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
      # Run in the guest by the test script.
      environment.etc."jitterentropy-nonblock-test.py".source = ./vm/nonblock-test.py;
      environment.etc."jitterentropy-maxinstances-test.py".source = ./vm/maxinstances-test.py;
      environment.etc."jitterentropy-maxkcapi-test.py".source = ./vm/maxkcapi-test.py;
      environment.etc."jitterentropy-loopcnt-test.py".source = ./vm/loopcnt-test.py;
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

  # The shared machine on the chosen kernel: a flake check, and a `nix run`
  # target through its interactive driver.
  mkVmTest = pkgs: name: kernelPackages:
    pkgs.testers.runNixOSTest {
      name = "jitterentropy-${name}";

      nodes.machine = {
        imports = [ (machineFor kernelPackages) ];
        boot.kernelParams = [ "clocksource=tsc" "tsc=reliable" ];
        virtualisation.qemu.options = [ "-cpu" "host" ];
        # The O_NONBLOCK test needs real concurrency: on one vCPU a
        # non-preemptible kernel (5.10) only reschedules after unlock, and
        # the poller never sees EAGAIN.
        virtualisation.cores = 2;
        # Every open runs the NTG.1 startup. On a runner's nested-KVM clock
        # it can fail permanently at osr 3, which the open reports as
        # ENOMEM; twice the minimum leaves headroom. Later modprobes that
        # set osr override this.
        boot.extraModprobeConfig = ''
          options jitter_rng osr=6
        '';
      };

      testScript = builtins.readFile ./vm/test.py;
    };

  # One per nixpkgs kernel, plus default and latest.
  vmTestsFor = pkgs:
    (lib.mapAttrs'
      (name: ps: lib.nameValuePair "vm-${name}" (mkVmTest pkgs "vm-${name}" ps))
      (kernelSetsFor pkgs)) // {
        vm = mkVmTest pkgs "vm" pkgs.linuxPackages;
        vm-latest = mkVmTest pkgs "vm-latest" pkgs.linuxPackages_latest;
      };
}
