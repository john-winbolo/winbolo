#!/usr/bin/env python3
"""
lua54to51.py — minimal Lua 5.4 -> 5.1/LuaJIT source down-transpiler.

Purpose: let the WinBolo brains (written for PUC-Lua 5.4) load on LuaJIT, which
is a Lua 5.1 VM. The brains use two pieces of >5.1 syntax LuaJIT cannot parse:

  * floor division `//`            (Lua 5.3+)  -> __idiv(a, b) = math.floor(a/b)
  * bitwise operators `<< >> & ~ |` (Lua 5.3+) -> LuaJIT bit.* library calls

Bitwise unary `~x` and 5.4 `<const>/<close>` attributes are NOT used by the
brains; if they appear the tool ERRORS rather than guessing.

Correctness:
  * A real lexer skips strings, short comments and long comments, so `//` in a
    URL ("http://...") or a string is never touched.
  * Operands are extracted as arithmetic spans (primaries joined by + - * / % ^
    .. and unary prefixes), and the operators are rewritten in strict Lua
    precedence order, highest first: //  then << >>  then &  then ~(xor) then |.
    Because each pass turns its operator into a *call* (a primary), the next
    (lower-precedence) pass sees a well-formed primary operand. This reproduces
    Lua 5.3/5.4 operator precedence.
  * Anything the extractor is not certain about raises LexError — it never
    emits a silently-wrong rewrite.

NOTE: numeric *results* still differ from PUC-5.4 in edge cases (LuaJIT has no
integer subtype; bit.* coerces through int32). See docs/luajit-port.md for the
determinism implications against recorded replays / baselines.

Usage:
    python lua54to51.py --report  FILE...     # list rewrites, no writes
    python lua54to51.py --in-place FILE...     # rewrite in place
    python lua54to51.py --out DIR  FILE...     # write rewritten copies to DIR
"""
import sys, os, argparse

KEYWORDS = {
    "and","break","do","else","elseif","end","false","for","function","goto",
    "if","in","local","nil","not","or","repeat","return","then","true","until","while",
}
MULTI = ["...","..","::","//","<<",">>","==","~=","<=",">="]
SINGLE = set("+-*/%^#&~|<>=(){}[];:,.")

# Arithmetic / concat operators absorbed into a bitwise operand span (all bind
# tighter than every bitwise operator in Lua 5.3+).
ARITH_BINOPS = {"+","-","*","/","//","%","^",".."}
UNARY_PREFIX = {"-","#","not"}   # bitwise unary ~ intentionally excluded (unused)

# Highest precedence first. Each entry: (operator, emitter(left,right)).
BITWISE_PASSES = [
    (">>", lambda l, r: f"bit.rshift({l}, {r})"),
    ("<<", lambda l, r: f"bit.lshift({l}, {r})"),
    ("&",  lambda l, r: f"bit.band({l}, {r})"),
    ("~",  lambda l, r: f"bit.bxor({l}, {r})"),   # binary xor
    ("|",  lambda l, r: f"bit.bor({l}, {r})"),
]

class Tok:
    __slots__ = ("kind","val","pos","line")
    def __init__(self, kind, val, pos, line):
        self.kind, self.val, self.pos, self.line = kind, val, pos, line
    def __repr__(self):
        return f"{self.kind}:{self.val!r}@{self.line}"

class LexError(Exception):
    pass

def _long_bracket_len(s, i):
    if s[i] != "[":
        return -1
    j = i + 1; eq = 0
    while j < len(s) and s[j] == "=":
        eq += 1; j += 1
    return eq if (j < len(s) and s[j] == "[") else -1

def _read_long(s, i, level, line):
    close = "]" + "=" * level + "]"
    end = s.find(close, i + 2 + level)
    if end == -1:
        raise LexError(f"unterminated long bracket (level {level}) at line {line}")
    seg = s[i:end + len(close)]
    return seg, end + len(close), line + seg.count("\n")

