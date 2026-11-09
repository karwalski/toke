#!/usr/bin/env python3
"""gbnf_bench.py — CPU mask-cost micro-benchmark for toke.gbnf (story 131.52).

Grammar-constrained decoding pays, at every generated token, for a MASK: the
set of vocabulary entries the grammar allows next.  XGrammar-class engines
make that cheap by precompiling the grammar against the vocabulary; neither
XGrammar nor llama.cpp is installed on this machine, so this benchmark
measures the cost with the dependency-free Earley recogniser in
scripts/gbnf_earley.py, and reports it as OPERATION COUNTS, not wall time:
counts are deterministic and machine-independent, wall time of a Python
Earley parser says nothing about a C++ engine.  Nothing here is gated on
time.

Measured, per grammar, over the same programs:
  char level   Earley items created per input character, and the size of the
               character mask (allowed next characters out of printable ASCII
               plus \\t \\n \\r) at each position;
  token level  at a deterministic sample of positions, the vocabulary mask by
               a depth-first walk of the vocabulary trie (the textbook
               "trie x grammar" walk XGrammar optimises): trie nodes visited,
               Earley items created during the walk, and the mask size.
               Vocabulary: tiktoken cl100k_base (local cache only; tokens
               that are not valid UTF-8 on their own are dropped and counted).

Comparison grammar: a standard JSON GBNF (ECMA-404 / RFC 8259 shape, the
grammar every constrained-decoding paper benchmarks), embedded below, over the
SAME programs serialised as JSON (`tkc --dump-ast`), so both sides describe
identical content.  JSON is a data format, not a programming language; a
mainstream programming-language grammar (C or Python subset) is the
obvious next comparison and is not attempted here.

Usage:
  gbnf_bench.py [--programs N] [--positions K] [--json] [--no-vocab]
Output is a measurement printed to stdout, not a published claim: quote it only
with its lane (grammar, vocabulary, N programs, K positions) per TEMSpec §6.3.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import random
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
from gbnf_earley import Grammar  # noqa: E402

JSON_GBNF = r'''
root    ::= ws value ws
value   ::= object | array | string | number | "true" | "false" | "null"
object  ::= "{" ws ( member ( ws "," ws member )* ws )? "}"
member  ::= string ws ":" ws value
array   ::= "[" ws ( value ( ws "," ws value )* ws )? "]"
string  ::= "\"" ( [^"\\\x00-\x1f] | "\\" ( ["\\/bfnrt] | "u" [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F] ) )* "\""
number  ::= "-"? ( "0" | [1-9] [0-9]* ) ( "." [0-9]+ )? ( [eE] [-+]? [0-9]+ )?
ws      ::= [ \t\n\r]*
'''
ALPHABET = [chr(c) for c in range(32, 127)] + ["\t", "\n", "\r"]


def programs(n: int) -> list[str]:
    """The first n gate fragments both sides accept (tree-sitter + conformance seeds)."""
    frags = json.load(open(ROOT / "test" / "gbnf" / "fragments.json", encoding="utf-8"))["fragments"]
    g = Grammar.from_file(ROOT / "docs" / "spec" / "toke.gbnf")
    out = []
    for f in frags:
        if "~" in f["src"] or not f["src"].startswith(("tree-sitter/", "grammar/", "extra/x56")):
            continue
        if g.accepts(f["text"]):
            out.append(f["text"])
        if len(out) >= n:
            break
    return out


def as_json(text: str) -> str:
    tkc = os.environ.get("TKC", str(ROOT / "tkc"))
    fd, p = tempfile.mkstemp(suffix=".tk", prefix="gbench_")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as fh:
            fh.write(text)
        r = subprocess.run([tkc, "--dump-ast", p], capture_output=True, text=True, timeout=30)
    finally:
        os.unlink(p)
    if r.returncode != 0:
        raise RuntimeError("dump-ast failed")
    return json.dumps(json.loads(r.stdout), separators=(",", ":"))


def load_vocab() -> tuple[dict, int, int]:
    import tiktoken
    enc = tiktoken.get_encoding("cl100k_base")
    trie: dict = {}
    kept = dropped = 0
    for b in enc._mergeable_ranks:
        try:
            s = b.decode("utf-8")
        except UnicodeDecodeError:
            dropped += 1
            continue
        node = trie
        for ch in s:
            node = node.setdefault(ch, {})
        node[None] = True
        kept += 1
    return trie, kept, dropped


def token_mask(r, trie: dict) -> dict:
    """DFS the vocabulary trie under the recogniser state; counts only."""
    before = r.stats["items"]
    visited = allowed = 0
    stack = [(trie, iter(sorted(k for k in trie if k is not None)))]
    while stack:
        node, it = stack[-1]
        ch = next(it, None)
        if ch is None:
            stack.pop()
            if stack:
                r.pop()
            continue
        if not r.feed(ch):
            continue
        visited += 1
        child = node[ch]
        if child.get(None):
            allowed += 1
        stack.append((child, iter(sorted(k for k in child if k is not None))))
    return {"trie_nodes_visited": visited, "mask_size": allowed, "earley_items": r.stats["items"] - before}


def measure(g: Grammar, texts: list[str], positions: int, trie: dict | None) -> dict:
    items_per_char, mask_chars, tok = [], [], []
    for t in texts:
        r = g.recogniser()
        rng = random.Random(int(hashlib.sha256(t.encode()).hexdigest()[:8], 16))
        sample = set(rng.sample(range(len(t)), min(positions, len(t)))) if trie is not None else set()
        start_items = r.stats["items"]
        for i, ch in enumerate(t):
            mask_chars.append(len(r.allowed_chars(ALPHABET)))
            if i in sample:
                tok.append(token_mask(r, trie))
            if not r.feed(ch):
                raise RuntimeError(f"grammar rejects its own benchmark input at {i}")
        if not r.complete():
            raise RuntimeError("grammar rejects its own benchmark input at EOF")
        tok_items = sum(x["earley_items"] for x in tok[-len(sample):]) if sample else 0
        items_per_char.append((r.stats["items"] - start_items - tok_items) / len(t))

    def summ(xs: list[float]) -> dict:
        xs = sorted(xs)
        return {"median": round(statistics.median(xs), 2), "mean": round(statistics.fmean(xs), 2),
                "p90": round(xs[int(0.9 * (len(xs) - 1))], 2), "max": round(xs[-1], 2)} if xs else {}
    out = {"programs": len(texts), "chars": sum(len(t) for t in texts),
           "earley_items_per_char": summ(items_per_char), "char_mask_size": summ(mask_chars)}
    if tok:
        out["token_positions"] = len(tok)
        for k in ("trie_nodes_visited", "earley_items", "mask_size"):
            out[f"token_{k}"] = summ([x[k] for x in tok])
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--programs", type=int, default=8)
    ap.add_argument("--positions", type=int, default=6, help="token-mask positions sampled per program")
    ap.add_argument("--no-vocab", action="store_true", help="character level only")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    texts = programs(a.programs)
    js = [as_json(t) for t in texts]
    trie, kept, dropped = (None, 0, 0)
    if not a.no_vocab:
        try:
            trie, kept, dropped = load_vocab()
        except Exception as e:      # offline without the cache: char level only, said so
            print(f"SKIP token level: cl100k_base not loadable offline ({type(e).__name__}); "
                  "fetch with: python3 -c \"import tiktoken; tiktoken.get_encoding('cl100k_base')\"",
                  file=sys.stderr)
    tg = Grammar.from_file(ROOT / "docs" / "spec" / "toke.gbnf")
    jg = Grammar(JSON_GBNF)
    res = {"story": "131.52", "unit": "operation counts (not wall time)",
           "vocab": {"name": "cl100k_base", "tokens_kept": kept, "tokens_dropped_not_utf8": dropped} if trie else None,
           "toke": {"grammar": "docs/spec/toke.gbnf", "nonterminals": len(tg.names), "productions": len(tg.prods),
                    **measure(tg, texts, a.positions, trie)},
           "json": {"grammar": "embedded JSON GBNF over the same programs' --dump-ast",
                    "nonterminals": len(jg.names), "productions": len(jg.prods),
                    **measure(jg, js, a.positions, trie)}}
    if a.json:
        print(json.dumps(res, indent=1))
        return 0
    print(f"gbnf mask-cost benchmark ({res['unit']}); N = {len(texts)} programs, "
          f"{a.positions} token positions each, vocabulary {'cl100k_base' if trie else 'none'}")
    for k in ("toke", "json"):
        d = res[k]
        print(f"  {k:<5} {d['nonterminals']} nonterminals / {d['productions']} productions; {d['chars']} chars")
        print(f"        earley items per char  {d['earley_items_per_char']}")
        print(f"        char mask size         {d['char_mask_size']}")
        if "token_positions" in d:
            print(f"        token mask: trie nodes {d['token_trie_nodes_visited']}")
            print(f"                    items      {d['token_earley_items']}")
            print(f"                    allowed    {d['token_mask_size']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
