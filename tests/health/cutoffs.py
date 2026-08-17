#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#
# Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
#
"""Recompute the health test cutoffs of src/jitterentropy-health.{c,h}.

They were derived by hand with R and a SageMath script that is not in this
tree, so a reader had no way to check them and a change of window size, alpha
or security margin had no way to be followed through. This script computes
them with mpmath and, with --check, compares what it gets against what the
source carries.

The cutoffs:

  RCT (SP800-90B section 4.4.1)
    C = ceil(-log2(alpha) / H) with H = margin/osr, one less than the
    standard's cutoff because ec->rct_count starts at zero. The common case
    is the two macros in src/jitterentropy-health.h; the NTG.1 margin of 8 is
    applied to them at run time by jent_rct_init(), rounding up.

  APT (SP800-90B section 4.4.2, with the corrected cutoff of comment #10b)
    C = 2 + qbinom(1 - alpha, JENT_APT_WINDOW_SIZE - 1, 2^(-margin/osr))
    capped at the window size, which FIPS 140-2 IG 7.19 resolution #16 asks
    for so the test can still fail. margin is 1, or 8 for the NTG.1 tables.

  Lag predictor, global cutoff
    qbinom(1 - alpha, JENT_LAG_WINDOW_SIZE - JENT_LAG_HISTORY_SIZE,
           2^(-1/osr))

  Lag predictor, local cutoff
    The shortest run of correct predictions whose probability of occurring
    anywhere in a window is below alpha. The probability of no run of length
    r in n Bernoulli(p) trials is

      (1 - p x) / ((r + 1 - r x) q) * x^-(n+1)

    with x the root near 1 of 1 - x + q p^r x^(r+1).

  Repetition count test with memory
    floor(n p + tau * sqrt(n p' (1 - p'))), capped at n and, for the
    permanent cutoff, at n + 1. n = 321/3 * osr is the number of observations
    a window makes, p = 2^(1 - margin/osr) is twice the 2^(-H) of the
    heuristic entropy H = margin/osr, and p' = min(p, 1/2) holds the variance
    at its maximum once p passes 1/2. tau is the 4 and 5 of the significance
    levels pnorm(-4) and pnorm(-5) the source quotes.

    In the common case, margin 1, p >= 1 at every oversampling rate, so both
    cutoffs are the cap - which is what "these values effectively disables the
    health test" beside them means.

alpha is 2^-30 for the RCT and the APT and 2^-22 for the lag predictor, whose
window is much larger; the permanent cutoffs use its square. The margin is the
safety factor of the entropy assumption: 1 in the common case, 8 for NTG.1
operation, whose tables this computes as well.

Usage:
    python3 tests/health/cutoffs.py            # print the cutoffs as C
    python3 tests/health/cutoffs.py --check    # compare against the source
"""

import argparse
import re
import sys

try:
    from mpmath import (mp, mpf, ceil, erfinv, exp, floor, log, loggamma,
                        power, sqrt)
except ImportError:
    sys.exit("this script needs mpmath (pip install mpmath)")

# src/jitterentropy-internal.h
APT_WINDOW_SIZE = 512
LAG_WINDOW_SIZE = 1 << 17
LAG_HISTORY_SIZE = 8

# JENT_MAX_OSR, the highest oversampling rate the library accepts: every
# table covers 1 up to it, which a build assertion in the source enforces.
MAX_OSR = 20

# The 8-fold entropy margin of NTG.1 operation.
NTG1_MARGIN = 8

# The window of the repetition count test with memory: the 321 * osr deltas
# jent_random_data_one() generates for one output block, of which tau = 3
# leaves every third one observed. The cutoffs sit that many standard
# deviations above the mean.
RCT_MEM_WINDOW = 321
RCT_MEM_TAU = 3
RCT_MEM_SIGMA = 4
RCT_MEM_SIGMA_PERMANENT = 5


