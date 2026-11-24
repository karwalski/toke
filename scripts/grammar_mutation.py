#!/usr/bin/env python3
"""grammar_mutation.py — alternative-deletion mutation score for an agreement check (story 131.149).

An agreement check says "engine form F accepts exactly what the reference
grammar accepts, on this input set".  It is only as strong as its inputs: if
no input contains `**`, deleting `**` from F still agrees.  This module
measures that.  It deletes ONE alternative of ONE alternation at a time from
the engine form (a mutant), runs the mutant over the agreement inputs, and
calls the mutant KILLED if some input's verdict differs from the reference's.
A mutant that survives either describes the same language (an equivalent
mutant) or marks a production no input exercises.

Alternations, in both dialects, at every nesting depth:
  * rule alternatives      `a | b` in a rule body or a parenthesised group;
  * lexeme alternatives    `a|b` inside a /regex/ (Lark only; the GBNF forms
                           spell their lexical rules as rules).
An alternative is FIXED-STRING when it is a single literal: a "..." in a rule,
or a regex alternative with no class, group, quantifier or anchor.

Library:
  mutants(text, dialect) -> [Mutant]          dialect "lark" or "gbnf"
  score(mutants, text, make_acceptor, inputs, reference) -> report dict
CLI:
  grammar_mutation.py --toke [--limit N]       report-only score for
      docs/spec/toke-lexeme.lark and toke-xgr.gbnf against
      test/gbnf/fragments.json (reference: gbnf_earley on toke.gbnf)
The Python-subset score is `bench/mask_cost/python_subset_gbnf.py --mutation`.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))


@dataclass
class Mutant:
    rule: str          # the rule or terminal the alternation is in
    kind: str          # "rule" or "lexeme"
    fixed: bool        # the deleted alternative is a single literal
    alt: str           # the deleted alternative's text
    start: int         # [start, end) is deleted from the grammar text
    end: int
    killed_by: str | None = None
    status: str = ""   # killed | survived | invalid | unreachable
    note: str = ""

    def label(self) -> str:
        a = self.alt if len(self.alt) <= 60 else self.alt[:57] + "..."
        return f"{self.rule} [{self.kind}] -{a!r}"

    def apply(self, text: str) -> str:
        return text[:self.start] + text[self.end:]


@dataclass
class _Group:
    start: int
    seps: list = field(default_factory=list)


def _deletions(rule: str, kind: str, g: _Group, end: int, text: str, fixed) -> list[Mutant]:
    """One mutant per alternative of the group [g.start, end) with separators g.seps."""
    if not g.seps:
        return []
    bounds = [g.start] + [s + 1 for s in g.seps]
    ends = list(g.seps) + [end]
    out = []
    for k, (a, b) in enumerate(zip(bounds, ends)):
        alt = text[a:b].strip()
        if k < len(g.seps):          # the alternative and the separator after it
            s, e = a, g.seps[k] + 1
        else:                        # the last: the separator before it and it
            s, e = g.seps[k - 1], b
        out.append(Mutant(rule, kind, fixed(alt), alt, s, e))
    return out


_RX_META = set("[]().*+?{}|^$")


def _rx_fixed(alt: str) -> bool:
    i = 0
    if not alt:
        return False
    while i < len(alt):
        c = alt[i]
        if c == "\\":
            if i + 1 < len(alt) and alt[i + 1] in "dDwWsSbB":
                return False
            if alt[i + 1:i + 3] == "x{":
                i = alt.index("}", i) + 1
                continue
            i += 2
            continue
        if c in _RX_META:
            return False
        i += 1
    return True


def _str_fixed(alt: str) -> bool:
    if len(alt) < 2 or alt[0] != '"' or alt[-1] != '"':
        return False
    i = 1
    while i < len(alt) - 1:
        if alt[i] == "\\":
            i += 2
            continue
        if alt[i] == '"':
            return False
        i += 1
    return True


def _regex(text: str, i: int, rule: str, out: list[Mutant]) -> int:
    """Scan a /regex/ whose opening slash is at i; return the index after the
    closing slash (and any flags).  Adds the regex's alternation mutants."""
    j = i + 1
    stack = [_Group(j)]
    while True:
        c = text[j]
        if c == "\\":
            j += 2
            continue
        if c == "[":
            j += 1
            if text[j] == "^":
                j += 1
            if text[j] == "]":
                j += 1
            while text[j] != "]":
                j += 2 if text[j] == "\\" else 1
            j += 1
            continue
        if c == "(":
            j += 1
            if text[j] == "?":         # (?:  (?=  (?!  ...
                j += 2
            stack.append(_Group(j))
            continue
        if c == ")":
            g = stack.pop()
            out += _deletions(rule, "lexeme", g, j, text, _rx_fixed)
            j += 1
            continue
        if c == "|":
            stack[-1].seps.append(j)
            j += 1
            continue
        if c == "/":
            out += _deletions(rule, "lexeme", stack[0], j, text, _rx_fixed)
            j += 1
            while j < len(text) and text[j].isalpha():
                j += 1
            return j
        j += 1


