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
    C = 1 + qbinom(1 - alpha, JENT_LAG_WINDOW_SIZE - JENT_LAG_HISTORY_SIZE,
                   2^(-1/osr))
    one above the quantile, because the test fires once the count of correct
    predictions reaches C, so P(X >= C) is what has to stay within alpha.

  Lag predictor, local cutoff
    The shortest run of correct predictions whose probability of occurring
    anywhere in a window is below alpha. The probability of no run of length
    r in n Bernoulli(p) trials is

      (1 - p x) / ((r + 1 - r x) q) * x^-(n+1)

    with x the root near 1 of 1 - x + q p^r x^(r+1).

  Repetition count test with memory
    floor(n p + tau * sqrt(n p' (1 - p'))), capped: at n for the two
    intermittent cutoffs and at n + 1 for the two permanent ones. n = 321/3 *
    osr is the number of observations a window makes - 321 being
    DATA_SIZE_BITS + ENTROPY_SAFETY_FACTOR, read from the header - p = 2^(1 - margin/osr)
    is twice the 2^(-H) of the heuristic entropy H = margin/osr, and
    p' = min(p, 1/2) holds the variance at its maximum once p passes 1/2. tau
    is the 4 and 5 of the significance levels pnorm(-4) and pnorm(-5) the
    source quotes.

    The caps are what the formula runs into wherever p >= 1 - at every
    oversampling rate in the common case, margin 1, and from osr 8 on for
    NTG.1 - because the mean alone then exceeds the n observations a window
    makes. A permanent cutoff of n + 1 is one past anything a window can
    count, so no window raises it: even a counter primed across a reallocation
    reaches the intermittent cutoff first - the tables rise with osr and a
    reset always raises it - and the recovery loop starts a fresh count. An
    intermittent cutoff of n is reached by a window whose every observation is
    stuck. See rct_mem_table() for why the intermittent cap is n and not
    n + 1.

alpha is 2^-30 for the RCT and the APT and 2^-22 for the lag predictor, whose
window is much larger; the permanent cutoffs use its square. The margin is the
safety factor of the entropy assumption: 1 in the common case, 8 for NTG.1
operation, whose tables this computes as well.

Usage:
    python3 tests/health/cutoffs.py            # print the cutoffs as C
    python3 tests/health/cutoffs.py --check    # compare against the source