def upper_tail(m, n, p):
    """P(X >= m) for X ~ Binomial(n, p).

    Summed from m upwards with the ratio between neighbouring terms, so no
    binomial coefficient is ever formed; the first term comes out of
    loggamma, which n = 131064 needs.
    """
    if m <= 0:
        return mpf(1)
    if m > n:
        return mpf(0)

    q = 1 - p
    term = exp(loggamma(n + 1) - loggamma(m + 1) - loggamma(n - m + 1)
               + m * log(p) + (n - m) * log(q))
    total = mpf(0)
    tiny = power(10, -mp.dps)

    j = m
    while j < n:
        total += term
        term *= mpf(n - j) / (j + 1) * p / q
        j += 1
        if total > 0 and term < total * tiny:
            return total

    return total + term


def qbinom(alpha, n, p):
    """min{k : P(X <= k) >= 1 - alpha}, i.e. R's qbinom(1 - alpha, n, p)."""
    lo, hi = 0, n

    # The normal approximation is within a few units of the answer at these
    # window sizes, and bisecting the whole range would spend most of its
    # evaluations deep in the body of the distribution, where the tail sum
    # above needs the most terms.
    sigma = sqrt(n * p * (1 - p))
    guess = int(n * p + sqrt(2) * erfinv(1 - 2 * alpha) * sigma)
    margin = int(20 * sigma) + 100
    if 0 < guess - margin and guess + margin < n:
        lo, hi = guess - margin, guess + margin
        if upper_tail(lo + 1, n, p) <= alpha or upper_tail(hi + 1, n, p) > alpha:
            lo, hi = 0, n

    while lo < hi:
        mid = (lo + hi) // 2
        if upper_tail(mid + 1, n, p) <= alpha:
            hi = mid
        else:
            lo = mid + 1

    return lo


def run_root(r, p):
    """The root near 1 of 1 - x + q p^r x^(r+1), by Newton's method.

    Bracketing solvers are no use here: at small r the root is a double one
    (p = 1/2, r = 1 gives (x - 2)^2 / 4) and the sign never changes.
    """
    c = (1 - p) * power(p, r)
    x = 1 + c
    eps = power(10, -mp.dps + 5)

    for _ in range(200):
        step = ((1 - x + c * power(x, r + 1))
                / (-1 + c * (r + 1) * power(x, r)))
        x -= step
        if abs(step) < abs(x) * eps:
            return x

    raise ArithmeticError("root did not converge for r=%d" % r)


def run_prob(r, n, p):
    """P(a run of r successes occurs in n Bernoulli(p) trials)."""
    q = 1 - p
    x = run_root(r, p)
    no_run = (1 - p * x) / ((r + 1 - r * x) * q) / power(x, n + 1)
    return 1 - no_run


def run_cutoff(alpha, n, p):
    """The shortest run whose probability of occurring is at most alpha."""
    # Every run of r has to start somewhere, so n q p^r is the leading term
    # of the probability of finding one; that inverts to a starting point a
    # few steps below the answer.
    r = max(int(ceil(log(n * (1 - p) / alpha) / log(1 / p))) - 8, 1)

    while r > 1 and run_prob(r, n, p) <= alpha:
        r -= 1
    while run_prob(r, n, p) > alpha:
        r += 1

    return r


def rct_cutoff(osr, alpha, margin=1):
    """C = ceil(-log2(alpha) / H) for the entropy rate H = margin/osr."""
    return int(ceil(-log(alpha, 2) * osr / margin))


def rct_table(margin, alpha):
    return [rct_cutoff(osr, alpha, margin)
            for osr in range(1, MAX_OSR + 1)]


def rct_mem_table(margin, sigmas, cap_offset):
    """The cutoffs of the repetition count test with memory.

    The mean plus @sigmas standard deviations of the stuck count over the
    n = 321/tau * osr observations a window makes, rounded down and capped at
    n (n + 1 for the permanent cutoff), which is where the test can no longer
    fail. p is twice the 2^(-H) of the heuristic entropy H = margin/osr, and
    the variance is held at its maximum once p passes 1/2.
    """
    table = []

    for osr in range(1, MAX_OSR + 1):
        n = RCT_MEM_WINDOW // RCT_MEM_TAU * osr
        p = power(2, 1 - mpf(margin) / osr)
        var_p = min(p, mpf(1) / 2)
        cutoff = int(floor(n * p + sigmas * sqrt(n * var_p * (1 - var_p))))
        table.append(min(cutoff, n + cap_offset))

    return table


