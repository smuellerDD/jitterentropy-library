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
        // import ./nix/consumers.nix ctx
        // import ./nix/kernel.nix ctx
        // import ./nix/vm.nix ctx
        // import ./nix/efi.nix ctx
        // import ./nix/raw-entropy.nix ctx
        // import ./nix/images.nix ctx);

      inherit (ctx)
        consumersFor crossTargets cryptoFor efiFor efiVmFor forAllSystems
        inTreeBuildsFor isosFor modulesFor muslFor muslStaticFor rawEntropyFor
        sdImagesFor toolsFor vmTestsFor;
    in {
      packages = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = toolsFor pkgs;
          jitterentropy-tools = toolsFor pkgs;
          musl = muslFor pkgs;
          musl-static = muslStaticFor pkgs;
        } // cryptoFor pkgs
          // modulesFor pkgs
          // inTreeBuildsFor pkgs
          // consumersFor pkgs
          // isosFor system pkgs
          # The SD images boot Raspberry Pi boards, which are aarch64.
          // lib.optionalAttrs (system == "aarch64-linux")
            (sdImagesFor system pkgs)
          # The cross toolchains are x86_64-linux only, and so is
          # the EFI application - see efiFor in nix/efi.nix.
          // lib.optionalAttrs (system == "x86_64-linux") (crossTargets pkgs // {
            efi = efiFor pkgs "x86_64";
            efi-aarch64 = efiFor pkgs "aarch64";
          }));

      # `nix flake check` boots every VM and runs its assertions.
      checks =
        forAllSystems (system:
          vmTestsFor nixpkgs.legacyPackages.${system} // {
            # Boots nothing: the scripts in the build sandbox.
            raw-entropy = rawEntropyFor nixpkgs.legacyPackages.${system};
          }
          # The EFI application boots no kernel and needs no NixOS, but it is a
          # VM that has to come up and say the right thing, so it belongs here
          # with the rest of them.
          // lib.optionalAttrs (system == "x86_64-linux") {
            efi-vm = efiVmFor nixpkgs.legacyPackages.${system} "x86_64";
            efi-vm-aarch64 = efiVmFor nixpkgs.legacyPackages.${system} "aarch64";
          });

      # `nix run .#vm-linux_6_6` opens the same VM interactively: `start_all()`
      # then `machine.shell_interact()`.
      apps = forAllSystems (system:
        let
          # The NixOS tests only: the EFI VM and the raw entropy check are
          # plain derivations, with no driver to open.
          runners = lib.mapAttrs (_name: test: {
            type = "app";
            program = "${test.driverInteractive}/bin/nixos-test-driver";
          }) (lib.filterAttrs (_name: test: test ? driverInteractive)
            self.checks.${system});
        in runners // { default = runners.vm; });
    };
}
