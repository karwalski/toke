#!/usr/bin/env python3
"""check_doc_examples.py — build-gate for full-program ```toke blocks in the docs.

Extracts every fenced ```toke / ```tk block that is a COMPLETE program (has a
top-level `m=` module decl and an `f=` function) from the canonical docs tree
(`toke/docs/` by default) and BUILDS each one to a binary with `tkc --out`.
Fragments (no `m=`/`f=`) are illustrative and skipped. Exits non-zero if any
full program fails to build, so docs can't drift from the compiler
(Epic 116 / 116.13-H5; linking added by 136.26).

WHAT THIS GATE PROVES, AND WHAT IT DOES NOT
-------------------------------------------
A pass proves each documented full program:
  * parses, resolves its imports, and type-checks (front end); and
  * reaches codegen and **links** — every function, stdlib binding and glue
    symbol it names actually exists in the linked image.

A pass does NOT prove:
  * that the example RUNS, or terminates, or exits 0 — nothing is executed
    here. Behaviour is the job of `make test-stdlib-glue-contract`, which
    compiles and runs real consumers and compares output against the
    documented values;
  * that the example does what the surrounding prose claims;
  * that a call passes semantically correct ARGUMENTS. Arity and type are
    checked against the implementation by 136.1's call-site checking, but a
    call that is well-typed and wrong is invisible here;
  * anything about fenced blocks that are fragments, or about the pages in
    SKIP below (intentional error demos).

Until 136.26 this gate ran `--check`, which stops at the front end. A
documented call to a function that exists NOWHERE type-checked cleanly and
passed. That is why the gate could read 432/432 while shipping examples that
could not be built. Do not weaken it back to `--check` for speed without
replacing the coverage: use `--check-only` explicitly and say so.

NO SKIP LIST FOR FAILURES. The SKIP tuple is for pages whose examples are
*meant* not to compile. An example that fails is a defect in the docs or in
the compiler; file it, don't list it.

COST (measured 2026-09-19, 432 blocks, 12 jobs, M4 Max): type-check 1.7s,
build 148s. CI affords that, so there is no fast/slow split. `--check-only`
exists for a 2-second local edit loop and prints a warning saying what it
stops proving; it must not be what CI runs.

`--self-test` is the gate's negative control. `make check-docs` runs it before
the docs sweep, so every run re-proves the gate can still fail. 136.48: it
asserts TWO different properties, because the original single canary stopped
proving the one it was added for --
  * an undeclared name (`io.thisfunctiondoesnotexist`) must be rejected. The
    front end now catches this at --check (E4027), so it is REPORTED and says
    nothing about the link step;
  * a QUARANTINED .tki export -- declared in the interface, no C symbol behind
    it -- must pass --check and FAIL AT LINK. That is the only assertion here
    that exercises the link backstop 136.26 added. Measured 2026-09-23: with
    the link branch of build_one() deleted outright, the old self-test printed
    byte-identical output and exited 0.

Reads nothing outside this repository (cf. 127.88); it does need a built `tkc`
and a working clang, which `make` already provides.

Usage: python3 scripts/check_doc_examples.py [docs_dir] [--check-only]
                                             [--jobs N] [--verbose]
       python3 scripts/check_doc_examples.py [docs_dir] --pattern-report
                                             (132.43: pattern rules, report only)
       python3 scripts/check_doc_examples.py --self-test
Env:   TKC (compiler path), TKC_STDLIB_DIR, TKC_JOBS
"""
import sys, os, re, glob, json, subprocess, tempfile, shutil, time
from concurrent.futures import ThreadPoolExecutor

