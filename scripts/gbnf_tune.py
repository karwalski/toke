#!/usr/bin/env python3
"""gbnf_tune.py — engine-tuned forms of a character-level GBNF, and their gate (story 131.141).

docs/spec/toke.gbnf is written at the CHARACTER level: the lexer is part of
the grammar, so `let` is three terminals, an identifier is a trie of
one-character rules and whitespace is a rule referenced between any two
tokens.  The 2026-11-23 mask-cost record (docs/about/reviews/mask-cost-2026-11-23.md)
measured what that form costs: tens of milliseconds per mask in XGrammar,
and llguidance stops on every program under its default limits.  This script
derives two engine-tuned forms of the SAME grammar, mechanically, so that
neither can drift from toke.gbnf:

  toke.gbnf ──lexeme_lark()──► docs/spec/toke-lexeme.lark     (llguidance)
     │
     └──xgrammar_tuned()──► docs/spec/toke-xgr.gbnf           (XGrammar)

lexeme_lark (llguidance runs a lexer in front of its Earley parser):
  * the LEXICAL rules (identifiers, numbers, strings, comments, whitespace
    runs) become regex lexemes, inlined to the bottom; a recursive rule
    (nested comments, string interpolation) is unrolled DEPTH levels, and
    deeper nesting is rejected -- a stated under-approximation of the parser;
  * a nullable lexical rule is TERM? at each use (TERM = the non-empty part);
    lexical rules with the same regex share one terminal;
  * a multi-character literal in a parser rule ("let", "||") is one lexeme;
  * `refactor` rules replace grammar rules by language-equivalent ones
    whose lexemes the engine can lex without backtracking.  For toke:
    whitespace is `(sp | comment)*`, two lexemes, because a single lexeme
    spanning ` (` could not give `(` back once the comment fails to open
    (llguidance's lexer is greedy and never backtracks).
  The GBNF already puts a required separator (ws1) wherever two word tokens
  would run together, so dropping the character-level lexer loses nothing;
  the gate measures that instead of assuming it.

xgrammar_tuned (XGrammar has no lexer; it compiles each rule to an automaton
and caches which tokens each automaton position accepts):
  * whitespace is INLINED: every `ws` / `ws1` reference is replaced by
    `[ \\t\\r\\n]* (comment [ \\t\\r\\n]*)*` (resp. the non-empty form), so a
    whitespace run is a character-class loop inside the referencing rule
    rather than a pushed rule;
  * LOOKAHEAD assertions `(= [^a-zA-Z0-9])` on the rules that end a word
    token (identifiers, numbers, scalar type names).  XGrammar uses an
    assertion only as a hint when it classifies tokens for its mask cache;
    string acceptance ignores it.  Each one listed is INTENDED to be implied
    by the grammar (toke.gbnf puts ws1 between word tokens, so a word
    character should never follow one); that is argued, not proved, and only
    sampled by --tokens below.  In XGrammar 0.2.9
    a deliberately wrong assertion changed no mask (negative control, story
    131.141), so `--check --tokens` -- every mask must allow the fragment's
    own next token -- guards the masks against a future engine that trusts
    the hint, rather than proving the hint right.

Usage:
  gbnf_tune.py --write     regenerate docs/spec/toke-lexeme.lark and toke-xgr.gbnf
  gbnf_tune.py --check     the gate (run by `make check-grammar-artefacts`):
      1. both artefacts are byte-identical to a fresh generation from toke.gbnf
         (standard library only; always runs);
      2. llguidance (toke-lexeme.lark) and XGrammar (toke-xgr.gbnf), when
         importable, agree with gbnf_earley on toke.gbnf -- itself tied to
         the parser by `make check-gbnf` -- on EVERY fragment of
         test/gbnf/fragments.json; an engine that is not installed is
         SKIPPED, never a pass (--require-engines makes that a failure).
         Agreement on the fragment set is evidence, not a proof that the
         tuned forms are equivalent to toke.gbnf: a production the fragments
         barely exercise can change unnoticed (removing `fn` from ident in
         both forms still agreed 2081/2081); check 1 is what catches a
         hand edit, and only a generator bug can get past it;
      --tokens  also fill XGrammar masks over the cl100k_base vocabulary at
         every token of every accepted fragment of a stride sample and fail if
         one disallows the fragment's own next token (the lookahead check;
         needs tiktoken and its cached vocabulary, slow: minutes).
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from gbnf_earley import GbnfError  # noqa: E402
from gbnf_to_lark import _emit as _lark_emit, _parse, lark_name  # noqa: E402

GBNF = ROOT / "docs" / "spec" / "toke.gbnf"
LEXEME = ROOT / "docs" / "spec" / "toke-lexeme.lark"
XGR = ROOT / "docs" / "spec" / "toke-xgr.gbnf"
DEPTH = 8

_EMPTY = None  # the empty LANGUAGE (no string), as opposed to the empty string


# ------------------------------------------------------------ AST -> regex --
def _rch(c: str) -> str:
    """One character as a Rust-regex literal (llguidance uses the regex crate)."""
    o = ord(c)
    if c in "\\.+*?()|[]{}^$#&-~":
        return "\\" + c
    if 33 <= o < 127 or c == " ":
        return c
    return f"\\x{{{o:x}}}"


def _rcls(neg: bool, ranges) -> str:
    def one(o: int) -> str:
        c = chr(o)
        if c in "\\]^-[&~":
            return "\\" + c
        return c if 33 <= o < 127 or c == " " else f"\\x{{{o:x}}}"
    return "[" + ("^" if neg else "") + "".join(
        one(lo) if lo == hi else f"{one(lo)}-{one(hi)}" for lo, hi in ranges) + "]"


class _Regexer:
    def __init__(self, rules: dict, depth: int):
        self.rules, self.depth, self.stack = rules, depth, []

    def rx(self, node) -> str | None:
        """The regex of `node`, or _EMPTY when it denotes no string at all
        (a recursion cut at the depth bound)."""
        k = node[0]
        if k == "alt":
            parts = [p for p in (self.rx(s) for s in node[1]) if p is not _EMPTY]
            if not parts:
                return _EMPTY
            return parts[0] if len(parts) == 1 else "(?:" + "|".join(parts) + ")"
        if k == "seq":
            out = []
            for x in node[1]:
                p = self.rx(x)
                if p is _EMPTY:
                    return _EMPTY
                out.append(p)
            return "".join(out)
        if k == "str":
            return "".join(_rch(c) for c in node[1])
        if k == "cls":
            return _rcls(*node[1])
        if k == "any":
            return "(?s:.)"
        if k == "name":
            n = node[1]
            if self.stack.count(n) >= self.depth:
                return _EMPTY
            self.stack.append(n)
            try:
                return self.rx(self.rules[n])
            finally:
                self.stack.pop()
        if k == "rep":
            inner = self.rx(node[1])
            lo, hi = node[2], node[3]
            if inner is _EMPTY:
                return "" if lo == 0 else _EMPTY
            if inner == "":
                return ""
            g = f"(?:{inner})"
            if (lo, hi) == (0, None):
                return g + "*"
            if (lo, hi) == (1, None):
                return g + "+"
            if (lo, hi) == (0, 1):
                return g + "?"
            return g + ("{%d,}" % lo if hi is None else "{%d,%d}" % (lo, hi))
        raise AssertionError(k)


def _nullable(node, rules, seen=()) -> bool:
    k = node[0]
    if k == "alt":
        return any(_nullable(s, rules, seen) for s in node[1])
    if k == "seq":
        return all(_nullable(x, rules, seen) for x in node[1])
    if k == "str":
        return node[1] == ""
    if k in ("cls", "any"):
        return False
    if k == "name":
        return node[1] not in seen and _nullable(rules[node[1]], rules, seen + (node[1],))
    if k == "rep":
        return node[2] == 0 or _nullable(node[1], rules, seen)
    raise AssertionError(k)


def _nullable_rx(r: str) -> bool:
    """Conservative: True unless `r` is one character class or literal run
    (the only bodies the (?:X)* -> (?:X)+ rewrite is applied to)."""
    import re
    return not re.fullmatch(r"(\[[^\]]*\]|[^()|*+?\[\\]|\\.)+", r)


def _refs(node) -> list[str]:
    k = node[0]
    if k in ("alt", "seq"):
        return [r for x in node[1] for r in _refs(x)]
    if k == "rep":
        return _refs(node[1])
    return [node[1]] if k == "name" else []


def _map(node, f):
    """Rebuild `node` bottom-up, replacing each leaf by f(leaf) (None keeps it)."""
    k = node[0]
    if k in ("alt", "seq"):
        return (k, [_map(x, f) for x in node[1]])
    if k == "rep":
        return ("rep", _map(node[1], f), node[2], node[3])
    r = f(node)
    return node if r is None else r


def _rules(gbnf_text: str, refactor: str) -> list[tuple[str, object]]:
    parsed = _parse(gbnf_text)
    if refactor:
        repl = dict(_parse(refactor))
        parsed = [(n, repl.pop(n, node)) for n, node in parsed] + list(repl.items())
    return parsed


def _reachable(rules: dict, stop: set[str]) -> set[str]:
    reach, todo = set(), ["root"]
    while todo:
        n = todo.pop()
        if n in reach or n in stop:
            continue
        reach.add(n)
        todo += _refs(rules[n])
    return reach


# ----------------------------------------------------- lexeme-level Lark ----
def lexeme_lark(gbnf_text: str, lexical: list[str], depth: int, header: str,
                refactor: str = "") -> str:
    """Lark text for llguidance: the parser rules of `gbnf_text` with its
    `lexical` rules as regex lexemes."""
    parsed = _rules(gbnf_text, refactor)
    rules = dict(parsed)
    missing = [n for n in lexical if n not in rules]
    if missing:
        raise GbnfError(f"lexical rules not in the grammar: {missing}")
    lex = set(lexical)
    by_regex: dict[str, str] = {}
    term: dict[str, tuple[str, bool]] = {}
    defs: list[str] = []
    for n in lexical:
        r = _Regexer(rules, depth).rx(("name", n))
        if r is _EMPTY:
            raise GbnfError(f"lexical rule `{n}` denotes no string at depth {depth}")
        nul = _nullable(rules[n], rules)
        if nul and r.startswith("(?:") and r.endswith(")*") and _balanced(r[3:-2]) \
                and not _nullable_rx(r[3:-2]):
            r = r[:-1] + "+"          # (?:X)* without the empty string is (?:X)+
            key = r
        else:
            key = r + (' & ~""' if nul else "")
        if key not in by_regex:
            t = lark_name(n).upper()
            by_regex[key] = t
            defs.append(f"{t}: /{key}/" if key == r else f'{t}: /{r}/ & ~""')
        term[n] = (by_regex[key], nul)

    def leaf(node):
        if node[0] == "name" and node[1] in lex:
            t, nul = term[node[1]]
            return ("rep", ("term", t), 0, 1) if nul else ("term", t)
        return None

    reach = _reachable(rules, lex)
    out = [header]
    for n, node in parsed:
        if n in reach:
            out.append(f"{lark_name(n)}: {_lk(_map(node, leaf), top=True)}\n")
    out.append("\n// lexemes: the lexical rules, inlined to regular expressions\n")
    out += [d + "\n" for d in defs]
    return "".join(out)


def _lk(node, top: bool = False) -> str:
    """gbnf_to_lark's printer, except that a literal stays ONE lexeme and
    ("term", T) is a terminal reference."""
    k = node[0]
    if k == "term":
        return node[1]
    if k == "str":
        return _lark_str(node[1]) if node[1] else ""
    if k == "alt":
        seqs = [_lk(s) for s in node[1]]
        empty = any(s == "" for s in seqs)
        seqs = [s for s in seqs if s != ""]
        if not seqs:
            return ""
        if top:
            return (("\n    | ".join(seqs) if len(seqs) > 3 else " | ".join(seqs))
                    + (" |" if empty else ""))
        inner = " | ".join(seqs)
        if empty:
            return f"({inner})?"
        return inner if len(seqs) == 1 and " " not in inner else f"({inner})"
    if k == "seq":
        return " ".join(p for p in (_lk(x) for x in node[1]) if p != "")
    if k == "rep":
        inner = _lk(node[1])
        if inner == "":
            return ""
        if " " in inner and not (inner.startswith("(") and inner.endswith(")")):
            inner = f"({inner})"
        return inner + _suffix(node[2], node[3], inner, lark=True)
    return _lark_emit(node, top)


def _suffix(lo, hi, inner: str, lark: bool) -> str:
    if (lo, hi) == (0, None):
        return "*"
    if (lo, hi) == (1, None):
        return "+"
    if (lo, hi) == (0, 1):
        return "?"
    if lark:
        if hi is None:
            return f" ~ {lo} {inner}*"
        return f" ~ {lo}..{hi}" if lo != hi else f" ~ {lo}"
    return "{%d,}" % lo if hi is None else ("{%d}" % lo if lo == hi else "{%d,%d}" % (lo, hi))


def _lark_str(s: str) -> str:
    out = []
    for c in s:
        o = ord(c)
        if c in '"\\':
            out.append("\\" + c)
        elif 32 <= o < 127:
            out.append(c)
        else:
            out.append({"\n": "\\n", "\t": "\\t", "\r": "\\r"}.get(
                c, f"\\x{o:02x}" if o < 256 else f"\\u{o:04x}"))
    return '"' + "".join(out) + '"'


# ------------------------------------------------- XGrammar-tuned GBNF ------
def xgrammar_tuned(gbnf_text: str, inline: dict[str, str], lookahead: dict[str, str],
                   header: str) -> str:
    """GBNF text for XGrammar: each rule in `inline` (name -> GBNF expression)
    is substituted at every reference and dropped; each rule in `lookahead`
    (name -> GBNF lookahead body, e.g. `[^a-z]`) gets `(= body)`."""
    parsed = _parse(gbnf_text)
    rules = dict(parsed)
    exprs = {}
    for n, e in inline.items():
        if n not in rules:
            raise GbnfError(f"inline rule `{n}` not in the grammar")
        exprs[n] = _parse(f"x ::= {e}")[0][1]
    for n in lookahead:
        if n not in rules:
            raise GbnfError(f"lookahead rule `{n}` not in the grammar")

    def leaf(node):
        if node[0] == "name" and node[1] in exprs:
            return exprs[node[1]]
        return None

    body = {n: _map(node, leaf) for n, node in parsed if n not in exprs}
    reach = _reachable(body, set())
    width = max(len(n) for n in body)
    out = [header]
    for n, _ in parsed:
        if n in body and n in reach:
            text = _gb(body[n], top=True)
            if n in lookahead:
                text = f"({text}) (= {lookahead[n]})" if "|" in text else f"{text} (= {lookahead[n]})"
            out.append(f"{n:<{width}} ::= {text}\n")
    return "".join(out)


def _gstr(s: str) -> str:
    out = []
    for c in s:
        o = ord(c)
        if c in '"\\':
            out.append("\\" + c)
        elif 32 <= o < 127:
            out.append(c)
        else:
            out.append({"\n": "\\n", "\t": "\\t", "\r": "\\r"}.get(
                c, f"\\x{o:02x}" if o < 256 else f"\\u{o:04x}"))
    return '"' + "".join(out) + '"'


def _gcls(neg: bool, ranges) -> str:
    def one(o: int) -> str:
        c = chr(o)
        if c in "\\]^-[":
            return "\\" + c
        if 32 <= o < 127:
            return c
        return {"\n": "\\n", "\t": "\\t", "\r": "\\r"}.get(c, f"\\x{o:02x}" if o < 256 else f"\\u{o:04x}")
    return "[" + ("^" if neg else "") + "".join(
        one(lo) if lo == hi else f"{one(lo)}-{one(hi)}" for lo, hi in ranges) + "]"


def _gb(node, top: bool = False) -> str:
    """A GBNF printer for gbnf_to_lark's AST."""
    k = node[0]
    if k == "str":
        return _gstr(node[1])
    if k == "cls":
        return _gcls(*node[1])
    if k == "any":
        return "."
    if k == "name":
        return node[1]
    if k == "alt":
        seqs = [_gb(s) for s in node[1]]
        seqs = [s if s else '""' for s in seqs]
        inner = " | ".join(seqs)
        if top or len(seqs) == 1:
            return inner
        return f"({inner})"
    if k == "seq":
        parts = [_gb(x) for x in node[1]]
        return " ".join(p for p in parts if p) if parts else ""
    if k == "rep":
        inner = _gb(node[1])
        atom = node[1][0] in ("str", "cls", "any", "name")
        if not atom and not (inner.startswith("(") and inner.endswith(")") and _balanced(inner[1:-1])):
            inner = f"({inner})"
        return inner + _suffix(node[2], node[3], inner, lark=False)
    raise AssertionError(k)