def apt_table(margin, alpha):
    n = APT_WINDOW_SIZE - 1
    return [min(2 + qbinom(alpha, n, power(2, mpf(-margin) / osr)),
                APT_WINDOW_SIZE)
            for osr in range(1, MAX_OSR + 1)]


def lag_global_table(alpha):
    n = LAG_WINDOW_SIZE - LAG_HISTORY_SIZE
    return [qbinom(alpha, n, power(2, mpf(-1) / osr))
            for osr in range(1, MAX_OSR + 1)]


def lag_local_table(alpha):
    n = LAG_WINDOW_SIZE - LAG_HISTORY_SIZE
    return [run_cutoff(alpha, n, power(2, mpf(-1) / osr))
            for osr in range(1, MAX_OSR + 1)]


# The name and type each table has in src/jitterentropy-health.c, in the
# order the file declares them.
TABLES = [
    ("jent_lag_global_cutoff_lookup", "unsigned int",
     lambda: lag_global_table(mpf(2) ** -22)),
    ("jent_lag_global_cutoff_permanent_lookup", "unsigned int",
     lambda: lag_global_table(mpf(2) ** -44)),
    ("jent_lag_local_cutoff_lookup", "unsigned int",
     lambda: lag_local_table(mpf(2) ** -22)),
    ("jent_lag_local_cutoff_permanent_lookup", "unsigned int",
     lambda: lag_local_table(mpf(2) ** -44)),
    ("jent_apt_cutoff_lookup", "unsigned int",
     lambda: apt_table(1, mpf(2) ** -30)),
    ("jent_apt_cutoff_permanent_lookup", "unsigned int",
     lambda: apt_table(1, mpf(2) ** -60)),
    ("jent_apt_cutoff_lookup_ntg1", "unsigned int",
     lambda: apt_table(NTG1_MARGIN, mpf(2) ** -30)),
    ("jent_apt_cutoff_permanent_lookup_ntg1", "unsigned int",
     lambda: apt_table(NTG1_MARGIN, mpf(2) ** -60)),
    ("jent_rct_mem_cutoff_lookup", "unsigned short",
     lambda: rct_mem_table(1, RCT_MEM_SIGMA, 0)),
    ("jent_rct_mem_cutoff_permanent_lookup", "unsigned short",
     lambda: rct_mem_table(1, RCT_MEM_SIGMA_PERMANENT, 1)),
    ("jent_rct_mem_cutoff_lookup_ntg1", "unsigned short",
     lambda: rct_mem_table(NTG1_MARGIN, RCT_MEM_SIGMA, 0)),
    ("jent_rct_mem_cutoff_permanent_lookup_ntg1", "unsigned short",
     lambda: rct_mem_table(NTG1_MARGIN, RCT_MEM_SIGMA_PERMANENT, 1)),
]

# The RCT has no table: src/jitterentropy-health.h states the cutoff as a
# macro multiplying the oversampling rate, which is what these check.
RCT_MACROS = [
    ("JENT_HEALTH_RCT_INTERMITTENT_CUTOFF", mpf(2) ** -30),
    ("JENT_HEALTH_RCT_PERMANENT_CUTOFF", mpf(2) ** -60),
]


