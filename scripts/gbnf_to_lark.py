#!/usr/bin/env python3
"""gbnf_to_lark.py — generate docs/spec/toke.lark from docs/spec/toke.gbnf, and gate both
against the engines that load them (story 131.52).

toke.gbnf is the grammar-constrained-decoding artefact that `make check-gbnf`
holds to the real parser.  This script derives the Lark form of the SAME
grammar mechanically, rule for rule, so the two cannot disagree:

    toke.gbnf ──(this script)──► toke.lark
      │                              │
      ├─ XGrammar  Grammar.from_ebnf ├─ Lark  (Python, Earley, dynamic lexer)
      ├─ llama.cpp GBNF              └─ llguidance  (Lark dialect)
      └─ scripts/gbnf_earley.py  (the gate's reference recogniser)

Why toke.gbnf and not grammar.ebnf: grammar.ebnf is the normative grammar
written over LEXER TOKEN CLASSES (IDENT, INT_LIT, ...) with prose for the
lexical rules and the `;` elision, so no engine can load it as it stands.
toke.gbnf is the character-level grammar (lexer included) that the drift gate
checks against `tkc --dump-ast`; generating from it keeps one source of truth.

The translation is 1:1: every GBNF rule becomes a Lark rule of the same name
with `-` spelled `_`; "literal" becomes a sequence of one-character Lark
strings (see _lit for why), [class] becomes a /[...]/
regex of one character, `.` becomes /(?s:.)/, postfix * + ? and {m,n} map to
Lark's * + ? and ~ m..n; an empty literal "" is an empty sequence.

Usage:
  gbnf_to_lark.py --write     regenerate docs/spec/toke.lark
  gbnf_to_lark.py --check     the gate (`make check-grammar-artefacts`):
      1. regenerate in memory and compare with the committed toke.lark,
         byte for byte (standard library only; always runs);
      2. for every engine that is importable -- lark, xgrammar, llguidance --
         load the artefact and compare its verdict with gbnf_earley's on every
         fragment of test/gbnf/fragments.json (the gate's fragment set, which
         `make check-gbnf` ties to the parser).  An engine that is not
         installed is reported as SKIPPED, never as a pass.
      --require-engines turns a SKIPPED engine into a failure; Python Lark is
      slow on a character-level grammar, so it sees every 8th fragment unless
      --full.
  gbnf_to_lark.py --stdout    print the generated Lark text
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from gbnf_earley import Grammar, GbnfError, _tokenise  # noqa: E402

GBNF = ROOT / "docs" / "spec" / "toke.gbnf"
LARK = ROOT / "docs" / "spec" / "toke.lark"
FRAGMENTS = ROOT / "test" / "gbnf" / "fragments.json"


# ----------------------------------------------------------- GBNF -> AST ----
def _parse(text: str) -> list[tuple[str, object]]:
    """[(rule name, AST)] in file order.  AST: ('alt', [seq]), ('seq', [node]),
    ('str', s), ('cls', (neg, ranges)), ('any',), ('name', n), ('rep', node, lo, hi)."""
    toks = _tokenise(text)
    heads = [k for k in range(len(toks) - 1) if toks[k][0] == "NAME" and toks[k + 1][0] == "DEF"]
    rules = []
    for h, nxt in zip(heads, heads[1:] + [len(toks)]):
        body = toks[h + 2:nxt]
        pos, node = _alts(body, 0)
        if pos != len(body):
            raise GbnfError(f"line {body[pos][2]}: unexpected `{body[pos][0]}`")
        rules.append((toks[h][1], node))
    return rules


def _alts(b, i):
    alts = [[]]
    while i < len(b) and b[i][0] != ")":
        if b[i][0] == "|":
            alts.append([])
            i += 1
            continue
        i, node = _atom(b, i)
        while i < len(b) and b[i][0] in ("*", "+", "?", "REP"):
            k, v, _ = b[i]
            lo, hi = {"*": (0, None), "+": (1, None), "?": (0, 1)}.get(k, v)
            node = ("rep", node, lo, hi)
            i += 1
        alts[-1].append(node)
    return i, ("alt", [("seq", s) for s in alts])


def _atom(b, i):
    k, v, line = b[i]
    if k == "STR":
        return i + 1, ("str", v)
    if k == "CLS":
        return i + 1, ("cls", v)
    if k == ".":
        return i + 1, ("any",)
    if k == "NAME":
        return i + 1, ("name", v)
    if k == "(":
        j, node = _alts(b, i + 1)
        if j >= len(b) or b[j][0] != ")":
            raise GbnfError(f"line {line}: unclosed (")
        return j + 1, node
    raise GbnfError(f"line {line}: unexpected `{k}`")


# ----------------------------------------------------------- AST -> Lark ----
def lark_name(n: str) -> str:
    out = n.replace("-", "_")
    if not re.fullmatch(r"[a-z][a-z0-9_]*", out):
        raise GbnfError(f"rule `{n}` has no Lark spelling")
    return out


def _lit(s: str) -> str:
    """A literal as a sequence of ONE-character Lark strings: Lark and llguidance
    treat every quoted string as a lexeme, and llguidance's lexer is greedy, so a
    multi-character "let" would be lexed as a unit inside `lettrue` and change
    the language (156 of the 2081 gate fragments, measured).  One character per
    terminal leaves nothing to be greedy about."""
    return " ".join(_lit1(c) for c in s)


def _lit1(s: str) -> str:
    out = []
    for c in s:
        o = ord(c)
        if c in '"\\':
            out.append("\\" + c)
        elif 32 <= o < 127:
            out.append(c)
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif c == "\r":
            out.append("\\r")
        else:
            out.append(f"\\x{o:02x}" if o < 256 else f"\\u{o:04x}")
    return '"' + "".join(out) + '"'


def _cch(o: int) -> str:
    c = chr(o)
    if c in "\\]^-[/":
        return "\\" + c
    if 33 <= o < 127:
        return c
    return f"\\x{o:02x}" if o < 256 else f"\\u{o:04x}"


def _cls(neg: bool, ranges) -> str:
    body = "".join(_cch(lo) if lo == hi else f"{_cch(lo)}-{_cch(hi)}" for lo, hi in ranges)
    return "/[" + ("^" if neg else "") + body + "]/"


def _emit(node, top: bool = False) -> str:
    kind = node[0]
    if kind == "alt":
        seqs = [_emit(s) for s in node[1]]
        empty = any(s == "" for s in seqs)
        seqs = [s for s in seqs if s != ""]
        if not seqs:
            return ""
        if top:
            # an empty alternative stays an empty alternative (`rule: a | b |`)
            return (("\n    | ".join(seqs) if len(seqs) > 3 else " | ".join(seqs))
                    + (" |" if empty else ""))
        inner = " | ".join(seqs)
        if empty:
            return f"({inner})?"
        return inner if len(seqs) == 1 and " " not in inner else f"({inner})"
    if kind == "seq":
        return " ".join(p for p in (_emit(x) for x in node[1]) if p != "")
    if kind == "str":
        return _lit(node[1]) if node[1] else ""
    if kind == "cls":
        return _cls(*node[1])
    if kind == "any":
        return "/(?s:.)/"
    if kind == "name":
        return lark_name(node[1])
    if kind == "rep":
        inner = _emit(node[1])
        if inner == "":
            return ""
        if " " in inner and not (inner.startswith("(") and inner.endswith(")")):
            inner = f"({inner})"
        lo, hi = node[2], node[3]
        if (lo, hi) == (0, None):
            return inner + "*"
        if (lo, hi) == (1, None):
            return inner + "+"
        if (lo, hi) == (0, 1):
            return inner + "?"
        if hi is None:
            return f"{inner} ~ {lo} {inner}*"
        return f"{inner} ~ {lo}..{hi}" if lo != hi else f"{inner} ~ {lo}"
    raise AssertionError(kind)


HEADER = """\
// toke v0.4 -- Lark grammar for grammar-constrained decoding and parsing (story 131.52).
//
// GENERATED from docs/spec/toke.gbnf by scripts/gbnf_to_lark.py -- do not edit.
// Edit toke.gbnf, then run `python3 scripts/gbnf_to_lark.py --write`.
// `make check-grammar-artefacts` fails if this file differs from a fresh
// generation, and compares Lark / XGrammar / llguidance verdicts with the
// reference recogniser on test/gbnf/fragments.json; `make check-gbnf` ties
// toke.gbnf itself to the parser (`tkc --dump-ast`).
//
// Character level: the grammar encodes the lexer too (see toke.gbnf's header),
// so every terminal is ONE character ("l" "e" "t", not "let"): a greedy
// lexeme lexer such as llguidance's would otherwise read the `let` inside
// `lettrue` as the keyword and change the language.  Load it
// with an Earley parser and the dynamic lexer, e.g.
//     Lark(open("toke.lark").read(), start="root", parser="earley", lexer="dynamic")
// or hand it to llguidance as a Lark grammar with `start: root` prepended.  Rule names are toke.gbnf's,
// with `-` spelled `_`.
"""


def generate(text: str | None = None) -> str:
    text = GBNF.read_text(encoding="utf-8") if text is None else text
    rules = _parse(text)
    seen: dict[str, str] = {}
    out = [HEADER]
    for name, node in rules:
        ln = lark_name(name)
        if ln in seen:
            raise GbnfError(f"`{name}` and `{seen[ln]}` collide as `{ln}`")
        seen[ln] = name
        body = _emit(node, top=True)
        out.append(f"{ln}: {body}\n")
    return "\n".join(out[:1]) + "\n" + "".join(out[1:])


# ----------------------------------------------------------------- gate ----
def _fragments() -> list[dict]:
    return json.load(open(FRAGMENTS, encoding="utf-8"))["fragments"]


def _engine_lark(lark_text: str):
    try:
        import lark  # noqa: F401
    except ImportError:
        return None
    from lark import Lark
    from lark.exceptions import LarkError
    p = Lark(lark_text, start="root", parser="earley", lexer="dynamic")

    def accepts(s: str) -> bool:
        try:
            p.parse(s)
            return True
        except LarkError:
            return False
    return accepts


def _engine_xgrammar(gbnf_text: str):
    try:
        import xgrammar as xgr
        from xgrammar.testing import _is_grammar_accept_string
    except ImportError:
        return None
    g = xgr.Grammar.from_ebnf(gbnf_text, root_rule_name="root")
    return lambda s: bool(_is_grammar_accept_string(g, s))


def _engine_llguidance(lark_text: str):
    try:
        import llguidance as llg
    except ImportError:
        return None
    # A byte-level tokenizer (one token per byte) makes the matcher a
    # character-level recogniser: no tokenizer vocabulary is involved here.
    import llguidance.tiktoken as lt  # noqa: F401
    tok = _byte_tokenizer(llg)
    # llguidance starts from a rule named `start`
    grammar = llg.grammar_from("lark", "start: root\n" + lark_text)
    is_err, msgs = llg.LLMatcher.validate_grammar_with_warnings(grammar, tok)
    if is_err:
        raise GbnfError("llguidance rejects toke.lark: " + "; ".join(msgs))

    def accepts(s: str) -> bool:
        m = llg.LLMatcher(tok, grammar, log_level=0)
        for b in s.encode("utf-8"):
            if not m.consume_token(b):
                return False
        return m.is_accepting()
    return accepts


def _byte_tokenizer(llg):
    class _Bytes:
        """256 one-byte tokens plus an EOS token: the identity tokenizer."""
        eos_token_id = 256
        bos_token_id = None
        tokens = [bytes([i]) for i in range(256)] + [b"<eos>"]
        special_token_ids = [256]

        def __call__(self, s):
            return list(s.encode("utf-8") if isinstance(s, str) else s)
    return llg.LLTokenizer(llg.TokenizerWrapper(_Bytes()))


LARK_STRIDE = 8


def check(require_engines: bool, full: bool = False) -> int:
    fail = False
    want = generate()
    have = LARK.read_text(encoding="utf-8") if LARK.exists() else ""
    if want != have:
        print(f"[lark] FAIL: {LARK.relative_to(ROOT)} is not the generation of toke.gbnf; "
              "run `python3 scripts/gbnf_to_lark.py --write`")
        fail = True
    else:
        print(f"[lark] {LARK.relative_to(ROOT)} matches a fresh generation from toke.gbnf")

    gbnf_text = GBNF.read_text(encoding="utf-8")
    frags = _fragments()
    verdict: dict[str, bool] = {}

    def reference() -> dict[str, bool]:
        # computed once, and only when some engine is installed to compare with
        if not verdict:
            ref = Grammar(gbnf_text)
            t0 = time.time()
            verdict.update({f["id"]: ref.accepts(f["text"]) for f in frags})
            acc = sum(verdict.values())
            print(f"[ref ] gbnf_earley: {len(frags)} fragments, {acc} accepted, {len(frags) - acc} "
                  f"rejected ({time.time() - t0:.0f}s)")
        return verdict

    for name, mk, src in (("lark", _engine_lark, want), ("xgrammar", _engine_xgrammar, gbnf_text),
                          ("llguidance", _engine_llguidance, want)):
        try:
            accepts = mk(src)
        except Exception as e:  # an engine that cannot load the artefact is a failure
            print(f"[{name}] FAIL: cannot load the grammar: {e}")
            fail = True
            continue
        if accepts is None:
            print(f"[{name}] SKIPPED: not installed (pip install -r bench/mask_cost/requirements.txt)")
            fail = fail or require_engines
            continue
        reference()
        t0 = time.time()
        # Python Lark's Earley parser takes ~400s over the whole set on an M4 Max,
        # so the gate checks every LARK_STRIDE-th fragment unless --full.
        sub = frags if (full or name != "lark") else frags[::LARK_STRIDE]
        bad = [f["id"] for f in sub if accepts(f["text"]) != verdict[f["id"]]]
        what = "" if sub is frags else f" (every {LARK_STRIDE}th; --full for all)"
        print(f"[{name}] {len(sub) - len(bad)}/{len(sub)} fragments agree with the reference{what} "
              f"({time.time() - t0:.0f}s)")
        for i in bad[:10]:
            print(f"    disagrees: {i} (reference {'accepts' if verdict[i] else 'rejects'})")
        fail = fail or bool(bad)
    print("[grammar-artefacts] " + ("FAIL" if fail else "PASS"))
    return 1 if fail else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    g.add_argument("--stdout", action="store_true")
    ap.add_argument("--require-engines", action="store_true")
    ap.add_argument("--full", action="store_true", help="run Lark over every fragment")
    a = ap.parse_args(argv)
    if a.write:
        LARK.write_text(generate(), encoding="utf-8")
        print(f"wrote {LARK.relative_to(ROOT)}")
        return 0
    if a.stdout:
        sys.stdout.write(generate())
        return 0
    return check(a.require_engines, a.full)


if __name__ == "__main__":
    sys.exit(main())