TKC = os.environ.get("TKC", os.path.join(os.getcwd(), "tkc"))
FENCE = re.compile(r"(?:^|\n)```(?:toke|tk)[ \t]*\n(.*?)\n```", re.DOTALL)
# intentional error demos — not expected to compile
SKIP = ("reference/errors.md", "spec/errors.md", "known-limitations.md",
        "reference/migration.md",
        # lint-rules pages demonstrate violations on purpose (unreachable code,
        # undeclared identifiers, underscore idents) — they MUST NOT compile.
        "lint-rules-v1.md", "compiler/lint-rules-v1.md",
        # about/web-server.md is an illustrative marketing example using an
        # aspirational API surface (store.to_json/push/get, http.rate_limit that
        # 120.13 found is dead); needs a real-API rewrite — tracked in 119.8.
        "about/web-server.md")


HAS_MAIN = re.compile(r"(^|\n)[ \t]*f=main[ \t]*\(")
# Linking needs an entry point; many documented examples are module-shaped
# (`m=example;` + a demo function, no `main`). Appending a do-nothing entry
# point lets those link without altering what is being tested: toke emits
# top-level functions with external linkage, so an unreferenced demo function
# is NOT dead-stripped and its undefined symbols still reach the linker —
# verified against a call to a function that exists nowhere (136.26).
SYNTH_MAIN = "\nf=main():i64{ <0 };\n"


def is_full_program(block):
    return re.search(r"(^|\n)m=", block) and re.search(r"(^|\n)f=", block) \
        and not re.search(r"(^|\n)i=\w+:(?!std\.)", block)  # only std imports


def _first_error(out, fallback):
    for line in out.splitlines():
        line = line.strip()
        if line.startswith("{"):
            try:
                d = json.loads(line)
                if d.get("severity") == "error":
                    return f'{d.get("error_code")}: {d.get("message")} | {d.get("source_line","").strip()}'
            except Exception:
                pass
    return fallback


def _undefined_symbols(out):
    """Pull the mangled symbols out of a linker 'Undefined symbols' block."""
    syms = re.findall(r'"_?(tk_[A-Za-z0-9_]+)"', out)
    seen, uniq = set(), []
    for s in syms:
        if s not in seen:
            seen.add(s); uniq.append(s)
    return uniq


def build_one(src, link):
    """Return (stage, detail) on failure, or None on success.

    stage is "check" for a front-end diagnostic and "link" for an example that
    type-checks but cannot be built into a binary.
    """
    d = tempfile.mkdtemp(prefix="docex-")
    try:
        path = os.path.join(d, "ex.tk")
        with open(path, "w") as f:
            f.write(src)
        r = subprocess.run([TKC, path, "--check"], capture_output=True, text=True)
        if r.returncode != 0:
            return ("check", _first_error(r.stdout + "\n" + r.stderr, "compile failed"))
        if not link:
            return None
        if not HAS_MAIN.search(src):
            # a trailing ';' is optional at EOF, so supply one before appending
            sep = "" if src.rstrip().endswith(";") else ";"
            with open(path, "a") as f:
                f.write(sep + SYNTH_MAIN)
        out = os.path.join(d, "ex.bin")
        r = subprocess.run([TKC, path, "--out", out], capture_output=True, text=True)
        combined = r.stdout + "\n" + r.stderr
        if r.returncode != 0 or not os.path.exists(out):
            syms = _undefined_symbols(combined)
            if syms:
                return ("link", "undefined: " + ", ".join(syms[:6])
                        + ("" if len(syms) <= 6 else f" (+{len(syms)-6} more)"))
            ir = re.search(r"^.*\.ll:\d+:\d+: error: (.*)$", combined, re.M)
            if ir:
                # the compiler emitted LLVM IR that does not verify — a toke
                # codegen bug, not a defect in the documented example
                return ("codegen", "invalid IR: " + ir.group(1).strip())
            return ("link", _first_error(combined, "build failed"))
        return None
    finally:
        shutil.rmtree(d, ignore_errors=True)


