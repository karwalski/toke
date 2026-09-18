#!/usr/bin/env python3
"""mask_strings.py — replace every toke string-literal BODY with `_`.

Story 131.4 (Epic 131); normative rule: docs/spec/patterns-protocol-v0.4.md §4.

    "hello \\(x) world"   ->   "_\\(x)_"
    ""                    ->   ""
    "a\\"b\\\\c\\n"       ->   "_"
    "\\(s.split(r;"f"))"  ->   "\\(s.split(r;"_"))"

Rules (mirrors src/lexer.c `lex_string` so masked text lexes exactly as the
original does):

* A literal starts at an unescaped `"` in code and ends at the next `"` that
  is not part of an escape.  Escapes are `\\"  \\\\  \\n  \\t  \\r  \\0  \\xHH`
  and are simply part of the body (they are masked away with it).
* Every maximal run of body text between the quotes / interpolations becomes
  ONE `_`, so the mask is length-stable per run, never per character.
* `\\( ... )` is string interpolation: its interior is CODE.  The extent is
  found the way the lexer finds it — raw paren counting, no string awareness
  (the lexer itself is not string-aware there, so a nested literal whose body
  contains a paren cannot compile and never occurs).  The interior is kept
  verbatim EXCEPT that string literals nested inside it are masked
  recursively (their bodies are still string bodies, not code).
* Input is expected to be `tkc --min` output (no comments).  Text outside
  string literals is copied byte-for-byte; the function is idempotent.
* Unterminated literal / interpolation: the remainder is treated as body /
  interior respectively (lenient; the compiler already rejected such input).

Library:
    mask_strings(text) -> str
    skip_string(text, i) -> int     index just past the literal opening at text[i]
CLI:
    python3 scripts/patterns/mask_strings.py FILE     (or - for stdin)
"""
from __future__ import annotations

import sys

MASK = "_"


def _interp_end(text: str, i: int) -> int:
    """text[i] is the `(` of a `\\(`; return index of its matching `)` (or len)."""
    depth = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return n


def skip_string(text: str, i: int) -> int:
    """text[i] == '"'.  Return the index just past the closing quote (len(text)
    if unterminated).  Escape-aware and interpolation-aware, like the lexer."""
    n = len(text)
    i += 1
    while i < n:
        c = text[i]
        if c == '"':
            return i + 1
        if c == "\\":
            if i + 1 < n and text[i + 1] == "(":
                i = _interp_end(text, i + 1) + 1
                continue
            i += 2  # escape char + escaped char (\xHH: the HH are plain body)
            continue
        i += 1
    return n


def _mask_literal(text: str, i: int) -> tuple[str, int]:
    """text[i] == '"'.  Return (masked literal incl. quotes, index past it)."""
    n = len(text)
    out = ['"']
    run = False  # currently inside an unemitted body run
    i += 1
    while i < n:
        c = text[i]
        if c == '"':
            if run:
                out.append(MASK)
            out.append('"')
            return "".join(out), i + 1
        if c == "\\":
            if i + 1 < n and text[i + 1] == "(":
                if run:
                    out.append(MASK)
                    run = False
                close = _interp_end(text, i + 1)
                interior = text[i + 2:close]
                out.append("\\(")
                out.append(mask_strings(interior))
                if close < n:
                    out.append(")")
                i = close + 1
                continue
            run = True
            i += 2
            continue
        run = True
        i += 1
    if run:
        out.append(MASK)
    return "".join(out), n


def mask_strings(text: str) -> str:
    """Return `text` with every string-literal body replaced by `_`."""
    out: list[str] = []
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            lit, i = _mask_literal(text, i)
            out.append(lit)
        else:
            out.append(c)
            i += 1
    return "".join(out)


def main(argv: list[str]) -> int:
    if len(argv) != 2 or argv[1] in ("-h", "--help"):
        print(__doc__)
        return 2
    src = sys.stdin.read() if argv[1] == "-" else open(argv[1], encoding="utf-8").read()
    sys.stdout.write(mask_strings(src))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
