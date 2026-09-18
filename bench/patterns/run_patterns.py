#!/usr/bin/env python3
"""run_patterns.py — runtime micro-benchmark harness for pattern fixtures (Epic 131.5).

Discovers  patterns/<id>/<form>.tk  (form = one letter a-z; *.blocked.tk skipped),
compiles each with `tkc -O2 --allow-all`, auto-sizes PAT_N per pattern so the
fastest form runs >= 50 ms, then measures every form:

  * wall     — 2 warm-up + N timed runs (default 15); median + bootstrap 95% CI
  * peak RSS — ru_maxrss of the child from os.wait4 (same kernel counter that
               /usr/bin/time -l prints; used directly because `time -lp`'s
               `real` has 10 ms resolution, too coarse for a 5 % gate at 50 ms)
  * allocs   — one extra run with DYLD_INSERT_LIBRARIES=liballoccount.dylib
  * big-O    — wall at 4N vs N; forms must agree within 1.5x
  * output   — stdout must be byte-identical across forms (else output_mismatch)

Gate (131.1, fixed): a form is runtime-tied with the best form iff its median
wall is within 5 %, the 95 % CIs overlap, median peak RSS is within 5 % and the
big-O ratio agrees within 1.5x. Allocation count is a tie-break only.

Wall time includes fork/exec of the child (~1-2 ms, identical for all forms).
Results: bench/patterns/results/<YYYYMMDD-HHMMSS>.json. Stdlib only. macOS only.
"""
import argparse
import atexit
import datetime as dt
import hashlib
import json
import math
import os
import platform
import random
import select
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent            # bench/patterns
ROOT = HERE.parents[1]                             # repo root
sys.path.insert(0, str(ROOT / "scripts" / "patterns"))
import tkc_pin  # noqa: E402  (131.39)

TKC = ROOT / "tkc"          # 131.39: main() replaces this with a pinned private copy
PIN = None
PATTERNS = ROOT / "patterns"
BUILD = HERE / "build"
RESULTS = HERE / "results"
DYLIB = HERE / "liballoccount.dylib"

FLOOR_MS = 50.0          # best form must run at least this long
WALL_TOL = 0.05          # tied: median wall within 5 % of best
RSS_TOL = 0.05           # tied: median peak RSS within 5 % of best
BIGO_TOL = 1.5           # tied: 4N/N wall ratio agrees within 1.5x
BOOTSTRAP = 1000         # resamples for the 95 % CI
DEFAULT_START_N = 1000
DEFAULT_MAX_N = 1 << 30
DEFAULT_TIMEOUT_S = 30.0
DEFAULT_MAX_LOAD = 2.0


# ─────────────────────────── helpers ────────────────────────────────────────
def die(msg, code=2):
    print(f"run_patterns: {msg}", file=sys.stderr)
    sys.exit(code)


def sh(cmd, **kw):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20, **kw).stdout.strip()
    except Exception:
        return ""