# ── negative control (136.26, repaired by 136.48) ────────────────────────────
# A gate nobody has watched fail is not a gate.
#
# 136.26 added CANARY_BAD: a call to a function that exists nowhere. At the
# time it type-checked (io's interface is handwritten and incomplete) and
# failed at link, so asserting "the gate rejects it" proved the LINK STEP was
# the backstop, which is the property 136.26 was added to defend.
#
# It stopped proving that. The front end now rejects the same program at
# `--check` with E4027, so the assertion became a test of the FRONT END and
# the link backstop went untested — while `--self-test` still printed a pass.
# 136.48 measured how far that had rotted: with the entire link branch of
# build_one() deleted (`if not link: return None` followed by an
# unconditional `return None`), `--self-test` produced BYTE-IDENTICAL output
# and exited 0.
#
# So there are now two canaries, and they assert different things:
#
#   CANARY_BAD       — a call to a name no interface declares. Rejected at
#                      --check today (E4027). Reported, never asserted as
#                      proof of the link step; if the front end ever stops
#                      catching it, the link step below still must.
#
#   CANARY_LINK_ONLY — a call that PASSES --check and CANNOT LINK. These are
#                      quarantined .tki exports: declared in the interface
#                      (so the front end is satisfied) with no defined C
#                      symbol behind them (see scripts/check_tki_skiplist.txt).
#                      This is the only thing that proves the link backstop.
#
# The link-only canary is a LIST, tried in order, because each candidate stops
# working the day someone implements its glue — which is a good day, and must
# not be a silent one. If every candidate has been implemented, the self-test
# FAILS and says so: the link backstop has become untestable and this list
# needs a new entry from the current quarantine population. It will not quietly
# go back to proving nothing.
CANARY_BAD = """m=canary;
i=io:std.io;
f=main():i64{ io.thisfunctiondoesnotexist(1); <0 };
"""
CANARY_GOOD = """m=canary;
i=io:std.io;
f=main():i64{ io.println("ok"); <0 };
"""
# NOTE the alias `cv`. Writing `i=canvas:std.canvas` makes the alias collide
# with the opaque type `canvas` that the same module exports, and 136.48 found
# that E4027's member check is then skipped ENTIRELY: `canvas.nosuchmember(c)`
# passes --check and fails at link. That would make this canary pass for the
# wrong reason — because of a front-end hole, not because the export is
# quarantined. `cv` keeps it honest.
_LINK_ONLY = """m=canary;
i=cv:std.canvas;
f=main():i64{{
  let c=cv.new("x";1;1);
  let d={call};
  <0
}};
"""
# 127.224 (4) made --check reject a call to any quarantined export, so the
# quarantine population no longer yields a link-only canary. A bodyless user
# extern keeps its bare C name (127.212) and the front end cannot know whether
# the symbol exists, so an extern for a symbol nothing defines passes --check
# (W8001 only) and can fail nowhere but at link -- independent of what the
# quarantine list holds. It is tried first; the old entries stay as fallbacks.
_LINK_ONLY_EXTERN = """m=canary;
f=tkcanarynosuchsymbol(x:i64):i64;
f=main():i64{
  let d=tkcanarynosuchsymbol(1);
  <0
};
"""
CANARY_LINK_ONLY = [
    ("extern tkcanarynosuchsymbol", _LINK_ONLY_EXTERN),
    ("canvas.beginpath", _LINK_ONLY.format(call="cv.beginpath(c)")),
    ("canvas.moveto", _LINK_ONLY.format(call="cv.moveto(c;0.0;0.0)")),
    ("canvas.lineto", _LINK_ONLY.format(call="cv.lineto(c;1.0;1.0)")),
    ("canvas.clearrect",
     _LINK_ONLY.format(call="cv.clearrect(c;0.0;0.0;1.0;1.0)")),
]