def _balanced(s: str) -> bool:
    """True when the parentheses of `s` (outside strings and classes) balance
    without ever going negative: `(a) (b)` is not one group."""
    d, i = 0, 0
    while i < len(s):
        c = s[i]
        if c == '"' or c == "[":
            end = '"' if c == '"' else "]"
            i += 1
            while i < len(s) and s[i] != end:
                i += 2 if s[i] == "\\" else 1
        elif c == "(":
            d += 1
        elif c == ")":
            d -= 1
            if d < 0:
                return False
        i += 1
    return d == 0


# --------------------------------------------------------- toke config ------
TOKE_LEXICAL = ["sp", "comment", "ident", "vname", "cname", "typeident", "int", "float",
                "string", "scalar"]
# language-equivalent to toke.gbnf's ws/ws1 ( ([ \t\r\n] | comment)* and + )
TOKE_REFACTOR = """
ws  ::= (sp | comment)*
ws1 ::= (sp | comment)+
sp  ::= [ \\t\\r\\n]+
"""
_SP = "[ \\t\\r\\n]"
TOKE_INLINE = {
    "ws": f"{_SP}* (comment {_SP}*)*",
    "ws1": f"{_SP}+ (comment {_SP}*)* | comment {_SP}* (comment {_SP}*)*",
}
WORD_END = "[^a-zA-Z0-9]"
TOKE_LOOKAHEAD = {n: WORD_END for n in
                  ("ident", "vname", "cname", "typeident", "int", "float", "scalar")}

