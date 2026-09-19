#!/usr/bin/env python3
"""prepare.py — draw the 131.8 macro-check program set and copy the originals.

Writes, for every pair, `programs/<name>.orig.tk` (a verbatim copy of the
original with one provenance header comment `(* source: … sha256 … *)`) and
`pairs.json` (name, kind, source path, sha256, and for library programs the
manifest test cases that drive it on stdin).

Set = the 12 `bench/programs/*.tk` + a stratified library sample: 2 programs per
category from ~/tk/toke-test-programs/results/library/<cat>.json (the verified,
passing set), drawn with random.Random("131:<cat>").sample(sorted ids, 2) so
the draw is reproducible and independent of dict order. 16 categories -> 32.

The `<name>.canon.tk` rewrites and `<name>.patterns.json` are written by hand
(see README.md); this script never touches them. Re-running is idempotent.
"""
import hashlib
import json
import os
import random
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent                 # bench/patterns/macro
ROOT = HERE.parents[2]                                  # toke repo
PROGRAMS = HERE / "programs"
PAIRS = HERE / "pairs.json"
BENCH = ROOT / "bench" / "programs"
TP = Path(os.path.expanduser("~/tk/toke-test-programs/results"))
LIB = TP / "library"
SOL = TP / "solutions"
SEED = 131
PER_CATEGORY = 2
BENCH_NAMES = ["fib_recursive", "fib_iterative", "sum_array", "nested_loops", "binary_search",
               "prime_sieve", "deep_recursion", "struct_ops", "large_expr", "chained_calls",
               "collatz", "gcd_euler"]                  # bench/run_bench.sh BENCHMARKS order


def sha256(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def rel_home(p: Path) -> str:
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return "~/" + str(p.relative_to(Path.home()))


def write_orig(name: str, src: Path, extra: str = "") -> dict:
    body = src.read_bytes()
    header = f"(* source: {rel_home(src)} sha256 {sha256(body)}{extra} *)\n"
    out = PROGRAMS / f"{name}.orig.tk"
    out.write_bytes(header.encode() + body)
    return {"source": rel_home(src), "source_sha256": sha256(body), "orig": f"programs/{name}.orig.tk"}


def draw_sample():
    picks = []
    for mf in sorted(LIB.glob("*.json")):
        if mf.name == "index.json":
            continue
        d = json.loads(mf.read_text())
        ids = sorted(p["id"] for p in d["programs"])
        rng = random.Random(f"{SEED}:{d['category']}")
        for pid in rng.sample(ids, PER_CATEGORY):
            p = next(x for x in d["programs"] if x["id"] == pid)
            picks.append((d["category"], pid, p, mf))
    return picks


def main():
    PROGRAMS.mkdir(parents=True, exist_ok=True)
    pairs = []
    for n in BENCH_NAMES:
        src = BENCH / f"{n}.tk"
        e = {"name": n, "kind": "bench", **write_orig(n, src),
             "canon": f"programs/{n}.canon.tk", "patterns": f"programs/{n}.patterns.json",
             "cases": None}
        pairs.append(e)
    for cat, pid, p, mf in draw_sample():
        src = SOL / cat / pid / "solution.tk"
        e = {"name": pid, "kind": "library", "category": cat, "title": p["title"],
             **write_orig(pid, src, extra=f" manifest {rel_home(mf)}"),
             "manifest": rel_home(mf),
             "canon": f"programs/{pid}.canon.tk", "patterns": f"programs/{pid}.patterns.json",
             "cases": [{"input": tc.get("input", ""), "expected_output": tc.get("expected_output", ""),
                        "fixtures": tc.get("fixtures")} for tc in p["test_cases"]]}
        pairs.append(e)
    PAIRS.write_text(json.dumps({"seed": SEED, "per_category": PER_CATEGORY, "pairs": pairs}, indent=1) + "\n")
    print(f"{len(pairs)} pairs -> {PAIRS.relative_to(ROOT)} "
          f"({sum(p['kind']=='bench' for p in pairs)} bench, {sum(p['kind']=='library' for p in pairs)} library)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