# ── 132.43: the idiom/pattern half, REPORT mode ─────────────────────────────
# The build gate above proves an example compiles and links. It says nothing
# about whether the example teaches CANONICAL toke -- and the docs feed the
# corpus. The idiom judge (toke-corpus regen/idiom_judge.py, 131.10) is built
# on the compiler's own pattern rules: its hard gate is "any pattern-rule hit
# at severity error or warning". `--pattern-report` runs exactly those rules
# (`tkc --lint --diag-json`) over every documented full program and REPORTS
# them. It does not fail: the docs predate the catalogue, and a rewrite into a
# form the catalogue has not measured is what 132.43 says not to do. It does
# not recompute the judge's numeric score -- its weights live in toke-corpus,
# and this script reads nothing outside this repository.
#
# The rule set is read from docs/lint-rules-v1.md ("## Pattern rules"), the
# normative list, so a new pattern rule is picked up without editing this.
#
# 132.99: a page that shows a deliberate "Not this" counter-example (guide/11's
# before/after pairs) marks the block with ALLOW_MARK on one of the three lines
# above its opening fence. An allowed block is still built by the build gate
# and is still listed here (as ALLOW, never silently); it just is not counted
# as a would-fail. A marker on a block with no gated hit is listed as STALE.
# `make check-doc-patterns` runs this report.
LINT_RULES_MD = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "docs", "lint-rules-v1.md")
JUDGE_GATE = ("error", "warning")     # idiom_judge.GATE_SEVERITIES
ALLOW_MARK = "<!-- pattern-report: counter-example -->"
ALLOW_LOOKBACK = 3                    # lines above the opening fence
CANARY_PATTERN = """m=canary;
i=io:std.io;
f=main():i64{
  let a=mut.0;
  if(1>0){a=1}el{a=2};
  io.println("\\(a)");
  <0
};
"""


def pattern_rules():
    """Rule ids under '## Pattern rules' in docs/lint-rules-v1.md."""
    text = open(LINT_RULES_MD, encoding="utf-8").read()
    m = re.search(r"^## Pattern rules.*?$(.*?)(?=^## )", text, re.M | re.S)
    return set(re.findall(r"^### `([a-z0-9-]+)`", m.group(1), re.M)) if m else set()


def lint_one(src, rules):
    """[(rule, severity, line)] pattern-rule hits for one program."""
    d = tempfile.mkdtemp(prefix="docpat-")
    try:
        path = os.path.join(d, "ex.tk")
        with open(path, "w") as f:
            f.write(src)
        r = subprocess.run([TKC, "--lint", "--diag-json", path],
                           capture_output=True, text=True, timeout=60)
        hits = []
        for line in (r.stdout + r.stderr).splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                j = json.loads(line)
            except ValueError:
                continue
            if j.get("stage") == "lint" and j.get("rule") in rules:
                hits.append((j["rule"], j.get("severity"), (j.get("pos") or {}).get("line")))
        return hits
    finally:
        shutil.rmtree(d, ignore_errors=True)


def fenced_blocks(text):
    """[(block_index, body, allowed)] for every ```toke / ```tk fence. `allowed`
    is True when ALLOW_MARK sits on one of the ALLOW_LOOKBACK lines above it."""
    out = []
    for i, m in enumerate(FENCE.finditer(text)):
        start = m.start() + (1 if text[m.start()] == "\n" else 0)
        above = text[:start].split("\n")[-(ALLOW_LOOKBACK + 1):]
        out.append((i, m.group(1), any(ALLOW_MARK in ln for ln in above)))
    return out