def mutants(text: str, dialect: str) -> list[Mutant]:
    """Every single-alternative deletion in a Lark ("lark") or GBNF ("gbnf") grammar."""
    out: list[Mutant] = []
    defsep = ":" if dialect == "lark" else "::="
    comment = "//" if dialect == "lark" else "#"
    lines = text.split("\n")
    pos = 0
    rule = None
    stack: list[_Group] = []
    refs: dict[str, set] = {}
    word = set("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-")

    def close_rule(at: int) -> None:
        if rule is not None and stack:
            out.extend(_deletions(rule, "rule", stack[0], at, text, _str_fixed))

    body_end = 0
    for line in lines:
        lstart, lend = pos, pos + len(line)
        pos = lend + 1
        if not line.strip() or line.lstrip().startswith(comment):
            continue
        i = lstart
        if not line[0].isspace():                 # a new rule
            close_rule(body_end)
            k = line.index(defsep)
            rule = line[:k].strip()
            refs.setdefault(rule, set())
            i = lstart + k + len(defsep)
            stack = [_Group(i)]
        while i < lend:
            c = text[i]
            if c == '"':
                i += 1
                while text[i] != '"':
                    i += 2 if text[i] == "\\" else 1
                i += 1
                continue
            if dialect == "gbnf" and c == "[":
                i += 1
                while text[i] != "]":
                    i += 2 if text[i] == "\\" else 1
                i += 1
                continue
            if dialect == "lark" and c == "/":
                i = _regex(text, i, rule, out)
                continue
            if c == "(":
                stack.append(_Group(i + 1))
            elif c == ")":
                g = stack.pop()
                out.extend(_deletions(rule, "rule", g, i, text, _str_fixed))
            elif c == "|":
                stack[-1].seps.append(i)
            elif c.isalpha() or c == "_":
                j = i
                while j < lend and text[j] in word:
                    j += 1
                refs[rule].add(text[i:j])
                i = j
                continue
            i += 1
        body_end = lend
    close_rule(body_end)
    # a rule no derivation from `root` uses cannot change the language
    live, todo = set(), ["root"]
    while todo:
        r = todo.pop()
        if r in live or r not in refs:
            continue
        live.add(r)
        todo += refs[r]
    for m in out:
        if m.rule not in live:
            m.status, m.note = "unreachable", "not reachable from root"
    return out


_W: dict = {}


def _init(text, make_acceptor, inputs, reference) -> None:
    _W.update(text=text, mk=make_acceptor, inputs=inputs, reference=reference)


def _run(span: tuple[int, int]) -> tuple[str, str | None, str]:
    """(status, killed_by, note) of the mutant that deletes text[span]."""
    text = _W["text"]
    try:
        acc = _W["mk"](text[:span[0]] + text[span[1]:])
    except Exception as e:  # noqa: BLE001 -- an engine's load error
        return "invalid", None, str(e)[:200]
    for (label, t), want in zip(_W["inputs"], _W["reference"]):
        if acc(t) != want:
            return "killed", label, ""
    return "survived", None, ""


def score(muts: list[Mutant], text: str, make_acceptor, inputs: list[tuple[str, str]],
          reference: list[bool], progress: bool = False, jobs: int | None = None) -> dict:
    """Run every mutant over `inputs` [(label, text)] until one input's verdict
    differs from `reference` (the mutant is killed) or none does (it survives).
    `make_acceptor(grammar_text)` returns a str -> bool function, and raises if
    the engine cannot load the mutant (status invalid).  It must be a
    module-level function: mutants run in `jobs` worker processes."""
    import multiprocessing as mp
    from concurrent.futures import ProcessPoolExecutor
    t0 = time.time()
    if not muts:
        return summarise(muts)
    jobs = jobs or os.cpu_count() or 1
    with ProcessPoolExecutor(min(jobs, len(muts)), mp_context=mp.get_context("spawn"),
                             initializer=_init,
                             initargs=(text, make_acceptor, inputs, reference)) as ex:
        run = [m for m in muts if m.status != "unreachable"]
        res = ex.map(_run, [(m.start, m.end) for m in run])
        for n, (m, (status, killed_by, note)) in enumerate(zip(run, res)):
            m.status, m.killed_by, m.note = status, killed_by, note
            if progress and n % 100 == 99:
                print(f"    ... {n + 1}/{len(muts)} mutants ({time.time() - t0:.0f}s)",
                      file=sys.stderr, flush=True)
    return summarise(muts)


