#!/usr/bin/env python3
"""123.11 — performance-regression gate over bench/programs/*.tk.

bench.yml uploaded benchmark artifacts and never compared them to anything,
so no regression could ever fail it.  This compares against a checked-in
baseline, bench/perf-baseline.json, with stated thresholds.

WHAT IS MEASURED — and why not wall time.  This machine routinely runs several
agents' builds at once (load average 20+, 290 observed); a wall-clock gate
there is a coin toss.  Every metric below is a COUNT, which load does not move:

  compile_insns  instructions retired by the tkc process compiling the program
                 at -O2 (macOS `/usr/bin/time -l`; Linux `perf stat -e
                 instructions:u`).  tkc's own work only: the C toolchain it
                 invokes is a child process and is not counted.
  compile_allocs malloc+calloc+realloc calls made by tkc during that compile
                 (bench/patterns/alloccount.c interposer; macOS only)
  run_insns      instructions retired running the produced binary
  run_allocs     malloc-family calls made by the produced binary (macOS only)

Instruction counts are not perfectly deterministic.  Measured on an M4 Max
at load 50-290 (2026-09-24): a whole `tkc -O2` compile of a bench program is
~120-130M instructions and ten consecutive compiles of one program ranged
125M-135M (+/-4%), and the noise is correlated in time -- one min-of-5 came
out 6.3% above the recorded min-of-5.  Almost none of that is tkc's own
frontend work (`--emit-ir` alone is ~20M, i.e. the process floor); it is the
driver work around the C toolchain it spawns.  A produced binary's run is far
steadier for large runs (0.1-0.5% between recordings of 100M+ instruction
programs) but not for small ones: struct_ops (~16M) moved +3.4% and
nested_loops (~21M) +2.0% between two recordings, process start-up being a
larger share.  Each count is the MINIMUM of --runs
runs (default 5); the thresholds absorb the rest.  Allocation counts are exact.

THRESHOLDS (a metric fails when current > baseline * (1 + pct) + abs):
  compile_insns   15%         above the measured 6.3% worst case with margin.
                              This is COARSE: it catches a compile-path
                              regression of ~20M+ instructions, not a 2%
                              frontend slowdown -- bench programs are too small
                              for tkc's own work to rise above process noise.
  run_insns       8%          codegen quality; measured worst case 3.4% on the
                              smallest program
  compile_allocs  2% + 16     exact, but a real change may add a handful
  run_allocs      2% + 16
An improvement never fails; improvements beyond the threshold are reported so
the baseline can be re-recorded deliberately (--record).

THE BASELINE IS HOST-SPECIFIC: instruction counts depend on the CPU, OS, and
the C compiler tkc drives.  The baseline records a fingerprint; on a host whose
fingerprint differs the gate prints a named SKIP and exits 0 (use
--require-host to make that a failure).  CI therefore does not compare against
the checked-in file: bench.yml measures the PR's base commit and head commit
on the same runner (--record into a temp file with the base compiler, then
compare the head compiler against it).

Usage:
  perf_gate.py [--tkc PATH] [--baseline FILE] [--runs N] [--only NAME ...]
  perf_gate.py --record [--baseline FILE]
Exit: 0 green or SKIP, 1 regression, 2 harness failure.
"""
import argparse
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
PROGRAMS = ROOT / "bench" / "programs"
DEFAULT_BASELINE = ROOT / "bench" / "perf-baseline.json"
ALLOC_DIR = ROOT / "bench" / "patterns"
ALLOC_LIB = ALLOC_DIR / "liballoccount.dylib"

THRESHOLDS = {
    "compile_insns": (0.15, 0),
    "run_insns": (0.08, 0),
    "compile_allocs": (0.02, 16),
    "run_allocs": (0.02, 16),
}


class Harness(Exception):
    pass


def fingerprint(cc):
    try:
        ccv = subprocess.run([cc, "--version"], capture_output=True, text=True,
                             timeout=30).stdout.splitlines()[0].strip()
    except (OSError, IndexError, subprocess.SubprocessError):
        ccv = "unknown"
    cpu = platform.machine()
    if platform.system() == "Darwin":
        try:
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                 capture_output=True, text=True).stdout.strip() or cpu
        except OSError:
            pass
    return {"os": platform.system(), "arch": platform.machine(), "cpu": cpu,
            "cc": ccv}


