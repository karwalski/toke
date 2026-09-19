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

`--self-test` is the gate's negative control: it builds a program whose only
fault is a call to a function that does not exist, and fails unless the gate
rejects it. `make check-docs` runs it before the docs sweep, so every run
re-proves the gate can still fail.

Reads nothing outside this repository (cf. 127.88); it does need a built `tkc`
and a working clang, which `make` already provides.

Usage: python3 scripts/check_doc_examples.py [docs_dir] [--check-only]
                                             [--jobs N] [--verbose]
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


# ── negative control (136.26) ────────────────────────────────────────────────
# A gate nobody has watched fail is not a gate. This is the counter-example the
# gate MUST reject: a program whose only fault is a call to a function that
# exists nowhere. It type-checks (io's interface is handwritten and incomplete,
# so 136.1's call-site checking cannot see the hole) and it must fail at link.
# `--self-test` asserts exactly that, and `make check-docs` runs it first, so
# every run of the gate re-proves the gate can still fail.
CANARY_BAD = """m=canary;
i=io:std.io;
f=main():i64{ io.thisfunctiondoesnotexist(1); <0 };
"""
CANARY_GOOD = """m=canary;
i=io:std.io;
f=main():i64{ io.println("ok"); <0 };
"""


def self_test():
    ok = True
    good = build_one(CANARY_GOOD, link=True)
    if good is not None:
        print(f"  SELF-TEST FAIL: the known-good canary does not build: {good}")
        ok = False
    else:
        print("  self-test: known-good canary builds")

    checked = build_one(CANARY_BAD, link=False)
    if checked is not None:
        print("  SELF-TEST NOTE: the bad canary no longer type-checks "
              f"({checked[1]}). The front end has closed this hole; the link "
              "step is still the backstop for the ones it has not.")
    else:
        print("  self-test: the bad canary PASSES --check "
              "(this is the hole 136.26 closed)")

    linked = build_one(CANARY_BAD, link=True)
    if linked is None:
        print("  SELF-TEST FAIL: a call to a function that exists nowhere "
              "BUILT. The gate proves nothing — fix it before trusting it.")
        ok = False
    else:
        print(f"  self-test: the bad canary is rejected at "
              f"{linked[0]} — {linked[1]}")
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

    work = []
    for md in sorted(glob.glob(os.path.join(docs, "**", "*.md"), recursive=True)):
        rel = os.path.relpath(md, docs)
        if any(rel.endswith(s) for s in SKIP):
            continue
        text = open(md, encoding="utf-8").read()
        for i, block in enumerate(FENCE.findall(text)):
            if is_full_program(block):
                work.append((rel, i, block))

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