def lex(s):
    toks = []; i, n, line = 0, len(s), 1
    while i < n:
        c = s[i]
        if c == "\n":
            line += 1; i += 1; continue
        if c in " \t\r\f\v":
            i += 1; continue
        if c == "-" and i + 1 < n and s[i+1] == "-":           # comment
            j = i + 2
            lvl = _long_bracket_len(s, j) if j < n else -1
            if lvl >= 0:
                _, j, line = _read_long(s, j, lvl, line); i = j; continue
            while i < n and s[i] != "\n":
                i += 1
            continue
        lvl = _long_bracket_len(s, i)                          # long string
        if lvl >= 0:
            seg, j, nl = _read_long(s, i, lvl, line)
            toks.append(Tok("string", seg, i, line)); i, line = j, nl; continue
        if c in "\"'":                                          # short string
            q = c; j = i + 1
            while j < n:
                if s[j] == "\\": j += 2; continue
                if s[j] == q: j += 1; break
                if s[j] == "\n": raise LexError(f"unterminated string at line {line}")
                j += 1
            else:
                raise LexError(f"unterminated string at line {line}")
            toks.append(Tok("string", s[i:j], i, line)); i = j; continue
        if c.isdigit() or (c == "." and i+1 < n and s[i+1].isdigit()):   # number
            j = i
            if c == "0" and i+1 < n and s[i+1] in "xX":
                j = i + 2
                while j < n and (s[j] in "0123456789abcdefABCDEF.pP" or (s[j] in "+-" and s[j-1] in "pP")):
                    j += 1
            else:
                while j < n and (s[j].isdigit() or s[j] in ".eE" or (s[j] in "+-" and s[j-1] in "eE")):
                    j += 1
            toks.append(Tok("number", s[i:j], i, line)); i = j; continue
        if c.isalpha() or c == "_":                            # name / keyword
            j = i
            while j < n and (s[j].isalnum() or s[j] == "_"):
                j += 1
            w = s[i:j]
            toks.append(Tok("kw" if w in KEYWORDS else "name", w, i, line)); i = j; continue
        for m in MULTI:                                        # operators
            if s.startswith(m, i):
                toks.append(Tok("op", m, i, line)); i += len(m); break
        else:
            if c in SINGLE:
                toks.append(Tok("op", c, i, line)); i += 1
            else:
                raise LexError(f"unexpected char {c!r} at line {line}")
    return toks

OPEN, CLOSE = {"(","[","{"}, {")","]","}"}

def _is_value_end(t):
    return (t.kind in ("name","number","string") or
            (t.kind == "op" and t.val in CLOSE) or
            (t.kind == "kw" and t.val in ("true","false","nil")) or
            (t.kind == "op" and t.val == "..."))

def _is_value_start(t):
    return (t.kind in ("name","number","string") or
            (t.kind == "op" and t.val in OPEN) or
            (t.kind == "kw" and t.val in ("true","false","nil","function")) or
            (t.kind == "op" and t.val == "..."))

def _skip_group_fwd(toks, k):
    open_v = toks[k].val; close_v = {"(":")","[":"]","{":"}"}[open_v]; depth = 0; n = len(toks)
    while k < n:
        if toks[k].kind == "op" and toks[k].val == open_v: depth += 1
        elif toks[k].kind == "op" and toks[k].val == close_v:
            depth -= 1
            if depth == 0: return k + 1
        k += 1
    raise LexError("unbalanced grouping")

def _primary_fwd(toks, k):
    """Parse one suffixed primary starting at k; return end index (exclusive)."""
    n = len(toks)
    while k < n and ((toks[k].kind == "op" and toks[k].val in ("-","#")) or
                     (toks[k].kind == "kw" and toks[k].val == "not")):
        k += 1
    if k >= n or not _is_value_start(toks[k]):
        raise LexError(f"expected operand near {toks[k] if k<n else 'EOF'}")
    if toks[k].kind == "kw" and toks[k].val == "function":
        raise LexError("function literal operand not supported")
    if toks[k].kind == "op" and toks[k].val in OPEN:
        k = _skip_group_fwd(toks, k)
    else:
        k += 1
    while k < n:                                              # suffixes
        t = toks[k]
        if t.kind == "op" and t.val == ".": k += 2
        elif t.kind == "op" and t.val == ":":
            k += 2
            if k < n and toks[k].kind == "op" and toks[k].val == "(":
                k = _skip_group_fwd(toks, k)
        elif t.kind == "op" and t.val in ("[","("): k = _skip_group_fwd(toks, k)
        elif t.kind == "string": k += 1
        else: break
    return k

def _operand_fwd(toks, k):
    """Arithmetic span: primary (arith-op primary)*  -> end index (exclusive)."""
    k = _primary_fwd(toks, k); n = len(toks)
    while k < n and toks[k].kind == "op" and toks[k].val in ARITH_BINOPS:
        k = _primary_fwd(toks, k + 1)
    return k

def _primary_back(toks, k):
    """Parse one suffixed primary ending at k (inclusive); return start index."""
    if k < 0 or not _is_value_end(toks[k]):
        raise LexError(f"expected operand before, got {toks[k] if k>=0 else 'BOF'}")
    while k >= 0:
        t = toks[k]
        if t.kind == "op" and t.val in CLOSE:                 # balance back
            depth = 0
            while k >= 0:
                tv = toks[k]
                if tv.kind == "op" and tv.val in CLOSE: depth += 1
                elif tv.kind == "op" and tv.val in OPEN:
                    depth -= 1
                    if depth == 0: break
                k -= 1
            if k < 0: raise LexError("unbalanced grouping (backward)")
            # an open '(' or '[' or '{' consumed; could be call/index suffix
            k -= 1
            if k >= 0 and (toks[k].kind in ("name",) or (toks[k].kind=="op" and toks[k].val in CLOSE)):
                continue                                       # it's a suffix; keep going
            return k + 1
        if t.kind in ("name","number","string") or (t.kind=="kw" and t.val in ("true","false","nil")) or (t.kind=="op" and t.val=="..."):
            if k-1 >= 0 and toks[k-1].kind == "op" and toks[k-1].val == ".":
                k -= 2; continue                              # a.b chain
            return k
        raise LexError(f"unsupported operand token {t}")
    return 0

