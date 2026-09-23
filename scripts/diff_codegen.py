#!/usr/bin/env python3
"""124.0b — differential codegen regression harness (root-of-trust safety net).

Compile + run every full-program `.tk` under the given directories and capture a
behaviour signature `(compiled, exit_code, stdout_sha)`. Compare against a recorded
baseline and fail if ANY program's observable behaviour changed. This is the gate
that must stay green before landing a codegen change that will feed the training
corpus: a compiler that silently alters a program's output corrupts the corpus,
which corrupts the model (AGENTS.md root-of-trust principle).

Usage:
  TOKE=./toke diff_codegen.py --record BASELINE  DIR [DIR...]   # snapshot behaviour
  TOKE=./toke diff_codegen.py --baseline BASELINE DIR [DIR...]  # diff (exit 1 on change)

Signatures are lists so JSON round-trips cleanly. `stdout_sha` is a short hash;
programs that don't compile record [false, null, null]; run-timeouts record
[true, "timeout", null].

── Unhealthy outcomes are not baselineable (story 127.139) ──────────────────

Until 2026-09-23 this gate diffed signatures and nothing else, so a program
that crashed, hung, or failed to compile was "green" as long as it kept doing
so. It did: `test/standalone/test_str.tk` was recorded [true,"signal",null] —
a clean build that segfaults with zero bytes of stdout — and eleven more were
recorded [false,null,null], a parse error frozen as expected behaviour. The
gate printed "118 checked, 0 behaviour change(s)" over thirteen broken
programs, and story 131.27's re-freeze is premised on this gate being the root
of trust.

So a signature is now classified. A *healthy* signature is a program that
compiled and ran to a normal exit; anything else (compile failure, compile
timeout, run timeout, signal kill) is *unhealthy* and fails the gate unless it
is listed in test/codegen-quarantine.txt with a date, a reason and a story.

The quarantine is self-failing, the same contract as
scripts/check_tki_skiplist.txt: an entry whose program has become healthy is
STALE and fails the gate, so a quarantined defect cannot be quietly fixed and
left rotting in the list. `--record` refuses to write an unquarantined
unhealthy signature at all, which is the step that would otherwise re-bless a
fresh crash the moment someone re-records.
"""
import sys, os, subprocess, hashlib, json, tempfile, argparse, glob

TOKE = os.environ.get("TOKE", "./toke")
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QUARANTINE = os.path.join(REPO_ROOT, "test", "codegen-quarantine.txt")

def is_full_program(src):
    return ("f=main" in src) or ("f= main" in src)

def unhealthy(sig):
    """Return a human reason if this signature is not a clean compile-and-run.

    None means healthy: compiled, ran, exited normally (any exit code — a test
    that deliberately exits 3 is a legitimate baseline).
    """
    if sig is None:
        return None
    compiled, code = sig[0], sig[1]
    if compiled == "compile-timeout":
        return "compile timed out"
    if compiled is False:
        return "does not compile"
    if code == "timeout":
        return "run timed out"
    if code == "signal":
        return "killed by a signal (crash)"
    return None

def load_quarantine(path=QUARANTINE):
    """Parse the quarantine list: `<relpath>   # <date> <reason> [<story>]`."""
    entries = {}
    if not os.path.exists(path):
        return entries
    with open(path, encoding="utf-8") as fh:
        for ln, raw in enumerate(fh, 1):
            line = raw.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            key, _, reason = line.partition("#")
            key = key.strip()
            if not key:
                continue
            reason = reason.strip()
            if not reason:
                print(f"{path}:{ln}: quarantine entry has no `# <date> <reason> "
                      f"<story>` justification: {key}", file=sys.stderr)
                sys.exit(2)
            entries[key] = reason
    return entries

