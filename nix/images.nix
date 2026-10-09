# Live ISO and SD card images for real hardware.
ctx:
let
  inherit (ctx) isosFor kernelSetsFor lib machineFor mkIso mkSdImage nixpkgs
    sdImagesFor;
in
{
  # A live ISO for exercising the Jitter RNG on real hardware, e.g.
  # `nix build .#iso-linux_6_6`.
  mkIso = system: name: kernelPackages:
    (lib.nixosSystem {
      modules = [
        "${nixpkgs}/nixos/modules/installer/cd-dvd/installation-cd-minimal.nix"
        (machineFor kernelPackages)
        ({ config, lib, ... }: {
          nixpkgs.hostPlatform = system;
          image.baseName = lib.mkForce
            "jitterentropy-${name}-${config.boot.kernelPackages.kernel.version}";
          # all-hardware enables ZFS, which not every kernel here has a
          # compatible module for.
          boot.supportedFilesystems.zfs = lib.mkForce false;
          # Throwaway image, no state to migrate.
          system.stateVersion = lib.trivial.release;
        })
      ];
    }).config.system.build.isoImage;

  # One per nixpkgs kernel, plus default and latest.
  isosFor = system: pkgs:
    (lib.mapAttrs'
      (name: ps: lib.nameValuePair "iso-${name}" (mkIso system name ps))
      (kernelSetsFor pkgs)) // {
        iso = mkIso system "default" pkgs.linuxPackages;
        iso-latest = mkIso system "latest" pkgs.linuxPackages_latest;
      };

  # The same for the 64-bit Raspberry Pi boards, which have no bootable CD
  # path, e.g. `nix build .#sd-image-linux_6_18`.
  mkSdImage = system: name: kernelPackages:
    (lib.nixosSystem {
      modules = [
        "${nixpkgs}/nixos/modules/installer/sd-card/sd-image-aarch64.nix"
        (machineFor kernelPackages)
        ({ config, lib, ... }: {
          nixpkgs.hostPlatform = system;
          image.baseName = lib.mkForce
            "jitterentropy-${name}-${config.boot.kernelPackages.kernel.version}";
          # As for the ISO: not every kernel here has a ZFS module.
          boot.supportedFilesystems.zfs = lib.mkForce false;
          # Throwaway image, no state to migrate.
          system.stateVersion = lib.trivial.release;
        })
      ];
    }).config.system.build.sdImage;

  # One per kernel set, plus default and latest, mirroring the ISO set.
  sdImagesFor = system: pkgs:
    (lib.mapAttrs'
      (name: ps:
        lib.nameValuePair "sd-image-${name}" (mkSdImage system name ps))
      (kernelSetsFor pkgs)) // {
        sd-image = mkSdImage system "default" pkgs.linuxPackages;
        sd-image-latest = mkSdImage system "latest" pkgs.linuxPackages_latest;
      };
}