def sha256_file(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def bootstrap_ci(samples, resamples=BOOTSTRAP, seed=131):
    """Percentile bootstrap 95 % CI of the median."""
    if len(samples) < 2:
        return [samples[0], samples[0]] if samples else [None, None]
    rng = random.Random(seed)
    n = len(samples)
    meds = sorted(statistics.median(rng.choices(samples, k=n)) for _ in range(resamples))
    lo = meds[int(0.025 * resamples)]
    hi = meds[min(resamples - 1, int(0.975 * resamples))]
    return [round(lo, 3), round(hi, 3)]


def ci_overlap(a, b):
    return not (a[1] < b[0] or b[1] < a[0])


# ─────────────────────────── running one binary ─────────────────────────────
class RunResult:
    __slots__ = ("wall_ms", "rss_kb", "stdout", "stderr", "status", "timed_out")

    def __init__(self):
        self.wall_ms = None
        self.rss_kb = None
        self.stdout = b""
        self.stderr = b""
        self.status = None
        self.timed_out = False


def run_once(binary, n, timeout_s, extra_env=None, want_stderr=False):
    """Run `binary` with PAT_N=n. Wall = perf_counter around spawn→exit;
    RSS = ru_maxrss from os.wait4 (bytes on macOS)."""
    env = dict(os.environ, PAT_N=str(n))
    if extra_env:
        env.update(extra_env)
    res = RunResult()
    stderr_target = subprocess.PIPE if want_stderr else subprocess.DEVNULL
    t0 = time.perf_counter()
    p = subprocess.Popen([str(binary)], env=env, stdout=subprocess.PIPE, stderr=stderr_target,
                         cwd=str(ROOT))
    fds = {p.stdout.fileno(): []}
    if want_stderr:
        fds[p.stderr.fileno()] = []
    deadline = t0 + timeout_s
    open_fds = set(fds)
    while open_fds:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            # os.kill, not p.kill(): Popen.kill() polls first and would reap a
            # child that exited at the deadline, breaking os.wait4 below.
            try:
                os.kill(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            res.timed_out = True
            break
        ready, _, _ = select.select(list(open_fds), [], [], remaining)
        for fd in ready:
            data = os.read(fd, 65536)
            if data:
                fds[fd].append(data)
            else:
                open_fds.discard(fd)
    try:
        _, status, ru = os.wait4(p.pid, 0)
        rss_bytes = ru.ru_maxrss
    except ChildProcessError:            # already reaped (should not happen); keep going
        status = 0 if p.returncode in (None, 0) else p.returncode << 8
        rss_bytes = 0
        p.returncode = p.returncode if p.returncode is not None else -1
    res.wall_ms = (time.perf_counter() - t0) * 1000.0
    p.returncode = os.waitstatus_to_exitcode(status) if not res.timed_out else -9
    out_fd = p.stdout.fileno()
    err_fd = p.stderr.fileno() if want_stderr else None
    res.stdout = b"".join(fds[out_fd])
    if err_fd is not None:
        res.stderr = b"".join(fds[err_fd])
    for f in (p.stdout, p.stderr):
        if f:
            f.close()
    res.status = p.returncode
    res.rss_kb = rss_bytes // 1024 if sys.platform == "darwin" else rss_bytes
    return res


def parse_alloc(stderr_bytes):
    for line in stderr_bytes.decode("utf-8", "replace").splitlines():
        if line.startswith("ALLOC "):
            kv = dict(tok.split("=", 1) for tok in line.split()[1:] if "=" in tok)
            try:
                return {"calls": int(kv["calls"]), "bytes": int(kv["bytes"]),
                        "malloc": int(kv.get("malloc", 0)), "calloc": int(kv.get("calloc", 0)),
                        "realloc": int(kv.get("realloc", 0)), "free": int(kv.get("free", 0))}
            except (KeyError, ValueError):
                return None
    return None


# ─────────────────────────── discovery + compile ────────────────────────────
def discover(only):
    pats = {}
    if not PATTERNS.is_dir():
        die(f"no patterns directory at {PATTERNS}")
    for d in sorted(PATTERNS.iterdir()):
        if not d.is_dir() or d.name.startswith((".", "_")):
            continue
        if only and d.name not in only:
            continue
        forms = {}
        for f in sorted(d.glob("*.tk")):
            stem = f.name[:-3]
            if len(stem) == 1 and "a" <= stem <= "z":      # excludes x.blocked.tk
                forms[stem] = f
        if forms:
            cfg = {}
            bj = d / "bench.json"
            if bj.exists():
                try:
                    cfg = json.loads(bj.read_text())
                except json.JSONDecodeError as e:
                    die(f"{bj}: invalid JSON ({e})")
            pats[d.name] = {"dir": d, "forms": forms, "cfg": cfg}
    return pats


def compile_form(pid, form, src):
    BUILD.mkdir(parents=True, exist_ok=True)
    out = BUILD / f"{pid}_{form}"
    cmd = [str(TKC), "-O2", "--allow-all", str(src), "--out", str(out)]
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=str(ROOT))
    if r.returncode != 0 or not out.exists():
        return None, (r.stderr or r.stdout).strip()[:2000]
    return out, None


def ensure_dylib():
    if DYLIB.exists() and DYLIB.stat().st_mtime >= (HERE / "alloccount.c").stat().st_mtime:
        return True
    r = subprocess.run(["make", "-C", str(HERE), "liballoccount.dylib"], capture_output=True, text=True)
    if r.returncode != 0:
        print(f"  warn: could not build liballoccount.dylib:\n{r.stderr}", file=sys.stderr)
        return False
    return True


# ─────────────────────────── measurement ────────────────────────────────────
def median_wall(binary, n, timeout_s, warmup, runs):
    """Returns (samples_ms, rss_samples_kb, stdout_set, timed_out)."""
    for _ in range(warmup):
        r = run_once(binary, n, timeout_s)
        if r.timed_out:
            return [], [], set(), True
    walls, rsss, outs = [], [], set()
    for _ in range(runs):
        r = run_once(binary, n, timeout_s)
        if r.timed_out:
            return walls, rsss, outs, True
        if r.status != 0:
            outs.add(f"<exit {r.status}>".encode())
        walls.append(r.wall_ms)
        rsss.append(r.rss_kb)
        outs.add(r.stdout)
    return walls, rsss, outs, False


def autosize(pid, forms_bin, cfg, log, start_n=None):
    """Double PAT_N from start_n until the fastest completing form >= FLOOR_MS.
    Each probe is the min of two runs: the first launch of a freshly built
    binary is cold on macOS (page-in + signature check) and can be 10x slower."""
    n = int(start_n or cfg.get("start_n", DEFAULT_START_N))
    max_n = int(cfg.get("max_n", DEFAULT_MAX_N))
    timeout_s = float(cfg.get("timeout_s", DEFAULT_TIMEOUT_S))
    alive = dict(forms_bin)
    timed_out = {}          # form -> {"at_n": n, "last_ok_n":..., "last_ok_ms":...}
    last_ok = {}
    series = {f: [] for f in forms_bin}   # form -> [[n, ms], ...] (min of 2 runs)
    while True:
        probe = {}
        for form, binary in list(alive.items()):
            r = run_once(binary, n, timeout_s)
            if not r.timed_out:
                r2 = run_once(binary, n, timeout_s)
                if r2.timed_out:
                    r = r2
                else:
                    r.wall_ms = min(r.wall_ms, r2.wall_ms)
            if r.timed_out:
                timed_out[form] = {"at_n": n, **last_ok.get(form, {})}
                del alive[form]
                log(f"    {form}: timeout (> {timeout_s:.0f} s) at N={n}")
            else:
                probe[form] = r.wall_ms
                series[form].append([n, round(r.wall_ms, 2)])
                last_ok[form] = {"last_ok_n": n, "last_ok_ms": round(r.wall_ms, 2),
                                 "growth_exp": growth_exponent(series[form])}
        if not alive:
            return n, timed_out, "all_timeout", timeout_s, series
        best = min(probe.values())
        log(f"    N={n}: " + "  ".join(f"{f}={ms:.1f}ms" for f, ms in sorted(probe.items())))
        if best >= FLOOR_MS:
            return n, timed_out, "ok", timeout_s, series
        if n * 2 > max_n:
            return n, timed_out, "below_floor", timeout_s, series
        n *= 2


def growth_exponent(series):
    """Least-squares slope of log(ms) vs log(N) over the probes that took
    >= 20 ms (below that process start-up dominates). ~1 = linear, ~2 = quadratic."""
    pts = [(math.log(n), math.log(ms)) for n, ms in series if ms >= 20.0 and n > 0]
    if len(pts) < 2:
        return None
    mx = sum(x for x, _ in pts) / len(pts)
    my = sum(y for _, y in pts) / len(pts)
    sxx = sum((x - mx) ** 2 for x, _ in pts)
    if sxx == 0:
        return None
    return round(sum((x - mx) * (y - my) for x, y in pts) / sxx, 2)


def measure_pattern(pid, pat, args, log):
    entry = {"status": "ok", "dir": str(pat["dir"].relative_to(ROOT)), "forms": {}, "pat_n": None}
    forms_bin = {}
    for form, src in pat["forms"].items():
        fe = {"fixture": str(src.relative_to(ROOT)), "src_sha256": sha256_file(src)}
        entry["forms"][form] = fe
        binary, err = compile_form(pid, form, src)
        if binary is None:
            fe["status"] = "compile_error"
            fe["compile_error"] = err
            log(f"    {form}: COMPILE ERROR")
            continue
        fe["status"] = "ok"
        fe["binary_bytes"] = binary.stat().st_size
        forms_bin[form] = binary
    if not forms_bin:
        entry["status"] = "compile_error"
        return entry

    log("  autosizing PAT_N")
    n = None
    max_n = int(pat["cfg"].get("max_n", DEFAULT_MAX_N))
    while True:
        n, timed_out, st, timeout_s, series = autosize(pid, forms_bin, pat["cfg"], log, start_n=n)
        entry["pat_n"] = n
        entry["timeout_s"] = timeout_s
        for form, pts in series.items():
            fe = entry["forms"][form]
            fe.setdefault("probe_series", []).extend(pts)
            fe["growth_exp"] = growth_exponent(fe["probe_series"])
        for form, info in timed_out.items():
            entry["forms"][form].update({"status": "timeout", **info})
            forms_bin.pop(form, None)
        if st == "all_timeout":
            entry["status"] = "all_timeout"
            return entry
        if st == "below_floor":
            entry["status"] = "below_floor"
            return entry

        # timed runs at N
        outputs = {}
        for form, binary in forms_bin.items():
            fe = entry["forms"][form]
            walls, rsss, outs, to = median_wall(binary, n, timeout_s, args.warmup, args.runs)
            if to:
                fe["status"] = "timeout"
                fe["at_n"] = n
                continue
            fe["wall_ms_samples"] = [round(w, 3) for w in walls]
            fe["wall_ms_median"] = round(statistics.median(walls), 3)
            fe["wall_ci95"] = bootstrap_ci(walls)
            fe["rss_kb_median"] = int(statistics.median(rsss))
            fe["rss_kb_samples"] = rsss
            fe["stdout_sha256"] = hashlib.sha256(b"\n".join(sorted(outs))).hexdigest()
            fe["stdout_first_line"] = next(iter(outs)).decode("utf-8", "replace").splitlines()[:1]
            if len(outs) > 1:
                fe["status"] = "nondeterministic"
            outputs[form] = outs
            log(f"    {form}: {fe['wall_ms_median']:.2f} ms  ci {fe['wall_ci95']}  rss {fe['rss_kb_median']} KB")
        forms_bin = {f: b for f, b in forms_bin.items() if entry["forms"][f]["status"] == "ok"}
        if not forms_bin:
            entry["status"] = "all_timeout"
            return entry
        best_med = min(entry["forms"][f]["wall_ms_median"] for f in forms_bin)
        if best_med >= FLOOR_MS:
            break
        if n * 2 > max_n:
            entry["status"] = "below_floor"
            return entry
        log(f"    best median {best_med:.1f} ms < {FLOOR_MS:.0f} ms floor — doubling N")
        n *= 2

    # output equality across forms
    all_outs = set().union(*outputs.values()) if outputs else set()
    if len(all_outs) > 1:
        entry["status"] = "output_mismatch"
        log("    OUTPUT MISMATCH across forms")

    # allocation counts (one run each)
    if not args.no_alloc and ensure_dylib():
        for form, binary in forms_bin.items():
            r = run_once(binary, n, timeout_s, {"DYLD_INSERT_LIBRARIES": str(DYLIB)}, want_stderr=True)
            entry["forms"][form]["allocs"] = None if r.timed_out else parse_alloc(r.stderr)

    # big-O at 4N
    n4 = n * 4
    outs4 = {}
    for form, binary in forms_bin.items():
        fe = entry["forms"][form]
        walls, _, outs, to = median_wall(binary, n4, timeout_s * 4, 1, args.bigo_runs)
        if to or not walls:
            fe["wall_ms_median_4n"] = None
            fe["bigO_ratio"] = None
            fe["bigO"] = f"timeout(>{timeout_s * 4:.0f}s at 4N)"
            log(f"    {form}: 4N timeout")
            continue
        fe["wall_ms_median_4n"] = round(statistics.median(walls), 3)
        fe["bigO_ratio"] = round(fe["wall_ms_median_4n"] / fe["wall_ms_median"], 3)
        outs4[form] = outs
        log(f"    {form}: 4N {fe['wall_ms_median_4n']:.2f} ms  ratio {fe['bigO_ratio']}")
    all4 = set().union(*outs4.values()) if outs4 else set()
    if len(all4) > 1 and entry["status"] == "ok":
        entry["status"] = "output_mismatch"
        log("    OUTPUT MISMATCH across forms at 4N")

    apply_gate(entry)
    return entry


# ─────────────────────────── gate ───────────────────────────────────────────
def apply_gate(entry):
    forms = entry["forms"]
    ok = {f: e for f, e in forms.items() if e.get("status") == "ok" and e.get("wall_ms_median") is not None}
    if entry["status"] == "output_mismatch":
        for e in forms.values():
            e["verdict"] = "output_mismatch"
        return
    if not ok:
        return
    best = min(ok, key=lambda f: ok[f]["wall_ms_median"])
    entry["best"] = best
    b = ok[best]
    b["verdict"] = "best"
    for f, e in forms.items():
        if f == best:
            continue
        st = e.get("status")
        if st == "compile_error":
            e["verdict"] = "compile_error"
            continue
        if st == "timeout":
            v = f"timeout(>{entry.get('timeout_s', DEFAULT_TIMEOUT_S):.0f}s"
            if e.get("last_ok_n"):
                v += f"; N={e['last_ok_n']}:{e['last_ok_ms']:.0f}ms"
            if e.get("growth_exp") is not None:
                v += f"; ~O(N^{e['growth_exp']})"
            e["verdict"] = v + ")"
            continue
        if st == "nondeterministic":
            e["verdict"] = "nondeterministic"
            continue
        pct = (e["wall_ms_median"] / b["wall_ms_median"] - 1.0) * 100.0
        rss_pct = (e["rss_kb_median"] / max(1, b["rss_kb_median"]) - 1.0) * 100.0
        rb, rf = b.get("bigO_ratio"), e.get("bigO_ratio")
        bigo = "agree"
        if rf is None or rb is None:
            bigo = "worse" if rf is None else "unknown"
        elif rf > rb * BIGO_TOL:
            bigo = "worse"
        elif rf * BIGO_TOL < rb:
            bigo = "better"
        e["bigO_vs_best"] = bigo
        if bigo == "worse":
            e["verdict"] = f"worse-bigO({pct:+.0f}%)"
            continue
        wall_ok = abs(pct) <= WALL_TOL * 100.0
        rss_ok = abs(rss_pct) <= RSS_TOL * 100.0
        ci_ok = ci_overlap(e["wall_ci95"], b["wall_ci95"])
        if wall_ok and rss_ok and ci_ok and bigo == "agree":
            e["verdict"] = "tied"
        else:
            reasons = []
            if not rss_ok:
                reasons.append(f"rss {rss_pct:+.0f}%")
            if wall_ok and not ci_ok:
                reasons.append("ci-disjoint")
            if bigo == "better":
                reasons.append("better-bigO")
            tail = f"; {', '.join(reasons)}" if reasons else ""
            e["verdict"] = f"slower({pct:+.1f}%{tail})"


# ─────────────────────────── meta + output ──────────────────────────────────
def collect_meta(args):
    therm = sh(["pmset", "-g", "therm"])
    git_sha = sh(["git", "-C", str(ROOT), "rev-parse", "HEAD"])
    dirty = bool(sh(["git", "-C", str(ROOT), "status", "--porcelain", "--untracked-files=no"]))
    return {
        "date": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "hw_model": sh(["sysctl", "-n", "hw.model"]),
        "hw_ncpu": os.cpu_count(),
        "os": f"{platform.system()} {platform.release()} ({platform.mac_ver()[0]})",
        "python": platform.python_version(),
        "loadavg_1min": round(os.getloadavg()[0], 2),
        "max_load": args.max_load,
        "load_warning": os.getloadavg()[0] > args.max_load,
        "thermal": [ln.strip() for ln in therm.splitlines() if ln.strip()],
        "tkc_version": sh([str(TKC), "--version"]),
        "tkc_git_sha": git_sha,
        "tkc_git_dirty": dirty,
        "tkc_binary_sha256": sha256_file(TKC) if TKC.exists() else None,
        "tkc_bin_sha": PIN.sha256 if PIN else None,          # 131.39: the pinned copy every form was compiled with
        "tkc_pinned_copy": PIN.path if PIN else None,
        "harness": "bench/patterns/run_patterns.py",
        "runs": args.runs, "warmup": args.warmup, "bigo_runs": args.bigo_runs,
        "gate": {"floor_ms": FLOOR_MS, "wall_tol": WALL_TOL, "rss_tol": RSS_TOL,
                 "bigO_tol": BIGO_TOL, "bootstrap": BOOTSTRAP,
                 "rule": "tied iff wall within 5% AND CI95 overlap AND rss within 5% AND bigO ratio within 1.5x; allocs tie-break only"},
        "alloc_counted": not args.no_alloc,
    }


def fmt_bytes(b):
    if b is None:
        return "-"
    for unit in ("B", "KB", "MB", "GB"):
        if b < 1024 or unit == "GB":
            return f"{b:.0f}{unit}" if unit == "B" else f"{b:.1f}{unit}"
        b /= 1024.0


def print_table(results):
    print()
    hdr = f"{'pattern':<22}{'N':>10}  {'form':<5} {'wall ms [ci95]':<26}{'rss KB':>9}  {'allocs (calls/bytes)':<22}{'bigO':>6}  verdict"
    print(hdr)
    print("-" * len(hdr))
    for pid, e in results["patterns"].items():
        first = True
        for form, fe in e["forms"].items():
            name = pid if first else ""
            nstr = str(e.get("pat_n") or "-") if first else ""
            first = False
            if fe.get("wall_ms_median") is not None:
                lo, hi = fe["wall_ci95"]
                wall = f"{fe['wall_ms_median']:.2f} [{lo:.2f},{hi:.2f}]"
                rss = f"{fe['rss_kb_median']}"
            else:
                wall = fe.get("status", "?")
                rss = "-"
            al = fe.get("allocs")
            alloc = f"{al['calls']}/{fmt_bytes(al['bytes'])}" if al else "-"
            ratio = fe.get("bigO_ratio")
            ratio = f"{ratio:.2f}" if ratio is not None else "-"
            print(f"{name:<22}{nstr:>10}  {form:<5} {wall:<26}{rss:>9}  {alloc:<22}{ratio:>6}  {fe.get('verdict', fe.get('status', ''))}")
        if e["status"] != "ok":
            print(f"{'':<22}{'':>10}  status: {e['status']}")
    print()


def main():
    ap = argparse.ArgumentParser(description="toke pattern micro-benchmark harness (131.5)")
    ap.add_argument("--pattern", action="append", help="pattern id to run (repeatable; default all)")
    ap.add_argument("--runs", type=int, default=15, help="timed runs per form (default 15)")
    ap.add_argument("--warmup", type=int, default=2, help="warm-up runs per form (default 2)")
    ap.add_argument("--bigo-runs", type=int, default=3, help="timed runs at 4N (default 3)")
    ap.add_argument("--no-alloc", action="store_true", help="skip the allocation-counting run")
    ap.add_argument("--json-only", action="store_true", help="print only the results path")
    ap.add_argument("--max-load", type=float, default=DEFAULT_MAX_LOAD,
                    help=f"refuse to run if 1-min loadavg exceeds this (default {DEFAULT_MAX_LOAD})")
    ap.add_argument("--allow-load", action="store_true",
                    help="run even above --max-load; the results file then carries load_warning: true")
    args = ap.parse_args()

    if sys.platform != "darwin":
        die("macOS only (ru_maxrss units, DYLD_INTERPOSE, sysctl hw.model)")
    # os.wait4 needs children to stay reapable; a parent shell that ignores
    # SIGCHLD (some background runners do) would make wait4 fail with ECHILD.
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    if not TKC.exists():
        die(f"tkc not found at {TKC}")
    global PIN
    PIN = tkc_pin.pin(toke_repo=ROOT).install(sys.modules[__name__])   # 131.39: TKC -> private copy
    atexit.register(PIN.close)
    load = os.getloadavg()[0]
    if load > args.max_load and not args.allow_load:
        die(f"1-min loadavg {load:.2f} > {args.max_load} — machine busy; retry later, "
            f"raise --max-load, or pass --allow-load (results flagged load_warning)")
    if load > args.max_load:
        print(f"run_patterns: WARNING loadavg {load:.2f} > {args.max_load}; results will carry load_warning: true",
              file=sys.stderr)

    log = (lambda *a: None) if args.json_only else (lambda *a: print(*a, flush=True))
    pats = discover(set(args.pattern) if args.pattern else None)
    if not pats:
        die("no patterns found" + (f" matching {args.pattern}" if args.pattern else ""))

    results = {"meta": collect_meta(args), "patterns": {}}
    log(f"run_patterns: {results['meta']['tkc_version']} @ {results['meta']['tkc_git_sha'][:12]}"
        f"{' (dirty)' if results['meta']['tkc_git_dirty'] else ''}  {results['meta']['hw_model']}"
        f"  load {load:.2f}  runs {args.runs}")
    for pid, pat in pats.items():
        log(f"\n[{pid}] forms: {' '.join(pat['forms'])}")
        results["patterns"][pid] = measure_pattern(pid, pat, args, log)

    RESULTS.mkdir(parents=True, exist_ok=True)
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    out = RESULTS / f"{stamp}.json"
    out.write_text(json.dumps(results, indent=1) + "\n")
    if args.json_only:
        print(out)
    else:
        print_table(results)
        print(f"results: {out.relative_to(ROOT)}")
    bad = [p for p, e in results["patterns"].items() if e["status"] != "ok"]
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
