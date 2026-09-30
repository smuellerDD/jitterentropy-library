# The EFI application and the VM that boots it.
ctx:
let
  inherit (ctx) efiArchs efiFor jentVersion lib self;
in
{
  # The library with no operating system under it: an EFI application, built
  # freestanding against gnu-efi. See tests/efi/README.md. aarch64 is
  # cross-built from the x86_64 package set.
  efiArchs = {
    x86_64 = {
      pkgsFor = pkgs: pkgs;
      image = "BOOTX64.EFI";
      qemu = "qemu-system-x86_64";
      # -cpu max: the default model has no invariant TSC and reports no cache
      # descriptors.
      machine = [ "-machine" "q35,accel=tcg" "-cpu" "max" ];
      fwCode = "OVMF_CODE.fd";
      fwVars = "OVMF_VARS.fd";
      # CPUID needs no operating system, so a zero means the one platform
      # query this build makes has stopped working.
      extraChecks = ''
        grep -q '"l1Bytes": 0' console.txt &&
          fail "no L1 cache size: the CPUID query returned nothing"
        grep -q '"allBytes": 0' console.txt &&
          fail "no total cache size: the CPUID query returned nothing"
      '';
    };
    aarch64 = {
      pkgsFor = pkgs: pkgs.pkgsCross.aarch64-multiplatform;
      image = "BOOTAA64.EFI";
      qemu = "qemu-system-aarch64";
      machine = [ "-machine" "virt,accel=tcg" "-cpu" "max" ];
      fwCode = "AAVMF_CODE.fd";
      fwVars = "AAVMF_VARS.fd";
      # This build makes no cache query; asserted so that a backend added for
      # it is noticed here.
      extraChecks = ''
        grep -q '"l1Bytes": 0' console.txt ||
          fail "a cache size is reported where this build queries none"
      '';
    };
  };

  efiFor = pkgs: efiArch:
    let
      spec = efiArchs.${efiArch};
      target = spec.pkgsFor pkgs;
    in target.stdenv.mkDerivation {
      pname = "jitterentropy-efi-${efiArch}";
      version = jentVersion;
      src = self;
      buildInputs = [ target.gnu-efi ];
      enableParallelBuilding = true;
      # The image is freestanding, position independent by its own linker
      # script, and compiled at -O0.
      hardeningDisable = [ "all" ];
      buildPhase = ''
        runHook preBuild
        # CC, LD and OBJCOPY from the (cross) stdenv, so the same recipe works
        # natively and cross.
        make -C tests/efi -j"$NIX_BUILD_CORES" esp \
          EFI_ARCH=${efiArch} \
          GNUEFI=${target.gnu-efi} \
          CC="$CC" LD="$LD" OBJCOPY="$OBJCOPY"
        runHook postBuild
      '';
      installPhase = ''
        runHook preInstall
        # Installed as the EFI system partition, so QEMU can boot $out.
        mkdir -p $out
        cp -r tests/efi/esp/EFI $out/EFI
        runHook postInstall
      '';
      meta = {
        description =
          "Jitter RNG as an EFI application for ${efiArch} (baremetal build)";
        license = lib.licenses.bsd3;
      };
    };

  # Boot the application under OVMF and read what it says: whether the
  # counter moves, the startup health tests pass and a collector can be built
  # on the firmware's allocator.
  #
  # A plain derivation, not a NixOS test: the firmware boots the application
  # from the removable-media path, and the application powers the machine off.
  efiVmFor = pkgs: efiArch:
    let
      spec = efiArchs.${efiArch};
      fw = (spec.pkgsFor pkgs).OVMF.fd;
    in pkgs.runCommand "jitterentropy-efi-vm-${efiArch}" {
      nativeBuildInputs = [ pkgs.qemu ];
      esp = efiFor pkgs efiArch;
      # TCG: no KVM in the sandbox. Joined, not shell-escaped: the expansion
      # below is unquoted to split into arguments, and none contains a space.
      qemuArgs = lib.concatStringsSep " " spec.machine;
    } ''
      # The firmware writes the variable store, and the one in the store is
      # read-only.
      cp ${fw}/FV/${spec.fwVars} vars.fd
      chmod +w vars.fd

      echo "booting the ${efiArch} EFI application under EDK2"
      timeout 600 ${spec.qemu} $qemuArgs \
        -m 512 -nographic -no-reboot \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=${fw}/FV/${spec.fwCode} \
        -drive if=pflash,format=raw,unit=1,file=vars.fd \
        -drive format=raw,file=fat:rw:$esp \
        -serial mon:stdio > console.raw 2>&1 || {
          # QEMU's diagnostics went into the transcript.
          echo "QEMU exited non-zero:"
          cat console.raw
          exit 1
        }

      # Drop the firmware's escape sequences and carriage returns.
      sed -e 's/\x1b\[[0-9;?]*[a-zA-Z]//g' -e 's/\x1b[()][A-Z0-9]//g' \
          console.raw | tr -d '\r' > console.txt
      cat console.txt

      fail() { echo "jitterentropy-efi-vm: $1"; exit 1; }

      # "failed" first, so that the reason above it is what a reader sees.
      grep -q 'jitterentropy-efi: failed' console.txt &&
        fail "the application reported a failure"
      grep -q 'jitterentropy-efi: start,' console.txt ||
        fail "the application did not start"
      grep -q 'jitterentropy-efi: startup passed' console.txt ||
        fail "the startup health tests did not pass"
      grep -q 'jitterentropy-efi: done' console.txt ||
        fail "the application did not run to the end"

      # The default configuration: a collector, 32 bytes, a status document.
      grep -q 'jitterentropy-efi: default collector allocated' console.txt ||
        fail "no default collector could be allocated"
      grep -q 'jitterentropy-efi: default status' console.txt ||
        fail "no status document for the default collector"

      hex=$(sed -n 's/^jitterentropy-efi: default entropy \([0-9a-f]*\)$/\1/p' \
            console.txt)
      [ "''${#hex}" = 64 ] || fail "expected 32 bytes, got ''${#hex} hex digits"
      # Counted: a backreference in an ERE is undefined by POSIX.
      [ "$(echo "$hex" | fold -w1 | sort -u | wc -l)" -gt 1 ] ||
        fail "the 32 bytes are all one value: nothing was measured"

      # The compliance modes have tighter cutoffs, so a startup failing on
      # this firmware is legitimate. Each must be attempted, and one that
      # came up must deliver.
      for mode in FIPS NTG.1; do
        grep -q "jitterentropy-efi: $mode " console.txt ||
          fail "the $mode collector was not attempted"

        grep -q "jitterentropy-efi: $mode collector allocated" \
          console.txt || continue

        hex=$(sed -n "s/^jitterentropy-efi: $mode entropy \([0-9a-f]*\)$/\1/p" \
              console.txt)
        [ "''${#hex}" = 64 ] ||
          fail "$mode came up but produced ''${#hex} hex digits"
        [ "$(echo "$hex" | fold -w1 | sort -u | wc -l)" -gt 1 ] ||
          fail "$mode produced 32 bytes of one value"
        grep -q "jitterentropy-efi: $mode status" console.txt ||
          fail "no status document for the $mode collector"
      done

      # No thread to count on: a collector accepting the internal timer would
      # spin on its first measurement.
      grep -q 'jitterentropy-efi: internal timer refused' console.txt ||
        fail "the internal timer was not refused in a build without one"

      # One whole status document per collector that came up, which also
      # says the application's snprintf() works.
      docs=1
      for mode in FIPS NTG.1; do
        grep -q "jitterentropy-efi: $mode collector allocated" \
          console.txt && docs=$((docs + 1))
      done
      [ "$(grep -c '"version": "' console.txt)" = "$docs" ] ||
        fail "expected $docs status documents, one per collector that came up"
      grep -q '"internalTimer": false' console.txt ||
        fail "the internal timer is reported present in a build without one"
      # No OS random pool, so a version 8 UUID.
      grep -Eq '"uuid": "[0-9a-f]{8}-[0-9a-f]{4}-8[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",' console.txt ||
        fail "expected a version 8 UUID where no CSPRNG exists"
      # Every build wipes memory on free, so false means the report broke.
      grep -q '"secureMemory": false' console.txt &&
        fail "secure memory is not reported"

      ${spec.extraChecks}

      echo "jitterentropy-efi-vm: ok"
      mkdir -p $out
      cp console.txt $out/console.txt
    '';
}
