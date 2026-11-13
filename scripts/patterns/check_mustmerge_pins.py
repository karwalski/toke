#!/usr/bin/env python3
"""check_mustmerge_pins.py — catch must-merge input drift in toke, where it starts (story 131.119).

toke-tokenizer's ``data/must_merge_v04.json`` (``scripts/derive_must_merge.py``, story 131.23) is
derived from two kinds of file in THIS repo: the pattern catalogue(s) ``patterns/catalogue*.json``
and, for every entry, the fixture of its **canonical** form and, when set, its **hot_path** form
(``patterns/<id>/<form>.tk``, or the candidate's ``fixture`` field).  Editing any of them makes the
committed list stale.  Twice (132.110, 131.118) a toke commit edited such a fixture and the drift
surfaced only later, as a red derivability test on toke-tokenizer main.

This gate pins those inputs by sha256 in ``patterns/must_merge_inputs.json`` and fails when the
tree no longer matches the pins, naming each changed file and its role:

    python3 scripts/patterns/check_mustmerge_pins.py            # check (make check-patterns)
    python3 scripts/patterns/check_mustmerge_pins.py --update   # re-pin (make pin-mustmerge)

Re-pinning is the acknowledgement that the list was re-derived: run
``python3 scripts/derive_must_merge.py`` in toke-tokenizer against this tree first (TOKE_REPO=<this
checkout>), commit it there, then ``--update`` here.  Fixture edits that do not touch a canonical or
hot-path form (``b.tk`` of an entry whose canonical is ``a``) are not inputs and never trip the gate.

Cross-check: when ``$TOKE_WORKSPACE/toke-tokenizer/data/must_merge_v04.json`` exists (local
workspace; GitHub CI has no sibling checkout) and records ``fixture_sha256`` per form (131.119+),
the pins must equal what that list was derived from, so a re-pin without a re-derivation fails
locally too.  A list predating 131.119 is reported and skipped, never failed.

Out of scope (still caught only by toke-tokenizer's derivability test): changes to ``tkc --min``
output, ``count_tokens.py`` or ``mask_strings.py`` that alter the extracted text.

Stdlib only.  Exit 0 = pins match; 1 = drift (one line per file); 2 = usage / unreadable input.
"""
from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PINS = os.path.join("patterns", "must_merge_inputs.json")
CONSUMER = "toke-tokenizer data/must_merge_v04.json (scripts/derive_must_merge.py, story 131.23)"
REMEDY = ("re-derive in toke-tokenizer (TOKE_REPO=<this checkout> python3 scripts/derive_must_merge.py), "
          "commit data/must_merge_v04.json there, then `make pin-mustmerge` here")


def sha256_file(path: str) -> str | None:
    if not os.path.isfile(path):
        return None
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def catalogue_paths(root: str) -> list[str]:
    """Same set derive_must_merge.default_catalogues() reads: patterns/catalogue*.json, sorted."""
    return sorted(os.path.relpath(p, root) for p in glob.glob(os.path.join(root, "patterns", "catalogue*.json")))


def input_fixtures(root: str, catalogues: list[str]) -> list[dict[str, str]]:
    """Every (id, role, form, fixture) derive_must_merge.catalogue_forms() extracts."""
    out = []
    for rel in catalogues:
        with open(os.path.join(root, rel), encoding="utf-8") as f:
            data = json.load(f)
        for e in data["entries"]:
            v = e.get("verdict") or {}
            for role in ("canonical", "hot_path"):
                form = v.get(role)
                if not form:
                    continue
                cand = next((c for c in e.get("candidates", []) if c.get("form") == form), None)
                fixture = (cand or {}).get("fixture") or f"patterns/{e['id']}/{form}.tk"
                out.append({"id": e["id"], "role": role, "form": form, "fixture": fixture})
    return out


def current_pins(root: str) -> dict:
    cats = catalogue_paths(root)
    fixtures = [{**f, "sha256": sha256_file(os.path.join(root, f["fixture"]))}
                for f in input_fixtures(root, cats)]
    fixtures.sort(key=lambda f: (f["fixture"], f["role"]))
    return {
        "story": "131.119",
        "generated_by": "scripts/patterns/check_mustmerge_pins.py --update",
        "consumer": CONSUMER,
        "note": ("sha256 pins of every input of the must-merge derivation. `make check-patterns` fails "
                 "when a pinned file changes; re-pin only after re-deriving the consumer. Never hand-edit."),
        "catalogues": {c: sha256_file(os.path.join(root, c)) for c in cats},
        "fixtures": fixtures,
    }


def render(pins: dict) -> str:
    return json.dumps(pins, indent=2, ensure_ascii=False) + "\n"