def counter():
    """Return a function(argv, env) -> instructions retired, or None."""
    if platform.system() == "Darwin" and os.path.exists("/usr/bin/time"):
        def mac(argv, env, cwd, any_rc=False):
            p = subprocess.run(["/usr/bin/time", "-l"] + argv, env=env, cwd=cwd,
                               capture_output=True, text=True, timeout=600)
            m = re.search(r"^\s*(\d+)\s+instructions retired", p.stderr, re.M)
            if (p.returncode != 0 and not any_rc) or not m:
                raise Harness("%s exited %d (stderr tail: %s)"
                              % (argv[0], p.returncode, p.stderr[-300:]))
            return int(m.group(1)), p
        return mac
    if shutil.which("perf"):
        def linux(argv, env, cwd, any_rc=False):
            p = subprocess.run(["perf", "stat", "-x,", "-e", "instructions:u"]
                               + argv, env=env, cwd=cwd, capture_output=True,
                               text=True, timeout=600)
            m = re.search(r"^(\d+),[^,]*,instructions", p.stderr, re.M)
            if (p.returncode != 0 and not any_rc) or not m:
                raise Harness("%s exited %d under perf (stderr tail: %s)"
                              % (argv[0], p.returncode, p.stderr[-300:]))
            return int(m.group(1)), p
        try:
            linux(["true"], os.environ.copy(), None)
            return linux
        except Harness:
            return None
    return None


