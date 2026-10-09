# The tests/raw-entropy recording and validation scripts.
ctx:
let
  inherit (ctx) machineFor self testInterfaceKernelFor;
in
{
  # The SP800-90B tool chain end to end, as tests/raw-entropy documents
  # it for user space: invoke_testing.sh records, the processdata.sh of
  # validation-runtime and validation-restart extract and run ea_non_iid
  # and ea_restart. What is checked is that the scripts get through; the
  # entropy of a build sandbox says nothing about a target platform, so
  # the estimates are printed and kept, not judged.
  rawEntropyFor = pkgs:
    pkgs.stdenv.mkDerivation {
      name = "jitterentropy-raw-entropy";
      src = self;
      nativeBuildInputs = [ pkgs.sp800-90b-entropyassessment ];
      # As for toolsFor: Makefile.hashtime builds at -O0.
      hardeningDisable = [ "fortify" "fortify3" ];
      dontConfigure = true;
      # The full SP800-90B sizes the scripts default to, 1000000 samples
      # and 1000 restarts of 1000: a minute or so in all.
      buildPhase = ''
        runHook preBuild

        data=$TMPDIR/measurements
        raw=$PWD/tests/raw-entropy

        cd $raw/recording_userspace
        OUTDIR=$data bash ./invoke_testing.sh

        cd $raw/validation-runtime
        EATOOL_NONIID=$(command -v ea_non_iid) \
          bash ./processdata.sh $data $TMPDIR/runtime

        # With RESULTS_DIR given, the caller builds extractlsb.
        cd $raw/validation-restart
        make
        # The restart sanity check fails on about 1% of good data, so a
        # failure gets one more try on a new recording.
        for attempt in 1 2; do
          if ENTROPYDATA_DIR=$data RESULTS_DIR=$TMPDIR/restart \
             EATOOL=$(command -v ea_restart) bash ./processdata.sh; then
            break
          fi
          if [ $attempt -eq 2 ]; then
            exit 1
          fi
          echo "restart validation failed, recording again"
          rm -rf $TMPDIR/restart $data/jent-raw-noise-restart-*.data
          (cd $raw/recording_userspace &&
           OUTDIR=$data bash -c '. ./invoke_testing_helper.sh; raw_entropy_restart')
        done

        runHook postBuild
      '';
      installPhase = ''
        runHook preInstall
        mkdir -p $out
        cp $TMPDIR/runtime/*.minentropy_*.txt $TMPDIR/restart/*.minentropy_*.txt $out/
        grep -h 'min(' $out/*.txt
        runHook postInstall
      '';
    };

  # The kernel-space recordings, with the vanilla Jitter RNG through its test
  # interface and with jitter_rng.ko of this tree: recording_runtime_kernelspace
  # with the validation of both, and boottime_test_record.sh, which records
  # early in each boot and reboots the VM - QEMU exits on that, and the test
  # starts it again. The validation needs no more than a recording that runs,
  # so the runtime sets are short and the boot series has two runs where the
  # real test takes 1000.
  rawEntropyKernelVmFor = pkgs:
    let
      restarts = 2;
      samples = 100000;
    in pkgs.testers.runNixOSTest {
      name = "jitterentropy-raw-entropy-kernel";

      nodes.machine = { pkgs, ... }: {
        imports = [ (machineFor (testInterfaceKernelFor pkgs)) ];
        boot.kernelParams = [
          # Buffers the first time stamps of each boot for the recorder.
          "jitterentropy_testing.boot_raw_hires_test=1"
          "clocksource=tsc"
          "tsc=reliable"
        ];
        # The kernel builds its drivers in and has no TPM driver.
        boot.initrd.includeDefaultModules = false;
        boot.initrd.systemd.tpm2.enable = false;
        virtualisation.qemu.options = [ "-cpu" "host" ];
        virtualisation.cores = 2;
        virtualisation.memorySize = 2048;
        virtualisation.diskSize = 4096;
        # The scripts build getrawentropy and extractlsb themselves.
        environment.systemPackages = with pkgs; [ gcc gnumake ];

        # boottime_test_record.service with the paths NixOS has; DEBUGFS_FILE
        # and the like come from the EnvironmentFile.
        systemd.services.boottime_test_record = {
          description = "Boot time test for Kernel Jitter RNG";
          unitConfig.DefaultDependencies = false;
          wants = [ "sys-kernel-debug.mount" ];
          after = [ "local-fs.target" "sys-kernel-debug.mount"
                    "systemd-modules-load.service" ];
          before = [ "sysinit.target" ];
          wantedBy = [ "basic.target" ];
          path = with pkgs; [ coreutils gnugrep util-linux systemd pciutils ];
          environment = {
            GETRAWENTROPY = "/run/current-system/sw/bin/getrawentropy";
            KCAPIRNG = "${pkgs.libkcapi}/bin/kcapi-rng";
            TESTS = toString restarts;
          };
          serviceConfig = {
            EnvironmentFile = "-/etc/default/boottime_test_record";
            ExecStart = "${pkgs.bash}/bin/bash ${self}/tests/raw-entropy/recording_restart_kernelspace/boottime_test_record.sh";
          };
        };
      };

      testScript = ''
        src = "${self}"
        restarts = ${toString restarts}
        samples = ${toString samples}
      '' + builtins.readFile ./raw-entropy/kernel-test.py;
    };
}
