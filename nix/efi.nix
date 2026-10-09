# The EFI application and the VM that boots it.
ctx:
let
  inherit (ctx) efiArchs efiFor efiVmFor lib self;
in
{
  # The library with no operating system under it at all: an EFI
  # application, built freestanding against gnu-efi. See tests/efi/README.md
  # for what it is for; in short, every other target here supplies an
  # allocator, a clock call, a CPU count and a random pool that the arch/
  # backends reach for, and this one supplies none of them.
  #
  # Both architectures firmware is found on. aarch64 is cross-built from
  # the x86_64 package set, which is also how it is booted below - QEMU
  # emulates the machine either way.
  efiArchs = {
    x86_64 = {
      pkgsFor = pkgs: pkgs;
      image = "BOOTX64.EFI";
      qemu = "qemu-system-x86_64";
      # -cpu max rather than the machine default: the default for this
      # board has no invariant TSC, and the counter is the whole point.
      machine = [ "-machine" "q35,accel=tcg" "-cpu" "max" ];
      fwCode = "OVMF_CODE.fd";
      fwVars = "OVMF_VARS.fd";
      # The cache geometry comes out of CPUID here, which needs no
      # operating system - so a zero would mean the one platform query
      # this build still makes has stopped working. It is also why the
      # machine above is -cpu max: the default model for this board
      # reports no cache descriptors at all, and the check would then be
      # asserting a property of QEMU rather than of the library.
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
      # No CPUID here, and the cache size registers are read through the
      # kernel's own accessors in the one backend that reads them - so this
      # build makes no cache query and takes the library's default memory
      # size. Asserted as such rather than left unsaid, so that a backend
      # added for it is noticed here.
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
      version = "3.8.0";
      src = self;
      buildInputs = [ target.gnu-efi ];
      enableParallelBuilding = true;
      # Every one of these would fight the build rather than harden it:
      # the image is freestanding, position independent by its own linker
      # script, and compiled at -O0 because the entropy core requires it.
      hardeningDisable = [ "all" ];
      buildPhase = ''
        runHook preBuild
        # CC, LD and OBJCOPY come from the (cross) stdenv rather than from
        # a prefix guessed in the Makefile, which is what makes the same
        # recipe work natively and cross.
        make -C tests/efi -j"$NIX_BUILD_CORES" esp \
          EFI_ARCH=${efiArch} \
          GNUEFI=${target.gnu-efi} \
          CC="$CC" LD="$LD" OBJCOPY="$OBJCOPY"
        runHook postBuild
      '';
      installPhase = ''
        runHook preInstall
        # Installed as the EFI system partition it is booted from, so that
        # the VM check below can point QEMU straight at $out. The two
        # architectures have different default image names, so one
        # partition could hold both.
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

  # Boot that application under OVMF and read what it says. This is the
  # part a compile test cannot do: whether the counter the freestanding
  # path reads actually moves, whether the startup health tests pass on
  # what it measures, and whether a collector can be built where the only
  # allocator is the firmware's.
  #
  # A plain derivation rather than a NixOS test: there is no NixOS here and
  # no Linux either - the firmware boots the application directly from the
  # removable-media path, and the application powers the machine off when
  # it is done, so the run ends on its own verdict.
  efiVmFor = pkgs: efiArch:
    let
      spec = efiArchs.${efiArch};
      fw = (spec.pkgsFor pkgs).OVMF.fd;
    in pkgs.runCommand "jitterentropy-efi-vm-${efiArch}" {
      nativeBuildInputs = [ pkgs.qemu ];
      esp = efiFor pkgs efiArch;
      # No KVM in the sandbox, and for aarch64 no possibility of it, so
      # this is TCG. The whole run is seconds either way: the application
      # boots, generates 32 bytes and stops.
      # Joined rather than shell-escaped: the expansion below is
      # unquoted so that it splits into arguments, and word splitting does
      # not remove quotes - escaping them would hand QEMU an option with
      # the quotes still on it. None of these contains a space.
      qemuArgs = lib.concatStringsSep " " spec.machine;
    } ''
      # The variable store has to be writable, and the one in the store is
      # not. Copied rather than shared: the application writes no variables,
      # but the firmware does.
      cp ${fw}/FV/${spec.fwVars} vars.fd
      chmod +w vars.fd

      echo "booting the ${efiArch} EFI application under EDK2"
      timeout 600 ${spec.qemu} $qemuArgs \
        -m 512 -nographic -no-reboot \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=${fw}/FV/${spec.fwCode} \
        -drive if=pflash,format=raw,unit=1,file=vars.fd \
        -drive format=raw,file=fat:rw:$esp \
        -serial mon:stdio > console.raw 2>&1 || {
          # QEMU's own diagnostics went into the transcript along with the
          # firmware's, and without this they would be swallowed by the
          # redirect and the build would fail with nothing to read.
          echo "QEMU exited non-zero:"
          cat console.raw
          exit 1
        }

      # The firmware draws on the same console, so the escape sequences go
      # and the carriage returns with them before anything is matched.
      sed -e 's/\x1b\[[0-9;?]*[a-zA-Z]//g' -e 's/\x1b[()][A-Z0-9]//g' \
          console.raw | tr -d '\r' > console.txt
      cat console.txt

      fail() { echo "jitterentropy-efi-vm: $1"; exit 1; }

      # It ran at all, and it ran to the end. "failed" is what the
      # application prints when any of its own steps did not work, and it
      # is checked first so that the reason above it is what a reader sees.
      grep -q 'jitterentropy-efi: failed' console.txt &&
        fail "the application reported a failure"
      grep -q 'jitterentropy-efi: start,' console.txt ||
        fail "the application did not start"
      grep -q 'jitterentropy-efi: startup passed' console.txt ||
        fail "the startup health tests did not pass"
      grep -q 'jitterentropy-efi: done' console.txt ||
        fail "the application did not run to the end"

      # The default configuration, where nothing is allowed to go wrong:
      # a collector, 32 bytes, and a status document.
      grep -q 'jitterentropy-efi: default collector allocated' console.txt ||
        fail "no default collector could be allocated"
      grep -q 'jitterentropy-efi: default status' console.txt ||
        fail "no status document for the default collector"

      hex=$(sed -n 's/^jitterentropy-efi: default entropy \([0-9a-f]*\)$/\1/p' \
            console.txt)
      [ "''${#hex}" = 64 ] || fail "expected 32 bytes, got ''${#hex} hex digits"
      # Counted rather than matched with a backreference, which POSIX
      # leaves undefined in an extended regular expression and some greps
      # refuse outright.
      [ "$(echo "$hex" | fold -w1 | sort -u | wc -l)" -gt 1 ] ||
        fail "the 32 bytes are all one value: nothing was measured"

      # The two compliance modes went the same way through the same
      # sequence, and each was answered. Which way is not asserted: both
      # carry tighter health test cutoffs than the common configuration,
      # and a startup firing on what this firmware and processor produce
      # is a legitimate outcome. What would not be is the attempt going
      # unreported - or, where it did come up, its 32 bytes not arriving.
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

      # And the internal timer is refused on both entry points that can
      # be asked for it. It is a counting thread and there is no thread
      # here to run it on, so an allocation that accepted the flag would
      # hand back a collector whose clock nothing increments - which spins
      # on the first measurement rather than returning an error.
      grep -q 'jitterentropy-efi: internal timer refused' console.txt ||
        fail "the internal timer was not refused in a build without one"

      # The status documents arrived whole, which also says the snprintf()
      # the application supplies works: it is the only caller of it in the
      # library. Three of them, one per configuration, and the two
      # compliance flags appear only in the ones that asked for them.
      [ "$(grep -c '"version": "' console.txt)" = 3 ] ||
        fail "expected three status documents, one per configuration"
      grep -q '"internalTimer": false' console.txt ||
        fail "the internal timer is reported present in a build without one"
      # There is no OS random pool to draw an identifier from, so the
      # library derives a version 8 UUID instead of a random version 4.
      grep -Eq '"uuid": "[0-9a-f]{8}-[0-9a-f]{4}-8[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}",' console.txt ||
        fail "expected a version 8 UUID where no CSPRNG exists"
      # Secure memory, and it is not a locked page: there is no swap
      # device here, no second process and no core dump, so the property
      # the flag is about holds by construction. A false would mean the
      # backend stopped saying so - and the compliance modes above, which
      # imply JENT_FORCE_SECURE_MEM, would be running on memory nothing
      # vouches for.
      grep -q '"secureMemory": false' console.txt &&
        fail "secure memory is not reported where nothing can page it out"

      ${spec.extraChecks}

      echo "jitterentropy-efi-vm: ok"
      mkdir -p $out
      cp console.txt $out/console.txt
    '';
}