def alloc_lib():
    if platform.system() != "Darwin":
        return None
    src = ALLOC_DIR / "alloccount.c"
    if not ALLOC_LIB.exists() or ALLOC_LIB.stat().st_mtime < src.stat().st_mtime:
        r = subprocess.run(["make", "-s", "-C", str(ALLOC_DIR), "liballoccount.dylib"],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise Harness("cannot build liballoccount.dylib: " + r.stderr[-300:])
    return ALLOC_LIB


def alloc_calls(argv, cwd, lib, any_rc=False):
    env = os.environ.copy()
    env["DYLD_INSERT_LIBRARIES"] = str(lib)
    p = subprocess.run(argv, env=env, cwd=cwd, capture_output=True, text=True,
                       timeout=600)
    m = re.search(r"^ALLOC calls=(\d+)", p.stderr, re.M)
    if (p.returncode != 0 and not any_rc) or not m:
        raise Harness("%s under the alloc counter exited %d, no ALLOC line"
                      % (argv[0], p.returncode))
    return int(m.group(1))


def measure(tkc, names, runs, count, lib):
    work = pathlib.Path(tempfile.mkdtemp(prefix="tkc_perf."))
    pinned = work / "tkc"
    shutil.copy2(os.path.realpath(tkc), pinned)
    env = os.environ.copy()
    out = {}
    try:
        for name in names:
            src = PROGRAMS / (name + ".tk")
            shutil.copy2(src, work / src.name)
            binp = work / name
            comp = ["./tkc", "-O2", src.name, "--out", name]
            ci = min(count(comp, env, work)[0] for _ in range(runs))
            # bench programs report their checksum as the exit status, so any
            # status is accepted -- but it must be the same every run, and it
            # is recorded, so a codegen change that alters the RESULT fails.
            rr = [count(["./" + name], env, work, any_rc=True) for _ in range(runs)]
            codes = {p.returncode for _, p in rr}
            if len(codes) != 1:
                raise Harness("%s: exit status varies between runs: %s" % (name, sorted(codes)))
            ri = min(n for n, _ in rr)
            rec = {"compile_insns": ci, "run_insns": ri, "exit_status": codes.pop()}
            if lib:
                rec["compile_allocs"] = alloc_calls(comp, work, lib)
                rec["run_allocs"] = alloc_calls(["./" + name], work, lib, any_rc=True)
            if not binp.exists():
                raise Harness("%s: no binary produced" % name)
            out[name] = rec
            print("  measured %-16s %s" % (name, "  ".join(
                "%s=%d" % kv for kv in sorted(rec.items()))), flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    return out, subprocess.run([str(tkc), "--version"], capture_output=True,
                               text=True).stdout.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tkc", default=os.environ.get("TKC", str(ROOT / "tkc")))
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--require-host", action="store_true",
                    help="a host-fingerprint mismatch fails instead of skipping")
    a = ap.parse_args()

    cc = os.environ.get("CC", "cc")
    fp = fingerprint(cc)
    count = counter()
    if count is None:
        print("SKIP perf-gate: no instruction counter on this host (macOS "
              "/usr/bin/time -l, or Linux perf with instructions:u). Nothing "
              "was measured.")
        return 2 if a.require_host else 0
    try:
        lib = alloc_lib()
    except Harness as e:
        print("perf-gate: harness failure: %s" % e)
        return 2

    names = sorted(p.stem for p in PROGRAMS.glob("*.tk"))
    if a.only:
        names = [n for n in names if n in a.only]
    if not names:
        print("perf-gate: no programs selected")
        return 2

    base = None
    if not a.record:
        try:
            base = json.loads(pathlib.Path(a.baseline).read_text())
        except OSError:
            print("perf-gate: no baseline at %s (record one with --record)" % a.baseline)
            return 2
        if base.get("host") != fp:
            print("SKIP perf-gate: this host is not the baseline's host, so its "
                  "counts are not comparable. Nothing was compared.")
            print("  baseline host: %s" % json.dumps(base.get("host")))
            print("  this host:     %s" % json.dumps(fp))
            return 2 if a.require_host else 0

    print("perf-gate: %d program(s), min of %d run(s), counts not wall time%s"
          % (len(names), a.runs, "" if lib else " (no alloc counter on this OS)"))
    try:
        cur, ver = measure(a.tkc, names, a.runs, count, lib)
    except Harness as e:
        print("perf-gate: harness failure: %s" % e)
        return 2

    if a.record:
        doc = {"story": "123.11", "host": fp, "tkc_version": ver,
               "runs": a.runs, "thresholds": {k: {"pct": v[0], "abs": v[1]}
                                              for k, v in THRESHOLDS.items()},
               "programs": cur}
        pathlib.Path(a.baseline).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
        print("recorded %d program(s) into %s" % (len(cur), a.baseline))
        return 0

    bad, better, compared = [], [], 0
    for name, rec in cur.items():
        b = base["programs"].get(name)
        if b is None:
            bad.append("  %-16s not in the baseline (re-record deliberately)" % name)
            continue
        if "exit_status" in b and rec.get("exit_status") != b["exit_status"]:
            bad.append("  %-16s exit_status     %12s -> %12s  (the program's RESULT changed)"
                       % (name, b["exit_status"], rec.get("exit_status")))
        for metric, (pct, abs_) in THRESHOLDS.items():
            if metric not in rec or metric not in b:
                continue
            compared += 1
            old, new = b[metric], rec[metric]
            limit = old * (1 + pct) + abs_
            delta = (new - old) / old * 100 if old else 0.0
            line = "  %-16s %-15s %12d -> %12d  (%+.1f%%, limit +%g%%%s)" % (
                name, metric, old, new, delta, pct * 100,
                " +%d" % abs_ if abs_ else "")
            if new > limit:
                bad.append(line)
            elif new < old * (1 - pct) - abs_:
                better.append(line)
    for n in sorted(set(base["programs"]) - set(cur)):
        if not a.only:
            bad.append("  %-16s in the baseline but no longer measured" % n)
    if better:
        print("improved beyond threshold (re-record the baseline deliberately):")
        print("\n".join(better))
    if bad:
        print("perf-gate: FAIL -- %d regression(s) against %s:" % (len(bad), a.baseline))
        print("\n".join(bad))
        return 1
    print("perf-gate: OK -- %d comparison(s) over %d program(s) within threshold"
          % (compared, len(cur)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
