#!/usr/bin/env python3
"""prove_identical_code.py — record that pattern forms lower to identical machine code (story 127.440).

Protocol: docs/spec/patterns-protocol-v0.4.md §6 step 3 (owner decision 4.11 (d)). Two forms are
*proven to lower to identical code* when the machine code `tkc -O2` produces for their fixtures is
identical, measured with the tkc the run records in `measured_at`, and a conformance test pins the
lowering that makes it so.

    python3 scripts/patterns/prove_identical_code.py --pattern acc-array --forms a,b --conform C267 \
        [--tkc ./tkc] [--write]

Compiles each fixture with `tkc -O2 --allow-all`, disassembles the text section with `otool -tV`
(the first line, which names the file, is dropped), and compares. Identical -> prints the proof
record and, with --write, stores it in the entry's `identical_code` list (replacing a proof for the
same forms). Different -> exit 1, nothing written. The tkc must report a clean build commit
(`--version --json`); that sha is the proof's `tkc_sha`, so measure with the same binary
(bench/patterns/run_patterns.py) and the validator will accept the proof. macOS only (otool).
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import render_catalogue as rc  # noqa: E402
import tkc_pin  # noqa: E402
import validate_catalogue as vc  # noqa: E402


def text_of(tkc: str, fixture: str, tmp: str) -> str:
    out = os.path.join(tmp, os.path.basename(fixture) + ".bin")
    r = subprocess.run([tkc, "-O2", "--allow-all", os.path.join(ROOT, fixture), "--out", out],
                       capture_output=True, text=True, cwd=ROOT)
    if r.returncode != 0:
        sys.exit(f"prove_identical_code: {fixture} does not compile:\n{r.stderr}")
    d = subprocess.run(["otool", "-tV", out], capture_output=True, text=True)
    if d.returncode != 0:
        sys.exit(f"prove_identical_code: otool failed on {fixture}:\n{d.stderr}")
    return "".join(d.stdout.splitlines(keepends=True)[1:])


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(prog="prove_identical_code.py", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalogue", default=os.path.join(ROOT, rc.CATALOGUE))
    ap.add_argument("--pattern", required=True)
    ap.add_argument("--forms", required=True, help="comma-separated forms, e.g. a,b")
    ap.add_argument("--conform", required=True, help="conformance test pinning the lowering, e.g. C267")
    ap.add_argument("--tkc", default=os.path.join(ROOT, "tkc"))
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args(argv)

    forms = sorted(f.strip() for f in a.forms.split(",") if f.strip())
    doc, _ = rc.load(a.catalogue)
    e = next((x for x in doc["entries"] if x["id"] == a.pattern), None)
    if e is None:
        sys.exit(f"prove_identical_code: no entry {a.pattern!r}")
    cands = {c["form"]: c for c in e["candidates"]}
    if len(forms) < 2 or any(f not in cands or rc.is_blocked(cands[f]) for f in forms):
        sys.exit(f"prove_identical_code: --forms must name >= 2 live candidates of {a.pattern}")
    if not vc.CONFORM_RE.match(a.conform) or not any(
            n.startswith(a.conform + "_") for n in os.listdir(os.path.join(ROOT, "test", "conform"))):
        sys.exit(f"prove_identical_code: no conformance test test/conform/{a.conform}_*.sh")
    sha, dirty = tkc_pin.build_info(a.tkc)
    tkc_pin.check_clean(sha, dirty, a.tkc)

    with tempfile.TemporaryDirectory() as tmp:
        texts = {f: text_of(a.tkc, cands[f]["fixture"], tmp) for f in forms}
    digests = {f: hashlib.sha256(t.encode()).hexdigest() for f, t in texts.items()}
    for f in forms:
        print(f"{a.pattern}/{f}: {len(texts[f].splitlines())} text lines, sha256 {digests[f]}")
    if len(set(digests.values())) != 1:
        print(f"prove_identical_code: {a.pattern} forms {','.join(forms)} do NOT lower to identical code (tkc {sha})")
        return 1
    proof = {"forms": forms, "tkc_sha": sha, "text_sha256": digests[forms[0]], "conform": a.conform,
             "date": datetime.date.today().isoformat()}
    print(f"identical: {proof}")
    if a.write:
        kept = [p for p in e.get("identical_code") or [] if sorted(p.get("forms", [])) != forms]
        e["identical_code"] = kept + [proof]
        rc.dump(doc, a.catalogue)
        print(f"wrote {os.path.relpath(a.catalogue, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
