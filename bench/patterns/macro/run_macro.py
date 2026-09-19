#!/usr/bin/env python3
"""run_macro.py — 131.8 macro check: original vs canonical-pattern rewrite, per real program.

For every pair in pairs.json (12 bench/programs + the 32-program library sample,
see prepare.py) it builds `<name>.orig.tk` and `<name>.canon.tk` with the pinned
tkc (`-O2 --allow-all`, scripts/patterns/tkc_pin.py — 131.39) and measures:

  * wall      — perf_counter around fork/exec -> exit of the binary, 2 warm-up +
                N timed runs (default 15), orig and canon INTERLEAVED so drift
                hits both; median + percentile-bootstrap 95 % CI (run_patterns.py's)
  * peak RSS  — ru_maxrss from os.wait4 (same method as run_patterns.py)
  * cpu       — ru_utime+ru_stime from the same wait4 (informational: it is
                load-insensitive, so it separates a real regression from the
                wall noise of a busy machine; the verdict stays on wall+RSS)
  * output    — stdout sha256 + exit status on every timed run; orig and canon
                must agree on all of them (else verdict output_mismatch)
  * tokens    — scripts/patterns/count_tokens.py on the WHOLE program (`tkc --min`
                text, strings masked; proxy8k is the decision metric). Whole
                program, not `--function main`: the rewrites live in helper
                functions as often as in main. For a pair whose canon was laid
                out by `tkc --fmt` (patterns.json "reformatted": true) the fmt'd
                original is measured too (`orig_fmt`) so the delta reported
                excludes the formatter's own normalisations (`@($str)` -> `@$str`).

A "run" of a bench program is one execution with no stdin (its result is the
exit status; nothing is printed). A run of a library program is one execution
per manifest test case with the case input on stdin (validate.py semantics,
fresh cwd per case); wall = sum over the cases, RSS = max over the cases.

Verdict per pair (protocol §5.4, 5 % gate on both axes, canon relative to orig):
  ok               wall and RSS medians within +5 % (faster/smaller is fine)
  regressed        wall or RSS > +5 %; `suspects` = the pattern ids applied
  unchanged        canon's `tkc --min` text equals the original's — nothing to
                   measure; the numbers are the harness's own noise floor
  output_mismatch  stdout/exit differ on some run (verify.py should have caught it)
  compile_error    a side failed to build

Load: refuses when the 1-min loadavg exceeds --max-load (default 2.0) unless
--allow-load, in which case meta.load_warning is true and the run is informational.
Results: results/<YYYYMMDD-HHMMSS>.json plus a markdown table on stdout (--md FILE
also writes it). macOS only (ru_maxrss units). Stdlib only.
"""
import argparse
import datetime as dt
import hashlib
import json
import os
import platform
import select
import signal
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent                  # bench/patterns/macro
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "scripts" / "patterns"))
sys.path.insert(0, str(HERE.parent))
import tkc_pin  # noqa: E402
import run_patterns as rp  # noqa: E402  (bootstrap_ci, ci_overlap, gate constants)

PAIRS = HERE / "pairs.json"
BUILD = HERE / "build"
RESULTS = HERE / "results"
WALL_TOL = rp.WALL_TOL           # 0.05
RSS_TOL = rp.RSS_TOL             # 0.05
BENCH_TIMEOUT = 120.0
CASE_TIMEOUT = 20.0
TKC = None


def die(msg, code=2):
    print(f"run_macro: {msg}", file=sys.stderr)
    sys.exit(code)