def probe(tk):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "b")
        try:
            # 124.4f: --allow-all so the deny-by-default capability flip (124.4g)
            # is a no-op here — the gate measures codegen behaviour, not grants.
            r = subprocess.run([TOKE, tk, "--allow-all", "--out", out],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               timeout=90)
        except subprocess.TimeoutExpired:
            return ["compile-timeout", None, None]
        if r.returncode != 0 or not os.path.exists(out):
            return [False, None, None]
        try:
            # stdin from /dev/null so stdin-reading programs get a deterministic
            # immediate EOF instead of blocking (which flapped timeout<->exit).
            rr = subprocess.run([out], capture_output=True, timeout=20,
                                stdin=subprocess.DEVNULL)
        except subprocess.TimeoutExpired:
            return [True, "timeout", None]
        # A signal-kill (negative returncode) is normalized to one "signal" token:
        # the specific signal for a memory bug is non-deterministic (SIGSEGV vs
        # SIGBUS depending on heap layout), so recording the number produces false
        # positives on already-crashing programs. Output is ignored for crashes
        # since a crashed program's partial stdout is also non-deterministic.
        # This token is NOT a passing verdict: see unhealthy() — it fails the
        # gate unless the program is quarantined.
        if rr.returncode < 0:
            return [True, "signal", None]
        return [True, rr.returncode, hashlib.sha256(rr.stdout).hexdigest()[:16]]

def collect(dirs):
    sig = {}
    for d in dirs:
        for tk in sorted(glob.glob(os.path.join(d, "**", "*.tk"), recursive=True)):
            try:
                src = open(tk, errors="ignore").read()
            except OSError:
                continue
            if not is_full_program(src):
                continue
            sig[os.path.relpath(tk)] = probe(tk)
    return sig

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--record", metavar="FILE")
    ap.add_argument("--baseline", metavar="FILE")
    ap.add_argument("dirs", nargs="+")
    a = ap.parse_args()

    quarantined = load_quarantine()
    cur = collect(a.dirs)

    # Health classification runs for both --record and --baseline: the point is
    # that an unhealthy program can never enter the baseline in the first place.
    broken = [(k, unhealthy(v)) for k, v in sorted(cur.items())
              if unhealthy(v) and k not in quarantined]
    # A quarantined program that is now healthy: the defect is fixed, so the
    # line must go (same self-failing contract as check_tki_skiplist.txt).
    stale = [k for k in sorted(quarantined)
             if k in cur and not unhealthy(cur[k])]

    for k, why in broken:
        print(f"  BROKEN  {k}\n    {why}; signature {cur[k]}\n"
              f"    fix the program, or quarantine it in "
              f"{os.path.relpath(QUARANTINE, REPO_ROOT)} with a date, reason and story")
    for k in stale:
        print(f"  STALE   {k}\n    quarantined as \"{quarantined[k]}\" but it now "
              f"compiles and runs cleanly\n    remove the line from "
              f"{os.path.relpath(QUARANTINE, REPO_ROOT)} and re-record its baseline")

    if a.record:
        if broken or stale:
            print(f"refusing to record: {len(broken)} unquarantined broken "
                  f"program(s), {len(stale)} stale quarantine entr(ies)",
                  file=sys.stderr)
            return 1
        json.dump(cur, open(a.record, "w"), indent=1, sort_keys=True)
        print(f"recorded {len(cur)} full-program signatures -> {a.record} "
              f"({len(quarantined)} quarantined)")
        return 0
    if not a.baseline:
        print("error: --record or --baseline required", file=sys.stderr)
        return 2

    base = json.load(open(a.baseline))
    diffs, added, removed = [], [], []
    for k in sorted(set(base) | set(cur)):
        b, c = base.get(k), cur.get(k)
        if b is None:
            added.append(k)
        elif c is None:
            removed.append(k)
        elif b != c:
            diffs.append((k, b, c))

    for k, b, c in diffs:
        print(f"  CHANGED {k}\n    baseline: {b}\n    current:  {c}")
    if added:
        print(f"  (+{len(added)} new programs not in baseline: {', '.join(added[:5])}"
              + (" ..." if len(added) > 5 else "") + ")")
    if removed:
        print(f"  (-{len(removed)} baseline programs missing: {', '.join(removed[:5])}"
              + (" ..." if len(removed) > 5 else "") + ")")
    print(f"diff_codegen: {len(cur)} checked, {len(diffs)} behaviour change(s), "
          f"{len(added)} added, {len(removed)} removed, "
          f"{len(broken)} broken, {len(stale)} stale quarantine, "
          f"{len(quarantined)} quarantined")
    # Behaviour changes, unquarantined breakage and stale quarantine entries all
    # fail; added/removed remain informational.
    return 1 if (diffs or broken or stale) else 0

if __name__ == "__main__":
    sys.exit(main())