def pattern_report(work, jobs, allowed=frozenset()):
    rules = pattern_rules()
    if len(rules) < 6:
        print(f"pattern report: only {len(rules)} pattern rules found in "
              f"{LINT_RULES_MD} -- the anchor moved; refusing to report 0 hits")
        return 2
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        hits = list(pool.map(lambda w: lint_one(w[2], rules), work))
    gated_all = [(w, h) for w, h in zip(work, hits) if any(sv in JUDGE_GATE for _, sv, _ in h)]
    allow = [(w, h) for w, h in gated_all if (w[0], w[1]) in allowed]
    gated = [(w, h) for w, h in gated_all if (w[0], w[1]) not in allowed]
    stale = sorted(k for k in allowed
                   if k not in {(w[0], w[1]) for w, _ in gated_all})
    hinted = [(w, h) for w, h in zip(work, hits) if h and not any(sv in JUDGE_GATE for _, sv, _ in h)]
    by_rule = {}
    for h in hits:
        for rule, sv, _ in h:
            by_rule[(rule, sv)] = by_rule.get((rule, sv), 0) + 1
    print(f"doc pattern report (132.43) -- Reported, not asserted: "
          f"{len(gated)}/{len(work)} documented full programs would fail the idiom "
          f"judge's hard gate (a pattern rule at error/warning); {len(hinted)} more "
          f"carry hints only; {len(work) - len(gated_all) - len(hinted)} are clean; "
          f"{len(allow)} are marked deliberate counter-examples. "
          f"Rules: {', '.join(sorted(rules))}.")
    for (rule, sv), n in sorted(by_rule.items()):
        print(f"  {n:4d}  {rule:<24s} {sv}")
    for (rel, i, _), h in gated:
        desc = ", ".join(f"{r}@{ln}" for r, sv, ln in h if sv in JUDGE_GATE)
        print(f"  GATE {rel} [block {i}]: {desc}")
    for (rel, i, _), h in allow:
        desc = ", ".join(f"{r}@{ln}" for r, sv, ln in h if sv in JUDGE_GATE)
        print(f"  ALLOW {rel} [block {i}] (counter-example): {desc}")
    for rel, i in stale:
        print(f"  STALE {rel} [block {i}]: marked counter-example, but no gated "
              f"pattern hit -- remove the marker")
    return 0


def self_test():
    ok = True
    # 132.43: the pattern report must see a known violation, and must find its
    # rule list; an empty list would report every page clean.
    rules = pattern_rules()
    canary = lint_one(CANARY_PATTERN, rules) if rules else []
    if len(rules) < 6 or not any(r == "mut-flag-if" and sv in JUDGE_GATE for r, sv, _ in canary):
        print(f"  SELF-TEST FAIL: the pattern report did not flag the mut-flag-if "
              f"canary ({len(rules)} rules read from {LINT_RULES_MD}, hits {canary})")
        ok = False
    else:
        print(f"  self-test: the pattern report reads {len(rules)} pattern rules and "
              f"flags the mut-flag-if canary")
    # 132.99: the allow marker must be seen directly above a fence, and must
    # not leak onto a block further down.
    md = ("x\n\n" + ALLOW_MARK + "\n\n```toke\na\n```\n\ntext\n\n```toke\nb\n```\n")
    flags = [a for _, _, a in fenced_blocks(md)]
    if flags != [True, False]:
        print(f"  SELF-TEST FAIL: the counter-example marker was read as {flags}, "
              f"expected [True, False]")
        ok = False
    else:
        print("  self-test: the counter-example marker allows the block below it "
              "and only that block")
    good = build_one(CANARY_GOOD, link=True)
    if good is not None:
        print(f"  SELF-TEST FAIL: the known-good canary does not build: {good}")
        ok = False
    else:
        print("  self-test: known-good canary builds")

    # ── 1. the undeclared-name canary: REPORTED, not relied on ──────────────
    checked = build_one(CANARY_BAD, link=False)
    linked = build_one(CANARY_BAD, link=True)
    if linked is None:
        print("  SELF-TEST FAIL: a call to a function that exists nowhere "
              "BUILT. The gate proves nothing — fix it before trusting it.")
        ok = False
    elif checked is not None:
        print(f"  self-test: the undeclared-name canary is rejected at "
              f"{linked[0]} — {linked[1]}")
        print("             (front-end enforced; this says NOTHING about the "
              "link backstop — see the link-only canary below)")
    else:
        print("  self-test: the undeclared-name canary PASSES --check and is "
              f"rejected at {linked[0]} — {linked[1]}")

    # ── 2. the link-only canary: the actual proof of the link backstop ──────
    tried = []
    for name, src in CANARY_LINK_ONLY:
        if build_one(src, link=False) is not None:
            tried.append(name)          # front end now rejects it too
            continue
        built = build_one(src, link=True)
        if built is None:
            print(f"  SELF-TEST FAIL: {name} type-checks AND LINKS. It is on "
                  "the .tki quarantine list, so either the glue landed (remove "
                  "it from scripts/check_tki_skiplist.txt and from "
                  "CANARY_LINK_ONLY) or the link step is no longer being run.")
            ok = False
        elif built[0] != "link":
            print(f"  SELF-TEST FAIL: {name} failed at {built[0]}, not at "
                  f"link — {built[1]}. This canary no longer isolates the "
                  "link step.")
            ok = False
        else:
            print(f"  self-test: the link-only canary {name} type-checks and "
                  f"is rejected at link — {built[1]}")
        break
    else:
        print("  SELF-TEST FAIL: every link-only canary is now rejected by the "
              f"front end ({', '.join(tried)}), so nothing here exercises the "
              "link step and a broken link backstop would pass unnoticed. Add "
              "a new entry to CANARY_LINK_ONLY from the current quarantine "
              "population in scripts/check_tki_skiplist.txt.")
        ok = False
    return ok


