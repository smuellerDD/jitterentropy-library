{
  description =
    "Jitter RNG: userspace library/tools (CMake) and out-of-tree kernel module, plus NixOS VMs runnable via nix run and nix flake check.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # One file per area in nix/: each takes this set and adds its entries,
      # lib.fix ties them together lazily.
      ctx = lib.fix (ctx: { inherit self nixpkgs lib; }
        // import ./nix/common.nix ctx
        // import ./nix/userspace.nix ctx
        // import ./nix/android.nix ctx
        // import ./nix/consumers.nix ctx
        // import ./nix/kernel.nix ctx
        // import ./nix/vm.nix ctx
        // import ./nix/efi.nix ctx
        // import ./nix/output-entropy.nix ctx
        // import ./nix/raw-entropy.nix ctx
        // import ./nix/rngd.nix ctx
        // import ./nix/images.nix ctx);

      inherit (ctx)
        androidAppFor androidEmulatorFor androidFor consumersFor crossTargets
        cryptoFor efiFor efiVmFor fipsKernelFor forAllSystems inTreeBuildsFor
        isosFor modulesFor muslFor muslStaticFor outputEntropyFor
        rawEntropyFipsVmFor rawEntropyVmFor rngdVmFor sdImagesFor toolsFor
        vmTestsFor;
    in {
      packages = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = toolsFor pkgs;
          jitterentropy-tools = toolsFor pkgs;
          musl = muslFor pkgs;
          musl-static = muslStaticFor pkgs;
          # The kernel of the raw-entropy-fips-vm check, and its config.
          fips-kernel = (fipsKernelFor pkgs).kernel;
          fips-kernel-config = (fipsKernelFor pkgs).kernel.configfile;
        } // cryptoFor pkgs
          // modulesFor pkgs
          // inTreeBuildsFor pkgs
          // consumersFor pkgs
          // isosFor system pkgs
          # Raspberry Pi boards are aarch64.
          // lib.optionalAttrs (system == "aarch64-linux")
            (sdImagesFor system pkgs)
          # The NDK, the cross toolchains and the EFI application are
          # x86_64-linux only.
          // lib.optionalAttrs (system == "x86_64-linux") (crossTargets pkgs // {
            efi = efiFor pkgs "x86_64";
            efi-aarch64 = efiFor pkgs "aarch64";
            android = androidFor system;
            android-example = androidAppFor system;
            android-example-emulator =
              androidEmulatorFor system (androidAppFor system);
          }));

      # `nix flake check` boots every VM and runs its assertions.
      checks =
        forAllSystems (system:
          vmTestsFor nixpkgs.legacyPackages.${system}
          # Boots nothing: runs the library in the build sandbox.
          // { output-entropy = outputEntropyFor nixpkgs.legacyPackages.${system}; }
          // { raw-entropy-vm = rawEntropyVmFor nixpkgs.legacyPackages.${system}; }
          // {
            raw-entropy-fips-vm =
              rawEntropyFipsVmFor nixpkgs.legacyPackages.${system};
          }
          // { rngd-vm = rngdVmFor nixpkgs.legacyPackages.${system}; }
          // lib.optionalAttrs (system == "x86_64-linux") {
            efi-vm = efiVmFor nixpkgs.legacyPackages.${system} "x86_64";
            efi-vm-aarch64 = efiVmFor nixpkgs.legacyPackages.${system} "aarch64";
          });

      # `nix run .#vm-linux_6_6` opens the same VM interactively:
      # `start_all()`, then `machine.shell_interact()`. Only the NixOS tests
      # have a driver.
      apps = forAllSystems (system:
        let
          runners = lib.mapAttrs (_name: test: {
            type = "app";
            program = "${test.driverInteractive}/bin/nixos-test-driver";
          }) (lib.filterAttrs (_: t: t ? driverInteractive)
            self.checks.${system});
        in runners // { default = runners.vm; });
    };
}
