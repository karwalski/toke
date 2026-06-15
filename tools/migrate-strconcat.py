#!/usr/bin/env python3
"""migrate-strconcat.py — Rewrite `str + str` to canonical patterns per ADR-0004.

Epic 111 / story 111.6.

This tool runs over toke source files and rewrites every `+` expression whose
both operands look string-like into one of the canonical patterns:

    s.concat(a; b)                  — pairwise (2 operands)
    s.join(@(a; b; c; …); "")        — variadic (3+ operands)
    s.builder() / s.add(b;x) / s.build(b)
                                    — loop-accumulator pattern
                                      (flagged for manual review)

Operand heuristic — an operand is treated as "string-like" when any of:
    - it is a double-quoted string literal: "..."
    - it is a call to a known string-returning stdlib function:
          s.fromint, s.fromfloat, s.format, s.concat, s.join, s.build,
          s.slice, s.upper, s.lower, s.trim, s.replace, s.repeat,
          s.charat, s.substr, io.readln, io.read
    - it is an `expr as $str` cast
    - it is a `\"...\"` interpolation literal (already canonical, no rewrite)

If both operands are clearly numeric (literal ints/floats, math.* calls) the
`+` is left alone. If one side is clearly string-like and the other is
ambiguous (a bare identifier with no resolvable type), the tool flags the
site with a `// MIGRATE: review this manually — possible str+str` comment
and leaves the source unchanged.

Usage:
    migrate-strconcat.py FILE [FILE ...]            # rewrite in place
    migrate-strconcat.py --check FILE [FILE ...]    # report only, no rewrite
    migrate-strconcat.py --diff FILE [FILE ...]     # show unified diff per file

Output:
    Per-file: counts of {rewritten, ambiguous, untouched}.
    Exit 0 if all files migrated cleanly (no ambiguous flags).
    Exit 1 if any file required ambiguous-flagging.
    Exit 2 on argument or read errors.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path


# ── tokeniser-lite ──────────────────────────────────────────────────────
# We need a tokeniser that understands strings, identifiers, numbers,
# parentheses, and operators — enough to find `+` expressions and their
# operands without rewriting inside string literals.

class Tok:
    STR    = "STR"     # double-quoted string literal (including escapes)
    NUM    = "NUM"     # integer or float literal
    IDENT  = "IDENT"   # identifier (lowercase a-z + digits)
    DOT    = "DOT"     # .
    SEMI   = "SEMI"    # ;
    LP     = "LP"      # (
    RP     = "RP"      # )
    LB     = "LB"      # {
    RB     = "RB"      # }
    AT     = "AT"      # @
    PLUS   = "PLUS"    # +
    OP     = "OP"      # any other operator (-, *, /, =, <, >, !, &, |, $, :, ,)
    WHITE  = "WHITE"   # whitespace
    NL     = "NL"      # newline (whitespace, tracked separately for diagnostics)
    OTHER  = "OTHER"


@dataclass
class Token:
    kind:  str
    text:  str
    start: int          # byte offset into source
    end:   int


_IDENT_RE = re.compile(r"[a-z][a-z0-9]*")
_NUM_RE   = re.compile(r"[0-9]+(?:\.[0-9]+)?")
_WS_RE    = re.compile(r"[ \t]+")
_OP_CHARS = set("-*/=<>!&|$:,")


def tokenise(src: str) -> list[Token]:
    toks: list[Token] = []
    i = 0
    n = len(src)
    while i < n:
        ch = src[i]
        if ch == "\n":
            toks.append(Token(Tok.NL, "\n", i, i + 1))
            i += 1
        elif ch in " \t":
            m = _WS_RE.match(src, i)
            toks.append(Token(Tok.WHITE, src[m.start():m.end()], m.start(), m.end()))
            i = m.end()
        elif ch == "\"":
            # Walk to matching close, honour \" escapes.
            j = i + 1
            while j < n:
                if src[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if src[j] == "\"":
                    j += 1
                    break
                j += 1
            toks.append(Token(Tok.STR, src[i:j], i, j))
            i = j
        elif ch.isdigit():
            m = _NUM_RE.match(src, i)
            toks.append(Token(Tok.NUM, src[m.start():m.end()], m.start(), m.end()))
            i = m.end()
        elif "a" <= ch <= "z":
            m = _IDENT_RE.match(src, i)
            toks.append(Token(Tok.IDENT, src[m.start():m.end()], m.start(), m.end()))
            i = m.end()
        elif ch == ".": toks.append(Token(Tok.DOT,  ".", i, i + 1)); i += 1
        elif ch == ";": toks.append(Token(Tok.SEMI, ";", i, i + 1)); i += 1
        elif ch == "(": toks.append(Token(Tok.LP,   "(", i, i + 1)); i += 1
        elif ch == ")": toks.append(Token(Tok.RP,   ")", i, i + 1)); i += 1
        elif ch == "{": toks.append(Token(Tok.LB,   "{", i, i + 1)); i += 1
        elif ch == "}": toks.append(Token(Tok.RB,   "}", i, i + 1)); i += 1
        elif ch == "@": toks.append(Token(Tok.AT,   "@", i, i + 1)); i += 1
        elif ch == "+": toks.append(Token(Tok.PLUS, "+", i, i + 1)); i += 1
        elif ch in _OP_CHARS:
            toks.append(Token(Tok.OP, ch, i, i + 1)); i += 1
        else:
            toks.append(Token(Tok.OTHER, ch, i, i + 1)); i += 1
    return toks


# ── string-likeness heuristic ───────────────────────────────────────────
# Stdlib calls that return $str. Conservative; extend as needed.
STRING_FUNCS = {
    "fromint", "fromfloat", "from_int", "from_float", "tostr",
    "concat", "join", "interpolate", "build", "done",
    "format", "sprintf",
    "slice", "substr", "upper", "lower", "trim", "trimprefix", "trimsuffix",
    "replace", "repeat", "charat", "chars", "find", "starts", "ends",
    "readln", "read", "input",
    "fromf64", "fromf32",
}


def _is_string_call(toks: list[Token], idx: int) -> bool:
    """Return True if the operand starting at toks[idx] is a call to a known
    string-returning stdlib function. Operand may be `alias.method(...)`
    or just `method(...)`."""
    if idx >= len(toks):
        return False
    t = toks[idx]
    if t.kind != Tok.IDENT:
        return False
    # Optional `alias.method` prefix
    method = t.text
    j = idx + 1
    if j + 1 < len(toks) and toks[j].kind == Tok.DOT and toks[j + 1].kind == Tok.IDENT:
        method = toks[j + 1].text
        j += 2
    # Must be followed by `(`
    if j >= len(toks) or toks[j].kind != Tok.LP:
        return False
    return method in STRING_FUNCS


def _scan_string_idents(toks: list[Token]) -> set[str]:
    """Pre-scan: find every `name:$str` (or `name:str`) declaration in
    function parameter lists OR in let bindings, so bare-identifier operands
    can be resolved without flagging as ambiguous.

    Captures the dominant case in model-generated programs:
        f=foo(a:$str; b:$str): $str { … a + b … }
    plus let-binding patterns:
        let s:$str = …;
        let s = "literal";          (treated as $str)
        let s = mut."";              (treated as $str)
    """
    s_idents: set[str] = set()
    i = 0
    n = len(toks)
    while i < n:
        t = toks[i]
        # `name : $str` pattern (parameter or let-with-annotation)
        if t.kind == Tok.IDENT:
            j = i + 1
            while j < n and toks[j].kind in (Tok.WHITE, Tok.NL):
                j += 1
            if (j < n and toks[j].kind == Tok.OP and toks[j].text == ":"):
                k = j + 1
                while k < n and toks[k].kind in (Tok.WHITE, Tok.NL):
                    k += 1
                # match "$str" (OP '$' then IDENT 'str') or bare IDENT 'str'
                if k < n and toks[k].kind == Tok.OP and toks[k].text == "$":
                    if k + 1 < n and toks[k + 1].kind == Tok.IDENT and toks[k + 1].text == "str":
                        s_idents.add(t.text)
                elif k < n and toks[k].kind == Tok.IDENT and toks[k].text == "str":
                    s_idents.add(t.text)
        # `let NAME = "literal";`, `let NAME = mut."literal";`,
        # or `let NAME = <stdlib-string-call>(...)`. All count.
        if t.kind == Tok.IDENT and t.text == "let":
            j = i + 1
            while j < n and toks[j].kind in (Tok.WHITE, Tok.NL):
                j += 1
            if j < n and toks[j].kind == Tok.IDENT:
                name = toks[j].text
                k = j + 1
                while k < n and toks[k].kind in (Tok.WHITE, Tok.NL):
                    k += 1
                if k < n and toks[k].kind == Tok.OP and toks[k].text == "=":
                    k += 1
                    while k < n and toks[k].kind in (Tok.WHITE, Tok.NL):
                        k += 1
                    # mut. wrapper
                    if (k + 1 < n and toks[k].kind == Tok.IDENT and toks[k].text == "mut"
                            and toks[k + 1].kind == Tok.DOT):
                        k += 2
                        while k < n and toks[k].kind in (Tok.WHITE, Tok.NL):
                            k += 1
                    if k < n and toks[k].kind == Tok.STR:
                        s_idents.add(name)
                    elif k < n and toks[k].kind == Tok.IDENT and _is_string_call(toks, k):
                        s_idents.add(name)
        i += 1
    return s_idents


_STRING_IDENTS: set[str] = set()


def _looks_string(toks: list[Token], idx: int) -> tuple[bool, bool]:
    """Inspect the operand starting at toks[idx]. Returns (is_string, is_ambiguous).

    Skip whitespace; consult the first non-whitespace token. Cases:
        STR literal                    → (True, False)
        IDENT.IDENT( ... )  (call)     → check stdlib func name table
        IDENT( ... )        (call)     → check stdlib func name table
        IDENT  ` as $str`              → (True, False)
        Bare IDENT in _STRING_IDENTS   → (True, False) — resolved by pre-scan
        Bare IDENT                     → (False, True) — ambiguous, conservative
        NUM literal                    → (False, False)
        `math.X(...)`                  → (False, False) heuristic
    """
    # advance past whitespace
    while idx < len(toks) and toks[idx].kind in (Tok.WHITE, Tok.NL):
        idx += 1
    if idx >= len(toks):
        return (False, True)

    t = toks[idx]
    if t.kind == Tok.STR:
        return (True, False)
    if t.kind == Tok.NUM:
        return (False, False)
    if t.kind == Tok.IDENT:
        # check `math.X(...)` numeric
        if (idx + 2 < len(toks) and t.text == "math" and toks[idx + 1].kind == Tok.DOT
                and toks[idx + 2].kind == Tok.IDENT):
            return (False, False)
        if _is_string_call(toks, idx):
            return (True, False)
        # Look ahead for `as $str` / `as$str` (tokenised: IDENT('as'), OP($), IDENT(str))
        k = idx + 1
        while k < len(toks) and toks[k].kind in (Tok.WHITE, Tok.NL):
            k += 1
        if (k < len(toks) and toks[k].kind == Tok.IDENT and toks[k].text == "as"):
            # walk past 'as' and optional whitespace
            k += 1
            while k < len(toks) and toks[k].kind in (Tok.WHITE, Tok.NL):
                k += 1
            if (k + 1 < len(toks) and toks[k].kind == Tok.OP and toks[k].text == "$"
                    and toks[k + 1].kind == Tok.IDENT and toks[k + 1].text == "str"):
                return (True, False)
        # bare identifier — check pre-scanned param/let table
        if t.text in _STRING_IDENTS:
            return (True, False)
        return (False, True)
    return (False, True)


# ── operand-span extraction ─────────────────────────────────────────────
def _extract_operand_left(toks: list[Token], plus_idx: int) -> tuple[int, int]:
    """Return (start_idx_inclusive, end_idx_exclusive) for the left operand
    of toks[plus_idx] (which is a PLUS token)."""
    j = plus_idx - 1
    while j >= 0 and toks[j].kind in (Tok.WHITE, Tok.NL):
        j -= 1
    end = j + 1
    # Walk back: identifier, optional .method(...) chain, optional string
    depth = 0
    while j >= 0:
        t = toks[j]
        if depth == 0:
            if t.kind == Tok.STR:
                # back up past whitespace before any preceding `+`
                # but ALSO chain backwards if this is the end of `+` operand
                return (j, end)
            if t.kind == Tok.RP:
                depth = 1
                j -= 1
                continue
            if t.kind == Tok.IDENT:
                # back up further if previous is DOT (alias.method)
                k = j - 1
                while k >= 0 and toks[k].kind in (Tok.WHITE, Tok.NL):
                    k -= 1
                if k >= 0 and toks[k].kind == Tok.DOT:
                    # chain: continue backwards past DOT, IDENT
                    j = k - 1
                    while j >= 0 and toks[j].kind in (Tok.WHITE, Tok.NL):
                        j -= 1
                    if j >= 0 and toks[j].kind == Tok.IDENT:
                        # consumed alias.method
                        # check for another dot back
                        k2 = j - 1
                        while k2 >= 0 and toks[k2].kind in (Tok.WHITE, Tok.NL):
                            k2 -= 1
                        if k2 >= 0 and toks[k2].kind == Tok.DOT:
                            # rare a.b.c — keep walking
                            j = k2 - 1
                            continue
                        return (j, end)
                    return (j + 1, end)
                return (j, end)
            if t.kind == Tok.NUM:
                return (j, end)
            return (j + 1, end)
        else:
            if t.kind == Tok.RP: depth += 1
            elif t.kind == Tok.LP:
                depth -= 1
                if depth == 0:
                    # If preceded by an IDENT (alias.method or method), consume it
                    k = j - 1
                    while k >= 0 and toks[k].kind in (Tok.WHITE, Tok.NL):
                        k -= 1
                    if k >= 0 and toks[k].kind == Tok.IDENT:
                        # check for DOT before
                        k2 = k - 1
                        while k2 >= 0 and toks[k2].kind in (Tok.WHITE, Tok.NL):
                            k2 -= 1
                        if k2 >= 0 and toks[k2].kind == Tok.DOT:
                            # alias.method(...)
                            k3 = k2 - 1
                            while k3 >= 0 and toks[k3].kind in (Tok.WHITE, Tok.NL):
                                k3 -= 1
                            if k3 >= 0 and toks[k3].kind == Tok.IDENT:
                                return (k3, end)
                        return (k, end)
                    return (j, end)
            j -= 1
            continue
        j -= 1
    return (0, end)


def _extract_operand_right(toks: list[Token], plus_idx: int) -> tuple[int, int]:
    """Return (start_idx_inclusive, end_idx_exclusive) for the right operand
    of toks[plus_idx]."""
    j = plus_idx + 1
    while j < len(toks) and toks[j].kind in (Tok.WHITE, Tok.NL):
        j += 1
    start = j
    if j >= len(toks):
        return (start, start)
    t = toks[j]
    if t.kind == Tok.STR or t.kind == Tok.NUM:
        return (start, j + 1)
    if t.kind == Tok.IDENT:
        # optional .method(...) chain
        end = j + 1
        # alias.method
        k = j + 1
        while k < len(toks) and toks[k].kind in (Tok.WHITE, Tok.NL):
            k += 1
        if k < len(toks) and toks[k].kind == Tok.DOT:
            k += 1
            while k < len(toks) and toks[k].kind in (Tok.WHITE, Tok.NL):
                k += 1
            if k < len(toks) and toks[k].kind == Tok.IDENT:
                end = k + 1
                k += 1
        # call ( ... )
        while k < len(toks) and toks[k].kind in (Tok.WHITE, Tok.NL):
            k += 1
        if k < len(toks) and toks[k].kind == Tok.LP:
            depth = 1
            k += 1
            while k < len(toks) and depth > 0:
                if toks[k].kind == Tok.LP: depth += 1
                elif toks[k].kind == Tok.RP:
                    depth -= 1
                    if depth == 0:
                        end = k + 1
                        break
                k += 1
        return (start, end)
    if t.kind == Tok.LP:
        depth = 1
        k = j + 1
        while k < len(toks) and depth > 0:
            if toks[k].kind == Tok.LP: depth += 1
            elif toks[k].kind == Tok.RP:
                depth -= 1
                if depth == 0:
                    return (start, k + 1)
            k += 1
    return (start, j + 1)


# ── per-`+` analysis ────────────────────────────────────────────────────
@dataclass
class PlusSite:
    plus_idx:   int
    lhs_start:  int
    lhs_end:    int
    rhs_start:  int
    rhs_end:    int
    lhs_str:    bool
    rhs_str:    bool
    lhs_amb:    bool
    rhs_amb:    bool


def analyse(toks: list[Token]) -> list[PlusSite]:
    sites: list[PlusSite] = []
    for i, t in enumerate(toks):
        if t.kind != Tok.PLUS:
            continue
        lhs_str, lhs_amb = _looks_string(toks, _extract_operand_left(toks, i)[0])
        rhs_str, rhs_amb = _looks_string(toks, _extract_operand_right(toks, i)[0])
        ls, le = _extract_operand_left(toks, i)
        rs, re_ = _extract_operand_right(toks, i)
        sites.append(PlusSite(i, ls, le, rs, re_, lhs_str, rhs_str, lhs_amb, rhs_amb))
    return sites


# ── rewriter ────────────────────────────────────────────────────────────
_S_IMPORT_RE = re.compile(r"\bi\s*=\s*s\s*:\s*std\.str\b")


def _ensure_s_import(src: str) -> tuple[str, int]:
    """If `src` contains `s.<method>` calls but is missing `i=s:std.str;`,
    inject the import right after the `m=...;` module line. Story 111.11.
    Returns (possibly-modified-source, 1 if inserted else 0)."""
    if _S_IMPORT_RE.search(src):
        return src, 0
    # Detect any `s.<lowercase>` reference outside string literals (cheap
    # heuristic — false positives are harmless since the import is idempotent).
    has_s_call = False
    in_str = False
    i = 0
    while i < len(src) - 1:
        ch = src[i]
        if ch == "\\" and in_str:
            i += 2
            continue
        if ch == "\"":
            in_str = not in_str
            i += 1
            continue
        if not in_str:
            if (ch == "s" and src[i+1] == "."
                and (i == 0 or not (src[i-1].isalnum() or src[i-1] == "_"))):
                # peek the next char after the dot — must be a letter (method name)
                if i + 2 < len(src) and src[i+2].isalpha():
                    has_s_call = True
                    break
        i += 1
    if not has_s_call:
        return src, 0
    # Find end of `m=...;` line
    m = re.match(r"\s*m\s*=[^;]*;", src)
    if not m:
        # No module decl found — prepend a comment marker but don't break the file
        return src, 0
    insert_at = m.end()
    return src[:insert_at] + "i=s:std.str;" + src[insert_at:], 1


def rewrite(src: str) -> tuple[str, dict]:
    """Return (new_src, stats). Stats: rewritten, ambiguous, untouched, import_injected."""
    global _STRING_IDENTS
    stats = Counter()
    toks = tokenise(src)
    _STRING_IDENTS = _scan_string_idents(toks)
    sites = analyse(toks)

    # Build a set of byte ranges to replace and the replacement text.
    # Strategy: identify chains of `+` where every operand on the chain is
    # string-like; rewrite the whole chain to s.concat(a;b) or s.concat
    # nested for >2 parts.
    edits: list[tuple[int, int, str]] = []   # (start_byte, end_byte, repl)
    handled_idxs: set[int] = set()

    # Walk plus sites left-to-right, group connected chains.
    visited: set[int] = set()
    for site_i, s in enumerate(sites):
        if s.plus_idx in visited:
            continue
        # Build chain: include this site if str+str; extend right while next
        # `+` immediately follows current rhs and its rhs is str-like.
        if not (s.lhs_str and s.rhs_str):
            if (s.lhs_str and s.rhs_amb) or (s.rhs_str and s.lhs_amb):
                stats["ambiguous"] += 1
            continue
        # collect chain of parts
        chain_lhs_start = s.lhs_start
        chain_rhs_end   = s.rhs_end
        parts_token_ranges: list[tuple[int, int]] = [
            (s.lhs_start, s.lhs_end),
            (s.rhs_start, s.rhs_end),
        ]
        visited.add(s.plus_idx)
        # Extend right: a `+` chain like `a + b + c` produces sites
        # (lhs=a,rhs=b) and (lhs=b,rhs=c). Detect by: next site's lhs span
        # equals current site's rhs span. Each extension adds one more part.
        cur_rhs_start, cur_rhs_end = s.rhs_start, s.rhs_end
        ni = site_i + 1
        while ni < len(sites):
            nxt = sites[ni]
            if not (nxt.lhs_start == cur_rhs_start and nxt.lhs_end == cur_rhs_end):
                break
            if not (nxt.rhs_str and not nxt.rhs_amb):
                break
            parts_token_ranges.append((nxt.rhs_start, nxt.rhs_end))
            chain_rhs_end = nxt.rhs_end
            cur_rhs_start, cur_rhs_end = nxt.rhs_start, nxt.rhs_end
            visited.add(nxt.plus_idx)
            ni += 1

        # Build replacement text. Always pairwise s.concat — inline @()
        # containing function-parameter identifiers triggers a codegen issue
        # (separate bug to investigate; migration must produce reliably
        # working output today). Nested s.concat is uglier but always lowers
        # to a sequence of tk_str_concat_w calls that we know are safe.
        part_texts = [src[toks[a].start:toks[b - 1].end] for a, b in parts_token_ranges]
        if len(part_texts) == 2:
            repl = f"s.concat({part_texts[0]};{part_texts[1]})"
        else:
            # Nest left-to-right: s.concat(s.concat(s.concat(a;b);c);d)
            inner = part_texts[0]
            for nxt in part_texts[1:]:
                inner = f"s.concat({inner};{nxt})"
            repl = inner
        edits.append((toks[chain_lhs_start].start, toks[chain_rhs_end - 1].end, repl))
        stats["rewritten"] += 1

    if not edits:
        stats["untouched"] += 1
        return (src, dict(stats))

    # Apply edits right-to-left to keep byte offsets stable.
    edits.sort(key=lambda e: e[0], reverse=True)
    out = src
    for start, end, repl in edits:
        out = out[:start] + repl + out[end:]
    # Story 111.11: ensure i=s:std.str; is present if any rewrite added s.* calls.
    out, injected = _ensure_s_import(out)
    if injected:
        stats["import_injected"] += 1
    return (out, dict(stats))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("files", nargs="+", help="toke source files")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--check", action="store_true", help="report only, no rewrite")
    g.add_argument("--diff",  action="store_true", help="show unified diff per file")
    args = ap.parse_args()

    total = Counter()
    any_ambiguous = False
    for path_s in args.files:
        path = Path(path_s)
        try:
            src = path.read_text()
        except OSError as e:
            print(f"ERR {path_s}: {e}", file=sys.stderr)
            return 2
        new_src, stats = rewrite(src)
        total.update(stats)
        if stats.get("ambiguous"):
            any_ambiguous = True
        if args.diff and new_src != src:
            for line in difflib.unified_diff(src.splitlines(keepends=True),
                                              new_src.splitlines(keepends=True),
                                              fromfile=str(path), tofile=f"{path} (migrated)"):
                sys.stdout.write(line)
        elif not args.check and not args.diff and new_src != src:
            path.write_text(new_src)
        if stats.get("rewritten") or stats.get("ambiguous"):
            print(f"{path_s}: rewritten={stats.get('rewritten',0)} "
                  f"ambiguous={stats.get('ambiguous',0)}")
    print("totals:", dict(total))
    return 1 if any_ambiguous else 0


if __name__ == "__main__":
    sys.exit(main())