def _operand_back(toks, k):
    """Arithmetic span ending at k -> start index."""
    start = _primary_back(toks, k)
    while start - 1 >= 0 and toks[start-1].kind == "op" and toks[start-1].val in ARITH_BINOPS:
        start = _primary_back(toks, start - 2)
    # absorb a leading unary prefix
    if start-1 >= 0 and toks[start-1].kind == "op" and toks[start-1].val in ("-","#"):
        p = toks[start-2] if start-2 >= 0 else None
        if p is None or p.kind == "kw" or (p.kind == "op" and p.val not in CLOSE):
            start -= 1
    return start

def _rewrite_unary_tilde(src, toks):
    """Rewrite unary bitwise NOT  ~x  ->  bit.bnot(x). A `~` is unary when the
    preceding significant token is not a value-end (operator/open/keyword/BOF)."""
    report = []
    while True:
        idx = None
        for i, t in enumerate(toks):
            if t.kind == "op" and t.val == "~":
                prev = toks[i-1] if i > 0 else None
                if prev is None or not _is_value_end(prev):
                    idx = i; break
        if idx is None:
            break
        end = _primary_fwd(toks, idx + 1)
        ostart = toks[idx].pos + 1
        oend = toks[end-1].pos + len(toks[end-1].val)
        operand = src[ostart:oend].strip()
        new = f"bit.bnot({operand})"
        report.append((toks[idx].line, f"~{operand}", new))
        src = src[:toks[idx].pos] + new + src[oend:]
        toks = lex(src)
    return src, toks, bool(report), report

def _rewrite_op(src, toks, op, emit):
    """Rewrite all binary `op` occurrences (left-to-right) and re-lex."""
    changed = False; report = []
    while True:
        idx = next((i for i,t in enumerate(toks) if t.kind=="op" and t.val==op), None)
        if idx is None:
            break
        ls = _operand_back(toks, idx-1)
        re = _operand_fwd(toks, idx+1)
        lstart, lend = toks[ls].pos, toks[idx].pos
        rstart = toks[idx].pos + len(op)
        rend = toks[re-1].pos + len(toks[re-1].val)
        left = src[lstart:lend].strip()
        right = src[rstart:rend].strip()
        new = emit(left, right)
        report.append((toks[idx].line, left, op, right, new))
        src = src[:lstart] + new + src[rend:]
        toks = lex(src)                                       # re-lex after each edit
        changed = True
    return src, toks, changed, report

def rewrite_source(src, path):
    toks = lex(src)
    report = []
    # 0) unary bitwise NOT (binds tightest) before anything that captures operands.
    src, toks, need_bit, rep = _rewrite_unary_tilde(src, toks)
    report += rep
    # 1) floor division (multiplicative precedence).
    src, toks, ch, rep = _rewrite_op(src, toks, "//", lambda l, r: f"__idiv({l}, {r})")
    report += [(ln, f"{l} // {r}", new) for (ln,l,o,r,new) in rep]
    need_idiv = ch
    # 2) bitwise binary, highest precedence first.
    for op, emit in BITWISE_PASSES:
        src, toks, ch, rep = _rewrite_op(src, toks, op, emit)
        if ch:
            need_bit = True
            report += [(ln, f"{l} {o} {r}", new) for (ln,l,o,r,new) in rep]
    # prepend the helpers actually used
    header = ""
    if need_idiv:
        header += "local function __idiv(a,b) return math.floor(a/b) end\n"
    if need_bit:
        header += "local bit = require('bitcompat')\n"
    return (header + src if header else src), report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--in-place", action="store_true")
    ap.add_argument("--out", default=None)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    total = 0
    for f in a.files:
        with open(f, "r", encoding="utf-8") as fh:
            src = fh.read()
        try:
            new, rep = rewrite_source(src, f)
        except LexError as e:
            print(f"ERROR {f}: {e}", file=sys.stderr); return 2
        if rep:
            total += len(rep)
            for ln, was, became in rep:
                print(f"  {os.path.basename(f)}:{ln}: {was}  ->  {became}")
        if a.report:
            continue
        if a.in_place:
            with open(f, "w", encoding="utf-8") as fh: fh.write(new)
        elif a.out:
            os.makedirs(a.out, exist_ok=True)
            with open(os.path.join(a.out, os.path.basename(f)), "w", encoding="utf-8") as fh:
                fh.write(new)
    print(f"[lua54to51] rewrote {total} operator(s) across {len(a.files)} file(s)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