def summarise(muts: list[Mutant]) -> dict:
    def count(sel):
        ms = [m for m in muts if sel(m)]
        return {"mutants": len(ms), "killed": sum(m.status == "killed" for m in ms),
                "survived": sum(m.status == "survived" for m in ms),
                "invalid": sum(m.status == "invalid" for m in ms),
                "unreachable": sum(m.status == "unreachable" for m in ms)}
    return {"all": count(lambda m: True),
            "lexeme": count(lambda m: m.kind == "lexeme"),
            "lexeme_fixed": count(lambda m: m.kind == "lexeme" and m.fixed),
            "rule": count(lambda m: m.kind == "rule"),
            "survivors": [m.label() for m in muts if m.status == "survived"],
            "invalid": [m.label() + " :: " + m.note for m in muts if m.status == "invalid"]}


def print_report(tag: str, rep: dict, show: int = 40) -> None:
    for k, title in (("all", "all alternatives"), ("lexeme", "regex-lexeme alternatives"),
                     ("lexeme_fixed", "  of which fixed strings"), ("rule", "rule alternatives")):
        r = rep[k]
        if r["mutants"]:
            print(f"{tag} {title}: {r['killed']}/{r['mutants']} killed, {r['survived']} survived"
                  + (f", {r['unreachable']} in rules unreachable from root (equivalent)"
                     if r["unreachable"] else "")
                  + (f", {r['invalid']} not loadable" if r["invalid"] else ""))
    for s in rep["survivors"][:show]:
        print(f"    survived: {s}")
    if len(rep["survivors"]) > show:
        print(f"    ... and {len(rep['survivors']) - show} more")
    for s in rep["invalid"][:5]:
        print(f"    not loadable: {s}")


# ------------------------------------------------------------------ toke ----
def toke_report(limit: int | None, out_json: Path | None, jobs: int | None = None) -> int:
    """Report only (131.149): the toke agreement set is test/gbnf/fragments.json."""
    from gbnf_earley import Grammar
    from gbnf_tune import GBNF, LEXEME, XGR, llg_acceptor, xgr_acceptor
    frags = json.load(open(ROOT / "test" / "gbnf" / "fragments.json", encoding="utf-8"))["fragments"]
    inputs = [(f["id"], f["text"]) for f in frags]
    t0 = time.time()
    ref_g = Grammar(GBNF.read_text(encoding="utf-8"))
    reference = [ref_g.accepts(t) for _, t in inputs]
    print(f"[toke] reference: gbnf_earley on toke.gbnf, {len(inputs)} fragments, "
          f"{sum(reference)} accepted ({time.time() - t0:.0f}s)")
    dump = {}
    for tag, path, dialect, mk in (("llguidance:toke-lexeme.lark", LEXEME, "lark", llg_acceptor),
                                   ("xgrammar:toke-xgr.gbnf", XGR, "gbnf", xgr_acceptor)):
        text = path.read_text(encoding="utf-8")
        ms = mutants(text, dialect)[:limit]
        t0 = time.time()
        rep = score(ms, text, mk, inputs, reference, progress=True, jobs=jobs)
        print(f"[toke] {tag}: {len(ms)} mutants ({time.time() - t0:.0f}s)")
        print_report(f"[toke] {tag}", rep)
        dump[tag] = rep
    if out_json:
        out_json.write_text(json.dumps(dump, indent=1), encoding="utf-8")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--toke", action="store_true", required=True,
                    help="score the toke tuned forms against test/gbnf/fragments.json")
    ap.add_argument("--limit", type=int, default=None, help="first N mutants per form only")
    ap.add_argument("--json", type=Path, default=None, help="write the report here")
    ap.add_argument("--jobs", type=int, default=None, help="worker processes (default: CPUs)")
    a = ap.parse_args(argv)
    return toke_report(a.limit, a.json, a.jobs)


if __name__ == "__main__":
    sys.exit(main())
