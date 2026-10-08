#!/usr/bin/env python3
# Check the SHA3-256 and SHAKE-256 digests of acvpparser's jent backend
# against hashlib. The requests stay within what acvpproxy registers for the
# Jitter RNG; message lengths straddle the 136 byte Keccak rate.
#
# Usage: acvp-parser-check.py <acvp-parser> <work directory>

import hashlib
import json
import random
import subprocess
import sys

RATE = 136
LENGTHS = [0, 1, 2, 3, RATE - 1, RATE, RATE + 1,
           2 * RATE - 1, 2 * RATE, 2 * RATE + 1, 1000, 8192]


def request(vs_id, algorithm, revision, tests):
    return [{"acvVersion": "1.0"},
            {"vsId": vs_id, "algorithm": algorithm, "revision": revision,
             "testGroups": [{"tgId": 1, "testType": "AFT", "tests": tests}]}]


def run(parser, workdir, name, req):
    req_path = f"{workdir}/{name}-request.json"
    rsp_path = f"{workdir}/{name}-response.json"
    with open(req_path, "w") as f:
        json.dump(req, f)
    subprocess.run([parser, req_path, rsp_path], check=True)
    with open(rsp_path) as f:
        rsp = json.load(f)
    vs = next(o for o in rsp if "testGroups" in o)
    return {t["tcId"]: t["md"] for g in vs["testGroups"] for t in g["tests"]}


def main():
    parser, workdir = sys.argv[1:3]
    # Seeded: a failure has to reproduce.
    rng = random.Random(0x4a454e54)

    sha3, sha3_want = [], {}
    shake, shake_want = [], {}
    for n in LENGTHS:
        msg = rng.randbytes(n)
        tc = len(sha3) + 1
        sha3.append({"tcId": tc, "msg": msg.hex(), "len": 8 * n})
        sha3_want[tc] = hashlib.sha3_256(msg).hexdigest()

        if n < 2:
            continue
        for out_bits in (512, 768):
            tc = len(shake) + 1
            shake.append({"tcId": tc, "msg": msg.hex(), "len": 8 * n,
                          "outLen": out_bits})
            shake_want[tc] = hashlib.shake_256(msg).hexdigest(out_bits // 8)

    failed = 0
    for name, req, want in (
            ("sha3-256", request(1, "SHA3-256", "2.0", sha3), sha3_want),
            ("shake-256", request(2, "SHAKE-256", "1.0", shake), shake_want)):
        got = run(parser, workdir, name, req)
        bad = [tc for tc, md in want.items() if got.get(tc, "").lower() != md]
        for tc in bad:
            print(f"::error::{name} tcId {tc}: got {got.get(tc)},"
                  f" expected {want[tc]}")
        print(f"{name}: {len(want) - len(bad)} of {len(want)} digests match")
        failed += len(bad)

    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
