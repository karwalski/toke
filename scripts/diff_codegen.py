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
"""
import sys, os, subprocess, hashlib, json, tempfile, argparse, glob

TOKE = os.environ.get("TOKE", "./toke")

def is_full_program(src):
    return ("f=main" in src) or ("f= main" in src)

def probe(tk):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "b")
        try:
            r = subprocess.run([TOKE, tk, "--out", out],
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
        # positives on already-crashing programs. A real regression (clean exit ->
        # crash, or vice-versa) still changes the signature. Output is ignored for
        # crashes since a crashed program's partial stdout is also non-deterministic.
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

    cur = collect(a.dirs)
    if a.record:
        json.dump(cur, open(a.record, "w"), indent=1, sort_keys=True)
        print(f"recorded {len(cur)} full-program signatures -> {a.record}")
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
          f"{len(added)} added, {len(removed)} removed")
    # Behaviour changes are the failure condition; added/removed are informational.
    return 1 if diffs else 0

if __name__ == "__main__":
    sys.exit(main())
