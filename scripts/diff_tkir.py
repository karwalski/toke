#!/usr/bin/env python3
"""76.1.6c -- source -> binary vs source -> .tkir -> binary parity.

`tkc --from-tkir f.tkir` rebuilds the parsed program a .tkir carries and runs
the same resolver, checker and LLVM backend a source compile runs.  This gate
proves it, over the program set `make diff-codegen` uses (test/standalone,
test/e2e, test/codegen):

  1. IR parity (always): for every full program that compiles from source,
       tkc P --emit-llvm                         -> a.ll
       tkc P --emit-tkir -> x.tkir ;
       tkc --from-tkir x.tkir --emit-llvm        -> b.ll
     and a.ll must equal b.ll byte for byte.  Identical IR is identical
     behaviour, whatever the program does.
  2. Behaviour parity (--run): build both binaries, run each with stdin from
     /dev/null, and compare exit status and stdout.  This is the end-to-end
     check; it is slower (two clang links per program).

Every program lands in exactly one bucket and the counts are printed:

  same          round-trips (IR identical; with --run, behaviour too)
  no-source     does not compile from source (not a .tkir question)
  emit-refused  --emit-tkir refused it with a structured diagnostic
  load-refused  --from-tkir refused the .tkir with a structured diagnostic
  DIFFERENT     IR or behaviour differs                        -> fails
  UNSTRUCTURED  a refusal without a JSON diagnostic, or a crash -> fails

A refusal is acceptable only as a diagnostic (AGENTS.md §6); a silent
difference never is.  The exit status is 1 when DIFFERENT or UNSTRUCTURED
is non-empty, or when fewer than --min programs round-trip; with --strict,
also when anything was refused.

Usage:
  TOKE=./tkc python3 scripts/diff_tkir.py [--run] [--strict] [--min N] DIR [DIR...]
"""
import argparse
import glob
import hashlib
import json
import os
import subprocess
import sys
import tempfile

TOKE = os.environ.get("TOKE", "./tkc")


def is_full_program(src):
    return ("f=main" in src) or ("f= main" in src)


def tkc(args, timeout=90):
    try:
        r = subprocess.run([TOKE] + args, capture_output=True, timeout=timeout,
                           stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return None, b"", b"timeout"
    return r.returncode, r.stdout, r.stderr


def diag_codes(blob):
    """Error codes of the JSON diagnostics in a tkc stdout/stderr blob."""
    codes = []
    for line in blob.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if isinstance(d, dict) and d.get("error_code"):
            codes.append(d["error_code"])
    return codes


def run_bin(path):
    try:
        r = subprocess.run([path], capture_output=True, timeout=20,
                           stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return ("timeout", None)
    if r.returncode < 0:
        return ("signal", None)
    return (r.returncode, hashlib.sha256(r.stdout).hexdigest()[:16])


def probe(tk, d, run):
    a_ll, b_ll = os.path.join(d, "a.ll"), os.path.join(d, "b.ll")
    x = os.path.join(d, "x.tkir")
    rc, out, err = tkc([tk, "--allow-all", "--emit-llvm", "--out", a_ll])
    if rc != 0 or not os.path.exists(a_ll):
        return "no-source", ""
    rc, out, err = tkc([tk, "--allow-all", "--emit-tkir", "--out", x])
    if rc != 0 or not os.path.exists(x):
        codes = diag_codes(out + err)
        if rc is None or rc < 0 or not codes:
            return "UNSTRUCTURED", f"--emit-tkir rc={rc} without a diagnostic"
        return "emit-refused", ",".join(sorted(set(codes)))
    rc, out, err = tkc(["--from-tkir", x, "--allow-all", "--emit-llvm", "--out", b_ll])
    if rc != 0 or not os.path.exists(b_ll):
        codes = diag_codes(out + err)
        if rc is None or rc < 0 or not codes:
            return "UNSTRUCTURED", f"--from-tkir rc={rc} without a diagnostic"
        return "load-refused", ",".join(sorted(set(codes)))
    if open(a_ll, "rb").read() != open(b_ll, "rb").read():
        return "DIFFERENT", "LLVM IR differs"
    if not run:
        return "same", ""
    a_bin, b_bin = os.path.join(d, "a"), os.path.join(d, "b")
    ra = tkc([tk, "--allow-all", "--out", a_bin])
    rb = tkc(["--from-tkir", x, "--allow-all", "--out", b_bin])
    if ra[0] != 0 or rb[0] != 0:
        return "DIFFERENT", f"binary build rc source={ra[0]} tkir={rb[0]}"
    sa, sb = run_bin(a_bin), run_bin(b_bin)
    if sa != sb:
        return "DIFFERENT", f"behaviour source={sa} tkir={sb}"
    return "same", f"exit={sa[0]}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", action="store_true",
                    help="also build and run both binaries")
    ap.add_argument("--min", type=int, default=0,
                    help="fail unless at least N programs round-trip")
    ap.add_argument("--strict", action="store_true",
                    help="also fail on any refusal: every program that compiles "
                         "from source must round-trip")
    ap.add_argument("--list", action="store_true",
                    help="print every program's bucket, not only failures")
    ap.add_argument("dirs", nargs="+")
    a = ap.parse_args()

    files = []
    for d in a.dirs:
        for tk in sorted(glob.glob(os.path.join(d, "**", "*.tk"), recursive=True)):
            try:
                if is_full_program(open(tk, errors="ignore").read()):
                    files.append(tk)
            except OSError:
                pass

    buckets = {}
    for tk in files:
        with tempfile.TemporaryDirectory() as d:
            b, why = probe(tk, d, a.run)
        buckets.setdefault(b, []).append((os.path.relpath(tk), why))
        if a.list or b in ("DIFFERENT", "UNSTRUCTURED", "emit-refused", "load-refused"):
            print(f"  {b:13s} {os.path.relpath(tk)}" + (f"  ({why})" if why else ""))

    order = ["same", "no-source", "emit-refused", "load-refused", "DIFFERENT", "UNSTRUCTURED"]
    n_src = len(files) - len(buckets.get("no-source", []))
    same = len(buckets.get("same", []))
    print("diff_tkir: %d programs, %d compile from source, %d round-trip%s; %s" % (
        len(files), n_src, same, " (IR + behaviour)" if a.run else " (IR identical)",
        ", ".join(f"{k} {len(buckets.get(k, []))}" for k in order)))
    bad = buckets.get("DIFFERENT") or buckets.get("UNSTRUCTURED")
    if a.strict and (buckets.get("emit-refused") or buckets.get("load-refused")):
        bad = True
    if bad or same < a.min:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
