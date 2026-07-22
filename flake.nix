{
  description =
    "Jitter RNG: userspace library/tools (CMake) and out-of-tree kernel module, plus NixOS VMs runnable via nix run and nix flake check.";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      lib = nixpkgs.lib;

      # VMs run under QEMU on the host architecture.
      systems = [ "x86_64-linux" "aarch64-linux" ];
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
          meta = {
            description = "Jitter RNG userspace library and validation tools";
            license = lib.licenses.bsd3;
          };
        };

      # Android build of the userspace library via ndk-build, verifying
      # arch/android/Android.mk against the real NDK toolchain. The NDK is
      # unfree, so a dedicated nixpkgs instance accepts its license for this
      # output only; every other output stays on the unmodified package set.
      # APP_PLATFORM=android-30 is the floor for the C11 <threads.h> that the
      # internal-timer thread helper uses on Linux/bionic.
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
              APP_PLATFORM=android-30 \
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
            "sample_kernel" = "getrawentropy --samples 1000000 --debugfs-file /sys/kernel/debug/jitter_rng/jent_raw_hires";
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

      # Every kernel package set exposed by nixpkgs that provides a real kernel
      # derivation. tryEval guards the sets that fail to evaluate (unfree,
      # unsupported on the current system, ...). Note that board-specific
      # kernels (e.g. linux_rpi*) only boot on a matching host architecture.
      kernelSetsFor = pkgs:
        lib.filterAttrs (_name: ps:
          let
            r = builtins.tryEval
              (lib.isAttrs ps && ps ? kernel && lib.isDerivation ps.kernel);
          in r.success && r.value) pkgs.linuxKernel.packages;

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
    in {
      packages = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = toolsFor pkgs;
          jitterentropy-tools = toolsFor pkgs;
          # Module built against the nixpkgs default kernel. Interface
          # selection is tunable via .override { withChardev, withHwrng,
          # withTestInterface }.
          jitterentropy-module = moduleFor pkgs pkgs.linuxPackages.kernel;
        } // isosFor system pkgs
          # The NDK host toolchain in nixpkgs is x86_64-linux only.
          // lib.optionalAttrs (system == "x86_64-linux") {
            android = androidFor system;
          });

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
