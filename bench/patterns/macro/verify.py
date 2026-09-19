#!/usr/bin/env python3
"""verify.py — prove every `<name>.canon.tk` is output-identical to its original (131.8).

For each pair in pairs.json (or the names given on the command line):
  1. `tkc --check` both files (the pinned compiler, 131.39);
  2. build both with `tkc -O2 --allow-all` (as bench/run_bench.sh does);
  3. run both on the pair's own workload and compare the RAW stdout bytes AND
     the exit status:
       bench    — one run, no stdin (the 12 bench programs print nothing; their
                  result is main's return value = the exit status);
       library  — one run per manifest test case with the case input on stdin,
                  fresh cwd per case (toke-corpus/regen/validate.py's stdin
                  runner semantics; its materialise_fixtures/norm_stdout are
                  imported when that repo is present, else re-implemented);
                  additionally both binaries must still PASS the manifest's
                  expected_output under validate.norm_stdout (exit 0) — so the
                  canon is proven identical to the original and still correct.
  4. `tkc --min` of both is recorded (sha256) so a reviewer can see that a
     canon whose only change is formatting has an unchanged min form.

Writes results/verify_<YYYYMMDD-HHMMSS>.json; exit 0 iff every pair is identical.
--orig-only checks the originals against their manifests without needing canons.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent                  # bench/patterns/macro
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "patterns"))
import tkc_pin  # noqa: E402

REGEN = Path(os.path.expanduser("~/tk/toke-corpus/regen"))
try:
    sys.path.insert(0, str(REGEN))
    import validate as regen_validate  # noqa: E402  (131.18 stdin runner)
    norm_stdout = regen_validate.norm_stdout
    materialise_fixtures = regen_validate.materialise_fixtures
    STDIN_RUNNER = "toke-corpus/regen/validate.py"
except Exception:                                        # pragma: no cover
    regen_validate = None
    STDIN_RUNNER = "builtin (validate.py not importable)"

    def norm_stdout(s):
        return "\n".join(l.rstrip() for l in (s or "").strip().splitlines())

    def materialise_fixtures(fixtures, cwd):
        fx = fixtures or {}
        for d in fx.get("dirs") or []:
            os.makedirs(d if os.path.isabs(d) else os.path.join(cwd, d), exist_ok=True)
        for fpath, content in (fx.get("files") or {}).items():
            ap = fpath if os.path.isabs(fpath) else os.path.join(cwd, fpath)
            os.makedirs(os.path.dirname(ap), exist_ok=True)
            with open(ap, "wb") as f:
                f.write(content.encode("latin-1"))

PAIRS = HERE / "pairs.json"
BUILD = HERE / "build"
RESULTS = HERE / "results"
BENCH_TIMEOUT = 120
CASE_TIMEOUT = 10
TKC = None


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def tkc_check(src):
    r = subprocess.run([TKC, "--check", str(src)], capture_output=True, text=True, timeout=60, cwd=str(ROOT))
    return r.returncode == 0, (r.stderr or r.stdout).strip()[:1500]


def tkc_min_sha(src):
    r = subprocess.run([TKC, "--min", str(src)], capture_output=True, text=True, timeout=60, cwd=str(ROOT))
    return sha256_bytes(r.stdout.encode()) if r.returncode == 0 else None


def build(src, out):
    BUILD.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([TKC, "-O2", "--allow-all", str(src), "--out", str(out)],
                       capture_output=True, text=True, timeout=300, cwd=str(ROOT))
    if r.returncode != 0 or not out.exists():
        return False, (r.stderr or r.stdout).strip()[:1500]
    return True, None


def run(binary, stdin_text, cwd, timeout):
    try:
        r = subprocess.run([str(binary)], input=(stdin_text or "").encode(), capture_output=True,
                           timeout=timeout, cwd=str(cwd))
        return {"exit": r.returncode, "stdout": r.stdout, "timeout": False}
    except subprocess.TimeoutExpired:
        return {"exit": None, "stdout": b"", "timeout": True}


def compare_case(ro, rc):
    return (not ro["timeout"] and not rc["timeout"] and ro["exit"] == rc["exit"]
            and ro["stdout"] == rc["stdout"])


def manifest_pass(r, expected):
    return (not r["timeout"] and r["exit"] == 0
            and norm_stdout(r["stdout"].decode("utf-8", "replace")) == norm_stdout(expected))


def verify_pair(pair, orig_only=False):
    name = pair["name"]
    rec = {"name": name, "kind": pair["kind"], "identical": False, "reasons": []}
    orig = HERE / pair["orig"]
    canon = HERE / pair["canon"]
    srcs = {"orig": orig} if orig_only else {"orig": orig, "canon": canon}
    for k, p in srcs.items():
        if not p.exists():
            rec["reasons"].append(f"{k}: missing {p.relative_to(ROOT)}")
            return rec
        rec[f"{k}_sha256"] = sha256_bytes(p.read_bytes())
        ok, msg = tkc_check(p)
        rec[f"{k}_check"] = ok
        if not ok:
            rec["reasons"].append(f"{k}: --check failed: {msg[:300]}")
            return rec
        rec[f"{k}_min_sha256"] = tkc_min_sha(p)
    bins = {}
    for k, p in srcs.items():
        b = BUILD / f"{name}.{k}"
        ok, msg = build(p, b)
        rec[f"{k}_build"] = ok
        if not ok:
            rec["reasons"].append(f"{k}: build failed: {msg[:300]}")
            return rec
        bins[k] = b
    if pair["kind"] == "bench":
        outs = {k: run(b, "", ROOT, BENCH_TIMEOUT) for k, b in bins.items()}
        rec["cases"] = [{"case": 0, "exit": {k: r["exit"] for k, r in outs.items()},
                         "stdout_sha256": {k: sha256_bytes(r["stdout"]) for k, r in outs.items()},
                         "stdout_bytes": {k: len(r["stdout"]) for k, r in outs.items()},
                         "identical": (orig_only or compare_case(outs["orig"], outs["canon"]))}]
        if outs["orig"]["timeout"]:
            rec["reasons"].append("orig: timeout")
    else:
        rec["cases"] = []
        with tempfile.TemporaryDirectory(prefix=f"macro-{name}-") as td:
            for i, tc in enumerate(pair["cases"]):
                c = {"case": i, "exit": {}, "stdout_sha256": {}, "manifest_pass": {}}
                outs = {}
                for k, b in bins.items():
                    cwd = Path(td) / k / f"t{i}"
                    cwd.mkdir(parents=True, exist_ok=True)
                    materialise_fixtures(tc.get("fixtures"), str(cwd))
                    r = run(b, tc.get("input", ""), cwd, CASE_TIMEOUT)
                    outs[k] = r
                    c["exit"][k] = r["exit"]
                    c["stdout_sha256"][k] = sha256_bytes(r["stdout"])
                    c["manifest_pass"][k] = manifest_pass(r, tc.get("expected_output", ""))
                    if not c["manifest_pass"][k]:
                        c.setdefault("stdout_head", {})[k] = r["stdout"][:300].decode("utf-8", "replace")
                c["identical"] = orig_only or compare_case(outs["orig"], outs["canon"])
                rec["cases"].append(c)
                if not c["manifest_pass"]["orig"]:
                    rec["reasons"].append(f"case {i}: orig does not pass its manifest (exit {outs['orig']['exit']})")
                if not orig_only and not c["manifest_pass"]["canon"]:
                    rec["reasons"].append(f"case {i}: canon does not pass the manifest (exit {outs['canon']['exit']})")
    bad = [c["case"] for c in rec["cases"] if not c["identical"]]
    if bad:
        rec["reasons"].append(f"stdout/exit differ on case(s) {bad}")
    rec["identical"] = not rec["reasons"]
    return rec


def main(argv=None):
    ap = argparse.ArgumentParser(description="131.8 output-identity proof: orig vs canon")
    ap.add_argument("names", nargs="*", help="pair names (default: all in pairs.json)")
    ap.add_argument("--orig-only", action="store_true", help="only check that the originals pass their manifests")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args(argv)
    global TKC
    pin = tkc_pin.pin(toke_repo=ROOT)
    TKC = pin.argv0
    pairs = json.loads(PAIRS.read_text())["pairs"]
    if args.names:
        want = set(args.names)
        pairs = [p for p in pairs if p["name"] in want]
        missing = want - {p["name"] for p in pairs}
        if missing:
            print(f"verify: unknown pair(s): {sorted(missing)}", file=sys.stderr)
            return 2
    results = {"meta": {"date": dt.datetime.now().isoformat(timespec="seconds"), **pin.stamp(),
                        "stdin_runner": STDIN_RUNNER, "orig_only": args.orig_only,
                        "rule": "identical iff raw stdout bytes AND exit status equal on every workload run; "
                                "library pairs must also pass their manifest (norm_stdout, exit 0)"},
               "pairs": []}
    ok_n = 0
    try:
        for p in pairs:
            rec = verify_pair(p, args.orig_only)
            results["pairs"].append(rec)
            ok_n += rec["identical"]
            if not args.quiet:
                ncase = len(rec.get("cases", []))
                print(f"{'OK  ' if rec['identical'] else 'FAIL'} {rec['name']:<16} {rec['kind']:<8} cases={ncase}"
                      + ("" if rec["identical"] else "  " + "; ".join(rec["reasons"])[:200]), flush=True)
    finally:
        pin.close()
    results["meta"]["identical"] = ok_n
    results["meta"]["total"] = len(pairs)
    RESULTS.mkdir(parents=True, exist_ok=True)
    out = RESULTS / f"verify_{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}.json"
    out.write_text(json.dumps(results, indent=1) + "\n")
    print(f"verify: {ok_n}/{len(pairs)} identical -> {out.relative_to(ROOT)}")
    return 0 if ok_n == len(pairs) else 1


if __name__ == "__main__":
    sys.exit(main())
