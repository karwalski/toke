#!/usr/bin/env python3
"""check_parallel_stdlib.py — story 135.11.

Fails when a stdlib module grows a SECOND, ungated description of itself.

The real stdlib is three gated surfaces:

  stdlib/<m>.tki      the interface   (read by glue_gen.c; gated by check-tki)
  src/stdlib/<m>.c    the implementation (built by `make`)
  docs/stdlib/<m>.md  the documentation (gated by `make check-docs`, which
                      runs check_doc_examples.py over docs/)

Anything else that describes the same module is a parallel description that no
gate reads, so it drifts silently. Two were found in 135.11:

  stdlib/<m>.tk   a toke-language reimplementation of a module whose real
                  implementation is C. Nothing resolves these: src/llvm.c
                  resolves imports to .tki plus C sources from
                  find_stdlib_sources(), and TKC_STDLIB_DIR is src/stdlib.
                  18 of the 21 present in 135.11 did not even parse.

  stdlib/<m>.md   the pre-move documentation location (validated once by the
                  48.4 story series in 2026-04, superseded by docs/stdlib/).
                  It sits outside docs/, so check_doc_examples.py never sees
                  it.

This is the shape that produced the wrapper-versus-core drift in Epic 136 and
cost 31 defects: two descriptions of one thing, one of them unchecked.

Exit 0 = clean. Exit 1 = a violation or a stale quarantine entry.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QUARANTINE = os.path.join(ROOT, "scripts", "check_parallel_stdlib_quarantine.txt")


def load_quarantine():
    """Return the set of quarantined repo-relative paths."""
    entries = set()
    if not os.path.exists(QUARANTINE):
        return entries
    with open(QUARANTINE, encoding="utf-8") as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                entries.add(line)
    return entries


def main():
    quarantined = load_quarantine()
    violations = []
    stale = []

    stdlib_dir = os.path.join(ROOT, "stdlib")
    docs_dir = os.path.join(ROOT, "docs", "stdlib")

    names = sorted(os.listdir(stdlib_dir))

    # Rule 1: no parallel .tk implementation.
    tk_live = 0
    for name in names:
        if not name.endswith(".tk"):
            continue
        rel = "stdlib/" + name
        if rel in quarantined:
            tk_live += 1
            continue
        violations.append(
            "%s — a toke reimplementation of a module implemented in "
            "src/stdlib/. Nothing resolves it, so nothing checks it. "
            "Delete it, or implement the module in toke for real and give it "
            "a gate." % rel)

    # Rule 2: no duplicate module doc outside docs/stdlib/.
    md_live = 0
    for name in names:
        if not name.endswith(".md"):
            continue
        rel = "stdlib/" + name
        if not os.path.exists(os.path.join(docs_dir, name)):
            continue  # no counterpart: not a duplicate (see README note)
        if rel in quarantined:
            md_live += 1
            continue
        violations.append(
            "%s — duplicates docs/stdlib/%s, which is the gated copy. "
            "This one is outside docs/, so check-docs never reads it. "
            "Delete it and keep docs/stdlib/%s." % (rel, name, name))

    # Rule 3: a quarantine entry whose file is gone is stale — remove the line.
    for rel in sorted(quarantined):
        if not os.path.exists(os.path.join(ROOT, rel)):
            stale.append(rel)

    for v in violations:
        print("parallel stdlib: FAIL %s" % v, file=sys.stderr)
    for s in stale:
        print("parallel stdlib: STALE quarantine entry %s — the file is gone; "
              "delete the line from %s" % (s, os.path.relpath(QUARANTINE, ROOT)),
              file=sys.stderr)

    if violations or stale:
        print("parallel stdlib: %d violation(s), %d stale entr(ies)"
              % (len(violations), len(stale)), file=sys.stderr)
        return 1

    print("parallel stdlib OK: no ungated parallel module description outside "
          "the quarantine (%d .tk, %d duplicate .md still quarantined — "
          "see %s)" % (tk_live, md_live, os.path.relpath(QUARANTINE, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