def drift(pinned: dict, live: dict) -> list[str]:
    """One human line per input that differs between the pin file and the tree."""
    out = []
    pc, lc = pinned.get("catalogues", {}), live["catalogues"]
    for c in sorted(set(pc) | set(lc)):
        if pc.get(c) != lc.get(c):
            what = "added" if c not in pc else "removed" if c not in lc else "changed"
            out.append(f"{c}: catalogue {what} (pinned {str(pc.get(c))[:12]}, live {str(lc.get(c))[:12]})")
    pf = {(f["fixture"], f["role"]): f for f in pinned.get("fixtures", [])}
    lf = {(f["fixture"], f["role"]): f for f in live["fixtures"]}
    for k in sorted(set(pf) | set(lf)):
        p, lv = pf.get(k), lf.get(k)
        f = lv or p
        label = f"{f['fixture']} ({f['id']} {f['role']} form {f['form']})"
        if p is None:
            out.append(f"{label}: now a must-merge input, not pinned")
        elif lv is None:
            out.append(f"{label}: pinned but no longer a canonical/hot-path input")
        elif p["sha256"] != lv["sha256"]:
            state = "deleted" if lv["sha256"] is None else "edited"
            out.append(f"{label}: {state} (pinned {str(p['sha256'])[:12]}, live {str(lv['sha256'])[:12]})")
    return out


def sibling_drift(live: dict, consumer_path: str) -> tuple[list[str], str]:
    """(mismatches, note) between the live pins and what the consumer list records."""
    if not os.path.isfile(consumer_path):
        return [], f"cross-check skipped: {consumer_path} not found"
    with open(consumer_path, encoding="utf-8") as f:
        d = json.load(f)
    forms = list(d.get("forms", [])) + list(d.get("unparsed", []))
    if not forms or any("fixture_sha256" not in f for f in forms):
        return [], "cross-check skipped: consumer list predates 131.119 (no fixture_sha256)"
    out = []
    rc = {c["path"]: c["sha256"] for c in d.get("catalogues", [])}
    lc = {os.path.basename(k): v for k, v in live["catalogues"].items()}
    if rc != lc:
        out.append(f"catalogues: consumer derived from {sorted(rc)} with other hashes than this tree")
    rf = {(f["fixture"], f["role"]): f["fixture_sha256"] for f in forms}
    lf = {(f["fixture"], f["role"]): f["sha256"] for f in live["fixtures"]}
    for k in sorted(set(rf) | set(lf)):
        if rf.get(k) != lf.get(k):
            out.append(f"{k[0]} ({k[1]}): consumer derived from {str(rf.get(k))[:12]}, tree has {str(lf.get(k))[:12]}")
    return out, f"cross-checked against {consumer_path}"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--update", action="store_true", help="rewrite the pin file from the tree")
    ap.add_argument("--root", default=ROOT, help="toke checkout (default: this script's repo)")
    ap.add_argument("--consumer", default=None,
                    help="must_merge_v04.json to cross-check (default: $TOKE_WORKSPACE/toke-tokenizer/data/...)")
    a = ap.parse_args(argv)

    try:
        live = current_pins(a.root)
    except (OSError, ValueError, KeyError) as e:
        print(f"check-mustmerge-pins: cannot read the catalogue: {e}", file=sys.stderr)
        return 2
    pin_path = os.path.join(a.root, PINS)
    if a.update:
        with open(pin_path, "w", encoding="utf-8") as f:
            f.write(render(live))
        print(f"check-mustmerge-pins: wrote {PINS} ({len(live['catalogues'])} catalogue(s), "
              f"{len(live['fixtures'])} fixtures)")
        return 0

    if not os.path.isfile(pin_path):
        print(f"check-mustmerge-pins: {PINS} missing; run `make pin-mustmerge`", file=sys.stderr)
        return 1
    with open(pin_path, encoding="utf-8") as f:
        pinned = json.load(f)
    bad = drift(pinned, live)

    consumer = a.consumer
    if consumer is None and os.environ.get("TOKE_WORKSPACE"):
        consumer = os.path.join(os.environ["TOKE_WORKSPACE"], "toke-tokenizer", "data", "must_merge_v04.json")
    note = "cross-check skipped: no $TOKE_WORKSPACE / --consumer"
    sib: list[str] = []
    if consumer:
        sib, note = sibling_drift(live, consumer)

    if bad or sib:
        print(f"check-mustmerge-pins: FAIL — must-merge inputs changed; {CONSUMER} is now stale:",
              file=sys.stderr)
        for line in bad:
            print(f"  {line}", file=sys.stderr)
        for line in sib:
            print(f"  consumer: {line}", file=sys.stderr)
        print(f"  fix: {REMEDY}", file=sys.stderr)
        return 1
    print(f"check-mustmerge-pins: OK — {len(live['catalogues'])} catalogue(s), "
          f"{len(live['fixtures'])} canonical/hot-path fixtures pinned; {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
