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
                 instructions:u`; else valgrind's cachegrind, 127.451).  tkc's own work only: the C toolchain it
                 invokes is a child process and is not counted.
  compile_allocs malloc+calloc+realloc calls made by tkc during that compile
                 (bench/patterns/alloccount.c interposer; macOS only)
  run_insns      instructions retired running the produced binary
  run_allocs     malloc-family calls made by the produced binary (macOS only)

Instruction counts are not perfectly deterministic.  Measured on an Apple M2 Pro
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

NEGATIVE CONTROLS (both sides now shown red, then green):
  compile  a tkc with a 20M-iteration loop injected into main() ->
           FAIL, compile_insns ~+90% (Batch P, 2026-09-24).
  run      (2026-09-26, 123.11) a tkc built with compile_binary() forcing
           clang to -O0 whatever -O was asked for (codegen-level injection;
           tkc's own work unchanged) -> "FAIL -- 11 regression(s)", every one
           run_insns, +53.7% (large_expr) to +674.1% (chained_calls).  The
           same injection at -O1 -> "FAIL -- 1 regression(s)", chained_calls
           run_insns +46.1%.  The unmodified tkc -> "OK -- 48 comparison(s)".
           struct_ops did not move even at -O0 (16.25M -> 16.55M).  The
           cause (123.13 (3), 2026-09-27) was not start-up as such: the old
           program overflowed i64 on about iteration 40 of 1M and died with
           RT002, so it measured start-up plus 40 iterations.  It now keeps
           its components bounded and runs all 1M (~580M run_insns), and a
           program that ends in a runtime trap fails the gate (KNOWN_TRAPS).
           Control, the same -O0 injection (not committed): struct_ops
           run_insns 583.3M -> 848.4M, "+45.5%" red (the old program under
           it: 17.8M -> 17.0M, blind); the unmodified tkc -> "OK -- 48
           comparison(s)".  A subtler injection -- `noinline` on every toke
           function (llvm.c attributes #0) -- moves struct_ops +6.9%, under
           the 8% threshold, and turns chained_calls red (+197.9%).
  Do NOT use a wrapper executable as the control: exec'ing through any
  wrapper, even an identity one, adds ~16M instructions to compile_insns, which
  put nested_loops (~46M) falsely red at +34.5%.  A shell-script wrapper is
  worse: SIP strips DYLD_INSERT_LIBRARIES, so the alloc counter sees nothing.

THE BASELINE IS HOST-SPECIFIC: instruction counts depend on the CPU, OS, and
the C compiler tkc drives.  The baseline records a fingerprint; on a host whose
fingerprint differs the gate prints a named SKIP and exits 0 (use
--require-host to make that a failure).  CI therefore does not compare against
the checked-in file: bench.yml measures the PR's base commit and head commit
on the same runner (--record into a temp file with the base compiler, then
compare the head compiler against it), on a Linux runner under cachegrind:
GitHub's VM runners expose no hardware counter (127.451), and cachegrind's
counts are exact, so a run there is measured once (--runs defaults to 1) and
carries no alloc counts (the interposer is macOS-only).

Usage:
  perf_gate.py [--tkc PATH] [--baseline FILE] [--runs N] [--only NAME ...]
  perf_gate.py --record [--baseline FILE] [--only NAME ...]
    (--record --only replaces just the named entries of an existing
     same-host baseline; entries so recorded are listed in partial_records)
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

# 123.13 (3): a bench program must run to completion.  struct_ops used to
# overflow i64 on about its 40th of 1M iterations and die with RT002, so its
# "checksum" was the trap's exit status and its run_insns (~16M) was process
# start-up plus 40 iterations -- the gate could not see codegen in it.  A
# program whose run ends in a runtime trap now fails the gate, recording
# included.  The two below trap the same way on main and are carried as known
# until they are rewritten (their baseline entries must be re-recorded then);
# an entry here that stops trapping also fails, so the list only shrinks.
# 123.13 (2): chained_calls and large_expr were the last two; their inputs
# are now bounded (chain(i%4093), compute(i%1024), C twins alike) and both run
# to completion with the C twin's checksum, so the list is empty.
KNOWN_TRAPS = {}
TRAP_RE = re.compile(r"^(RT\d{3}:.*)$", re.M)

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
    """Return (name, function(argv, env, cwd, any_rc) -> (instructions, proc)),
    or None when this host has no instruction counter.

    127.451: every counter is PROBED before it is used.  A macOS VM (GitHub's
    macos-* runners) has no performance counters, and there /usr/bin/time -l
    prints its resource table without an "instructions retired" line; the
    gate took any macOS host as "has a counter" and then reported each clean
    run as a harness failure (exit 2) on every pull request.  The order is:
    macOS /usr/bin/time -l, Linux perf (instructions:u), then valgrind's
    cachegrind, which counts instructions in software -- exact and
    deterministic, so it works on a VM with no hardware counter (GitHub's
    runners; test/perf/strlen/scaling.sh uses it the same way since 127.439).
    The first two are "hw"; cachegrind's counts are not comparable with
    theirs, so the counter's name is recorded in the baseline and compared.
    """
    probe_env = os.environ.copy()
    if platform.system() == "Darwin" and os.path.exists("/usr/bin/time"):
        def mac(argv, env, cwd, any_rc=False):
            p = subprocess.run(["/usr/bin/time", "-l"] + argv, env=env, cwd=cwd,
                               capture_output=True, text=True, timeout=600)
            m = re.search(r"^\s*(\d+)\s+instructions retired", p.stderr, re.M)
            if (p.returncode != 0 and not any_rc) or not m:
                raise Harness("%s exited %d (stderr tail: %s)"
                              % (argv[0], p.returncode, p.stderr[-300:]))
            return int(m.group(1)), p
        try:
            mac(["/usr/bin/true"], probe_env, None)
            return "hw", mac
        except Harness:
            pass
    if platform.system() == "Linux" and shutil.which("perf"):
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
            linux(["true"], probe_env, None)
            return "hw", linux
        except Harness:
            pass
    if shutil.which("valgrind"):
        def cachegrind(argv, env, cwd, any_rc=False):
            # --trace-children=no (the default): the process's own
            # instructions only, as with the hardware counters -- the C
            # toolchain tkc spawns is not counted.  The summary line is
            # "==PID== I refs: 1,234,567"; the program's own stderr shares
            # the stream, hence the anchor.
            p = subprocess.run(["valgrind", "--tool=cachegrind", "--cache-sim=no",
                                "--cachegrind-out-file=/dev/null"] + argv,
                               env=env, cwd=cwd, capture_output=True, text=True,
                               timeout=1800)
            m = re.search(r"^==\d+==\s+I\s+refs:\s+([\d,]+)", p.stderr, re.M)
            if (p.returncode != 0 and not any_rc) or not m:
                raise Harness("%s exited %d under cachegrind (stderr tail: %s)"
                              % (argv[0], p.returncode, p.stderr[-300:]))
            return int(m.group(1).replace(",", "")), p
        try:
            cachegrind(["true"], probe_env, None)
            return "cachegrind", cachegrind
        except (Harness, OSError, subprocess.SubprocessError):
            pass
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
    out, traps = {}, {}
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
            trap = TRAP_RE.search(rr[0][1].stderr or "")
            if trap:
                traps[name] = trap.group(1).strip()
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
    return out, traps, subprocess.run([str(tkc), "--version"], capture_output=True,
                                      text=True).stdout.strip()


def trap_problems(names, traps):
    """A program that dies in a runtime trap measures nothing (123.13 (3))."""
    bad = []
    for name in names:
        if name in traps and name not in KNOWN_TRAPS:
            bad.append("  %-16s run ends in a runtime trap (%s): its loop does not run "
                       "to completion, so run_insns cannot see codegen and exit_status "
                       "is the trap's, not a checksum" % (name, traps[name]))
        elif name in KNOWN_TRAPS and name not in traps:
            bad.append("  %-16s no longer traps: delete it from KNOWN_TRAPS in %s"
                       % (name, pathlib.Path(__file__).name))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tkc", default=os.environ.get("TKC", str(ROOT / "tkc")))
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE))
    ap.add_argument("--runs", type=int, default=None,
                    help="runs per count, minimum taken (default 5 on a hardware "
                         "counter, 1 under cachegrind, whose counts are exact)")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--require-host", action="store_true",
                    help="a host-fingerprint mismatch fails instead of skipping")
    a = ap.parse_args()

    cc = os.environ.get("CC", "cc")
    fp = fingerprint(cc)
    found = counter()
    if found is None:
        print("SKIP perf-gate: no instruction counter on this host (macOS "
              "/usr/bin/time -l reports none on a VM; Linux perf with "
              "instructions:u; or valgrind's cachegrind). Nothing was measured.")
        return 2 if a.require_host else 0
    counter_name, count = found
    if a.runs is None:
        a.runs = 1 if counter_name == "cachegrind" else 5
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
        # 127.451: a baseline from before the counter was recorded is "hw".
        if base.get("host") != fp or base.get("counter", "hw") != counter_name:
            print("SKIP perf-gate: this host is not the baseline's host, so its "
                  "counts are not comparable. Nothing was compared.")
            print("  baseline host: %s" % json.dumps(base.get("host")))
            print("  this host:     %s" % json.dumps(fp))
            print("  counter:       baseline %s, this host %s"
                  % (base.get("counter", "hw"), counter_name))
            return 2 if a.require_host else 0

    print("perf-gate: %d program(s), min of %d run(s), counts not wall time "
          "(counter: %s)%s" % (len(names), a.runs, counter_name,
                               "" if lib else " (no alloc counter on this OS)"))
    try:
        cur, traps, ver = measure(a.tkc, names, a.runs, count, lib)
    except Harness as e:
        print("perf-gate: harness failure: %s" % e)
        return 2
    trapped = trap_problems(names, traps)

    if a.record:
        if trapped:
            print("perf-gate: refusing to record -- %d program(s) do not measure what "
                  "they claim:" % len(trapped))
            print("\n".join(trapped))
            return 1
        programs = cur
        if a.only:
            # 123.13 (3): --record --only used to write a baseline holding only
            # the selected programs, silently dropping every other entry.  It
            # now replaces just those entries, and only on the same host.
            try:
                old = json.loads(pathlib.Path(a.baseline).read_text())
            except (OSError, ValueError):
                old = None
            if old is not None:
                if old.get("host") != fp or old.get("counter", "hw") != counter_name:
                    print("perf-gate: --record --only into a baseline from another host "
                          "would mix incomparable counts; record all programs instead")
                    return 2
                programs = dict(old.get("programs", {}))
                programs.update(cur)
        doc = {"story": "123.11", "host": fp, "tkc_version": ver,
               "counter": counter_name,
               "runs": a.runs, "thresholds": {k: {"pct": v[0], "abs": v[1]}
                                              for k, v in THRESHOLDS.items()},
               "programs": programs}
        if programs is not cur:
            # say which entries came from a different compiler than the rest
            doc["tkc_version"] = old.get("tkc_version", ver)
            partial = dict(old.get("partial_records", {}))
            partial.update({n: ver for n in cur})
            doc["partial_records"] = partial
        pathlib.Path(a.baseline).write_text(json.dumps(doc, indent=1, sort_keys=True) + "\n")
        print("recorded %d program(s) into %s" % (len(cur), a.baseline))
        return 0

    bad, better, compared = list(trapped), [], 0
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
