#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
#
# Copyright (C) 2026, Markus Theil <theil.markus@gmail.com>
#
"""Read the C sources the health test cutoff check compares against.

cutoffs.py recomputes the cutoffs; this is how it gets at what the source
carries: the integer macros of a header, the members of a struct, and the
statements of a function, which c_parse() turns into a tree and c_run()
executes in C's integer semantics. Whatever it does not model raises
CannotEvaluate rather than being guessed at: a construct the check cannot
read may change a cutoff it would then miss.
"""

import re
import sys


def read_c_source(path):
    """@path with its comments blanked out, string literals left alone.

    A comment is a space to the C preprocessor, and one here would otherwise
    put its words and numbers into what the patterns below take out."""
    text = open(path).read()
    return re.sub(r"(\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*')"
                  r"|/\*.*?\*/|//[^\n]*",
                  lambda m: m.group(1) or " ", text, flags=re.S)


def braced(text, start):
    """The text between the brace at @start and the one that closes it."""
    depth = 0

    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                return text[start + 1:i]

    return None


# An integer literal as C writes it, with any of its suffixes.
C_INT_SUFFIX = r"(?:[uU](?:ll|LL|[lL])?|(?:ll|LL|[lL])[uU]?)?"


class CannotEvaluate(ValueError):
    """Source the RCT division check does not know how to judge."""


def header_macros(names, *paths):
    """The integer macros of @paths that @names lists, evaluated.

    Only what the definitions of the jitterentropy headers use is understood: decimal literals with
    an optional integer suffix, +, <<, parentheses and each other's names. Anything
    else is refused rather than guessed at.
    """
    path = " + ".join(paths)
    text = "\n".join(open(p).read() for p in paths)
    raw = {}

    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+([^\n]*?)"
                         r"[ \t]*(?:/\*.*)?$", text, re.M):
        if m.group(1) in names:
            if m.group(1) in raw:
                sys.exit("%s: %s is defined more than once"
                         % (path, m.group(1)))
            raw[m.group(1)] = m.group(2)

    values = {}

    def evaluate(name, depth=0):
        if name in values:
            return values[name]
        if name not in raw or depth > len(names):
            sys.exit("%s: cannot find or resolve %s" % (path, name))
        expr = re.sub(r"\b(\d+)" + C_INT_SUFFIX + r"\b", r"\1", raw[name])
        expr = re.sub(r"\b([A-Z_][A-Z0-9_]*)\b",
                      lambda m: str(evaluate(m.group(1), depth + 1)), expr)
        if not re.fullmatch(r"[\d\s()+<]*", expr):
            sys.exit("%s: cannot evaluate %s = %s" % (path, name, raw[name]))
        values[name] = int(eval(expr, {"__builtins__": {}}))
        return values[name]

    return {name: evaluate(name) for name in names}


def struct_int_members(path, struct):
    """The integer members of struct @struct in @path, name -> C type.

    Members of any other type are left out, so an expression that reads one
    is refused as an unknown name."""
    text = read_c_source(path)
    head = re.search(r"struct %s\s*\{" % re.escape(struct), text)
    body = head and braced(text, head.end() - 1)
    fields = {}
    for m in re.finditer(r"(?:^|[;{}])\s*([a-z][a-z ]*?)\s+(\w+)\s*;",
                         body or "", re.M):
        try:
            fields[m.group(2)] = c_type(m.group(1).split())
        except CannotEvaluate:
            pass
    return fields


# The integer types an expression may carry, as (width, signed, rank). The
# width of long is not the same on every platform the library builds on -
# 64 bits for LP64, 32 for Windows' LLP64 - so it is a parameter: every
# evaluation runs under both, and a result that depends on it is refused.
# int is taken to be 32 bits and short 16, as on every one of them.
def c_types(long_bits):
    return {"unsigned char": (8, False, 1), "signed char": (8, True, 1),
            "short": (16, True, 2), "unsigned short": (16, False, 2),
            "int": (32, True, 3), "unsigned int": (32, False, 3),
            "long": (long_bits, True, 4),
            "unsigned long": (long_bits, False, 4),
            "long long": (64, True, 5), "unsigned long long": (64, False, 5)}


