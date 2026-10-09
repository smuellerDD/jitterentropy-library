# The kernel module, the kernel sets and the in-tree kernel builds.
ctx:
let
  inherit (ctx) inTreeBuildsFor inTreeFor kernelSetsFor lib moduleFor
    modulesFor self;
in
{
  # jitter_rng.ko against a given kernel. The with* arguments mirror the
  # CONFIG_EXTERNAL_JITTERENTROPY_* options of Kbuild.config, overriding it
  # on the make command line, and are settable via .override.
  # withTestInterface starves the RNG of entropy: never in production.
  moduleFor = pkgs: kernel:
    lib.makeOverridable ({ withChardev, withHwrng, withTestInterface }:
      let
        flag = enabled: if enabled then "y" else "n";
      in pkgs.stdenv.mkDerivation {
        pname = "jitterentropy-kmod";
        version = kernel.version;
        src = self;

        hardeningDisable = [ "pic" "format" ];
        nativeBuildInputs = kernel.moduleBuildDependencies;

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
      }) {
        withChardev = true;
        withHwrng = true;
        withTestInterface = false;
      };

  # One per nixpkgs kernel, plus default and latest, mirroring the VM test
  # and image sets. Interfaces are tunable per attribute via .override.
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

  # The numbered kernel package sets plus linux_testing; the flavored
  # variants are of no interest. tryEval guards the sets that do not
  # evaluate on the current system.
  kernelSetsFor = pkgs:
    lib.filterAttrs (name: ps:
      let
        r = builtins.tryEval
          (lib.isAttrs ps && ps ? kernel && lib.isDerivation ps.kernel);
      in (builtins.match "linux_[0-9]+_[0-9]+" name != null
        || name == "linux_testing") && r.success && r.value)
      pkgs.linuxKernel.packages;

  # linux_kernel/README.md "Build in Tree": the library copied into the
  # kernel source, crypto/Makefile pointed at it, the result linked into
  # vmlinux. Unlike moduleFor, this compiles with CONFIG_MODULES unset and
  # MODULE undefined - nothing else here does. tinyconfig makes a whole
  # kernel per kernel affordable; HW_RANDOM and PROC_FS are on because
  # Kbuild.config enables those interfaces.
  inTreeFor = pkgs: kernel:
    # Stated, not left to the host: under plain "x86" CONFIG_64BIT is
    # user-selectable and allnoconfig switches it off.
    let arch = pkgs.stdenv.hostPlatform.linuxArch;
    in pkgs.stdenv.mkDerivation {
      pname = "jitterentropy-in-tree";
      version = kernel.version;
      src = kernel.src;

      nativeBuildInputs = with pkgs; [ bc bison flex perl elfutils openssl ];

      # As nixpkgs' own kernel derivations: it sets its own flags.
      hardeningDisable = [ "all" ];
      enableParallelBuilding = true;

      # --replace-fail throughout: a reworded upstream line must be an
      # error, not a silent no-op leaving the kernel's own copy building.
      postPatch = ''
        # The older kernels name interpreters the sandbox has not got
        # (5.10's ld-version.sh is /usr/bin/awk -f), which Kconfig then
        # reports as a syntax error in init/Kconfig.
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

        # olddefconfig drops whatever has unmet dependencies, so check
        # the outcome rather than the request.
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

        # In the kernel binary at all.
        grep -q ' jent_entropy_collector_alloc$' jent-symbols || {
          echo "no jent_entropy_collector_alloc in vmlinux"
          exit 1
        }

        # And this tree's copy: the status API has no counterpart in the
        # kernel's crypto/jitterentropy.c.
        grep -q ' jent_status$' jent-symbols || {
          echo "no jent_status in vmlinux - the kernel's own Jitter RNG" \
               "copy was built instead of this tree"
          exit 1
        }

        # A swap, not an addition - the two define the same jent_* names.
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

  # One in-tree kernel build per nixpkgs kernel, plus the default and
  # latest kernels, mirroring the module set.
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
