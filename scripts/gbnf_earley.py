#!/usr/bin/env python3
"""gbnf_earley.py — a dependency-free GBNF loader and Earley recogniser (story 131.52).

Why this exists: `docs/spec/toke.gbnf` is shipped for grammar-constrained
decoding (llama.cpp / XGrammar / Outlines), but nothing ever *ran* it, so it
drifted from the real parser for a year (116.5 shipped it; v0.4 changed the
language under it).  The drift gate (`scripts/gbnf_gate.py`) needs a
recogniser for the grammar exactly as a GBNF consumer reads it, and this
machine has neither llama.cpp nor XGrammar installed, so this module is that
recogniser, in the standard library only.

Supported GBNF (the llama.cpp dialect, which is what toke.gbnf is written in):
  rule ::= alternatives        rules may continue over indented lines
  "literal"   with \\" \\\\ \\n \\r \\t \\xHH \\uHHHH escapes
  [class]     [^class], ranges a-z, the same escapes
  .           any single character
  ( ... )     grouping;  postfix  *  +  ?  {m}  {m,}  {m,n}
  # comment   to end of line
Matching is on Unicode characters (Python `str`), as llama.cpp matches code
points.

Library:
  g = Grammar.from_file(path)      g.accepts(text) -> bool
  r = g.recogniser(); r.feed(ch) -> bool; r.pop(); r.complete(); r.allowed_chars(alphabet)
  r.stats  -> counters (items created, scans, completions) for the mask benchmark
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

__all__ = ["Grammar", "GbnfError", "Recogniser"]


class GbnfError(ValueError):
    pass


# ------------------------------------------------------------- tokeniser ----
_ESC = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\", '"': '"', "[": "[", "]": "]",
        "-": "-", "^": "^", "'": "'", "/": "/", ".": "."}


def _unescape(s: str, i: int) -> tuple[str, int]:
    """s[i] == '\\'; return (char, next index)."""
    c = s[i + 1]
    if c in _ESC:
        return _ESC[c], i + 2
    if c == "x":
        return chr(int(s[i + 2:i + 4], 16)), i + 4
    if c == "u":
        return chr(int(s[i + 2:i + 6], 16)), i + 6
    if c == "U":
        return chr(int(s[i + 2:i + 10], 16)), i + 10
    raise GbnfError(f"unknown escape \\{c}")


def _tokenise(text: str) -> list[tuple[str, object, int]]:
    toks: list[tuple[str, object, int]] = []
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r":
            i += 1
        elif c == "#":
            while i < n and text[i] != "\n":
                i += 1
        elif text.startswith("::=", i):
            toks.append(("DEF", None, line))
            i += 3
        elif c == '"':
            j, out = i + 1, []
            while j < n and text[j] != '"':
                if text[j] == "\\":
                    ch, j = _unescape(text, j)
                    out.append(ch)
                else:
                    out.append(text[j])
                    j += 1
            if j >= n:
                raise GbnfError(f"line {line}: unterminated string")
            toks.append(("STR", "".join(out), line))
            i = j + 1
        elif c == "[":
            j, neg, ranges = i + 1, False, []
            if j < n and text[j] == "^":
                neg, j = True, j + 1
            while j < n and text[j] != "]":
                if text[j] == "\\":
                    lo, j = _unescape(text, j)
                else:
                    lo, j = text[j], j + 1
                hi = lo
                if j + 1 < n and text[j] == "-" and text[j + 1] != "]":
                    if text[j + 1] == "\\":
                        hi, j = _unescape(text, j + 1)
                    else:
                        hi, j = text[j + 1], j + 2
                ranges.append((ord(lo), ord(hi)))
            if j >= n:
                raise GbnfError(f"line {line}: unterminated character class")
            toks.append(("CLS", (neg, tuple(ranges)), line))
            i = j + 1
        elif c == "{":
            m = re.match(r"\{\s*(\d+)\s*(,\s*(\d*)\s*)?\}", text[i:])
            if not m:
                raise GbnfError(f"line {line}: bad repetition")
            lo = int(m.group(1))
            hi = lo if m.group(2) is None else (int(m.group(3)) if m.group(3) else None)
            toks.append(("REP", (lo, hi), line))
            i += m.end()
        elif c in "()|*+?.":
            toks.append((c, None, line))
            i += 1
        else:
            m = re.match(r"[A-Za-z0-9_-]+", text[i:])
            if not m:
                raise GbnfError(f"line {line}: unexpected character {c!r}")
            toks.append(("NAME", m.group(0), line))
            i += m.end()
    return toks


# --------------------------------------------------------------- grammar ----
class _Matcher:
    """A character class; `ok(ch)` is cached per character."""
    __slots__ = ("neg", "ranges", "_cache", "label")

    def __init__(self, neg: bool, ranges: tuple, label: str):
        self.neg, self.ranges, self._cache, self.label = neg, ranges, {}, label

    def ok(self, ch: str) -> bool:
        r = self._cache.get(ch)
        if r is None:
            o = ord(ch)
            r = any(lo <= o <= hi for lo, hi in self.ranges) != self.neg
            self._cache[ch] = r
        return r


class Grammar:
    """GBNF compiled to a plain BNF: nonterminals are ints >= 0, terminals are
    ints < 0 indexing `self.term` (term k is symbol -(k+1))."""

    def __init__(self, text: str, root: str = "root"):
        self.text = text
        self.names: list[str] = []
        self.ids: dict[str, int] = {}
        self.prods: list[tuple[int, tuple[int, ...]]] = []
        self.by_lhs: list[list[int]] = []
        self.term: list[_Matcher] = []
        self._term_ids: dict[tuple, int] = {}
        self.rule_lines: dict[str, int] = {}
        self._compile(_tokenise(text))
        if root not in self.ids:
            raise GbnfError(f"no `{root}` rule")
        self.root = self.ids[root]
        self._check_defined()
        self._nullable()

    @classmethod
    def from_file(cls, path: str | Path, root: str = "root") -> "Grammar":
        return cls(Path(path).read_text(encoding="utf-8"), root)

    # -- construction --
    def _nt(self, name: str) -> int:
        if name not in self.ids:
            self.ids[name] = len(self.names)
            self.names.append(name)
            self.by_lhs.append([])
        return self.ids[name]

    def _fresh(self, base: str) -> int:
        k = 1
        while f"{base}~{k}" in self.ids:
            k += 1
        return self._nt(f"{base}~{k}")

    def _t(self, neg: bool, ranges: tuple, label: str) -> int:
        key = (neg, ranges)
        if key not in self._term_ids:
            self._term_ids[key] = len(self.term)
            self.term.append(_Matcher(neg, ranges, label))
        return -(self._term_ids[key] + 1)

    def _add(self, lhs: int, rhs: list[int]) -> None:
        self.by_lhs[lhs].append(len(self.prods))
        self.prods.append((lhs, tuple(rhs)))

    def _compile(self, toks: list) -> None:
        heads = [k for k in range(len(toks) - 1) if toks[k][0] == "NAME" and toks[k + 1][0] == "DEF"]
        if not heads:
            raise GbnfError("no rules")
        if heads[0] != 0:
            raise GbnfError(f"line {toks[0][2]}: text before the first rule")
        self._defined: set[str] = set()
        for h, nxt in zip(heads, heads[1:] + [len(toks)]):
            name = toks[h][1]
            if name in self._defined:
                raise GbnfError(f"line {toks[h][2]}: rule `{name}` defined twice")
            self._defined.add(name)
            self.rule_lines[name] = toks[h][2]
            lhs = self._nt(name)
            body = toks[h + 2:nxt]
            pos, alts = self._alts(body, 0, name)
            if pos != len(body):
                raise GbnfError(f"line {body[pos][2]}: unexpected `{body[pos][0]}` in rule `{name}`")
            for seq in alts:
                self._add(lhs, seq)

    def _alts(self, b: list, i: int, ctx: str) -> tuple[int, list[list[int]]]:
        alts = [[]]
        while i < len(b):
            k = b[i][0]
            if k == "|":
                alts.append([])
                i += 1
            elif k == ")":
                break
            else:
                i, syms = self._postfix(b, i, ctx)
                alts[-1].extend(syms)
        return i, alts

    def _atom(self, b: list, i: int, ctx: str) -> tuple[int, list[int]]:
        k, v, line = b[i]
        if k == "STR":
            return i + 1, [self._t(False, ((ord(c), ord(c)),), repr(c)) for c in v]
        if k == "CLS":
            return i + 1, [self._t(v[0], v[1], "[class]")]
        if k == ".":
            return i + 1, [self._t(True, (), "any")]
        if k == "NAME":
            return i + 1, [self._nt(v)]
        if k == "(":
            j, alts = self._alts(b, i + 1, ctx)
            if j >= len(b) or b[j][0] != ")":
                raise GbnfError(f"line {line}: unclosed `(` in rule `{ctx}`")
            g = self._fresh(ctx)
            for seq in alts:
                self._add(g, seq)
            return j + 1, [g]
        raise GbnfError(f"line {line}: unexpected `{k}` in rule `{ctx}`")

    def _postfix(self, b: list, i: int, ctx: str) -> tuple[int, list[int]]:
        i, syms = self._atom(b, i, ctx)
        while i < len(b) and b[i][0] in ("*", "+", "?", "REP"):
            k, v = b[i][0], b[i][1]
            lo, hi = {"*": (0, None), "+": (1, None), "?": (0, 1)}.get(k, v)
            syms = self._repeat(syms, lo, hi, ctx)
            i += 1
        return i, syms

    def _repeat(self, syms: list[int], lo: int, hi, ctx: str) -> list[int]:
        out = list(syms) * lo
        if hi is None:                         # X* as a LEFT-recursive list (cheap in Earley)
            star = self._fresh(ctx)
            self._add(star, [])
            self._add(star, [star] + list(syms))
            return out + [star]
        for _ in range(hi - lo):               # X? nested: (X (X ...)?)?
            opt = self._fresh(ctx)
            self._add(opt, [])
            self._add(opt, list(syms))
            out.append(opt)
        return out

    def _check_defined(self) -> None:
        missing = sorted(n for n in self.names if "~" not in n and n not in self._defined)
        if missing:
            raise GbnfError(f"undefined rule(s): {', '.join(missing)}")

    def _nullable(self) -> None:
        self.nullable = [False] * len(self.names)
        changed = True
        while changed:
            changed = False
            for lhs, rhs in self.prods:
                if not self.nullable[lhs] and all(s >= 0 and self.nullable[s] for s in rhs):
                    self.nullable[lhs] = changed = True

    # -- static checks a GBNF consumer cares about --
    def left_recursive_rules(self) -> list[str]:
        """Named rules that can derive themselves at their left edge (through
        nullable prefixes).  llama.cpp's GBNF engine loops on these."""
        first: dict[int, set[int]] = {n: set() for n in range(len(self.names))}
        for lhs, rhs in self.prods:
            for s in rhs:
                if s < 0:
                    break
                first[lhs].add(s)
                if not self.nullable[s]:
                    break
        out = []
        for n, name in enumerate(self.names):
            if "~" in name:
                continue
            seen, stack = set(), list(first[n])
            while stack:
                x = stack.pop()
                if x == n:
                    out.append(name)
                    break
                if x not in seen:
                    seen.add(x)
                    stack.extend(first[x])
        return out

    def unreachable_rules(self) -> list[str]:
        seen, stack = {self.root}, [self.root]
        while stack:
            for p in self.by_lhs[stack.pop()]:
                for s in self.prods[p][1]:
                    if s >= 0 and s not in seen:
                        seen.add(s)
                        stack.append(s)
        return sorted(n for i, n in enumerate(self.names) if "~" not in n and i not in seen)

    # -- recognition --
    def recogniser(self) -> "Recogniser":
        return Recogniser(self)

    def accepts(self, text: str) -> bool:
        r = Recogniser(self)
        for ch in text:
            if not r.feed(ch):
                return False
        return r.complete()


