# The helpers of the raw entropy VM tests in ../raw-entropy.nix, for use once
# the tree is copied to /root/jent.

ea = (
    "EATOOL_NONIID=$(command -v ea_non_iid) "
    "EATOOL=$(command -v ea_restart) "
)
raw = "/root/jent/tests/raw-entropy"

def run(d, cmd):
    machine.succeed(f"cd {raw}/{d} && {ea} {cmd} >&2")

# Print each verdict, failing on a result without one.
def show(pattern):
    print(machine.succeed(
        f"for f in {raw}/{pattern}; do echo \"== $f\";"
        " grep -E '^(H_|min\\(|X_max|ALPHA|\\*\\*\\*|Validation)'"
        " $f || exit 1; done"
    ))

# A good source fails ea_restart's sanity check in about 1% of the
# runs (alpha 0.01), which is no tooling error. Any other
# errorMessage fails the check.
verdict = (
    "(.errorLevel == 0 and any(.testCases[];"
    " .testCaseDesc == \"Overall\" and .h_r and .h_c))"
    " or (.errorMessage // \"\" |"
    " test(\"Restart Sanity Check Failed\"))"
)

def restart(cmd, dirs):
    status, _ = machine.execute(
        f"cd {raw}/validation-restart && {ea} {cmd} >&2"
    )
    base = [
        f"{raw}/{d}/jent-raw-noise-restart-consolidated"
        ".minentropy_FF_8bits" for d in dirs
    ]
    jsons = " ".join(f"{b}.json" for b in base)
    machine.succeed(
        f"for f in {jsons}; do jq -e '{verdict}' $f >/dev/null"
        " || { echo $f; exit 1; }; done"
    )
    if status != 0:
        machine.succeed(
            f"jq -e -s 'any(.[]; .errorLevel != 0)' {jsons}"
        )

def reset():
    machine.succeed(
        f"rm -rf {raw}/results-measurements {raw}/results-analysis-*"
    )

# One sample per line.
def recorded(pattern, files, lines):
    out = machine.succeed(
        f"cd {raw}/results-measurements && ls {pattern} | wc -l"
    ).strip()
    assert out == str(files), f"{pattern}: {out} files, not {files}"
    machine.succeed(
        f"cd {raw}/results-measurements && for f in {pattern}; do"
        f' [ "$(wc -l < $f)" = {lines} ] || {{ echo $f; exit 1; }};'
        " done"
    )

# The sets of invoke_testing.sh and invoke_testing_fips.sh.
def assess(what):
    recorded("jent-raw-noise-0001.data", 1, 1000000)
    recorded("jent-raw-noise-restart-*.data", 1000, 1000)
    run("validation-runtime",
        f'./processdata.sh "" ../results-analysis-runtime-{what}')
    restart(f"RESULTS_DIR=../results-analysis-restart-{what}"
            " ./processdata.sh",
            [f"results-analysis-restart-{what}"])
    show(f"results-analysis-*-{what}/*.minentropy_FF_8bits.txt")
    reset()

# invoke_testing_ntg1.sh adds the hash loop and memory access sets.
def assess_ntg1():
    for name in ("jent-raw-noise", "jent-raw-noise_hashloop",
                 "jent-raw-noise_memaccloop"):
        recorded(f"{name}-0001.data", 1, 1000000)
    for name in ("jent-raw-noise-restart",
                 "jent-raw-noise-hashloop-restart",
                 "jent-raw-noise-memaccloop-restart"):
        recorded(f"{name}-*.data", 1000, 1000)
    run("validation-runtime",
        './processdata_ntg1.sh "" ../results-analysis-runtime-ntg1')
    restart("./processdata_ntg1.sh",
            ["results-analysis-restart",
             "results-analysis-hashloop-restart",
             "results-analysis-memaccloop-restart"])
    show("results-analysis-*/*.minentropy_FF_8bits.txt")
    reset()