def sha256_file(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def sh(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout.strip()
    except Exception:
        return ""


# ─────────────────────────── one execution ──────────────────────────────────
def run_once(binary, stdin_bytes, cwd, timeout_s):
    """wall = perf_counter around spawn -> exit; rss = ru_maxrss via os.wait4
    (bytes on macOS). stdin is written fully before the output loop starts."""
    res = {"wall_ms": None, "cpu_ms": None, "rss_kb": None, "stdout": b"", "status": None, "timed_out": False}
    t0 = time.perf_counter()
    p = subprocess.Popen([str(binary)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, cwd=str(cwd))
    try:
        p.stdin.write(stdin_bytes)
    except BrokenPipeError:
        pass
    p.stdin.close()
    chunks = []
    fd = p.stdout.fileno()
    deadline = t0 + timeout_s
    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            try:
                os.kill(p.pid, signal.SIGKILL)   # not p.kill(): keep the child reapable by wait4
            except ProcessLookupError:
                pass
            res["timed_out"] = True
            break
        ready, _, _ = select.select([fd], [], [], remaining)
        if ready:
            data = os.read(fd, 65536)
            if not data:
                break
            chunks.append(data)
    try:
        _, status, ru = os.wait4(p.pid, 0)
        rss_bytes = ru.ru_maxrss
        cpu_ms = (ru.ru_utime + ru.ru_stime) * 1000.0
    except ChildProcessError:
        status, rss_bytes, cpu_ms = 0, 0, 0.0
    res["wall_ms"] = (time.perf_counter() - t0) * 1000.0
    res["cpu_ms"] = cpu_ms
    p.stdout.close()
    res["status"] = -9 if res["timed_out"] else os.waitstatus_to_exitcode(status)
    res["stdout"] = b"".join(chunks)
    res["rss_kb"] = rss_bytes // 1024 if sys.platform == "darwin" else rss_bytes
    return res


class Workload:
    """One 'run' = the pair's own workload: a bench program once with no stdin,
    a library program once per manifest test case (fresh cwd per case)."""

    def __init__(self, pair, tmp):
        self.pair = pair
        self.cases = []
        if pair["kind"] == "bench":
            self.cases = [(b"", ROOT, BENCH_TIMEOUT)]
        else:
            for i, tc in enumerate(pair["cases"]):
                cwd = Path(tmp) / f"t{i}"
                cwd.mkdir(parents=True, exist_ok=True)
                if tc.get("fixtures"):
                    _materialise(tc["fixtures"], cwd)
                self.cases.append(((tc.get("input") or "").encode(), cwd, CASE_TIMEOUT))

    def run(self, binary):
        """Returns (wall_ms_sum, cpu_ms_sum, rss_kb_max, signature, timed_out);
        signature = sha256 over every case's exit status and stdout, in order."""
        h = hashlib.sha256()
        wall, cpu, rss = 0.0, 0.0, 0
        for stdin_bytes, cwd, to in self.cases:
            r = run_once(binary, stdin_bytes, cwd, to)
            if r["timed_out"]:
                return None, None, None, None, True
            wall += r["wall_ms"]
            cpu += r["cpu_ms"]
            rss = max(rss, r["rss_kb"])
            h.update(f"<exit {r['status']}>".encode())
            h.update(r["stdout"])
        return wall, cpu, rss, h.hexdigest(), False


def _materialise(fx, cwd):
    for d in fx.get("dirs") or []:
        os.makedirs(d if os.path.isabs(d) else os.path.join(cwd, d), exist_ok=True)
    for fpath, content in (fx.get("files") or {}).items():
        ap = fpath if os.path.isabs(fpath) else os.path.join(cwd, fpath)
        os.makedirs(os.path.dirname(ap), exist_ok=True)
        with open(ap, "wb") as f:
            f.write(content.encode("latin-1"))


# ─────────────────────────── compile / tokens ───────────────────────────────
def compile_src(name, side, src):
    BUILD.mkdir(parents=True, exist_ok=True)
    out = BUILD / f"{name}.{side}"
    r = subprocess.run([TKC, "-O2", "--allow-all", str(src), "--out", str(out)],
                       capture_output=True, text=True, cwd=str(ROOT))
    if r.returncode != 0 or not out.exists():
        return None, (r.stderr or r.stdout).strip()[:2000]
    return out, None


def min_sha(src):
    r = subprocess.run([TKC, "--min", str(src)], capture_output=True, text=True, cwd=str(ROOT))
    return hashlib.sha256(r.stdout.encode()).hexdigest() if r.returncode == 0 else None


def fmt_copy(name, src):
    out = BUILD / f"{name}.origfmt.tk"
    r = subprocess.run([TKC, "--fmt", str(src)], capture_output=True, text=True, cwd=str(ROOT))
    if r.returncode != 0:
        return None
    out.write_text(r.stdout)
    return out


def measure_tokens(counter, src):
    if counter is None:
        return None
    try:
        r = counter.measure_file(src, function=None)          # whole program
    except Exception as e:                                       # pragma: no cover
        return {"error": str(e)[:300]}
    return {"proxy8k": r["tokens"]["proxy8k"], "byte256": r["tokens"]["byte256"],
            "v03": r["tokens"].get("v03"), "qwen25coder": r["tokens"].get("qwen25coder"),
            "cl100k": r["tokens"].get("cl100k"), "min_bytes": r["min_bytes"]}


# ─────────────────────────── one pair ───────────────────────────────────────
def pct(new, base):
    return None if not base else round((new / base - 1.0) * 100.0, 2)


def measure_pair(pair, args, counter, log):
    name = pair["name"]
    e = {"name": name, "kind": pair["kind"], "category": pair.get("category"), "status": "ok",
         "sides": {}, "patterns_applied": [], "verdict": None}
    pj = HERE / pair["patterns"]
    patterns = json.loads(pj.read_text()) if pj.exists() else {}
    e["patterns_applied"] = [a["id"] for a in patterns.get("applied", [])]
    e["patterns_not_applied"] = patterns.get("not_applied", [])
    e["reformatted"] = bool(patterns.get("reformatted"))
    srcs = {"orig": HERE / pair["orig"], "canon": HERE / pair["canon"]}
    bins = {}
    for side, src in srcs.items():
        s = {"file": str(src.relative_to(ROOT)), "src_sha256": sha256_file(src) if src.exists() else None}
        e["sides"][side] = s
        if not src.exists():
            s["status"] = "missing"
            e["status"] = "compile_error"
            continue
        s["min_sha256"] = min_sha(src)
        b, err = compile_src(name, side, src)
        if b is None:
            s["status"] = "compile_error"
            s["compile_error"] = err
            e["status"] = "compile_error"
            continue
        s["status"] = "ok"
        s["binary_bytes"] = b.stat().st_size
        s["tokens"] = measure_tokens(counter, src)
        bins[side] = b
    if e["reformatted"] and srcs["orig"].exists():
        f = fmt_copy(name, srcs["orig"])
        if f is not None:
            e["sides"]["orig_fmt"] = {"file": str(f.relative_to(ROOT)), "status": "ok",
                                      "min_sha256": min_sha(f), "tokens": measure_tokens(counter, f)}
    if e["status"] != "ok":
        e["verdict"] = "compile_error"
        return e
    unchanged = e["sides"]["orig"]["min_sha256"] == e["sides"]["canon"]["min_sha256"]
    e["min_identical"] = unchanged

    with tempfile.TemporaryDirectory(prefix=f"macro-{name}-") as td:
        wl = Workload(pair, td)
        samples = {"orig": [], "canon": []}
        cpus = {"orig": [], "canon": []}
        rsss = {"orig": [], "canon": []}
        sigs = {"orig": set(), "canon": set()}
        order = ["orig", "canon"]
        for i in range(args.warmup + args.runs):
            for side in (order if i % 2 == 0 else order[::-1]):     # interleave, alternate order
                wall, cpu, rss, sig, to = wl.run(bins[side])
                if to:
                    e["sides"][side]["status"] = "timeout"
                    e["status"] = "timeout"
                    e["verdict"] = "timeout"
                    return e
                if i >= args.warmup:
                    samples[side].append(wall)
                    cpus[side].append(cpu)
                    rsss[side].append(rss)
                    sigs[side].add(sig)
    for side in order:
        s = e["sides"][side]
        s["wall_ms_samples"] = [round(w, 3) for w in samples[side]]
        s["wall_ms_median"] = round(statistics.median(samples[side]), 3)
        s["wall_ci95"] = rp.bootstrap_ci(samples[side])
        s["cpu_ms_samples"] = [round(c, 3) for c in cpus[side]]
        s["cpu_ms_median"] = round(statistics.median(cpus[side]), 3)
        s["cpu_ci95"] = rp.bootstrap_ci(cpus[side])
        s["rss_kb_median"] = int(statistics.median(rsss[side]))
        s["rss_kb_samples"] = rsss[side]
        s["output_sigs"] = sorted(sigs[side])
        log(f"    {side:<5} {s['wall_ms_median']:8.2f} ms  ci {s['wall_ci95']}  cpu {s['cpu_ms_median']:.2f} ms"
            f"  rss {s['rss_kb_median']} KB"
            f"  tokens {s['tokens']['proxy8k'] if s.get('tokens') else '-'}")
    o, c = e["sides"]["orig"], e["sides"]["canon"]
    e["wall_pct"] = pct(c["wall_ms_median"], o["wall_ms_median"])
    e["rss_pct"] = pct(c["rss_kb_median"], o["rss_kb_median"])
    e["cpu_pct"] = pct(c["cpu_ms_median"], o["cpu_ms_median"])      # informational: load-insensitive
    e["cpu_ci_overlap"] = rp.ci_overlap(o["cpu_ci95"], c["cpu_ci95"])
    e["ci_overlap"] = rp.ci_overlap(o["wall_ci95"], c["wall_ci95"])
    base = e["sides"].get("orig_fmt") or o
    if o.get("tokens") and c.get("tokens"):
        e["tokens_delta"] = {"proxy8k": c["tokens"]["proxy8k"] - o["tokens"]["proxy8k"],
                             "proxy8k_pct": pct(c["tokens"]["proxy8k"], o["tokens"]["proxy8k"]),
                             "byte256": c["tokens"]["byte256"] - o["tokens"]["byte256"],
                             "min_bytes": c["tokens"]["min_bytes"] - o["tokens"]["min_bytes"]}
        if base is not o and base.get("tokens"):
            e["tokens_delta_vs_fmt"] = {"proxy8k": c["tokens"]["proxy8k"] - base["tokens"]["proxy8k"],
                                        "proxy8k_pct": pct(c["tokens"]["proxy8k"], base["tokens"]["proxy8k"]),
                                        "min_bytes": c["tokens"]["min_bytes"] - base["tokens"]["min_bytes"]}
    if sigs["orig"] != sigs["canon"] or len(sigs["orig"]) != 1:
        e["status"] = "output_mismatch"
        e["verdict"] = "output_mismatch"
        e["suspects"] = e["patterns_applied"]
    elif unchanged:
        e["verdict"] = "unchanged"
    elif e["wall_pct"] <= WALL_TOL * 100 and e["rss_pct"] <= RSS_TOL * 100:
        e["verdict"] = "ok"
    else:
        e["verdict"] = "regressed"
        e["suspects"] = e["patterns_applied"]
        e["regression"] = {"wall": e["wall_pct"] > WALL_TOL * 100, "rss": e["rss_pct"] > RSS_TOL * 100,
                           "ci_overlap": e["ci_overlap"]}
    return e


# ─────────────────────────── meta / output ──────────────────────────────────
def collect_meta(args, pin, load, counter):
    return {
        "story": "131.8", "date": dt.datetime.now().isoformat(timespec="seconds"),
        "hw_model": sh(["sysctl", "-n", "hw.model"]), "ncpu": os.cpu_count(),
        "os": f"{platform.system()} {platform.release()}",
        "loadavg_1min": round(load, 2), "max_load": args.max_load,
        "load_warning": load > args.max_load,
        "therm": [l.strip() for l in sh(["pmset", "-g", "therm"]).splitlines() if l.strip()],
        **pin.stamp(),
        "proxy_file": counter.proxy_path.name if counter else None,
        "proxy_sha": counter.proxy_sha if counter else None,
        "token_region": "whole program (tkc --min, strings masked); orig_fmt measured for reformatted pairs",
        "runs": args.runs, "warmup": args.warmup, "interleaved": True,
        "gate": {"wall_tol": WALL_TOL, "rss_tol": RSS_TOL,
                 "rule": "ok iff canon median wall <= orig+5% AND canon median RSS <= orig+5%; "
                         "unchanged when tkc --min texts are equal; regressed names the applied pattern ids"},
        "workload": "bench: 1 exec, no stdin (exit status is the result); library: 1 exec per manifest "
                    "test case with stdin, wall summed, RSS max",
    }


def md_table(results):
    rows = ["| pair | kind | patterns | tokens orig→canon (Δ) | wall orig→canon ms (Δ%) | cpu Δ% | RSS orig→canon KB (Δ%) | verdict |",
            "|---|---|---|---|---|---|---|---|"]
    for e in results["pairs"]:
        o, c = e["sides"].get("orig", {}), e["sides"].get("canon", {})
        pats = ", ".join(e["patterns_applied"]) or "—"
        if e["verdict"] in ("compile_error", "timeout"):
            rows.append(f"| {e['name']} | {e['kind']} | {pats} | – | – | – | – | {e['verdict']} |")
            continue
        td = e.get("tokens_delta_vs_fmt") or e.get("tokens_delta") or {}
        base = e["sides"].get("orig_fmt") or o
        tok = (f"{base['tokens']['proxy8k']}→{c['tokens']['proxy8k']} ({td.get('proxy8k', 0):+d})"
               if base.get("tokens") and c.get("tokens") else "–")
        if e.get("tokens_delta_vs_fmt"):
            tok += "†"
        wall = f"{o['wall_ms_median']:.1f}→{c['wall_ms_median']:.1f} ({e['wall_pct']:+.1f}%)"
        rss = f"{o['rss_kb_median']}→{c['rss_kb_median']} ({e['rss_pct']:+.1f}%)"
        v = e["verdict"]
        if v == "regressed":
            v += " → " + ", ".join(e.get("suspects") or [])
        cpu = f"{e['cpu_pct']:+.1f}%"
        rows.append(f"| {e['name']} | {e['kind']} | {pats} | {tok} | {wall} | {cpu} | {rss} | {v} |")
    return "\n".join(rows)


def summary(results):
    ps = [e for e in results["pairs"] if e["verdict"] not in ("compile_error", "timeout")]
    changed = [e for e in ps if e["verdict"] != "unchanged"]
    tok = [e.get("tokens_delta_vs_fmt") or e.get("tokens_delta") for e in changed]
    tok = [t for t in tok if t]
    s = {"pairs": len(results["pairs"]), "measured": len(ps),
         "verdicts": {v: sum(e["verdict"] == v for e in results["pairs"])
                      for v in ("ok", "regressed", "unchanged", "output_mismatch", "compile_error", "timeout")},
         "changed_pairs": len(changed),
         "tokens_proxy8k_sum_delta": sum(t["proxy8k"] for t in tok),
         "tokens_proxy8k_median_delta": statistics.median(t["proxy8k"] for t in tok) if tok else None,
         "tokens_proxy8k_median_pct": statistics.median(t["proxy8k_pct"] for t in tok) if tok else None,
         "tokens_orig_sum": sum((e["sides"].get("orig_fmt") or e["sides"]["orig"])["tokens"]["proxy8k"]
                                for e in changed if e["sides"]["canon"].get("tokens")),
         "tokens_canon_sum": sum(e["sides"]["canon"]["tokens"]["proxy8k"] for e in changed if e["sides"]["canon"].get("tokens")),
         "wall_pct_median_changed": statistics.median(e["wall_pct"] for e in changed) if changed else None,
         "rss_pct_median_changed": statistics.median(e["rss_pct"] for e in changed) if changed else None,
         "wall_pct_median_unchanged": statistics.median(e["wall_pct"] for e in ps if e["verdict"] == "unchanged")
         if any(e["verdict"] == "unchanged" for e in ps) else None,
         "cpu_pct_median_changed": statistics.median(e["cpu_pct"] for e in changed) if changed else None,
         "regressed": [{"name": e["name"], "wall_pct": e["wall_pct"], "cpu_pct": e["cpu_pct"], "rss_pct": e["rss_pct"],
                        "ci_overlap": e["ci_overlap"], "cpu_ci_overlap": e["cpu_ci_overlap"], "suspects": e.get("suspects")}
                       for e in ps if e["verdict"] == "regressed"]}
    return s


def main():
    ap = argparse.ArgumentParser(description="131.8 macro check: original vs canonical rewrite")
    ap.add_argument("names", nargs="*", help="pair names (default: all in pairs.json)")
    ap.add_argument("--runs", type=int, default=15)
    ap.add_argument("--warmup", type=int, default=2)
    ap.add_argument("--max-load", type=float, default=rp.DEFAULT_MAX_LOAD)
    ap.add_argument("--allow-load", action="store_true",
                    help="run even above --max-load; results carry meta.load_warning: true")
    ap.add_argument("--no-tokens", action="store_true", help="skip the token count")
    ap.add_argument("--external", action="store_true", help="also count v03/qwen/cl100k (slow to load)")
    ap.add_argument("--md", help="also write the markdown table to this file")
    ap.add_argument("--json-only", action="store_true")
    args = ap.parse_args()
    if sys.platform != "darwin":
        die("macOS only (ru_maxrss units)")
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    global TKC
    pin = tkc_pin.pin(toke_repo=ROOT).install()
    TKC = pin.argv0
    load = os.getloadavg()[0]
    if load > args.max_load and not args.allow_load:
        die(f"1-min loadavg {load:.2f} > {args.max_load} — retry later, raise --max-load, or --allow-load")
    if load > args.max_load:
        print(f"run_macro: WARNING loadavg {load:.2f} > {args.max_load}; results carry load_warning: true",
              file=sys.stderr)
    counter = None
    if not args.no_tokens:
        import count_tokens                      # after pin.install(): binds to the pinned tkc
        counter = count_tokens.Counter(external=args.external)
    log = (lambda *a: None) if args.json_only else (lambda *a: print(*a, flush=True))
    pairs = json.loads(PAIRS.read_text())["pairs"]
    if args.names:
        want = set(args.names)
        pairs = [p for p in pairs if p["name"] in want]
        if len(pairs) != len(want):
            die(f"unknown pair(s): {sorted(want - {p['name'] for p in pairs})}")
    results = {"meta": collect_meta(args, pin, load, counter), "pairs": []}
    log(f"run_macro: {pin.version} @ {(pin.toke_head or '?')[:12]}{' (src dirty)' if pin.toke_dirty else ''}"
        f"  load {load:.2f}  runs {args.runs}  pairs {len(pairs)}")
    try:
        for p in pairs:
            log(f"\n[{p['name']}] {p['kind']}")
            e = measure_pair(p, args, counter, log)
            results["pairs"].append(e)
            log(f"    -> {e['verdict']}" + (f"  wall {e['wall_pct']:+.1f}%  cpu {e['cpu_pct']:+.1f}%  rss {e['rss_pct']:+.1f}%"
                                            if e.get("wall_pct") is not None else ""))
    finally:
        pin.close()
    results["summary"] = summary(results)
    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = RESULTS / f"{stamp}.json"
    out.write_text(json.dumps(results, indent=1) + "\n")
    table = md_table(results)
    if args.md:
        Path(args.md).write_text(table + "\n")
    if args.json_only:
        print(out)
    else:
        print()
        print(table)
        print()
        print(json.dumps(results["summary"], indent=1))
        print(f"results: {out.relative_to(ROOT)}")
    return 1 if results["summary"]["verdicts"]["regressed"] or results["summary"]["verdicts"]["output_mismatch"] else 0


if __name__ == "__main__":
    sys.exit(main())