"""

import argparse
import os
import re
import sys

from csource import (C_INT_SUFFIX, CannotEvaluate, braced, c_parse,
                     c_run, c_tokens, c_type, header_macros,
                     read_c_source, struct_int_members)

try:
    from mpmath import (mp, mpf, ceil, erfinv, exp, floor, log, loggamma,
                        power, sqrt)
except ImportError:
    sys.exit("this script needs mpmath (pip install mpmath)")

# The window sizes and the rate range are read out of the headers that define
# them, src/jitterentropy-internal.h and jitterentropy.h, by load_header():
# written out here, a change of JENT_APT_WINDOW_SIZE would move the tables in
# the source and here not at all, and --check would compare the new tables against the old
# derivation - or, with both regenerated from this script, stay green on
# cutoffs no longer derived for the window they guard.
APT_WINDOW_SIZE = None
LAG_WINDOW_SIZE = None
LAG_HISTORY_SIZE = None

# JENT_MAX_OSR: every table has exactly one entry per rate from 1 up to it
# (asserted in the source).
MAX_OSR = None

# The window of the repetition count test with memory:
# JENT_ADJUSTED_MEASURE_JITTER_LOOP_CTR(osr, ENTROPY_SAFETY_FACTOR), the
# (DATA_SIZE_BITS + ENTROPY_SAFETY_FACTOR) * osr deltas jent_random_data_one()
# generates for one output block rounded up to a multiple of three, of which
# tau = 3 leaves every third one observed. See rct_mem_observations().
RCT_MEM_BLOCK_BITS = None

# The 8-fold entropy margin of NTG.1 operation. The APT and RCT-mem tables
# carry it in their values; the RCT gets it at run time from the factor
# jent_health_init() passes jent_rct_init(), which --check reads back.
NTG1_MARGIN = 8

# The cutoffs of the repetition count test with memory sit that many standard
# deviations above the mean.
RCT_MEM_TAU = 3
RCT_MEM_SIGMA = 4
RCT_MEM_SIGMA_PERMANENT = 5

# The macros load_header() takes from src/jitterentropy-internal.h and
# jitterentropy.h, and the ones their values may refer to.
HEADER_MACROS = ["JENT_APT_WINDOW_SIZE", "JENT_LAG_WINDOW_SIZE",
                 "JENT_LAG_HISTORY_SIZE", "JENT_MAX_OSR", "DATA_SIZE_BITS",
                 "ENTROPY_SAFETY_FACTOR", "JENT_SHA3_256_SIZE_DIGEST_BITS"]


def load_header(*paths):
    """Take the window sizes and JENT_MAX_OSR from @paths."""
    global APT_WINDOW_SIZE, LAG_WINDOW_SIZE, LAG_HISTORY_SIZE, MAX_OSR
    global RCT_MEM_BLOCK_BITS

    macros = header_macros(HEADER_MACROS, *paths)
    APT_WINDOW_SIZE = macros["JENT_APT_WINDOW_SIZE"]
    LAG_WINDOW_SIZE = macros["JENT_LAG_WINDOW_SIZE"]
    LAG_HISTORY_SIZE = macros["JENT_LAG_HISTORY_SIZE"]
    MAX_OSR = macros["JENT_MAX_OSR"]
    RCT_MEM_BLOCK_BITS = (macros["DATA_SIZE_BITS"]
                          + macros["ENTROPY_SAFETY_FACTOR"])


def rct_mem_observations(osr):
    """The observations one window of the RCT with memory makes."""
    return (RCT_MEM_BLOCK_BITS * osr + RCT_MEM_TAU - 1) // RCT_MEM_TAU


# The case of jent_health_init()'s switch each factor comes from, by the
# label that names it.
RCT_INIT_CASES = {"common": "jent_health_init_type_common",
                  "ntg1": "jent_health_init_type_ntg1"}


def source_rct_factors(path):
    """The safety factor of every call of jent_rct_init() in @path.

    Returned as {"common": factor, "ntg1": factor}, the cases of
    jent_health_init()'s switch they stand in. Every call in the file has to
    be one of exactly these two, one in each, and pass ec and a decimal
    literal: a call anywhere else, or a factor that is not a plain number,
    raises CannotEvaluate rather than leave a cutoff it sets unchecked."""
    text = read_c_source(path)
    head = re.search(r"\bint\s+jent_health_init\s*\([^)]*\)\s*\{", text)
    body = head and braced(text, head.end() - 1)
    if body is None:
        raise CannotEvaluate("no jent_health_init() in %s" % path)
    start = head.end()

    calls = [m.start() for m in re.finditer(r"\bjent_rct_init\s*\(", text)
             if not re.search(r"\bvoid\s+$", text[:m.start()])]
    factors = {}
    for case in re.finditer(r"((?:\b(?:case\s+\w+|default)\s*:\s*)+)"
                            r"(.*?)\bbreak\s*;", body, re.S):
        labels = re.findall(r"case\s+(\w+)", case.group(1))
        names = [n for n, label in RCT_INIT_CASES.items() if label in labels]
        inside = [pos for pos in calls
                  if start + case.start(2) <= pos < start + case.end(2)]
        if not inside:
            continue
        if len(names) != 1 or len(inside) != 1:
            raise CannotEvaluate("jent_rct_init() called %d times in the "
                                 "case of %s" % (len(inside),
                                                 ", ".join(labels)
                                                 or "default"))
        call = re.match(r"jent_rct_init\s*\(\s*ec\s*,\s*(\d+)"
                        + C_INT_SUFFIX + r"\s*\)\s*;", text[inside[0]:])
        if not call or (len(call.group(1)) > 1 and call.group(1)[0] == "0"):
            raise CannotEvaluate("jent_rct_init() gets no plain decimal "
                                 "factor for %s: %s"
                                 % (names[0], text[inside[0]:].split(";")[0]))
        factors[names[0]] = int(call.group(1))
        calls.remove(inside[0])

    if calls:
        raise CannotEvaluate("jent_rct_init() called outside the common and "
                             "NTG.1 cases of jent_health_init()")
    missing = set(RCT_INIT_CASES) - set(factors)
    if missing:
        raise CannotEvaluate("no jent_rct_init() in the %s case"
                             % ", ".join(sorted(missing)))
    return factors


def source_rct_init(path):
    """The parameter list and body of jent_rct_init() in @path, or None."""
    text = read_c_source(path)
    head = re.search(r"static void jent_rct_init\(([^)]*)\)\s*\{", text)
    body = head and braced(text, head.end() - 1)
    return (head.group(1), body) if body is not None else None


def source_rct_program(path, header):
    """jent_rct_init() of @path, parsed for c_run().

    Returned as (statements, parameters, members): its parameters other than
    the collector, name -> C type, and the integer members of struct rand_data
    in @header it may read and set. Every statement of the body is taken, not
    only its "if (safety)" block: a later division of a cutoff, or one outside
    that block, changes the cutoff as much as the one inside it. None if the
    function is not there."""
    found = source_rct_init(path)
    if not found:
        return None
    params = {}
    for param in found[0].split(","):
        words = param.replace("*", " * ").split()
        if words == ["struct", "rand_data", "*", "ec"]:
            continue
        if len(words) < 2 or not re.fullmatch(r"[A-Za-z_]\w*", words[-1]):
            raise CannotEvaluate("parameter %s" % param.strip())
        params[words[-1]] = c_type(words[:-1])
    calls = [name for name, _ in RCT_MACROS]
    return (c_parse(c_tokens(found[1]), calls), params,
            struct_int_members(header, "rand_data"))


def rct_init_cutoffs(program, macros, safety):
    """The two cutoffs @program sets for every rate, called with @safety.

    Returned as {member: [value per osr]}, a member the function never sets
    as None. Run once with a 32 and once with a 64-bit long; a result that
    differs between the two raises CannotEvaluate."""
    tree, params, members = program
    if set(params) != {"safety"}:
        raise CannotEvaluate("jent_rct_init(ec, %s) is not "
                             "jent_rct_init(ec, safety)"
                             % ", ".join(sorted(params)))
    calls = {name: macros[name] for name, _ in RCT_MACROS}
    got = {"rct_cutoff": [], "rct_cutoff_permanent": []}
    for osr in range(1, MAX_OSR + 1):
        runs = []
        for long_bits in (32, 64):
            variables = {"safety": [params["safety"], safety]}
            fields = {"ec->" + name: [ctype, None]
                      for name, ctype in members.items()}
            if "ec->osr" in fields:
                fields["ec->osr"][1] = osr
            c_run(tree, variables, fields, calls, long_bits)
            runs.append([fields.get("ec->" + name, [None, None])[1]
                         for name in got])
        if runs[0] != runs[1]:
            raise CannotEvaluate("the result depends on the width of long")
        for name, val in zip(got, runs[0]):
            got[name].append(val)
    return {name: None if None in vals else vals
            for name, vals in got.items()}


# How far above the n observations of a window the formula may be capped.
# See rct_mem_table(): the intermittent cutoffs are capped one lower than the
# permanent ones, so that they stay reachable within their window.
RCT_MEM_CAP_INTERMITTENT = 0
RCT_MEM_CAP_PERMANENT = 1


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
    n + @cap_offset.

    The cap is not cosmetic: with p = 2^(1 - margin/osr) >= 1 the mean alone
    is at or above n, so the formula asks for a cutoff no window can ever
    count up to. Which cap is applied is a choice, and the two differ:

      RCT_MEM_CAP_PERMANENT = 1 - n + 1, one past the largest count a window
        can reach, so no window can raise a permanent failure: even one
        continuing from jent_rct_mem_duplicate()'s priming passes the
        intermittent cutoff first, and its recovery loop starts a fresh count.

      RCT_MEM_CAP_INTERMITTENT = 0 - n, a window whose every observation is
        stuck. This is one below what the formula says, deliberately: capping
        at n + 1 like the permanent cutoff would leave the intermittent test
        and the recovery loop behind it unable to fire at all within their
        window. The cost is a bound that is slightly conservative wherever
        the cap binds; the gain is that a dead noise source is still caught.

    p is twice the 2^(-H) of the heuristic entropy H = margin/osr, and the
    variance is held at its maximum once p passes 1/2.
    """
    table = []

    for osr in range(1, MAX_OSR + 1):
        n = rct_mem_observations(osr)
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
    # jent_lag_insert() fires at count >= C: P(X >= qbinom + 1) <= alpha.
    return [1 + qbinom(alpha, n, power(2, mpf(-1) / osr))
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
     lambda: rct_mem_table(1, RCT_MEM_SIGMA, RCT_MEM_CAP_INTERMITTENT)),
    ("jent_rct_mem_cutoff_permanent_lookup", "unsigned short",
     lambda: rct_mem_table(1, RCT_MEM_SIGMA_PERMANENT,
                           RCT_MEM_CAP_PERMANENT)),
    ("jent_rct_mem_cutoff_lookup_ntg1", "unsigned short",
     lambda: rct_mem_table(NTG1_MARGIN, RCT_MEM_SIGMA,
                           RCT_MEM_CAP_INTERMITTENT)),
    ("jent_rct_mem_cutoff_permanent_lookup_ntg1", "unsigned short",
     lambda: rct_mem_table(NTG1_MARGIN, RCT_MEM_SIGMA_PERMANENT,
                           RCT_MEM_CAP_PERMANENT)),
]