def c_type(words):
    """The canonical name of the integer type the specifiers @words spell."""
    words = [w for w in words if w != "const"]
    sign = [w for w in words if w in ("signed", "unsigned")]
    rest = sorted(w for w in words if w not in ("signed", "unsigned"))
    if len(sign) > 1 or any(w not in ("char", "short", "int", "long")
                            for w in rest):
        raise CannotEvaluate("type %s" % " ".join(words))
    base = {(): "int", ("int",): "int", ("char",): "char",
            ("short",): "short", ("int", "short"): "short",
            ("long",): "long", ("int", "long"): "long",
            ("long", "long"): "long long",
            ("int", "long", "long"): "long long"}.get(tuple(rest))
    if not base or not words or (base == "char" and not sign):
        raise CannotEvaluate("type %s" % " ".join(words))
    if sign == ["unsigned"]:
        return "unsigned " + base
    return "signed char" if base == "char" else base


# The tokens of the statements jent_rct_init() is made of. ec->name is one
# token, so ec->osr and a local osr stay apart.
C_TOKEN = re.compile(r"\s+|(0[xX][0-9a-fA-F]+|\d+)(" + C_INT_SUFFIX
                     + r")(?![\w.])|(\d[\w.]*)|(ec\s*->\s*[A-Za-z_]\w*"
                     r"|[A-Za-z_]\w*)|([-+*/]=|[<>=!]=|->|[-+*/()<>=;{},])"
                     r"|(.)", re.S)


def c_tokens(text):
    tokens = []
    for m in C_TOKEN.finditer(text):
        if m.group(1):
            lit = m.group(1)
            try:
                val = int(lit, 16 if lit[:2] in ("0x", "0X")
                          else 8 if len(lit) > 1 and lit[0] == "0" else 10)
            except ValueError:
                raise CannotEvaluate("bad literal %s" % lit)
            tokens.append(("lit", val, m.group(2).lower(), lit[0] != "0"
                           or lit == "0"))
        elif m.group(3):
            raise CannotEvaluate("not an integer literal: %s" % m.group(3))
        elif m.group(4):
            tokens.append(("name", re.sub(r"\s+", "", m.group(4))))
        elif m.group(5):
            tokens.append(m.group(5))
        elif m.group(6):
            raise CannotEvaluate("unexpected %r" % m.group(6))
    return tokens


# The macros and statements the body may use beyond plain C, and what the
# check makes of them: the table lookups are the header's macros, and a
# compile-time assertion does nothing at run time.
C_IGNORED_CALLS = ("JENT_BUILD_BUG_ON",)
C_TYPE_WORDS = ("unsigned", "signed", "short", "int", "long", "char", "const")
C_ASSIGN_OPS = ("=", "+=", "-=", "*=", "/=")


