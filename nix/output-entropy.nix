# The entropy of the library's conditioned output, not of its raw events.
ctx:
let
  inherit (ctx) toolsFor;
in
{
  # 1 MiB of generated output through the NIST SP800-90B estimators.
  #
  # The output, not the noise source: the bytes are conditioned, so a pass
  # only says the stream is not degenerate. The noise source is assessed from
  # raw time deltas in tests/raw-entropy.
  #
  # The floor is 6.0 bits per byte: at 2^20 samples ideal random data
  # assesses at 6.66 to 7.39.
  outputEntropyFor = pkgs:
    pkgs.runCommand "jitterentropy-output-entropy" {
      nativeBuildInputs = [ pkgs.sp800-90b-entropyassessment pkgs.jq ];
      tools = toolsFor pkgs;
      floor = "6.0";
    } ''
      fail() { echo "jitterentropy-output-entropy: $1"; exit 1; }

      # 32768 reads of 32 bytes, the status document on stderr.
      echo "generating 1 MiB of output"
      $tools/bin/jitterentropy-rng 32768 > output.bin 2> status.json ||
        fail "the generator failed"
      cat status.json

      size=$(stat -c %s output.bin)
      [ "$size" = 1048576 ] || fail "expected 1048576 bytes, got $size"

      # -i: the initial entropy estimate per 8-bit symbol, -a: every bit
      # rather than the first million. The path has to be relative.
      ea_non_iid -i -a -o result.json output.bin 8 ||
        fail "the assessment failed"
      cat result.json

      [ "$(jq -r .errorLevel result.json)" = 0 ] ||
        fail "the tool reported an error"

      h=$(jq -r 'first(.testCases[]
                       | select(.testCaseDesc == "Overall")
                       | .hAssessed)' result.json)
      [ -n "$h" ] && [ "$h" != null ] ||
        fail "the JSON carries no overall assessment"

      echo "assessed min-entropy: $h bits per byte, floor $floor"
      jq -ne --argjson h "$h" --argjson floor "$floor" '$h >= $floor' \
        > /dev/null || fail "$h bits per byte is under the floor of $floor"

      echo "jitterentropy-output-entropy: ok"
      mkdir -p $out
      cp result.json status.json $out/
    '';
}