LEXEME_HEADER = """\
// toke v0.4 -- LEXEME-level grammar for llguidance (story 131.141).
//
// GENERATED from docs/spec/toke.gbnf by scripts/gbnf_tune.py -- do not edit.
// Edit toke.gbnf, then run `python3 scripts/gbnf_tune.py --write`.
// `make check-grammar-artefacts` fails if this file differs from a fresh
// generation, and (llguidance installed) if llguidance's verdict differs from
// the reference recogniser's on any fragment of test/gbnf/fragments.json.
//
// The parser rules are toke.gbnf's (`-` spelled `_`); its lexical rules
// (identifiers, numbers, strings, comments, whitespace runs) are regex
// lexemes below.  Nested comments and string interpolation are unrolled to
// %d levels of nesting; deeper nesting is rejected (the parser accepts it).
// Load with `start: root` prepended:
//     llguidance.grammar_from("lark", "start: root\\n" + text)
// toke.lark (one character per terminal) is the form for Lark's own parser.

""" % DEPTH

XGR_HEADER = """\
# toke v0.4 -- GBNF tuned for XGrammar (story 131.141).
#
# GENERATED from docs/spec/toke.gbnf by scripts/gbnf_tune.py -- do not edit.
# Edit toke.gbnf, then run `python3 scripts/gbnf_tune.py --write`.
# `make check-grammar-artefacts` fails if this file differs from a fresh
# generation, and (XGrammar installed) if XGrammar's verdict differs from the
# reference recogniser's on any fragment of test/gbnf/fragments.json.
#
# The same language as toke.gbnf, written for XGrammar's automata: every
# ws / ws1 reference is inlined as a whitespace character-class loop (comments
# stay a rule), and the rules that end a word token carry the lookahead
# assertion (= [^a-zA-Z0-9]), which XGrammar uses only to classify tokens
# when it builds its mask cache.  For other engines, use toke.gbnf.

"""