def c_parse(tokens, calls):
    """The statements of @tokens as a tree, for c_run().

    Understood: blocks, if/else, declarations of integer locals, assignments
    (plain and compound with + - * /) to ec->name and to locals, the macros in
    @calls applied to one argument, and C_IGNORED_CALLS. Expressions are
    integer literals, names, casts to an integer type, unary + and -, the
    binary + - * / and the relations. Anything else raises CannotEvaluate:
    a statement the check cannot model may change a cutoff it would miss."""
    pos = [0]

    def peek(ahead=0):
        i = pos[0] + ahead
        return tokens[i] if i < len(tokens) else None

    def take(want=None):
        tok = peek()
        if tok is None:
            raise CannotEvaluate("the source ends early")
        if want is not None and tok != want:
            raise CannotEvaluate("expected %s, found %s" % (want, tok))
        pos[0] += 1
        return tok

    def is_name(tok, *names):
        return isinstance(tok, tuple) and tok[0] == "name" and \
            (not names or tok[1] in names)

    def type_words(start):
        # the specifiers from @start ahead of the cursor, however many
        words = []
        while is_name(peek(start + len(words)), *C_TYPE_WORDS):
            words.append(peek(start + len(words))[1])
        return words

    def primary():
        tok = peek()
        if tok == "(":
            words = type_words(1)
            if words and peek(1 + len(words)) == ")":
                pos[0] += len(words) + 2
                return ("cast", c_type(words), unary())
            take()
            node = relational()
            take(")")
            return node
        tok = take()
        if isinstance(tok, tuple) and tok[0] == "lit":
            return tok
        if is_name(tok) and peek() == "(":
            if tok[1] not in calls:
                raise CannotEvaluate("call of %s" % tok[1])
            take()
            arg = relational()
            take(")")
            return ("call", tok[1], arg)
        if is_name(tok) and tok[1] not in C_TYPE_WORDS:
            return tok
        raise CannotEvaluate("unexpected %s" % (tok,))

    def unary():
        if peek() in ("+", "-"):
            return ("neg" if take() == "-" else "pos", unary())
        return primary()

    def binary(sub, ops):
        def parse():
            node = sub()
            while peek() in ops:
                node = ("bin", take(), node, sub())
            return node
        return parse

    multiplicative = binary(unary, ("*", "/"))
    additive = binary(multiplicative, ("+", "-"))
    relational = binary(binary(additive, ("<", ">", "<=", ">=")),
                        ("==", "!="))

    def statement():
        tok = peek()
        if tok == ";":
            take()
            return ("block", [])
        if tok == "{":
            take()
            stmts = []
            while peek() != "}":
                stmts.append(statement())
            take()
            return ("block", stmts)
        if is_name(tok, "if"):
            take()
            take("(")
            cond = relational()
            take(")")
            then = statement()
            other = None
            if is_name(peek(), "else"):
                take()
                other = statement()
            return ("if", cond, then, other)
        if is_name(tok, *C_IGNORED_CALLS) and peek(1) == "(":
            take()
            depth = 0
            while True:
                tok = take()
                depth += tok == "("
                depth -= tok == ")"
                if not depth:
                    break
            take(";")
            return ("block", [])
        words = type_words(0)
        if words:
            pos[0] += len(words)
            name = take()
            if not is_name(name) or "->" in name[1]:
                raise CannotEvaluate("declaration of %s" % (name,))
            init = None
            if peek() == "=":
                take()
                init = relational()
            take(";")
            return ("decl", c_type(words), name[1], init)
        if is_name(tok) and peek(1) in C_ASSIGN_OPS:
            take()
            op = take()
            value = relational()
            take(";")
            return ("assign", tok[1], op, value)
        raise CannotEvaluate("a statement that is not modelled, at %s"
                             % " ".join(str(t[1] if isinstance(t, tuple)
                                            else t)
                                        for t in tokens[pos[0]:pos[0] + 6]))

    stmts = []
    while peek() is not None:
        stmts.append(statement())
    return ("block", stmts)