def main():
    args = [a for a in sys.argv[1:]]
    if "--self-test" in args:
        sys.exit(0 if self_test() else 1)
    link = "--check-only" not in args
    verbose = "--verbose" in args
    jobs = int(os.environ.get("TKC_JOBS", "0") or 0)
    if "--jobs" in args:
        jobs = int(args[args.index("--jobs") + 1])
        del args[args.index("--jobs"):args.index("--jobs") + 2]
    args = [a for a in args if not a.startswith("--")]
    jobs = jobs or min(12, (os.cpu_count() or 4))
    docs = args[0] if args else os.path.join(os.getcwd(), "docs")

    work, allowed = [], set()
    for md in sorted(glob.glob(os.path.join(docs, "**", "*.md"), recursive=True)):
        rel = os.path.relpath(md, docs)
        if any(rel.endswith(s) for s in SKIP):
            continue
        text = open(md, encoding="utf-8").read()
        for i, block, allow in fenced_blocks(text):
            if is_full_program(block):
                work.append((rel, i, block))
                if allow:
                    allowed.add((rel, i))

    if "--pattern-report" in sys.argv:
        sys.exit(pattern_report(work, jobs, allowed))

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda w: build_one(w[2], link), work))
    elapsed = time.time() - t0

    failures = [(w[0], w[1], r[0], r[1]) for w, r in zip(work, results) if r]
    total = len(work)
    passed = total - len(failures)
    verb = "build (check+link)" if link else "type-check only (--check-only)"
    print(f"doc full-program blocks: {passed}/{total} {verb}"
          f"  [{elapsed:.1f}s, {jobs} jobs]")
    if link:
        synth = sum(1 for w in work if not HAS_MAIN.search(w[2]))
        print(f"  {synth}/{total} are module-shaped and were linked with a "
              f"synthesised do-nothing entry point")
    if not link:
        print("  WARNING: --check-only stops at the front end. A call to a "
              "function that exists nowhere PASSES in this mode.")
    labels = {
        "check": "front-end (does not type-check)",
        "codegen": "CODEGEN (type-checks, then the compiler emits invalid "
                   "LLVM IR — a compiler bug, not a docs bug)",
        "link": "LINK (type-checks but does not build — names a symbol that "
                "does not exist in the linked image)",
    }
    for stage in ("check", "codegen", "link"):
        group = [f for f in failures if f[2] == stage]
        if not group:
            continue
        print(f"  {len(group)} {labels[stage]}:")
        for rel, i, _, err in group:
            print(f"    FAIL {rel} [block {i}]: {err}")
    if verbose and not failures:
        print("  every documented full program builds to a binary")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
