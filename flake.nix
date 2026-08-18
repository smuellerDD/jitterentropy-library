{
  description =
    "Jitter RNG: userspace library/tools (CMake) and out-of-tree kernel module, plus NixOS VMs runnable via nix run and nix flake check.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # QEMU on the host architecture.
      systems = [ "x86_64-linux" "aarch64-linux" "i686-linux" ];
      forAllSystems = f: lib.genAttrs systems (system: f system);

      # The CMake build: the library plus its tools in bin.
      toolsFor = pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-tools";
          version = "3.7.1";
          src = self;
          nativeBuildInputs = [ pkgs.cmake ];
          enableParallelBuilding = true;
          # The entropy core must build at -O0, which _FORTIFY_SOURCE cannot
          # combine with: 17 "requires compiling with optimization" warnings,
          # one per translation unit. No hardening is given up - glibc defines
          # the _chk variants only under optimization, so the flag was already
          # inert, and the object file is byte-identical without it.
          hardeningDisable = [ "fortify" "fortify3" ];
          meta = {
            description = "Jitter RNG userspace library and validation tools";
            license = lib.licenses.bsd3;
          };
        };

      # The same tools against musl, which disagrees with glibc about much the
      # library uses: the CPU_SET pinning, the mlock/getrlimit handling, and
      # clock_gettime(), which it keeps in libc. pkgsMusl, not pkgsCross.musl64,
      # so the tools run on the machine that built them.
      #
      # CI covers dynamic musl through Debian's musl-gcc and builds only
      # muslStaticFor from here; this stays for use out of the flake directly.
      muslFor = pkgs:
        (toolsFor pkgs.pkgsMusl).overrideAttrs
        (_: { pname = "jitterentropy-tools-musl"; });

      # The same libc with the linkage changed: static is where the use of
      # pthreads and clock_gettime() has to hold up with no dynamic loader. The
      # static stdenv already names itself in the store path, hence no pname.
      muslStaticFor = pkgs: toolsFor pkgs.pkgsStatic;

      # Compile-and-link smoke builds for the targets nothing else here
      # evaluates - the ones whose arch/ backends are selected by preprocessor
      # conditions. Not run, which still catches the breakage that occurs: a
      # missing declaration, an absent header, inline asm that does not
      # assemble.
      crossFor = { cross, timer ? true, shared ? false }:
        cross.stdenv.mkDerivation {
          pname = "jitterentropy-cross";
          version = "3.7.1";
          src = self;
          nativeBuildInputs = [ nixpkgs.legacyPackages.x86_64-linux.cmake ];
          # BUILD_TESTING explicitly: the nixpkgs cmake hook passes it as OFF,
          # and here the test programs are a good part of what is worth
          # compiling - they are the code that reaches these targets' arch
          # backends directly.
          cmakeFlags = [ "-DINTERNAL_TIMER=${if timer then "on" else "off"}"
                         "-DBUILD_TESTING=ON" ]
            ++ lib.optional shared "-DBUILD_SHARED_LIBS=ON";
          enableParallelBuilding = true;
          # As in toolsFor, and it matters more here: these exist to surface
          # diagnostics, which 17 lines per target would bury.
          hardeningDisable = [ "fortify" "fortify3" ];
          meta = {
            description = "Jitter RNG cross-compilation smoke build";
            license = lib.licenses.bsd3;
          };
        };

      crossTargets = pkgs:
        let p = pkgs.pkgsCross;
        in {
          # The dedicated jent_get_nstime() backends: stcke, the PowerPC
          # timebase, rdtime, rdtime.d. armv7 covers the clock_gettime()
          # fallback and, with i686, the 32-bit paths.
          cross-s390x = crossFor { cross = p.s390x; };
          cross-ppc64 = crossFor { cross = p.powernv; };
          cross-riscv64 = crossFor { cross = p.riscv64; };
          cross-loongarch64 = crossFor { cross = p.loongarch64-linux; };
          cross-armv7 = crossFor { cross = p."armv7l-hf-multiplatform"; };
          cross-i686 = crossFor { cross = p.gnu32; };

          # Both link widths, plus shared - the only configuration exercising
          # the dllexport/dllimport split and the bcrypt import library. The
          # internal timer builds despite nixpkgs' mingw-w64 shipping no
          # <pthread.h>: the Win32 back-end needs only the CRT and kernel32,
          # which is what this target proves.
          cross-mingwW64 = crossFor { cross = p.mingwW64; };
          cross-mingwW64-shared = crossFor { cross = p.mingwW64; shared = true; };
          cross-mingw32 = crossFor { cross = p.mingw32; };
        };

      # ndk-build over arch/android/Android.mk against the real NDK toolchain,
      # which is unfree - hence a dedicated nixpkgs instance for this output.
      #
      # APP_PLATFORM is the NDK's own floor, not this library's: r29 takes API
      # 21 upwards, and nothing the library calls is guarded above that except
      # getrandom() at __INTRODUCED_IN(28), which the UUID backend answers with
      # its /dev/urandom fallback. Building at the floor is what keeps that
      # branch compiled at all.
      androidFor = system:
        let
          pkgsAndroid = import nixpkgs {
            inherit system;
            config = {
              allowUnfree = true;
              android_sdk.accept_license = true;
            };
          };
          ndk = (pkgsAndroid.androidenv.composeAndroidPackages {
            includeNDK = true;
          }).ndk-bundle;
        in pkgsAndroid.stdenv.mkDerivation {
          pname = "jitterentropy-android";
          version = "3.7.1";
          src = self;
          nativeBuildInputs = [ ndk ];
          dontConfigure = true;

          buildPhase = ''
            runHook preBuild
            ndk-build \
              NDK_PROJECT_PATH=null \
              APP_BUILD_SCRIPT=$(pwd)/arch/android/Android.mk \
              APP_PLATFORM=android-21 \
              APP_ABI="arm64-v8a x86_64" \
              APP_OPTIM=release \
              NDK_OUT=$TMPDIR/obj \
              NDK_LIBS_OUT=$TMPDIR/libs \
              -j"$NIX_BUILD_CORES" V=1
            runHook postBuild
          '';

          installPhase = ''
            runHook preInstall
            mkdir -p $out/lib
            cp -r $TMPDIR/libs/* $out/lib/
            runHook postInstall
          '';

          meta = {
            description =
              "Jitter RNG userspace library built with the Android NDK";
            license = lib.licenses.bsd3;
          };
        };

      # rng-tools and ESDM built against this tree. A build of this repository
      # alone cannot see what breaks a consumer: a header that stops declaring
      # something, a symbol that stops being exported, a link dependency missing
      # from the .pc file.
      #
      # nixpkgs' own derivation with src and cmakeFlags replaced, not toolsFor,
      # so what is tested is the library as a distribution installs it - the
      # dev/out split included - and the swap stays source-only. Compile and
      # link only; behaviour is what the tool runs and the VM tests cover.
      consumersFor = pkgs:
        let
          # INTERNAL_TIMER decides which symbols are exported, and rng-tools'
          # configure probes for jent_notime_settick(): off, rngd_jitter.c
          # compiles without the tick handling; on, it drives the timer thread.
          # Two different compilations, hence both - off is what nixpkgs and so
          # the distributions build, on is this repository's default.
          #
          # ESDM compiles the same either way but gates much of esdm_es_jent.c
          # on JENT_VERSION, which makes it the consumer pinning the widest
          # part of the API.
          libFor = timer:
            pkgs.jitterentropy.overrideAttrs (_: {
              version = "3.7.1";
              src = self;
              cmakeFlags =
                [ "-DINTERNAL_TIMER=${if timer then "on" else "off"}" ];
            });
          notimer = libFor false;
          timer = libFor true;
          buildOnly = drv:
            drv.overrideAttrs (_: {
              doCheck = false;
              doInstallCheck = false;
            });
        in {
          consumer-rng-tools =
            buildOnly (pkgs.rng-tools.override { jitterentropy = notimer; });
          consumer-rng-tools-timer =
            buildOnly (pkgs.rng-tools.override { jitterentropy = timer; });
          consumer-esdm =
            buildOnly (pkgs.esdm.override { jitterentropy = notimer; });
          consumer-esdm-timer =
            buildOnly (pkgs.esdm.override { jitterentropy = timer; });
        };

      # The EXTERNAL_CRYPTO backends. Set, the library calls the named library
      # instead of its own SHA-3 and secure memory, reaching the halves of the
      # memory and FIPS backends the default build never compiles: libgcrypt's
      # secmem pool, OpenSSL's secure heap, and AWS-LC's OPENSSL_malloc(), which
      # is wiped but not locked and so reports itself as not secure.
      #
      # BUILD_SHARED_LIBS because CMakeLists.txt ties the two searches together:
      # a static jitterentropy makes it look for a static libcrypto.a, which is
      # not what these packages install.
      cryptoFor = pkgs:
        let
          backendFor = { name, external, dep }:
            (toolsFor pkgs).overrideAttrs (old: {
              pname = "jitterentropy-tools-${name}";
              buildInputs = (old.buildInputs or [ ]) ++ [ dep ];
              cmakeFlags = (old.cmakeFlags or [ ]) ++ [
                "-DEXTERNAL_CRYPTO=${external}"
                "-DBUILD_SHARED_LIBS=ON"
              ];
            });
        in {
          crypto-openssl = backendFor {
            name = "openssl";
            external = "OPENSSL";
            dep = pkgs.openssl;
          };
          crypto-awslc = backendFor {
            name = "awslc";
            external = "AWSLC";
            dep = pkgs.aws-lc;
          };
          # <gpg-error.h> arrives through libgcrypt's propagated input.
          crypto-libgcrypt = backendFor {
            name = "libgcrypt";
            external = "LIBGCRYPT";
            dep = pkgs.libgcrypt;
          };
        };

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

      # Shared by the VM tests and the live images: the chosen kernel with
      # jitter_rng loaded, the tools, and the testing conveniences. Both are
      # test environments, hence the debugfs test interface.
      machineFor = kernelPackages:
        { config, lib, pkgs, ... }: {
          boot.kernelPackages = kernelPackages;
          boot.extraModulePackages = [
            ((moduleFor pkgs config.boot.kernelPackages.kernel).override {
              withTestInterface = true;
            })
          ];
          boot.kernelModules = [ "jitter_rng" ];
          # Per-instance JSON status to the kernel log; test systems only.
          boot.extraModprobeConfig = ''
            options jitter_rng verbose=1 ntg1=1 cache_all=1 selftest_interval=15
          '';
          environment.systemPackages = [
            (toolsFor pkgs)
          ] ++ (with pkgs; [
            fx
            htop
            jq
            libkcapi
            python3
            sp800-90b-entropyassessment
            tmux
            vim
            xxd
          ]);
          # The chardev O_NONBLOCK semantics: reads capped at one 32-byte
          # buffer, and EAGAIN rather than waiting on a concurrent reader.
          environment.etc."jitterentropy-nonblock-test.py".text = ''
              import os
              import threading
              import time

              fd = os.open("/dev/jitterentropy", os.O_RDONLY | os.O_NONBLOCK)

              data = os.read(fd, 4096)
              assert len(data) == 32, f"nonblocking read returned {len(data)} bytes"

              # A large blocking read holds the instance lock per 32-byte
              # chunk, so a nonblocking read usually sees EAGAIN; the loop
              # below tolerates winning the gap between chunks. O_NONBLOCK is
              # checked on entry, hence the sleep before flipping it back.
              os.set_blocking(fd, True)
              t = threading.Thread(target=os.read, args=(fd, 4 * 1024 * 1024),
                                   daemon=True)
              t.start()
              time.sleep(0.5)
              os.set_blocking(fd, False)

              deadline = time.monotonic() + 10
              while True:
                  try:
                      os.read(fd, 16)
                  except BlockingIOError:
                      break
                  assert time.monotonic() < deadline, "no EAGAIN observed"
                  time.sleep(0.01)
              print("OK")
          '';
          # The ISO profile autologs in "nixos"; these are test images.
          services.getty.autologinUser = lib.mkForce "root";
          console.keyMap = "de";
          environment.shellAliases = {
            "sample_kernel" = "getrawentropy --ntg1 --samples 1000000 --debugfs-file /sys/kernel/debug/jitter_rng/jent_raw_hires";
            "clock_rdtsc" = "echo tsc > /sys/devices/system/clocksource/clocksource0/current_clocksource";
            "jitter_hwrng" = "echo jitterentropy > /sys/class/misc/hw_random/rng_current";
            "show_hwrng" = "cat /sys/class/misc/hw_random/rng_current";
            "kcapi_read" = "kcapi-rng -n jitter_rng -b 32 --hex";
          };
        };

      # Boots the shared machine configuration on the chosen kernel: a flake
      # check, and a `nix run` target through its interactive driver.
      mkVmTest = pkgs: name: kernelPackages:
        pkgs.testers.runNixOSTest {
          name = "jitterentropy-${name}";

          nodes.machine = {
            imports = [ (machineFor kernelPackages) ];
            boot.kernelParams = [ "clocksource=tsc" "tsc=reliable" ];
            virtualisation.qemu.options = [ "-cpu" "host" ];
            # The O_NONBLOCK test needs a poller running while a reader holds
            # the instance lock. On one vCPU a non-preemptible build (5.10,
            # PREEMPT_VOLUNTARY) only reschedules after unlock, so the poller
            # would never see EAGAIN. A second vCPU makes it real
            # concurrency.
            virtualisation.cores = 2;
          };

          testScript = ''
            machine.wait_for_unit("multi-user.target")

            # The module is loaded and its interfaces are present.
            machine.succeed("lsmod | grep -q '^jitter_rng'")
            machine.succeed("test -c /dev/jitterentropy")

            # procfs exports, including the per-instance status directory.
            print(machine.succeed("cat /proc/jitterentropy/statistics"))
            print(machine.succeed("cat /proc/jitterentropy/hwrng_status"))

            # Reading opens an instance; its UUID-named status file appears.
            machine.succeed(
                "exec 3</dev/jitterentropy; "
                "test \"$(ls /proc/jitterentropy/instances | wc -l)\" -ge 1; "
                "head -c 32 /proc/jitterentropy/instances/* >/dev/null; "
                "exec 3<&-"
            )
            machine.succeed("test \"$(head -c 32 /dev/jitterentropy | wc -c)\" = 32")

            # The chardev JENT_IOCSTATUS ioctl delivers the instance's JSON
            # status (the tool also probes the EOVERFLOW length-report path).
            print(machine.succeed(
                "jitterentropy-chardev-status | jq -e .uuid"
            ))

            # And the single-field ioctls agree with that document, field by
            # field: UUID, version, osr, flags, health failure state, output
            # counters and reinitialization count.
            print(machine.succeed("jitterentropy-chardev-fields"))

            # O_NONBLOCK reads: short-read cap and EAGAIN on contention.
            print(machine.succeed("python3 /etc/jitterentropy-nonblock-test.py"))

            # The debugfs raw entropy test interface delivers the raw noise
            # time deltas of the measure_jitter operation.
            machine.succeed("dmesg --clear")
            machine.succeed(
                "test \"$(head -c 64 /sys/kernel/debug/jitter_rng/jent_raw_hires"
                " | wc -c)\" = 64"
            )

            # With verbose=1 (set via modprobe.d in the machine
            # configuration), the open of the test interface logged the
            # recording instance's JSON status to the kernel log. printk
            # truncates records at about 1 kB, so the status is emitted line
            # by line; verify that the complete document landed in the log
            # buffer.
            import json
            import re

            kernel_log = machine.succeed("dmesg")
            # Strip the timestamp prefix and undo dmesg's escaping of the
            # tab indentation.
            msgs = [
                re.sub(r"^\[[^\]]*\] ?", "", line).replace("\\x09", "\t")
                for line in kernel_log.splitlines()
            ]
            start = msgs.index("{")
            end = msgs.index("}", start)
            doc = "\n".join(msgs[start:end + 1])
            print(doc)
            json.loads(doc)

            # The JENT_IOCSTATUS ioctl is also exposed on the debugfs test
            # interface, reporting the status of the per-open raw-noise
            # recording instance (the tool takes the file to query as
            # argument). Raw instances skip the startup sequence and thus
            # carry no UUID, so assert on the version field instead.
            print(machine.succeed(
                "jitterentropy-chardev-status"
                " /sys/kernel/debug/jitter_rng/jent_raw_hires"
                " | jq -e .version"
            ))

            # The same single-field ioctls, on the same interface. A raw
            # instance carries no UUID, so JENT_IOCUUID must report ENODATA
            # there rather than an empty string; the tool asserts that.
            print(machine.succeed(
                "jitterentropy-chardev-fields"
                " /sys/kernel/debug/jitter_rng/jent_raw_hires"
            ))

            # getrawentropy drives the same interface end-to-end: it sets the
            # testing_osr module parameter and prints the raw time delta
            # samples unmodified. --samples N yields exactly N values.
            machine.succeed(
                "test \"$(getrawentropy --samples 100 --osr 3 | wc -l)\" = 100"
            )

            # --loopcnt drives the JENT_IOCLOOPCNT ioctl: a fixed loop count
            # overrides the instance's configured hash and memory access loop
            # counts for the recorded measurements.
            machine.succeed(
                "test \"$(getrawentropy --samples 100 --loopcnt 4 | wc -l)\""
                " = 100"
            )

            # --status fetches the recording instance's JSON status via
            # JENT_IOCSTATUS and records nothing.
            print(machine.succeed(
                "getrawentropy --samples 1 --status | jq -e .version"
            ))

            # The CMake-built userspace tools are on PATH.
            for tool in ("jitterentropy-rng", "jitterentropy-osr",
                         "jitterentropy-hashtime", "jitterentropy-cpuinfo",
                         "gcd", "extractlsb", "getrawentropy",
                         "jitterentropy-chardev-status",
                         "jitterentropy-chardev-fields"):
                machine.succeed(f"command -v {tool}")
          '';
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
          version = "3.7.1";
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
          # library says so rather than inventing one. This is the documented
          # baremetal shortfall and it is asserted so that it stays documented.
          grep -q '"uuid": "00000000-0000-0000-0000-000000000000"' console.txt ||
            fail "expected the nil UUID where no CSPRNG exists"
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

      # One VM test per nixpkgs kernel, plus the default and latest kernels.
      vmTestsFor = pkgs:
        (lib.mapAttrs'
          (name: ps: lib.nameValuePair "vm-${name}" (mkVmTest pkgs "vm-${name}" ps))
          (kernelSetsFor pkgs)) // {
            vm = mkVmTest pkgs "vm" pkgs.linuxPackages;
            vm-latest = mkVmTest pkgs "vm-latest" pkgs.linuxPackages_latest;
          };

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
          # The NDK and the cross toolchains are x86_64-linux only, and so is
          # the EFI application - see efiFor above.
          // lib.optionalAttrs (system == "x86_64-linux") (crossTargets pkgs // {
            efi = efiFor pkgs "x86_64";
            efi-aarch64 = efiFor pkgs "aarch64";
            android = androidFor system;
            # The module for 32-bit x86, built natively through pkgsi686Linux.
            # Reaches the div64 helpers that stand in for the libgcc division
            # routines the kernel does not provide.
            jitterentropy-module-i686 =
              moduleFor pkgs.pkgsi686Linux pkgs.pkgsi686Linux.linuxPackages_latest.kernel;
          }));

      # `nix flake check` boots every VM and runs its assertions.
      checks =
        forAllSystems (system:
          vmTestsFor nixpkgs.legacyPackages.${system}
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
          runners = lib.mapAttrs (_name: test: {
            type = "app";
            program = "${test.driverInteractive}/bin/nixos-test-driver";
          }) self.checks.${system};
        in runners // { default = runners.vm; });
    };
}