def format_table(name, ctype, values):
    width = max(len("%d" % v) for v in values)
    per_line = max(1, (72 - 8) // (width + 2))
    lines = []

    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        row = ", ".join("%*d" % (width, v) for v in chunk)
        lines.append(("\t{ " if i == 0 else "\t  ") + row)

    return ("static const %s %s[%d] =\n" % (ctype, name, len(values))
            + ",\n".join(lines) + " };")


def format_rct():
    """The RCT cutoffs as src/jitterentropy-health.h states them."""
    out = []

    for name, alpha in RCT_MACROS:
        out.append("/* RCT: cutoff for alpha = 2**%d, H = 1/osr */"
                   % int(log(alpha, 2)))
        out.append("#define %s(x) ((x) * %d)"
                   % (name, rct_cutoff(1, alpha)))

    ntg1 = ", ".join("%d" % v for v in
                     rct_table(NTG1_MARGIN, RCT_MACROS[0][1])[:8])
    out.append("/* NTG.1 divides those by %d at run time, rounding up:"
               % NTG1_MARGIN)
    out.append(" * intermittent, osr 1..8: %s, ... */" % ntg1)

    return "\n".join(out)


def source_tables(path):
    """The tables as src/jitterentropy-health.c carries them."""
    text = open(path).read()
    found = {}

    for match in re.finditer(
            r"static const unsigned (?:int|short) (\w+)\[\d*\]\s*=\s*\{([^}]*)\}",
            text):
        found[match.group(1)] = [int(v) for v in
                                 re.findall(r"\d+", match.group(2))]

    return found


def source_rct_macros(path):
    """The RCT cutoff macros as src/jitterentropy-health.h states them."""
    text = open(path).read()

    return {m.group(1): int(m.group(2)) for m in re.finditer(
        r"#define (JENT_HEALTH_RCT_\w+)\(x\)\s*\(\(x\) \* (\d+)\)", text)}


def main():
    parser = argparse.ArgumentParser(
        description="Recompute the Jitter RNG health test cutoff tables.")
    parser.add_argument("--check", metavar="FILE", nargs="?",
                        const="src/jitterentropy-health.c",
                        help="compare against FILE instead of printing the "
                             "tables (default src/jitterentropy-health.c)")
    parser.add_argument("--table", action="append", metavar="NAME",
                        help="only this table, repeatable")
    parser.add_argument("--dps", type=int, default=60,
                        help="decimal digits mpmath works with (default 60)")
    args = parser.parse_args()

    mp.dps = args.dps

    tables = TABLES
    if args.table:
        tables = [t for t in TABLES if t[0] in args.table]
        unknown = set(args.table) - {name for name, _, _ in TABLES}
        if unknown:
            sys.exit("unknown table(s): %s" % ", ".join(sorted(unknown)))

    if not args.check:
        if not args.table:
            print(format_rct())
        for name, ctype, compute in tables:
            print(format_table(name, ctype, compute()))
        return 0

    found = source_tables(args.check)
    failed = 0

    def report(name, ok, detail):
        print("%-45s %s" % (name, "ok (%s)" % detail if ok else detail))
        return 0 if ok else 1

    # The macros live in the header beside the source the tables come from.
    if not args.table:
        macros = source_rct_macros(re.sub(r"\.c$", ".h", args.check))
        for name, alpha in RCT_MACROS:
            computed = rct_cutoff(1, alpha)
            failed += report(name, macros.get(name) == computed,
                             "x * %d" % computed if macros.get(name) == computed
                             else "MISMATCH: header has %s, computed x * %d"
                                  % (macros.get(name), computed))

        # jent_rct_init() applies the NTG.1 margin to the macro above with
        # integer arithmetic; that has to agree with the entropy rate 8/osr.
        intermittent = RCT_MACROS[0][1]
        ntg1 = [(rct_cutoff(osr, intermittent) + NTG1_MARGIN - 1)
                // NTG1_MARGIN for osr in range(1, MAX_OSR + 1)]
        failed += report("RCT NTG.1 margin",
                         ntg1 == rct_table(NTG1_MARGIN, intermittent),
                         "%d values" % len(ntg1))

    for name, _ctype, compute in tables:
        computed = compute()
        if name not in found:
            failed += report(name, False, "not found in %s" % args.check)
        elif found[name] != computed:
            failed += report(name, False,
                             "MISMATCH\n  source:   %s\n  computed: %s"
                             % (found[name], computed))
        else:
            failed += report(name, True, "%d values" % len(computed))

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
