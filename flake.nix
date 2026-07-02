{
  description =
    "Jitter RNG: userspace library/tools (CMake) and out-of-tree kernel module, plus NixOS VMs runnable via nix run and nix flake check.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # VMs run under QEMU on the host architecture.
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: lib.genAttrs systems (system: f system);

      # Userspace library and tools, built with the project's CMake build.
      # Installs jitterentropy-{hashtime,osr,rng}, gcd and extractlsb into bin.
      toolsFor = pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-tools";
          version = "3.7.1";
          src = self;
          nativeBuildInputs = [ pkgs.cmake ];
          enableParallelBuilding = true;
          meta = {
            description = "Jitter RNG userspace library and validation tools";
            license = lib.licenses.bsd3;
          };
        };

      # Out-of-tree kernel module (jitter_rng.ko) built against a given kernel.
      # The Kbuild.config in linux_kernel/ enables the crypto, character device
      # and hwrng interfaces, so the resulting module carries all of them.
      moduleFor = pkgs: kernel:
        pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-kmod";
          version = kernel.version;
          src = self;

          hardeningDisable = [ "pic" "format" ];
          nativeBuildInputs = kernel.moduleBuildDependencies;

          buildPhase = ''
            runHook preBuild
            make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
              M=$(pwd)/linux_kernel modules
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            install -D linux_kernel/jitter_rng.ko \
              "$out/lib/modules/${kernel.modDirVersion}/extra/jitter_rng.ko"
            runHook postInstall
          '';

          meta = {
            description = "Jitter RNG out-of-tree Linux kernel module";
            license = lib.licenses.gpl2Plus;
          };
        };

      # A NixOS integration test that boots a VM with the chosen kernel, the
      # jitter_rng module built against it and loaded at boot, and the userspace
      # tools in the system profile. Used both as a flake check (the test runs
      # the assertions in a VM) and, via its interactive driver, as a `nix run`
      # target (no dedicated system.build.vm image is produced).
      mkVmTest = pkgs: name: kernelPackages:
        pkgs.testers.runNixOSTest {
          name = "jitterentropy-${name}";

          nodes.machine = { config, pkgs, ... }: {
            boot.kernelPackages = kernelPackages;
            boot.extraModulePackages =
              [ (moduleFor pkgs config.boot.kernelPackages.kernel) ];
            boot.kernelModules = [ "jitter_rng" ];
            environment.systemPackages = [
              (toolsFor pkgs)
            ] ++ (with pkgs; [
              fx
              htop
              jq
              libkcapi
              sp800-90b-entropyassessment
              tmux
              vim
              xxd
            ]);
            services.getty.autologinUser = "root";
            console.keyMap = "de";
            environment.shellAliases = {
              "jitter_hwrng" = "echo jitterentropy > /sys/class/misc/hw_random/rng_current";
              "show_hwrng" = "cat /sys/class/misc/hw_random/rng_current";
              "kcapi_read" = "kcapi-rng -n jitter_rng -b 32 --hex";
            };
          };

          testScript = ''
            machine.wait_for_unit("multi-user.target")

            # The module is loaded and its interfaces are present.
            machine.succeed("lsmod | grep -q '^jitter_rng'")
            machine.succeed("test -c /dev/jitterentropy")

            # procfs exports, including the per-instance status directory.
            print(machine.succeed("cat /proc/jitter_rng/statistics"))
            print(machine.succeed("cat /proc/jitter_rng/hwrng_status"))

            # Reading opens an instance; its UUID-named status file appears.
            machine.succeed(
                "exec 3</dev/jitterentropy; "
                "test \"$(ls /proc/jitter_rng/instances | wc -l)\" -ge 1; "
                "head -c 32 /proc/jitter_rng/instances/* >/dev/null; "
                "exec 3<&-"
            )
            machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")

            # The CMake-built userspace tools are on PATH.
            for tool in ("jitterentropy-rng", "jitterentropy-osr",
                         "jitterentropy-hashtime", "gcd", "extractlsb"):
                machine.succeed(f"command -v {tool}")
          '';
        };

      # Every kernel package set exposed by nixpkgs that provides a real kernel
      # derivation. tryEval guards the sets that fail to evaluate (unfree,
      # unsupported on the current system, ...). Note that board-specific
      # kernels (e.g. linux_rpi*) only boot on a matching host architecture.
      kernelSetsFor = pkgs:
        lib.filterAttrs (_name: ps:
          let
            r = builtins.tryEval
              (lib.isAttrs ps && ps ? kernel && lib.isDerivation ps.kernel);
          in r.success && r.value) pkgs.linuxKernel.packages;

      # One VM test per nixpkgs kernel, plus the default and latest kernels.
      vmTestsFor = pkgs:
        (lib.mapAttrs'
          (name: ps: lib.nameValuePair "vm-${name}" (mkVmTest pkgs "vm-${name}" ps))
          (kernelSetsFor pkgs)) // {
            vm = mkVmTest pkgs "vm" pkgs.linuxPackages;
            vm-latest = mkVmTest pkgs "vm-latest" pkgs.linuxPackages_latest;
          };
    in {
      packages = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = toolsFor pkgs;
          jitterentropy-tools = toolsFor pkgs;
          # Module built against the nixpkgs default kernel.
          jitterentropy-module = moduleFor pkgs pkgs.linuxPackages.kernel;
        });

      # `nix flake check` boots every VM and runs its assertions. Individual
      # VMs can be run with e.g. `nix build .#checks.x86_64-linux.vm-linux_6_6`.
      checks =
        forAllSystems (system: vmTestsFor nixpkgs.legacyPackages.${system});

      # `nix run .#vm-linux_6_6` launches the same VM interactively through the
      # NixOS test driver (run `start_all()` then `machine.shell_interact()`).
      apps = forAllSystems (system:
        let
          runners = lib.mapAttrs (_name: test: {
            type = "app";
            program = "${test.driverInteractive}/bin/nixos-test-driver";
          }) self.checks.${system};
        in runners // { default = runners.vm; });
    };
}