def toke_lexeme(text: str | None = None) -> str:
    text = GBNF.read_text(encoding="utf-8") if text is None else text
    return lexeme_lark(text, TOKE_LEXICAL, DEPTH, LEXEME_HEADER, TOKE_REFACTOR)


def toke_xgr(text: str | None = None) -> str:
    text = GBNF.read_text(encoding="utf-8") if text is None else text
    return xgrammar_tuned(text, TOKE_INLINE, TOKE_LOOKAHEAD, XGR_HEADER)


# ----------------------------------------------------------------- engines --
def llg_acceptor(lark_text: str):
    """A string acceptor for llguidance over a byte tokenizer, or None."""
    try:
        import llguidance as llg
    except ImportError:
        return None
    from gbnf_to_lark import _byte_tokenizer
    tok = _byte_tokenizer(llg)
    grammar = llg.grammar_from("lark", "start: root\n" + lark_text)
    is_err, msgs = llg.LLMatcher.validate_grammar_with_warnings(grammar, tok)
    if is_err:
        raise GbnfError("llguidance rejects the grammar: " + "; ".join(msgs))

    def accepts(s: str) -> bool:
        m = llg.LLMatcher(tok, grammar, log_level=0)
        for b in s.encode("utf-8"):
            if not m.consume_token(b):
                return False
        return m.is_accepting()
    return accepts


