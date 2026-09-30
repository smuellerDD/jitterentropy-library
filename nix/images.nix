# Bootable ISO and SD card images with the kernel module.
ctx:
let
  inherit (ctx) kernelSetsFor lib machineFor mkIso mkSdImage nixpkgs;
in
{
  # A live ISO for real hardware, e.g. `nix build .#iso-linux_6_6`.
  mkIso = system: name: kernelPackages:
    (lib.nixosSystem {
      modules = [
        "${nixpkgs}/nixos/modules/installer/cd-dvd/installation-cd-minimal.nix"
        (machineFor kernelPackages)
        ({ config, lib, ... }: {
          nixpkgs.hostPlatform = system;
          image.baseName = lib.mkForce
            "jitterentropy-${name}-${config.boot.kernelPackages.kernel.version}";
          # Not every kernel here has a compatible ZFS module.
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

  # The same for the 64-bit Raspberry Pi boards, e.g.
  # `nix build .#sd-image-linux_6_18`.
  mkSdImage = system: name: kernelPackages:
    (lib.nixosSystem {
      modules = [
        "${nixpkgs}/nixos/modules/installer/sd-card/sd-image-aarch64.nix"
        (machineFor kernelPackages)
        ({ config, lib, ... }: {
          nixpkgs.hostPlatform = system;
          image.baseName = lib.mkForce
            "jitterentropy-${name}-${config.boot.kernelPackages.kernel.version}";
          boot.supportedFilesystems.zfs = lib.mkForce false;
          system.stateVersion = lib.trivial.release;
        })
      ];
    }).config.system.build.sdImage;

  sdImagesFor = system: pkgs:
    (lib.mapAttrs'
      (name: ps:
        lib.nameValuePair "sd-image-${name}" (mkSdImage system name ps))
      (kernelSetsFor pkgs)) // {
        sd-image = mkSdImage system "default" pkgs.linuxPackages;
        sd-image-latest = mkSdImage system "latest" pkgs.linuxPackages_latest;
      };
}
