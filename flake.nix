{
  description =
    "Jitter RNG: userspace library/tools (CMake) and out-of-tree kernel module, plus NixOS VMs runnable via nix run and nix flake check.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # VMs run under QEMU on the host architecture.
      systems = [ "x86_64-linux" "aarch64-linux" "i686-linux" ];
      forAllSystems = f: lib.genAttrs systems (system: f system);

      # Userspace library and tools, built with the project's CMake build.
      # Installs jitterentropy-{hashtime,osr,rng}, gcd, extractlsb,
      # getrawentropy and jitterentropy-chardev-status into bin.
      toolsFor = pkgs:
        pkgs.stdenv.mkDerivation {
          pname = "jitterentropy-tools";
          version = "3.7.1";
          src = self;
          nativeBuildInputs = [ pkgs.cmake ];
          enableParallelBuilding = true;
          # The entropy-collection core must be built at -O0 (CMakeLists.txt
          # sets it explicitly, and src/jitterentropy-base.c has an __OPTIMIZE__
          # guard to enforce it), which _FORTIFY_SOURCE cannot be combined with.
          # Left enabled, the nixpkgs default hardening emits "_FORTIFY_SOURCE
          # requires compiling with optimization" once per translation unit -
          # 17 warnings that bury anything worth reading.
          #
          # This gives up no hardening. The flag appends -D_FORTIFY_SOURCE=2
          # after the project's flags, where -O0 has already made it inert -
          # glibc only defines the _chk variants under optimization, which is
          # exactly what it is warning about. It also prepends -O2, but before
          # the project's flags, so the -O0 that follows wins either way.
          # Compiling src/jitterentropy-base.c with and without this line
          # produces a byte-identical object file.
          hardeningDisable = [ "fortify" "fortify3" ];
          meta = {
            description = "Jitter RNG userspace library and validation tools";
            license = lib.licenses.bsd3;
          };
        };

      # The same tools against musl instead of glibc.
      #
      # Every Linux build elsewhere in this flake is a glibc one, yet the
      # library reaches for a good deal of what the two libcs disagree about:
      # the pthread_setaffinity_np()/CPU_SET pinning in
      # arch/jitterentropy-arch-sched.c, the mlock/getrlimit handling in
      # arch/jitterentropy-arch-memory.c, and clock_gettime(), which musl keeps
      # in libc while glibc split it out into librt. musl is also stricter about
      # which headers declare what, so a missing include that glibc supplies
      # transitively fails here and nowhere else.
      #
      # pkgsMusl rather than pkgsCross.musl64: it is the same architecture, so
      # the tools this produces run on the machine that built them rather than
      # only linking.
      #
      # CI does not build this one - the workflow covers dynamic musl through
      # Debian's musl-gcc and takes only muslStaticFor from here, so that the
      # Nix job is the configuration nothing else reaches. It stays for anyone
      # wanting dynamic musl out of the flake directly.
      #
      # Renamed from what toolsFor would otherwise call it, so that the store
      # path and the build log say which libc this is - the two derivations are
      # identical but for the package set behind them.
      muslFor = pkgs:
        (toolsFor pkgs.pkgsMusl).overrideAttrs
        (_: { pname = "jitterentropy-tools-musl"; });

      # Fully static musl: no interpreter, no libc.so, nothing to resolve at
      # run time. pkgsStatic is musl-based on Linux
      # (x86_64-unknown-linux-musl), so this is the same libc as muslFor with
      # the linkage changed, which is the point - static linking is where the
      # library's use of pthreads and of clock_gettime() has to hold up without
      # a dynamic loader to fall back on.
      #
      # pname is deliberately left as toolsFor sets it: the static stdenv
      # already appends "-static-x86_64-unknown-linux-musl" to the derivation
      # name, so the store path says both libc and linkage on its own.
      muslStaticFor = pkgs: toolsFor pkgs.pkgsStatic;

      # Cross-compilation smoke builds.
      #
      # The VM tests below only ever exercise the host architecture, which left
      # every platform outside x86-64/aarch64/i686 Linux with no coverage at
      # all - and those are exactly the targets whose arch/ backends are
      # selected by preprocessor conditions that nothing here evaluates. These
      # derivations compile and link the library and its tools for each target;
      # they are not run, which is enough to catch the class of breakage that
      # actually occurs (a missing declaration, an unavailable header, an
      # unlinked import library, inline asm that does not assemble).
      #
      # Build them all with:
      #   nix build .#cross-riscv64 .#cross-s390x .#cross-mingwW64 ...
      crossFor = { cross, timer ? true, shared ? false }:
        cross.stdenv.mkDerivation {
          pname = "jitterentropy-cross";
          version = "3.7.1";
          src = self;
          nativeBuildInputs = [ nixpkgs.legacyPackages.x86_64-linux.cmake ];
          cmakeFlags = [ "-DINTERNAL_TIMER=${if timer then "on" else "off"}" ]
            ++ lib.optional shared "-DBUILD_SHARED_LIBS=ON";
          enableParallelBuilding = true;
          # Same reason as in toolsFor above, and it matters more here: these
          # builds exist to surface diagnostics, so 17 lines of
          # "_FORTIFY_SOURCE requires compiling with optimization" per target
          # would bury the one warning worth reading.
          hardeningDisable = [ "fortify" "fortify3" ];
          meta = {
            description = "Jitter RNG cross-compilation smoke build";
            license = lib.licenses.bsd3;
          };
        };

      crossTargets = pkgs:
        let p = pkgs.pkgsCross;
        in {
          # Architectures with a dedicated jent_get_nstime() backend that the
          # native builds never compile: stcke, the PowerPC timebase, rdtime,
          # rdtime.d. armv7 covers the generic clock_gettime() fallback and,
          # with i686, the 32-bit paths (JENT_MAX_AUTO_MEMSIZE, the div64
          # helpers).
          cross-s390x = crossFor { cross = p.s390x; };
          cross-ppc64 = crossFor { cross = p.powernv; };
          cross-riscv64 = crossFor { cross = p.riscv64; };
          cross-loongarch64 = crossFor { cross = p.loongarch64-linux; };
          cross-armv7 = crossFor { cross = p."armv7l-hf-multiplatform"; };
          cross-i686 = crossFor { cross = p.gnu32; };

          # Windows/MinGW. Both link widths, and the shared build in addition
          # because it is the only configuration that exercises the
          # dllexport/dllimport split in jitterentropy.h and the bcrypt import
          # library.
          #
          # The internal timer is built here, unlike everything else about this
          # toolchain would suggest: nixpkgs builds its mingw-w64 without
          # winpthreads, so it ships no <pthread.h> at all, and the timer had
          # to be switched off for want of a threading back-end.
          # It now uses the Win32 threads, which come from the CRT and
          # kernel32, so this is the one target that proves the Win32 back-end
          # needs nothing a Windows toolchain may lack.
          cross-mingwW64 = crossFor { cross = p.mingwW64; };
          cross-mingwW64-shared = crossFor { cross = p.mingwW64; shared = true; };
          cross-mingw32 = crossFor { cross = p.mingw32; };
        };

      # Android build of the userspace library via ndk-build, verifying
      # arch/android/Android.mk against the real NDK toolchain. The NDK is
      # unfree, so a dedicated nixpkgs instance accepts its license for this
      # output only; every other output stays on the unmodified package set.
      #
      # APP_PLATFORM is the NDK's own floor and not a level of this library's
      # choosing: r29 supports API 21 upwards for every ABI built here (see
      # ndk/abis.py, where anything below is rejected outright). It used to be
      # android-30, which was the floor the internal timer's threading back-end
      # imposed on bionic. Linux, Android included, now takes pthreads
      # unconditionally (see the threading back-end note in jitterentropy.h),
      # and bionic has had those since API 1, so that floor is gone with it.
      #
      # Nothing else the library calls holds a floor above 21 either: mmap,
      # munmap, madvise, mlock, munlock, sched_setaffinity, sysconf and
      # clock_gettime are all unguarded in bionic at that level. getrandom() is
      # the one exception at __INTRODUCED_IN(28), and rather than let it set the
      # floor arch/jitterentropy-arch-uuid.c takes its /dev/urandom fallback
      # below 28 - the same thing it does for glibc older than 2.25. Building at
      # the toolchain floor is what keeps that branch honest: at android-30 it
      # was never compiled.
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

      # Downstream consumers compiled against this working tree.
      #
      # Everything else here builds the library and its own tools, which says
      # nothing about whether the installed result is still usable by the
      # programs that link it: an installed header that no longer declares what
      # it used to, a symbol that stops being exported, a link dependency that
      # is not carried into the .pc file are all invisible to a build of this
      # repository alone and break the consumer instead. rng-tools (whose rngd
      # feeds the kernel from a jitter entropy source) and ESDM (whose es_jent
      # entropy source is this library) are the two that matter, and both are
      # packaged in nixpkgs, so the smoke test is the packaged consumer with its
      # jitterentropy input swapped for this tree.
      #
      # nixpkgs' own jitterentropy derivation with only src and cmakeFlags
      # replaced, rather than toolsFor above: what is being tested is the
      # library as a distribution installs it - the dev/out split that puts the
      # headers, the .pc file and the CMake package files in a separate output -
      # and reusing the packaging keeps the swap a source-only one.
      #
      # Compile and link only. Both consumers ship test suites that exercise the
      # jitter source at run time (rng-tools' tests/rngtestjitter.sh starts rngd
      # on it, ESDM's meson test set has an "ES Jitter RNG" case), and both are
      # switched off here: this is a build check, and the library's behaviour is
      # covered by the tool runs and the VM tests elsewhere in this flake.
      # Turning doCheck back on is what to change if that ever becomes the
      # point.
      consumersFor = pkgs:
        let
          # INTERNAL_TIMER decides which symbols the library exports, and
          # rng-tools' configure probes for jent_notime_settick(): with the
          # internal timer off it is absent and rngd_jitter.c compiles without
          # the tick handling, with it on the same file drives the timer thread
          # itself. Two different compilations of the consumer, hence both.
          #
          # off is what nixpkgs builds (and what the distributions therefore
          # ship), on is this repository's own default (see the INTERNAL_TIMER
          # option in CMakeLists.txt).
          #
          # ESDM compiles the same code either way, but gates a large part of it
          # on the JENT_VERSION the installed header defines - jent_status(),
          # jent_secure_memory_supported(), JENT_NTG1 and the
          # JENT_MAX_MEMSIZE_*/JENT_HASHLOOP_* flags are all behind
          # "JENT_VERSION >= 3070000" in its esdm_es_jent.c - which makes it the
          # consumer that pins the widest part of the API.
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

      # The three external crypto backends, i.e. the EXTERNAL_CRYPTO option of
      # CMakeLists.txt.
      #
      # With it set, the library stops using its own SHA-3 and its own secure
      # memory and calls into the named library instead - which changes both
      # halves of arch/jitterentropy-arch-memory.c and
      # arch/jitterentropy-arch-fips.c that the default build never compiles:
      # gcry_malloc_secure() out of libgcrypt's secmem pool, OpenSSL's secure
      # heap (OPENSSL_secure_malloc()), and AWS-LC's OPENSSL_malloc(), which is
      # wiped but not locked and so is the one backend that reports itself as
      # not secure. Nothing else in this flake or the workflow builds any of
      # them, and each is selected by a preprocessor condition, so a break there
      # is invisible until a consumer that configures the library this way hits
      # it.
      #
      # BUILD_SHARED_LIBS is on because the two searches in CMakeLists.txt are
      # tied together: a static jitterentropy makes it look for a static
      # libcrypto.a / libgcrypt.a, which is not what these packages install.
      #
      # The secure-memory arena these two allocate the collector out of is not
      # sized by the library - it is process-wide state that the application
      # establishes, which for these builds means the recording tools
      # themselves (tests/raw-entropy/recording_userspace/jitterentropy-memlock.h).
      # An application that establishes none still gets a collector, from
      # ordinary memory, unless it asked for JENT_FORCE_SECURE_MEM.
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
          # libgcrypt's headers include <gpg-error.h>, which reaches the
          # compile through libgcrypt's own propagated libgpg-error rather than
          # being named here.
          crypto-libgcrypt = backendFor {
            name = "libgcrypt";
            external = "LIBGCRYPT";
            dep = pkgs.libgcrypt;
          };
        };

      # Out-of-tree kernel module (jitter_rng.ko) built against a given kernel.
      # The with* arguments mirror the CONFIG_EXTERNAL_JITTERENTROPY_* options
      # in linux_kernel/Kbuild.config and are passed on the make command line,
      # overriding that file's defaults. They can be changed on the resulting
      # derivation via .override. withTestInterface enables the debugfs raw
      # entropy test interface, which starves the RNG of entropy and thus must
      # never be enabled on production systems.
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

      # Machine configuration shared between the VM tests and the live ISO
      # images: the chosen kernel with the jitter_rng module built against it
      # and loaded at boot, the userspace tools in the system profile, and the
      # testing conveniences (root autologin, shell aliases, smoke-test
      # script). Both consumers are test environments, hence the module is
      # built with the debugfs raw entropy test interface that must never be
      # enabled on production systems.
      machineFor = kernelPackages:
        { config, lib, pkgs, ... }: {
          boot.kernelPackages = kernelPackages;
          boot.extraModulePackages = [
            ((moduleFor pkgs config.boot.kernelPackages.kernel).override {
              withTestInterface = true;
            })
          ];
          boot.kernelModules = [ "jitter_rng" ];
          # Verbose logging of the kcapi and test interface per-instance
          # JSON status to the kernel log; only useful on test systems like
          # these images.
          boot.extraModprobeConfig = ''
            options jitter_rng verbose=1 ntg1=1 cache_all=1
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
          # Exercises the chardev O_NONBLOCK semantics: reads are capped at
          # one 32-byte buffer, and a reader that would have to wait for a
          # concurrent read on the same file description gets EAGAIN.
          environment.etc."jitterentropy-nonblock-test.py".text = ''
              import os
              import threading
              import time

              fd = os.open("/dev/jitterentropy", os.O_RDONLY | os.O_NONBLOCK)

              data = os.read(fd, 4096)
              assert len(data) == 32, f"nonblocking read returned {len(data)} bytes"

              # A large blocking read takes the instance lock per 32-byte
              # chunk, and generation dominates the time between chunks, so a
              # nonblocking read on the same file description sees EAGAIN with
              # high probability per attempt (the retry loop below tolerates
              # the occasional win between chunks). The read must be issued
              # while the fd is still blocking (O_NONBLOCK is checked on
              # entry), hence the sleep before flipping the shared flag back.
              # The daemon thread is killed on process exit; the kernel read
              # loop honors the pending signal, so exit is not delayed by the
              # large request.
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
          # mkForce: the ISO's installation-device profile autologs in the
          # "nixos" user; these images are for testing, log in root directly.
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

      # A NixOS integration test that boots a VM with the shared machine
      # configuration on the chosen kernel. Used both as a flake check (the
      # test runs the assertions in a VM) and, via its interactive driver, as
      # a `nix run` target (no dedicated system.build.vm image is produced).
      mkVmTest = pkgs: name: kernelPackages:
        pkgs.testers.runNixOSTest {
          name = "jitterentropy-${name}";

          nodes.machine = {
            imports = [ (machineFor kernelPackages) ];
            boot.kernelParams = [ "clocksource=tsc" "tsc=reliable" ];
            virtualisation.qemu.options = [ "-cpu" "host" ];
            # The O_NONBLOCK test needs a poller that runs while a concurrent
            # reader holds the instance lock. On one vCPU that requires the
            # kernel to preempt the lock holder inside the locked section;
            # non-preemptible kernel builds (5.10, plain PREEMPT_VOLUNTARY)
            # only reschedule at the reader's cond_resched() after unlock, so
            # the poller would always find the lock free and never see
            # EAGAIN. A second vCPU makes the contention real concurrency,
            # independent of the kernel's preemption model.
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
                         "jitterentropy-hashtime", "gcd", "extractlsb",
                         "getrawentropy", "jitterentropy-chardev-status"):
                machine.succeed(f"command -v {tool}")
          '';
        };

      # One kernel module package per nixpkgs kernel, plus the default and
      # latest kernels, mirroring the VM test and image sets. Interface
      # selection is tunable on every attribute via .override { withChardev,
      # withHwrng, withTestInterface }.
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

      # The numbered kernel package sets exposed by nixpkgs (linux_6_12, ...)
      # plus the mainline testing kernel — the flavored variants (zen,
      # xanmod, hardened, libre, rpi, ...) are of no interest here. tryEval
      # guards the sets that fail to evaluate (unsupported on the current
      # system, ...).
      kernelSetsFor = pkgs:
        lib.filterAttrs (name: ps:
          let
            r = builtins.tryEval
              (lib.isAttrs ps && ps ? kernel && lib.isDerivation ps.kernel);
          in (builtins.match "linux_[0-9]+_[0-9]+" name != null
            || name == "linux_testing") && r.success && r.value)
          pkgs.linuxKernel.packages;

      # One VM test per nixpkgs kernel, plus the default and latest kernels.
      vmTestsFor = pkgs:
        (lib.mapAttrs'
          (name: ps: lib.nameValuePair "vm-${name}" (mkVmTest pkgs "vm-${name}" ps))
          (kernelSetsFor pkgs)) // {
            vm = mkVmTest pkgs "vm" pkgs.linuxPackages;
            vm-latest = mkVmTest pkgs "vm-latest" pkgs.linuxPackages_latest;
          };

      # A live ISO image booting the shared machine configuration on the
      # chosen kernel, for exercising the Jitter RNG on real hardware. Build
      # with e.g. `nix build .#iso-linux_6_6`; the image lands in
      # result/iso/jitterentropy-<name>-<kernel version>.iso.
      mkIso = system: name: kernelPackages:
        (lib.nixosSystem {
          modules = [
            "${nixpkgs}/nixos/modules/installer/cd-dvd/installation-cd-minimal.nix"
            (machineFor kernelPackages)
            ({ config, lib, ... }: {
              nixpkgs.hostPlatform = system;
              image.baseName = lib.mkForce
                "jitterentropy-${name}-${config.boot.kernelPackages.kernel.version}";
              # The installation CD enables ZFS via all-hardware; not every
              # kernel here has a compatible ZFS module (latest, testing,
              # xanmod, ...), and the live image does not need ZFS.
              boot.supportedFilesystems.zfs = lib.mkForce false;
              # Throwaway live image, no state to migrate.
              system.stateVersion = lib.trivial.release;
            })
          ];
        }).config.system.build.isoImage;

      # One live ISO per nixpkgs kernel, plus the default and latest kernels,
      # mirroring the VM test set.
      isosFor = system: pkgs:
        (lib.mapAttrs'
          (name: ps: lib.nameValuePair "iso-${name}" (mkIso system name ps))
          (kernelSetsFor pkgs)) // {
            iso = mkIso system "default" pkgs.linuxPackages;
            iso-latest = mkIso system "latest" pkgs.linuxPackages_latest;
          };

      # A bootable SD card image for the 64-bit Raspberry Pi boards (Zero 2,
      # 3, 4, 5) booting the shared machine configuration on the chosen
      # kernel, for exercising the Jitter RNG on real hardware without a
      # bootable CD path. Build with e.g. `nix build .#sd-image-linux_6_18`;
      # the image lands in
      # result/sd-image/jitterentropy-<name>-<kernel version>.img.zst.
      mkSdImage = system: name: kernelPackages:
        (lib.nixosSystem {
          modules = [
            "${nixpkgs}/nixos/modules/installer/sd-card/sd-image-aarch64.nix"
            (machineFor kernelPackages)
            ({ config, lib, ... }: {
              nixpkgs.hostPlatform = system;
              image.baseName = lib.mkForce
                "jitterentropy-${name}-${config.boot.kernelPackages.kernel.version}";
              # The base profile pulled in by sd-image-aarch64.nix enables
              # ZFS whenever the platform supports it; not every kernel here
              # has a compatible ZFS module (latest, rpi, ...), and the test
              # image does not need ZFS.
              boot.supportedFilesystems.zfs = lib.mkForce false;
              # Throwaway test image, no state to migrate.
              system.stateVersion = lib.trivial.release;
            })
          ];
        }).config.system.build.sdImage;

      # One SD image per kernel set, plus the default and latest kernels,
      # mirroring the ISO set. The nixos-hardware vendor kernels (rpi3, rpi4,
      # rpi5) are among them; the default mainline kernel is the combination
      # the upstream image targets.
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
          // consumersFor pkgs
          // isosFor system pkgs
          # The SD images boot Raspberry Pi boards, which are aarch64.
          // lib.optionalAttrs (system == "aarch64-linux")
            (sdImagesFor system pkgs)
          # The NDK host toolchain in nixpkgs is x86_64-linux only. The cross
          # toolchains are likewise only assembled for an x86_64-linux host.
          // lib.optionalAttrs (system == "x86_64-linux") (crossTargets pkgs // {
            android = androidFor system;
            # 32-bit x86 build of the kernel module, compiled natively via the
            # pkgsi686Linux package set (x86_64 hosts execute i686 binaries
            # directly). Exercises the 32-bit code paths, e.g. the div64
            # helpers replacing the libgcc 64-bit division routines that the
            # kernel does not provide.
            jitterentropy-module-i686 =
              moduleFor pkgs.pkgsi686Linux pkgs.pkgsi686Linux.linuxPackages_latest.kernel;
          }));

      # `nix flake check` boots every VM and runs its assertions. Individual
      # VMs can be run with e.g. `nix build .#checks.x86_64-linux.vm-linux_6_6`.
      checks =
        forAllSystems (system: vmTestsFor nixpkgs.legacyPackages.${system});

      # `nix run .#vm-linux_6_6` launches the same VM interactively through the
      # NixOS test driver (run `start_all()` then `machine.shell_interact()`).
      apps = forAllSystems (system:
        let
          runners = lib.mapAttrs (_name: test: {
            type = "app";
            program = "${test.driverInteractive}/bin/nixos-test-driver";
          }) self.checks.${system};
        in runners // { default = runners.vm; });
    };
}