def c_run(tree, variables, fields, calls, long_bits):
    """Run @tree, the statements c_parse() made, in C's integer semantics.

    @variables are the parameters and locals, name -> [C type, value]; @fields
    the members of the collector, "ec->name" -> [C type, value or None until
    set]; @calls the one-argument macros, name -> multiplier. Integer
    promotion, the usual arithmetic conversions, division truncating toward
    zero and wraparound of the unsigned types only are modelled; whatever C
    leaves undefined or to the implementation - signed overflow, division by
    zero, an out of range conversion to a signed type, reading a member
    before it is set - raises CannotEvaluate, as a result that would depend on
    the compiler cannot be checked. Updates @variables and @fields."""
    types = c_types(long_bits)

    def convert(val, ctype):
        bits, signed, _ = types[ctype]
        if not signed:
            return val % (1 << bits)
        if not -(1 << (bits - 1)) <= val < (1 << (bits - 1)):
            raise CannotEvaluate("%d does not fit %s" % (val, ctype))
        return val

    def promote(ctype):
        return "int" if types[ctype][2] < types["int"][2] else ctype

    def common(a, b):
        a, b = promote(a), promote(b)
        (_, sa, ra), (_, sb, rb) = types[a], types[b]
        if a == b:
            return a
        if sa == sb:
            return a if ra > rb else b
        uns, sig = (a, b) if not sa else (b, a)
        if types[uns][2] >= types[sig][2]:
            return uns
        if types[sig][0] > types[uns][0]:
            return sig
        return "unsigned " + sig

    def literal(val, suffix, decimal):
        # the first type of C11 6.4.4.1 that holds the value
        unsigned = "u" in suffix
        longs = suffix.count("l")
        cand = []
        for t in ["int", "long", "long long"][longs:]:
            if not unsigned:
                cand.append(t)
            if unsigned or not decimal:
                cand.append("unsigned " + t)
        for t in cand:
            bits, signed, _ = types[t]
            if val < 1 << (bits - 1 if signed else bits):
                return val, t
        raise CannotEvaluate("literal %d fits no type" % val)

    def arith(op, a, b):
        ctype = common(a[1], b[1])
        x, y = convert(a[0], ctype), convert(b[0], ctype)
        if op in ("<", ">", "<=", ">=", "==", "!="):
            return int({"<": x < y, ">": x > y, "<=": x <= y, ">=": x >= y,
                        "==": x == y, "!=": x != y}[op]), "int"
        if op == "/":
            if not y:
                raise CannotEvaluate("division by zero")
            val = abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)
        else:
            val = x + y if op == "+" else x - y if op == "-" else x * y
        bits, signed, _ = types[ctype]
        if signed and not -(1 << (bits - 1)) <= val < (1 << (bits - 1)):
            raise CannotEvaluate("signed overflow in %s" % ctype)
        return convert(val, ctype), ctype

    def value(node):
        kind = node[0]
        if kind == "lit":
            return literal(node[1], node[2], node[3])
        if kind == "name":
            var = variables.get(node[1]) or fields.get(node[1])
            if var is None:
                raise CannotEvaluate("unknown name %s" % node[1])
            if var[1] is None:
                raise CannotEvaluate("%s is read before it is set"
                                     % node[1])
            return var[1], var[0]
        if kind == "cast":
            val, _ = value(node[2])
            return convert(val, node[1]), node[1]
        if kind in ("neg", "pos"):
            val, ctype = value(node[1])
            ctype = promote(ctype)
            if kind == "pos":
                return val, ctype
            return arith("-", (0, ctype), (val, ctype))
        if kind == "call":
            return arith("*", value(node[2]),
                         literal(calls[node[1]], "", True))
        return arith(node[1], value(node[2]), value(node[3]))

    def run(node):
        kind = node[0]
        if kind == "block":
            for stmt in node[1]:
                run(stmt)
        elif kind == "if":
            if value(node[1])[0]:
                run(node[2])
            elif node[3] is not None:
                run(node[3])
        elif kind == "decl":
            if node[2] in variables:
                raise CannotEvaluate("%s is declared twice" % node[2])
            variables[node[2]] = [node[1], None]
            if node[3] is not None:
                variables[node[2]][1] = convert(value(node[3])[0], node[1])
        else:
            target = variables.get(node[1]) or fields.get(node[1])
            if target is None:
                raise CannotEvaluate("assignment to %s" % node[1])
            val = value(node[3])
            if node[2] != "=":
                val = arith(node[2][0], value(("name", node[1])), val)
            target[1] = convert(val[0], target[0])

    run(tree)
