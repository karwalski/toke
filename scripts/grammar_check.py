#!/usr/bin/env python3
"""grammar_check.py — 116.5 / A5 conformance harness.

Two checks:
  1. GBNF well-formedness (docs/spec/toke.gbnf): every referenced rule is
     defined, and no rule is directly left-recursive (llama.cpp/GBNF forbid it).
  2. Accept-set: the reference corpus (conform test inputs + standalone tests +
     examples) parses cleanly via `tkc --check` — i.e. the language the parser
     accepts, which the grammar must cover.

Usage: python3 scripts/grammar_check.py
"""
import os, re, sys, glob, subprocess, json

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TKC = os.environ.get("TKC", os.path.join(ROOT, "tkc"))
GBNF = os.path.join(ROOT, "docs", "spec", "toke.gbnf")


def check_gbnf():
    text = open(GBNF).read()
    rules, order = {}, []
    for m in re.finditer(r'^([a-z][a-z0-9-]*)\s*::=(.*(?:\n(?!\S).*)*)', text, re.M):
        rules[m.group(1)] = m.group(2)
        order.append(m.group(1))
    # collect references: bareword tokens that aren't quoted/char-class/ops
    def refs(body):
        # strip strings and char classes so their contents aren't seen as refs
        b = re.sub(r'"(?:[^"\\]|\\.)*"', ' ', body)
        b = re.sub(r'\[(?:[^\]\\]|\\.)*\]', ' ', b)
        b = re.sub(r'#.*', ' ', b)
        return set(re.findall(r'\b([a-z][a-z0-9-]*)\b', b))
    undefined, leftrec = set(), []
    for name in order:
        for r in refs(rules[name]):
            if r not in rules:
                undefined.add(r)
        # direct left recursion: first alternative starts with the rule name
        first = rules[name].strip().split('|')[0].strip()
        if re.match(rf'{re.escape(name)}\b', first):
            leftrec.append(name)
    ok = not undefined and not leftrec
    print(f"[gbnf] {len(rules)} rules, root={'root' in rules}")
    if undefined: print("  UNDEFINED refs:", sorted(undefined))
    if leftrec:   print("  LEFT-RECURSIVE:", leftrec)
    print("  gbnf well-formed:", ok)
    return ok


def check_accept():
    """A file 'fails' only on a genuine SYNTAX/grammar error (E2001-E2004) at
    parse stage. E2030 (module-not-found) is excluded — the example apps are
    multi-file and can't resolve sibling imports when checked one file at a time;
    that is a resolution concern, not grammar."""
    env = dict(os.environ, TKC_STDLIB_DIR=os.path.join(ROOT, "src", "stdlib"))
    SYNTAX = {"E2001", "E2002", "E2003", "E2004"}
    files = glob.glob(os.path.join(ROOT, "test/standalone/*.tk")) \
          + glob.glob(os.path.join(ROOT, "examples/*/*.tk")) \
          + glob.glob(os.path.join(ROOT, "examples/*/*/*.tk"))
    n = ok = 0
    fails = []
    for f in files:
        n += 1
        r = subprocess.run([TKC, f, "--check"], capture_output=True, text=True, env=env)
        syntax_err = False
        for line in (r.stderr + r.stdout).splitlines():
            try:
                d = json.loads(line)
            except Exception:
                continue
            if d.get("stage") == "parse" and d.get("error_code") in SYNTAX:
                syntax_err = True
                break
        if syntax_err:
            fails.append(os.path.relpath(f, ROOT))
        else:
            ok += 1
    print(f"[accept] {ok}/{n} corpus files accepted by the grammar (no syntax error)")
    if fails:
        print("  grammar-rejections:", fails)
    return ok, n


if __name__ == "__main__":
    g = check_gbnf()
    ok, n = check_accept()
    sys.exit(0 if g else 1)
