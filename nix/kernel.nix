# The Linux kernel module: out of tree per kernel, the FIPS kernel with signed
# modules, and in-tree builds.
ctx:
let
  inherit (ctx) inTreeFor kernelSetsFor lib moduleFor moduleSigningKey self;
in
{
  # jitter_rng.ko against a given kernel. The with* arguments override the
  # CONFIG_EXTERNAL_JITTERENTROPY_* options of Kbuild.config and are settable
  # via .override. withTestInterface starves the RNG of entropy: never in
  # production. signingKey (PEM, key and certificate) signs the module,
  # compressZstd installs it as jitter_rng.ko.zst.
  moduleFor = pkgs: kernel:
    lib.makeOverridable ({ withChardev, withHwrng, withTestInterface,
                           signingKey, compressZstd }:
      let
        flag = enabled: if enabled then "y" else "n";
        build = "${kernel.dev}/lib/modules/${kernel.modDirVersion}/build";
      in pkgs.stdenv.mkDerivation {
        pname = "jitterentropy-kmod";
        version = kernel.version;
        src = self;

        # Kbuild sets the kernel's own flags; the wrapper's would add to the
        # entropy core what a distribution's module does not have.
        hardeningDisable = [ "all" ];
        nativeBuildInputs = kernel.moduleBuildDependencies
          ++ lib.optional compressZstd pkgs.zstd;

        # A strip would cut off the signature appended to the module.
        dontStrip = signingKey != null;

        buildPhase = ''
          runHook preBuild
          make -C ${kernel.dev}/lib/modules/${kernel.modDirVersion}/build \
            M=$(pwd)/linux_kernel \
            CONFIG_EXTERNAL_JITTERENTROPY_CHARDEV=${flag withChardev} \
            CONFIG_EXTERNAL_JITTERENTROPY_HWRNG=${flag withHwrng} \
            CONFIG_EXTERNAL_JITTERENTROPY_TESTINTERFACE=${
              flag withTestInterface
            } \
            modules
        '' + lib.optionalString (signingKey != null) ''
          ${build}/scripts/sign-file sha512 ${signingKey} ${signingKey} \
            linux_kernel/jitter_rng.ko
          tail -c 28 linux_kernel/jitter_rng.ko |
            grep -q '~Module signature appended~'
        '' + lib.optionalString compressZstd ''
          zstd -q --rm linux_kernel/jitter_rng.ko
        '' + ''
          runHook postBuild
        '';

        installPhase = let ko = "jitter_rng.ko"
          + lib.optionalString compressZstd ".zst";
        in ''
          runHook preInstall
          install -D linux_kernel/${ko} \
            "$out/lib/modules/${kernel.modDirVersion}/extra/${ko}"
          runHook postInstall
        '';

        meta = {
          description = "Jitter RNG out-of-tree Linux kernel module";
          license = lib.licenses.gpl2Plus;
        };
      }) {
        withChardev = true;
        withHwrng = true;
        withTestInterface = false;
        signingKey = null;
        compressZstd = false;
      };

  # One per nixpkgs kernel, plus default and latest.
  modulesFor = pkgs:
    (lib.mapAttrs'
      (name: ps:
        lib.nameValuePair "jitterentropy-module-${name}"
          (moduleFor pkgs ps.kernel))
      (kernelSetsFor pkgs)) // {
        jitterentropy-module = moduleFor pkgs pkgs.linuxPackages.kernel;
        jitterentropy-module-latest =
          moduleFor pkgs pkgs.linuxPackages_latest.kernel;
      };

  # The numbered kernel package sets plus linux_testing. tryEval guards the
  # sets that do not evaluate on the current system.
  kernelSetsFor = pkgs:
    lib.filterAttrs (name: ps:
      let
        r = builtins.tryEval
          (lib.isAttrs ps && ps ? kernel && lib.isDerivation ps.kernel);
      in (builtins.match "linux_[0-9]+_[0-9]+" name != null
        || name == "linux_testing") && r.success && r.value)
      pkgs.linuxKernel.packages;

  # The test key the FIPS kernel signs its modules with and trusts:
  # tests/kernel-signing/README.md.
  moduleSigningKey = ../tests/kernel-signing/signing_key.pem;

  # A small kernel in FIPS mode for the raw entropy recordings: the default
  # kernel's source from defconfig and kvm_guest.config, with what NixOS
  # needs, the vanilla Jitter RNG and its test interface. FIPS mode requires
  # signed modules: it signs with moduleSigningKey and trusts only that key.
  fipsKernelFor = pkgs:
    let
      base = pkgs.linuxPackages.kernel;
      arch = pkgs.stdenv.hostPlatform.linuxArch;

      enable = [
        # FIPS mode, the vanilla Jitter RNG with its test interface, and the
        # AF_ALG RNG interface kcapi-rng drives it through.
        "EXPERT" "CRYPTO_SELFTESTS" "CRYPTO_DRBG_MENU" "CRYPTO_DRBG_HMAC"
        "CRYPTO_DRBG_HASH" "CRYPTO_DRBG_CTR" "CRYPTO_JITTERENTROPY"
        "CRYPTO_JITTERENTROPY_TESTINTERFACE" "CRYPTO_FIPS"
        "CRYPTO_USER_API_RNG" "CRYPTO_USER_API_HASH"
        # The netlink interface kcapi-rng looks the RNG up through.
        "CRYPTO_USER"
        # Signed modules, which CRYPTO_FIPS requires, and only those.
        "MODULES" "MODULE_SIG" "MODULE_SIG_FORCE" "MODULE_SIG_ALL"
        "MODULE_SIG_SHA512" "MODULE_COMPRESS" "MODULE_COMPRESS_ZSTD"
        "MODULE_COMPRESS_ALL" "MODULE_DECOMPRESS"
        # What jitter_rng's interfaces need.
        "HW_RANDOM" "HW_RANDOM_VIRTIO" "PROC_FS" "DEBUG_FS"
        # The NixOS test VM.
        "VIRTIO_PCI" "VIRTIO_BLK" "VIRTIO_NET" "VIRTIO_CONSOLE"
        "VIRTIO_BALLOON" "VIRTIO_MMIO" "SCSI_VIRTIO" "NET_9P"
        "NET_9P_VIRTIO" "9P_FS" "9P_FS_POSIX_ACL" "FUSE_FS" "VIRTIO_FS"
        "DRM" "DRM_VIRTIO_GPU" "OVERLAY_FS" "EXT4_FS" "EXT4_FS_POSIX_ACL"
        "BLK_DEV_LOOP"
        # systemd and NixOS.
        "DEVTMPFS" "CGROUPS" "CGROUP_BPF" "BPF_SYSCALL" "INOTIFY_USER"
        "SIGNALFD" "TIMERFD" "EPOLL" "FHANDLE" "AUTOFS_FS" "TMPFS"
        "TMPFS_POSIX_ACL" "TMPFS_XATTR" "SECCOMP" "DMIID" "BLK_DEV_INITRD"
        "RD_ZSTD" "BINFMT_ELF" "UNIX" "NET" "INET"
      ];

      configfile = pkgs.stdenv.mkDerivation {
        pname = "jitterentropy-fips-kernel-config";
        inherit (base) version src;

        nativeBuildInputs = with pkgs; [ bc bison flex perl ];

        postPatch = "patchShebangs scripts";

        buildPhase = ''
          runHook preBuild

          make ARCH=${arch} defconfig kvm_guest.config
          ./scripts/config ${lib.concatMapStringsSep " "
            (o: "--enable ${o}") enable} \
            --set-str MODULE_SIG_KEY ${moduleSigningKey} \
            --set-str SYSTEM_TRUSTED_KEYS "" \
            --set-val CRYPTO_JITTERENTROPY_OSR 3 \
            --set-str LOCALVERSION "" \
            --disable LOCALVERSION_AUTO \
            --disable MODULE_COMPRESS_GZIP \
            --disable MODULE_COMPRESS_XZ \
            --disable MODULE_SIG_SHA1 \
            --disable MODULE_SIG_SHA256 \
            --disable MODULE_SIG_SHA384 \
            --disable MODULE_SIG_SHA3_256 \
            --disable MODULE_SIG_SHA3_384 \
            --disable MODULE_SIG_SHA3_512
          make ARCH=${arch} olddefconfig

          # olddefconfig drops whatever has unmet dependencies, so check the
          # outcome rather than the request.
          for opt in ${lib.concatStringsSep " " enable}; do
            grep -qx "CONFIG_$opt=y" .config || {
              echo "CONFIG_$opt=y is missing from the generated .config"
              exit 1
            }
          done
          grep -qx 'CONFIG_MODULE_SIG_KEY="${moduleSigningKey}"' .config
          # defconfig left this at 1 while FIPS mode was still off.
          grep -qx 'CONFIG_CRYPTO_JITTERENTROPY_OSR=3' .config

          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          cp .config $out
          runHook postInstall
        '';
      };

      kernel = pkgs.linuxManualConfig {
        inherit (base) src version modDirVersion;
        inherit configfile;
        # Stated rather than read back from the generated .config, which
        # would be import from derivation.
        config = lib.listToAttrs
          (map (o: lib.nameValuePair "CONFIG_${o}" "y") enable);
      };
    in pkgs.linuxPackagesFor kernel;

  # linux_kernel/README.md "Build in Tree": the library copied into the
  # kernel source and linked into vmlinux, with CONFIG_MODULES unset - nothing
  # else here compiles that. tinyconfig keeps a kernel per kernel affordable;
  # HW_RANDOM and PROC_FS are on because Kbuild.config enables those
  # interfaces.
  inTreeFor = pkgs: kernel:
    # Stated: under plain "x86" allnoconfig switches CONFIG_64BIT off.
    let arch = pkgs.stdenv.hostPlatform.linuxArch;
    in pkgs.stdenv.mkDerivation {
      pname = "jitterentropy-in-tree";
      version = kernel.version;
      src = kernel.src;

      nativeBuildInputs = with pkgs; [ bc bison flex perl elfutils openssl ];

      # The kernel sets its own flags.
      hardeningDisable = [ "all" ];
      enableParallelBuilding = true;

      # --replace-fail: a reworded upstream line must be an error, not leave
      # the kernel's own copy building.
      postPatch = ''
        # Older kernels name interpreters the sandbox lacks.
        patchShebangs scripts

        cp -a ${self} crypto/jitterentropy-library
        chmod -R u+w crypto/jitterentropy-library

        substituteInPlace crypto/Makefile --replace-fail \
          'obj-$(CONFIG_CRYPTO_JITTERENTROPY) += jitterentropy_rng.o' \
          'obj-$(CONFIG_CRYPTO_JITTERENTROPY) += jitterentropy-library/linux_kernel/'

        substituteInPlace \
          crypto/jitterentropy-library/linux_kernel/Kbuild.config \
          --replace-fail 'CONFIG_EXTERNAL_JITTERENTROPY=m' \
                         'CONFIG_EXTERNAL_JITTERENTROPY=y' \
          --replace-fail '# CONFIG_BUILTIN_JITTERENTROPY=y' \
                         'CONFIG_BUILTIN_JITTERENTROPY=y'
      '';

      configurePhase = ''
        runHook preConfigure

        make ARCH=${arch} tinyconfig
        ./scripts/config --enable CRYPTO \
                         --enable CRYPTO_JITTERENTROPY \
                         --enable HW_RANDOM \
                         --enable PROC_FS
        make ARCH=${arch} olddefconfig

        # olddefconfig drops whatever has unmet dependencies, so check the
        # outcome rather than the request.
        for opt in CONFIG_CRYPTO_JITTERENTROPY CONFIG_HW_RANDOM \
                   CONFIG_PROC_FS; do
          grep -qx "$opt=y" .config || {
            echo "$opt=y is missing from the generated .config"
            exit 1
          }
        done
        if grep -qx 'CONFIG_MODULES=y' .config; then
          echo "CONFIG_MODULES is set, this is not a builtin build"
          exit 1
        fi

        runHook postConfigure
      '';

      buildPhase = ''
        runHook preBuild
        make ARCH=${arch} vmlinux -j$NIX_BUILD_CORES
        runHook postBuild
      '';

      installPhase = ''
        runHook preInstall

        nm vmlinux | grep ' [TtRrDdBb] jent_' | sort > jent-symbols

        grep -q ' jent_entropy_collector_alloc$' jent-symbols || {
          echo "no jent_entropy_collector_alloc in vmlinux"
          exit 1
        }

        # This tree's copy: the kernel's crypto/jitterentropy.c has no
        # status API.
        grep -q ' jent_status$' jent-symbols || {
          echo "no jent_status in vmlinux - the kernel's own Jitter RNG" \
               "copy was built instead of this tree"
          exit 1
        }

        # A swap, not an addition: the two define the same jent_* names.
        if [ -e crypto/jitterentropy.o ]; then
          echo "crypto/jitterentropy.o was built as well"
          exit 1
        fi

        install -D vmlinux $out/vmlinux
        install -D System.map $out/System.map
        install -D .config $out/config
        install -D jent-symbols $out/jent-symbols

        runHook postInstall
      '';

      # The symbol table is the artifact.
      dontStrip = true;

      meta = {
        description =
          "Linux kernel with the Jitter RNG built in from this tree";
        license = lib.licenses.gpl2Plus;
      };
    };

  # One per nixpkgs kernel, plus default and latest.
  inTreeBuildsFor = pkgs:
    (lib.mapAttrs'
      (name: ps:
        lib.nameValuePair "jitterentropy-in-tree-${name}"
          (inTreeFor pkgs ps.kernel))
      (kernelSetsFor pkgs)) // {
        jitterentropy-in-tree = inTreeFor pkgs pkgs.linuxPackages.kernel;
        jitterentropy-in-tree-latest =
          inTreeFor pkgs pkgs.linuxPackages_latest.kernel;
      };
}