def xgr_acceptor(gbnf_text: str):
    try:
        import xgrammar as xgr
        from xgrammar.testing import _is_grammar_accept_string
    except ImportError:
        return None
    g = xgr.Grammar.from_ebnf(gbnf_text, root_rule_name="root")
    return lambda s: bool(_is_grammar_accept_string(g, s))


def xgr_token_check(gbnf_text: str, texts: list[str]) -> tuple[int, int, list[str]]:
    """Fill an XGrammar mask (cl100k_base) before every token of every text and
    count the tokens the mask disallows although the text continues with them.
    Returns (positions, disallowed, examples)."""
    import numpy as np
    import xgrammar as xgr
    sys.path.insert(0, str(ROOT / "bench" / "mask_cost"))
    from mask_cost import cl100k
    enc, vocab, info = cl100k()
    ti = xgr.TokenizerInfo(vocab, xgr.VocabType.RAW, vocab_size=info["n_vocab"],
                           stop_token_ids=[info["eos"]])
    cg = xgr.GrammarCompiler(ti, max_threads=1, cache_enabled=False).compile_grammar(gbnf_text)
    bm = xgr.allocate_token_bitmask(1, ti.vocab_size)
    n = bad = 0
    ex: list[str] = []
    for t in texts:
        m = xgr.GrammarMatcher(cg)
        toks = enc.encode_ordinary(t)
        for i, tok in enumerate(toks):
            m.fill_next_token_bitmask(bm)
            a = bm.numpy().view(np.uint32)[0]
            n += 1
            if not (a[tok >> 5] >> (tok & 31)) & 1:
                bad += 1
                if len(ex) < 5:
                    ex.append(f"{enc.decode(toks[max(0, i - 6):i])!r} | {enc.decode([tok])!r}")
            if not m.accept_token(tok):
                break
    return n, bad, ex