# The RCT has no table: src/jitterentropy-health.h states the cutoff as a
# macro multiplying the oversampling rate, which is what these check.
RCT_MACROS = [
    ("JENT_HEALTH_RCT_INTERMITTENT_CUTOFF", mpf(2) ** -30),
    ("JENT_HEALTH_RCT_PERMANENT_CUTOFF", mpf(2) ** -60),
]

# The member of struct rand_data jent_rct_init() sets from each of them.
RCT_FIELDS = {"JENT_HEALTH_RCT_INTERMITTENT_CUTOFF": "rct_cutoff",
             "JENT_HEALTH_RCT_PERMANENT_CUTOFF": "rct_cutoff_permanent"}


def format_table(name, ctype, values):
    width = max(len("%d" % v) for v in values)
    per_line = max(1, (72 - 8) // (width + 2))
    lines = []

    for i in range(0, len(values), per_line):
        chunk = values[i:i + per_line]
        row = ", ".join("%*d" % (width, v) for v in chunk)
        lines.append(("\t{ " if i == 0 else "\t  ") + row)

    return ("static const %s %s[] =\n" % (ctype, name)
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
    text = read_c_source(path)
    found = {}

    for match in re.finditer(
            r"static const unsigned (?:int|short) (\w+)\[\d*\]\s*=\s*\{([^}]*)\}",
            text):
        found[match.group(1)] = [int(v) for v in
                                 re.findall(r"\d+", match.group(2))]

    return found


def source_rct_macros(path):
    """The RCT cutoff macros as src/jitterentropy-health.h states them."""
    text = read_c_source(path)

    return {m.group(1): int(m.group(2)) for m in re.finditer(
        r"#define (JENT_HEALTH_RCT_\w+)\(x\)\s*\(\(x\) \* (\d+)"
        + C_INT_SUFFIX + r"\)", text)}


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

    # The header sits beside the source the tables are checked against, or
    # for printing beside this script's source tree.
    srcdir = (os.path.dirname(os.path.abspath(args.check)) if args.check
              else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                os.pardir, os.pardir, "src"))
    load_header(os.path.join(srcdir, "jitterentropy-internal.h"),
                os.path.join(srcdir, os.pardir, "jitterentropy.h"))

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

        # jent_rct_init() divides the macros above by the factor
        # jent_health_init() passes it: the NTG.1 margin for NTG.1, none in
        # the common case. Each factor is read from its call, not assumed.
        try:
            factors = source_rct_factors(args.check)
            margin = factors["ntg1"]
            failed += report("RCT NTG.1 margin", margin == NTG1_MARGIN,
                             "jent_rct_init(ec, %d)" % NTG1_MARGIN
                             if margin == NTG1_MARGIN
                             else "MISMATCH: jent_rct_init() gets %s for "
                                  "NTG.1, expected %d" % (margin, NTG1_MARGIN))
        except CannotEvaluate as err:
            factors = None
            failed += report("RCT NTG.1 margin", False,
                             "CANNOT EVALUATE jent_health_init(): %s" % err)

        # And jent_rct_init(), run as the source writes it on the header's
        # macro values with the factor each case passes, has to give
        # C = ceil(-log2(alpha) * osr / margin) for every rate: the macros
        # themselves in the common case, the NTG.1 table for NTG.1. A
        # truncating division would halve the margin the cutoff keeps at osr
        # 1 (30 / 8 = 3, not 4), and a factor in the common case would divide
        # the cutoffs SP800-90B asks for. The whole function runs, in C's
        # integer semantics, every statement in turn and each result stored
        # to its field, so a second division of either field is caught
        # wherever it stands.
        division_ok = False
        try:
            program = source_rct_program(
                args.check, os.path.join(srcdir, "jitterentropy-internal.h"))
            if factors is None:
                detail = "CANNOT EVALUATE: the factors of jent_rct_init() " \
                         "are unreadable"
            elif not program:
                detail = "MISMATCH: no jent_rct_init() in %s" % args.check
            elif any(macros.get(name) is None for name, _ in RCT_MACROS):
                detail = "MISMATCH: the RCT macros are not in the header"
            else:
                division_ok = True
                detail = "%d values" % (4 * MAX_OSR)
                for case, margin in (("ntg1", NTG1_MARGIN), ("common", 1)):
                    safety = factors[case]
                    got = rct_init_cutoffs(program, macros, safety)
                    for name, alpha in RCT_MACROS:
                        field = RCT_FIELDS[name]
                        want = rct_table(margin, alpha)
                        if got[field] is None:
                            division_ok = False
                            detail = "MISMATCH: jent_rct_init(ec, %d) " \
                                     "does not set ec->%s" % (safety, field)
                        elif got[field] != want:
                            division_ok = False
                            detail = "MISMATCH: jent_rct_init(ec, %d) " \
                                     "(%s) sets ec->%s to %s,\n  computed %s" \
                                     % (safety, case, field, got[field], want)
                        if not division_ok:
                            break
                    if not division_ok:
                        break
        except CannotEvaluate as err:
            division_ok = False
            detail = "CANNOT EVALUATE jent_rct_init(): %s" % err
        failed += report("RCT division", division_ok, detail)

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
