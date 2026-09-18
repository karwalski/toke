#!/usr/bin/env python3
"""recheck_caveats.py — re-check pattern-catalogue bug caveats after a 127.x closure.

Story 131.26 (Epic 131). Protocol: docs/spec/patterns-protocol-v0.4.md §3 (bug_caveats,
.blocked.tk fixtures) and §8 (compiler-bug closures trigger this script).

Modes
-----
default   For every `bug_caveats` item in patterns/catalogue.json, look up its `127.N`
          issue in docs/progress.md. When the issue is CLOSED (status cell contains
          DONE), take the fixture of `preferred_when_fixed` (may be `.blocked.tk`),
          run `tkc --check`, `tkc -O2 -o`, execute with PAT_N and compare stdout with
          the entry's canonical form at the same PAT_N. Each caveat gets an `action`:
              unblock-and-remeasure   closed + preferred fixture compiles/builds/runs
                                      and prints the canonical output → open a scoped
                                      follow-up (rename .blocked.tk, re-run 131.5 bench)
              still-broken            closed but the preferred fixture still fails
              n/a                     issue still open, or no preferred form
--stale   List entries whose measured_at.tkc_sha differs from `git rev-parse HEAD`.

Exit status is 0 (this is a report). `--fail-on-action` exits 1 when at least one
caveat is `unblock-and-remeasure` (CI hook). Stdlib only.

Usage
-----
    python3 scripts/patterns/recheck_caveats.py [--json] [--issue 127.6] [--fail-on-action]
    python3 scripts/patterns/recheck_caveats.py --stale [--json]
Env: TKC (compiler path, default <repo>/tkc).
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from validate_catalogue import ISSUE_RE  # noqa: E402  (shared constant, protocol §3)

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_CATALOGUE = os.path.join(REPO_ROOT, "patterns", "catalogue.json")
DEFAULT_PROGRESS = os.path.join(REPO_ROOT, "docs", "progress.md")
DEFAULT_PAT_N = 1000
DEFAULT_TIMEOUT_S = 30

ACTION_UNBLOCK = "unblock-and-remeasure"
ACTION_BROKEN = "still-broken"
ACTION_NA = "n/a"
RESULT_KEYS = (
    "entry", "issue", "status", "closed", "preferred_form", "fixture",
    "compiles", "builds", "runs", "output_matches", "action", "detail",
)

# `| 127.6 | title | **DONE** 2026-09-20 | P0 | ... |` — first cell is the id, third is status.
_ROW_RE = re.compile(r"^\s*\|(.*)\|\s*$")
_MARKUP_RE = re.compile(r"[*_`~]+")


# --------------------------------------------------------------------------- catalogue
def load_catalogue(path: str) -> dict:
    with open(path, encoding="utf-8") as fh:
        doc = json.load(fh)
    if not isinstance(doc, dict) or not isinstance(doc.get("entries"), list):
        raise ValueError(f"{path}: top level must be {{protocol, entries}}")
    return doc


def collect_caveats(doc: dict) -> list[tuple[str, str, str | None]]:
    """Every (entry id, issue, preferred_when_fixed) from bug_caveats, catalogue order."""
    out: list[tuple[str, str, str | None]] = []
    for e in doc["entries"]:
        for b in e.get("bug_caveats") or []:
            if isinstance(b, dict) and "issue" in b:
                out.append((e["id"], str(b["issue"]), b.get("preferred_when_fixed")))
    return out


def candidate_fixture(entry: dict, form: str | None) -> str | None:
    for c in entry.get("candidates") or []:
        if isinstance(c, dict) and c.get("form") == form:
            return c.get("fixture")
    return None


def resolve_fixture(root: str, fixture: str | None) -> str | None:
    """Absolute path of the fixture; tolerates a .blocked.tk that was already renamed (or vice versa)."""
    if not fixture:
        return None
    cands = [fixture]
    if fixture.endswith(".blocked.tk"):
        cands.append(fixture[: -len(".blocked.tk")] + ".tk")
    elif fixture.endswith(".tk"):
        cands.append(fixture[: -len(".tk")] + ".blocked.tk")
    for rel in cands:
        p = os.path.join(root, rel)
        if os.path.isfile(p):
            return p
    return None


def fixture_timeout(root: str, entry_id: str) -> int:
    p = os.path.join(root, "patterns", entry_id, "bench.json")
    try:
        with open(p, encoding="utf-8") as fh:
            return int(json.load(fh).get("timeout_s", DEFAULT_TIMEOUT_S))
    except (OSError, ValueError, TypeError):
        return DEFAULT_TIMEOUT_S


# --------------------------------------------------------------------------- progress.md
def _clean_cell(s: str) -> str:
    return _MARKUP_RE.sub("", s).strip()


def parse_issue_status(text: str) -> dict[str, str]:
    """Map '127.N' -> raw status cell for every `| 127.N | title | status | …` row anywhere in the file."""
    status: dict[str, str] = {}
    for line in text.splitlines():
        m = _ROW_RE.match(line)
        if not m:
            continue
        cells = [c.strip() for c in m.group(1).split("|")]
        if len(cells) < 3:
            continue
        issue = _clean_cell(cells[0])
        if not ISSUE_RE.match(issue):
            continue
        status.setdefault(issue, cells[2].strip())
    return status


def is_closed(status_cell: str | None) -> bool:
    return bool(status_cell) and "done" in _clean_cell(status_cell).lower()


# --------------------------------------------------------------------------- tkc driver
def _run(cmd: list[str], *, env: dict | None = None, timeout: int = DEFAULT_TIMEOUT_S, cwd: str | None = None):
    """Thin subprocess wrapper (tests monkeypatch this). Returns (rc, stdout, stderr)."""
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return 124, "", f"timeout after {timeout}s"
    except OSError as exc:
        return 127, "", str(exc)
    return r.returncode, r.stdout, r.stderr


def tkc_check(tkc: str, src: str, timeout: int) -> tuple[bool, str]:
    rc, out, err = _run([tkc, "--check", src], timeout=timeout)
    return rc == 0, (err or out).strip()


def tkc_build(tkc: str, src: str, out_bin: str, timeout: int) -> tuple[bool, str]:
    rc, out, err = _run([tkc, "-O2", "-o", out_bin, src], timeout=timeout)
    return rc == 0 and os.path.exists(out_bin), (err or out).strip()


def run_binary(binary: str, pat_n: int, timeout: int) -> tuple[bool, str, str]:
    env = dict(os.environ, PAT_N=str(pat_n))
    rc, out, err = _run([binary], env=env, timeout=timeout)
    return rc == 0, out, err.strip()


def build_and_run(tkc: str, src: str, workdir: str, tag: str, pat_n: int, timeout: int) -> dict:
    """compile → build → run one fixture; every stage records a bool + first failure detail."""
    res = {"compiles": False, "builds": False, "runs": False, "stdout": None, "detail": ""}
    ok, msg = tkc_check(tkc, src, timeout)
    res["compiles"] = ok
    if not ok:
        res["detail"] = f"--check failed: {msg[:200]}"
        return res
    out_bin = os.path.join(workdir, tag)
    ok, msg = tkc_build(tkc, src, out_bin, timeout)
    res["builds"] = ok
    if not ok:
        res["detail"] = f"build failed: {msg[:200]}"
        return res
    ok, out, err = run_binary(out_bin, pat_n, timeout)
    res["runs"] = ok
    res["stdout"] = out
    if not ok:
        res["detail"] = f"run failed: {err[:200]}"
    return res


# --------------------------------------------------------------------------- recheck
def recheck(doc: dict, status_map: dict[str, str], *, root: str, tkc: str, pat_n: int,
            issue_filter: str | None = None, workdir: str | None = None) -> list[dict]:
    entries = {e["id"]: e for e in doc["entries"]}
    results: list[dict] = []
    tmp = workdir or tempfile.mkdtemp(prefix="recheck_caveats_")
    canon_cache: dict[str, dict] = {}
    for eid, issue, pref in collect_caveats(doc):
        if issue_filter and issue != issue_filter:
            continue
        entry = entries[eid]
        status = status_map.get(issue)
        closed = is_closed(status)
        r = {k: None for k in RESULT_KEYS}
        r.update(entry=eid, issue=issue, status=status, closed=closed, preferred_form=pref,
                 fixture=candidate_fixture(entry, pref), action=ACTION_NA, detail="")
        if status is None:
            r["detail"] = "issue not found in progress.md"
        if not closed or pref is None:
            if closed and pref is None:
                r["detail"] = "closed but preferred_when_fixed is null — nothing to unblock"
            results.append(r)
            continue
        timeout = fixture_timeout(root, eid)
        src = resolve_fixture(root, r["fixture"])
        if src is None:
            r.update(compiles=False, builds=False, runs=False, output_matches=False,
                     action=ACTION_BROKEN, detail=f"fixture not found: {r['fixture']}")
            results.append(r)
            continue
        pr = build_and_run(tkc, src, tmp, f"{eid}_{pref}", pat_n, timeout)
        r.update(compiles=pr["compiles"], builds=pr["builds"], runs=pr["runs"], detail=pr["detail"])
        if not pr["runs"]:
            r.update(output_matches=False, action=ACTION_BROKEN)
            results.append(r)
            continue
        # canonical form reference output (built once per entry)
        canon_form = (entry.get("verdict") or {}).get("canonical")
        if eid not in canon_cache:
            csrc = resolve_fixture(root, candidate_fixture(entry, canon_form))
            if csrc is None:
                canon_cache[eid] = {"runs": False, "stdout": None, "detail": f"canonical fixture missing ({canon_form})"}
            else:
                canon_cache[eid] = build_and_run(tkc, csrc, tmp, f"{eid}_{canon_form}_canon", pat_n, timeout)
        cr = canon_cache[eid]
        if not cr["runs"]:
            r.update(output_matches=False, action=ACTION_BROKEN, detail=f"canonical form {canon_form}: {cr['detail']}")
        elif pr["stdout"] == cr["stdout"]:
            r.update(output_matches=True, action=ACTION_UNBLOCK,
                     detail=f"stdout matches canonical {canon_form} at PAT_N={pat_n}")
        else:
            r.update(output_matches=False, action=ACTION_BROKEN,
                     detail=f"stdout differs from canonical {canon_form}: {pr['stdout'][:80]!r} vs {cr['stdout'][:80]!r}")
        results.append(r)
    return results


# --------------------------------------------------------------------------- stale
def git_head(root: str) -> str | None:
    rc, out, _ = _run(["git", "rev-parse", "HEAD"], cwd=root, timeout=10)
    return out.strip() if rc == 0 and out.strip() else None


def stale_entries(doc: dict, head: str | None) -> list[dict]:
    out = []
    for e in doc["entries"]:
        sha = (e.get("measured_at") or {}).get("tkc_sha")
        if head is not None and sha == head:
            continue
        out.append({
            "entry": e["id"],
            "status": (e.get("verdict") or {}).get("status"),
            "tkc_sha": sha,
            "head": head,
            "reason": "never measured" if sha is None else "tkc_sha != HEAD",
        })
    return out


# --------------------------------------------------------------------------- output
def _fmt(v) -> str:
    if v is None:
        return "-"
    if isinstance(v, bool):
        return "yes" if v else "no"
    return str(v)


def render_table(rows: list[dict], cols: list[str]) -> str:
    if not rows:
        return "(no rows)"
    widths = [max(len(c), *(len(_fmt(r.get(c))) for r in rows)) for c in cols]
    line = lambda vals: "  ".join(v.ljust(w) for v, w in zip(vals, widths))  # noqa: E731
    out = [line(cols), line(["-" * w for w in widths])]
    out += [line([_fmt(r.get(c)) for c in cols]) for r in rows]
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--catalogue", default=DEFAULT_CATALOGUE)
    ap.add_argument("--progress", default=DEFAULT_PROGRESS)
    ap.add_argument("--tkc", default=os.environ.get("TKC", os.path.join(REPO_ROOT, "tkc")))
    ap.add_argument("--pat-n", type=int, default=DEFAULT_PAT_N)
    ap.add_argument("--issue", help="only caveats naming this issue, e.g. 127.6")
    ap.add_argument("--json", action="store_true", help="machine-readable output")
    ap.add_argument("--fail-on-action", action="store_true",
                    help="exit 1 when any caveat is unblock-and-remeasure")
    ap.add_argument("--stale", action="store_true",
                    help="list entries whose measured_at.tkc_sha != git HEAD instead")
    args = ap.parse_args(argv)

    root = os.path.dirname(os.path.dirname(os.path.abspath(args.catalogue)))
    doc = load_catalogue(args.catalogue)

    if args.stale:
        head = git_head(root)
        rows = stale_entries(doc, head)
        if args.json:
            print(json.dumps({"head": head, "stale": rows}, indent=2))
        else:
            print(f"HEAD {head or '(unknown)'} — {len(rows)} of {len(doc['entries'])} entries need re-measurement")
            print(render_table(rows, ["entry", "status", "tkc_sha", "reason"]))
        return 0

    with open(args.progress, encoding="utf-8") as fh:
        status_map = parse_issue_status(fh.read())
    rows = recheck(doc, status_map, root=root, tkc=args.tkc, pat_n=args.pat_n, issue_filter=args.issue)
    n_unblock = sum(r["action"] == ACTION_UNBLOCK for r in rows)
    n_broken = sum(r["action"] == ACTION_BROKEN for r in rows)
    if args.json:
        print(json.dumps({"pat_n": args.pat_n, "issue_filter": args.issue,
                          "summary": {"caveats": len(rows), ACTION_UNBLOCK: n_unblock, ACTION_BROKEN: n_broken},
                          "results": rows}, indent=2))
    else:
        closed = sorted(k for k in status_map if is_closed(status_map[k]))
        print(f"{len(rows)} caveat(s) in {os.path.relpath(args.catalogue, root)}; "
              f"closed 127.x issues: {', '.join(closed) or 'none'}; "
              f"{n_unblock} {ACTION_UNBLOCK}, {n_broken} {ACTION_BROKEN}")
        print(render_table(rows, ["entry", "issue", "closed", "preferred_form", "compiles", "builds",
                                  "runs", "output_matches", "action", "detail"]))
    if args.fail_on_action and n_unblock:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
