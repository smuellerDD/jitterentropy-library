# The tests/raw-entropy recording and validation scripts.
ctx:
let
  inherit (ctx) self;
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

}