class Recogniser:
    """Incremental Earley recogniser.  `feed` appends one character and
    returns False (leaving the state unchanged) if no item can scan it;
    `pop` undoes the last successful feed, which is what a vocabulary-trie
    mask walk needs."""

    def __init__(self, g: Grammar):
        self.g = g
        self.sets: list[dict] = []
        self.stats = {"items": 0, "scans": 0, "completions": 0, "predictions": 0, "chars": 0}
        s0 = self._new_set()
        for p in g.by_lhs[g.root]:
            self._add(s0, (p, 0, 0))
        self._close(s0, 0)
        self.sets.append(s0)

    @staticmethod
    def _new_set() -> dict:
        # items: insertion-ordered worklist; seen: dedupe; wait: NT -> [items]; term: [(item, sym)]
        return {"items": [], "seen": set(), "wait": {}, "term": []}

    def _add(self, st: dict, it: tuple) -> None:
        if it not in st["seen"]:
            st["seen"].add(it)
            st["items"].append(it)
            self.stats["items"] += 1

    def _close(self, st: dict, k: int) -> None:
        g, prods, items = self.g, self.g.prods, st["items"]
        j = 0
        while j < len(items):
            p, d, o = items[j]
            j += 1
            lhs, rhs = prods[p]
            if d < len(rhs):
                s = rhs[d]
                if s < 0:
                    st["term"].append(((p, d, o), s))
                    continue
                w = st["wait"].get(s)
                if w is None:
                    st["wait"][s] = [(p, d, o)]
                    self.stats["predictions"] += 1
                    for q in g.by_lhs[s]:
                        self._add(st, (q, 0, k))
                else:
                    w.append((p, d, o))
                if g.nullable[s]:
                    self._add(st, (p, d + 1, o))
            else:                                       # complete
                self.stats["completions"] += 1
                src = st if o == k else self.sets[o]
                for (p2, d2, o2) in list(src["wait"].get(lhs, ())):
                    self._add(st, (p2, d2 + 1, o2))

    def feed(self, ch: str) -> bool:
        cur = self.sets[-1]
        nxt = self._new_set()
        term = self.g.term
        for (p, d, o), s in cur["term"]:
            if term[-s - 1].ok(ch):
                self._add(nxt, (p, d + 1, o))
                self.stats["scans"] += 1
        if not nxt["items"]:
            return False
        self.stats["chars"] += 1
        self.sets.append(nxt)          # visible to completions of items with origin < k
        self._close(nxt, len(self.sets) - 1)
        return True

    def pop(self) -> None:
        if len(self.sets) > 1:
            self.sets.pop()

    def complete(self) -> bool:
        root = self.g.root
        return any(self.g.prods[p][0] == root and d == len(self.g.prods[p][1]) and o == 0
                   for p, d, o in self.sets[-1]["items"])

    def can_continue(self, ch: str) -> bool:
        term = self.g.term
        return any(term[-s - 1].ok(ch) for _, s in self.sets[-1]["term"])

    def allowed_chars(self, alphabet) -> list[str]:
        """The character-level mask: which characters may come next."""
        matchers = {s for _, s in self.sets[-1]["term"]}
        term = self.g.term
        return [c for c in alphabet if any(term[-s - 1].ok(c) for s in matchers)]


def main(argv: list[str] | None = None) -> int:
    import argparse
    ap = argparse.ArgumentParser(description="Check text files against a GBNF grammar")
    ap.add_argument("grammar")
    ap.add_argument("files", nargs="*")
    a = ap.parse_args(argv)
    g = Grammar.from_file(a.grammar)
    print(f"{len(g.names)} nonterminals, {len(g.prods)} productions, {len(g.term)} terminal classes")
    rc = 0
    for f in a.files:
        ok = g.accepts(Path(f).read_text(encoding="utf-8", errors="replace"))
        print(("ACCEPT " if ok else "REJECT ") + f)
        rc |= 0 if ok else 1
    return rc


if __name__ == "__main__":
    sys.exit(main())