# -------------------------------------------------------------------- gate --
def check(require_engines: bool, tokens: bool, stride: int) -> int:
    import json
    from gbnf_earley import Grammar
    fail = False
    for path, want in ((LEXEME, toke_lexeme()), (XGR, toke_xgr())):
        have = path.read_text(encoding="utf-8") if path.exists() else ""
        rel = path.relative_to(ROOT)
        if want != have:
            print(f"[tuned] FAIL: {rel} is not the generation of toke.gbnf; "
                  "run `python3 scripts/gbnf_tune.py --write`")
            fail = True
        else:
            print(f"[tuned] {rel} matches a fresh generation from toke.gbnf")
    frags = json.load(open(ROOT / "test" / "gbnf" / "fragments.json", encoding="utf-8"))["fragments"]
    verdict: dict[str, bool] = {}

    def reference() -> dict[str, bool]:
        if not verdict:
            ref = Grammar(GBNF.read_text(encoding="utf-8"))
            t0 = time.time()
            verdict.update({f["id"]: ref.accepts(f["text"]) for f in frags})
            acc = sum(verdict.values())
            print(f"[ref  ] gbnf_earley on toke.gbnf: {len(frags)} fragments, {acc} accepted, "
                  f"{len(frags) - acc} rejected ({time.time() - t0:.0f}s)")
        return verdict

    xgr_text = XGR.read_text(encoding="utf-8") if XGR.exists() else ""
    for name, mk, src in (("llguidance", llg_acceptor, LEXEME.read_text(encoding="utf-8")
                           if LEXEME.exists() else ""),
                          ("xgrammar", xgr_acceptor, xgr_text)):
        tag = f"[{name}:{'toke-lexeme.lark' if name == 'llguidance' else 'toke-xgr.gbnf'}]"
        try:
            accepts = mk(src)
        except Exception as e:  # an engine that cannot load the artefact is a failure
            print(f"{tag} FAIL: cannot load the grammar: {e}")
            fail = True
            continue
        if accepts is None:
            print(f"{tag} SKIPPED: not installed (pip install -r bench/mask_cost/requirements.txt)")
            fail = fail or require_engines
            continue
        reference()
        t0 = time.time()
        bad = [f["id"] for f in frags if accepts(f["text"]) != verdict[f["id"]]]
        print(f"{tag} {len(frags) - len(bad)}/{len(frags)} fragments agree with the reference "
              f"({time.time() - t0:.0f}s)")
        for i in bad[:10]:
            print(f"    disagrees: {i} (reference {'accepts' if verdict[i] else 'rejects'})")
        fail = fail or bool(bad)
    if tokens:
        texts = [f["text"] for f in frags if reference()[f["id"]]][::stride]
        t0 = time.time()
        n, bad, ex = xgr_token_check(xgr_text, texts)
        print(f"[xgrammar:tokens] {len(texts)} accepted fragments (every {stride}th), {n} masks, "
              f"{bad} disallow the fragment's own next token ({time.time() - t0:.0f}s)")
        for e in ex:
            print(f"    disallowed: {e}")
        fail = fail or bad > 0
    print("[grammar-tuned] " + ("FAIL" if fail else "PASS"))
    return 1 if fail else 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--write", action="store_true")
    g.add_argument("--check", action="store_true")
    ap.add_argument("--require-engines", action="store_true")
    ap.add_argument("--tokens", action="store_true", help="also check XGrammar masks (slow)")
    ap.add_argument("--stride", type=int, default=8, help="--tokens: every N-th accepted fragment")
    a = ap.parse_args(argv)
    if a.write:
        LEXEME.write_text(toke_lexeme(), encoding="utf-8")
        XGR.write_text(toke_xgr(), encoding="utf-8")
        print(f"wrote {LEXEME.relative_to(ROOT)} and {XGR.relative_to(ROOT)}")
        return 0
    return check(a.require_engines, a.tokens, a.stride)


if __name__ == "__main__":
    sys.exit(main())
