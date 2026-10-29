# jitterentropy-rngd in a VM, under its systemd unit and in the foreground, on
# the default kernel and on one with the LRNG of
# https://github.com/smuellerDD/lrng in place of drivers/char/random.c.
ctx:
let
  inherit (ctx) lib lrngKernelFor rngdFor rngdVmTestFor trimmedKernelFor
    vmKernelOptions;
in
{
  # linux_6_18 with the LRNG's v60 patches: RANDOM_DEFAULT_IMPL off selects the
  # LRNG with its random(4) interface and the sysctls the daemon opens.
  lrngKernelFor = pkgs:
    let
      lrng = pkgs.fetchFromGitHub {
        owner = "smuellerDD";
        repo = "lrng";
        rev = "v60";
        hash = "sha256-pM6pzzC9TRq6De1w2Yn4SedgUadHxWSe6WmGX4tIq/k=";
      };
      # One patch without the cover letter: reading the directory at
      # evaluation would be import from derivation.
      patch = pkgs.runCommand "lrng-v60-linux-6.18.patch" { } ''
        for p in ${lrng}/kernel_patches/v6.18/v60-*.patch; do
          case $p in *-0000-*) ;; *) cat "$p" ;; esac
        done > $out
      '';
    in trimmedKernelFor pkgs {
      name = "lrng";
      base = pkgs.linux_6_18;
      kernelPatches = [ { name = "lrng-v60"; inherit patch; } ];
      # MODULES, though defconfig has it: linuxManualConfig installs the
      # module tree only when told, and the VM's initrd loads virtio_rng.
      enable = [ "LRNG" "LRNG_RANDOM_IF" "LRNG_SYSCTL" "PROC_FS" "SYSCTL"
                 "MODULES" "HW_RANDOM" "HW_RANDOM_VIRTIO" ] ++ vmKernelOptions;
      disable = [ "RANDOM_DEFAULT_IMPL" ];
    };

  # The daemon of .#jitterentropy-rngd with its unit, on kernelPackages; lrng
  # says whether that kernel has the LRNG, and so what to expect.
  rngdVmTestFor = pkgs: name: kernelPackages: lrng:
    pkgs.testers.runNixOSTest {
      name = "jitterentropy-${name}";

      nodes.machine = { lib, ... }: {
        boot.kernelPackages = kernelPackages;
        environment.systemPackages = [ (rngdFor pkgs) ];
        systemd.packages = [ (rngdFor pkgs) ];
        # NixOS reads no [Install] section.
        systemd.services.jitterentropy.wantedBy = [ "basic.target" ];
        virtualisation.qemu.options = [ "-cpu" "host" ];
        virtualisation.cores = 2;
        # The LRNG kernel builds its drivers in and has no TPM driver.
        boot.initrd.includeDefaultModules = lib.mkIf lrng false;
        boot.initrd.systemd.tpm2.enable = lib.mkIf lrng false;
      };

      testScript = ''
        lrng = ${if lrng then "True" else "False"}
      '' + builtins.readFile ./rngd/rngd-test.py;
    };

  rngdVmTestsFor = pkgs: {
    rngd = rngdVmTestFor pkgs "rngd" pkgs.linuxPackages false;
    rngd-lrng = rngdVmTestFor pkgs "rngd-lrng" (lrngKernelFor pkgs) true;
  };
}
