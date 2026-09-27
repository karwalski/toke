#!/usr/bin/env python3
"""check_stdlib_doc_quarantine.py -- docs/stdlib/*.md marks exactly the
quarantined exports (story 127.230 item 6).

THE DEFECT.  scripts/check_tki_skiplist.txt is the single authoritative record
of the quarantine (136.7): an export declared by a .tki whose glue symbol no C
source defines.  Since 127.224 (4), `tkc --check` rejects a call to one.  The
docs/stdlib pages were written by hand from the interfaces and never consulted
that record, so canvas.md (and a dozen other pages) documented quarantined
calls -- canvas.arc, canvas.fill -- as ordinary functions, with nothing to say
that a program calling them does not compile.

THE RULE.  A documented function entry is either
  * a table row whose first cell is `mod.name` (optionally with its argument
    list inside the backticks), or
  * a heading `##`-`####` that starts with `mod.name(`;
and it is MARKED when the marker link (MARKER below) is on the row itself or,
for a heading, on the first non-blank line after it.  The check fails when
  * an entry names a quarantined export and is not marked, or
  * an entry is marked and does not name a quarantined export (the glue
    landed, or the marker was pasted onto the wrong row) --
so the markers are derived from the skiplist and cannot drift from it in
either direction.  `--fix` rewrites the pages to agree (adds the missing
markers, strips stale ones); never hand-maintain a second list.

The population comes from check_tki_coverage.load_skiplist, the same reader
that renders docs/reference/tki-quarantine.md, so the two cannot disagree.

Usage:
    check_stdlib_doc_quarantine.py [--fix] [--docs DIR] [--skiplist FILE]
Exit: 0 agree, 1 disagreement (listed), 2 cannot read the inputs.
"""
import argparse
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))
import check_tki_coverage as ctc  # noqa: E402

MARKER = "[quarantined](/docs/reference/tki-quarantine/)"
HEADING_NOTE = ("> " + MARKER + ": declared in the interface, but no C symbol "
                "implements it, so `tkc --check` rejects a call to it.")

ROW = re.compile(r"^\|\s*`([a-z][a-z0-9_]*\.[a-z0-9_.]+)(?:\([^`]*\))?`")
HEAD = re.compile(r"^#{2,4}\s+`?([a-z][a-z0-9_]*\.[a-z0-9_.]+)\(")
ROW_MARK = re.compile(r"\s*" + re.escape(MARKER))


def quarantined(skiplist):
    names = set()
    for key in ctc.load_skiplist(skiplist):
        _, _, name = key.partition("::")
        names.add(name)
    return names


def scan(path, quar, fix):
    """Return (problems, new_lines or None)."""
    lines = path.read_text().splitlines(keepends=True)
    out, problems, changed = [], [], False
    i = 0
    while i < len(lines):
        line = lines[i]
        m = ROW.match(line)
        if m:
            name, marked = m.group(1), MARKER in line
            want = name in quar
            if want != marked:
                problems.append((i + 1, name, want))
                if fix:
                    if want:  # append the marker to the first cell
                        end = m.end()
                        line = line[:end] + " " + MARKER + line[end:]
                    else:
                        line = ROW_MARK.sub("", line, count=1)
                    changed = True
            out.append(line)
            i += 1
            continue
        m = HEAD.match(line)
        if m:
            name = m.group(1)
            out.append(line)
            j = i + 1
            blanks = []
            while j < len(lines) and not lines[j].strip():
                blanks.append(lines[j])
                j += 1
            nxt = lines[j] if j < len(lines) else ""
            marked = MARKER in nxt
            want = name in quar
            if want != marked:
                problems.append((i + 1, name, want))
                if fix:
                    changed = True
                    if want:
                        out.append("\n" + HEADING_NOTE + "\n")
                        out.extend(blanks)
                        i = j
                        continue
                    if nxt.strip() == HEADING_NOTE:
                        # drop the note and the blank line that preceded it
                        out.extend(blanks[1:] if blanks else [])
                        i = j + 1
                        continue
            i += 1
            continue
        out.append(line)
        i += 1
    return problems, (out if changed else None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fix", action="store_true")
    ap.add_argument("--docs", default=str(REPO / "docs" / "stdlib"))
    ap.add_argument("--skiplist", default=str(ctc.SKIPLIST))
    a = ap.parse_args()

    quar = quarantined(pathlib.Path(a.skiplist))
    pages = sorted(pathlib.Path(a.docs).glob("*.md"))
    if not quar or not pages:
        print("check-stdlib-doc-quarantine: nothing to compare (%d quarantined, "
              "%d pages) -- refusing to report a pass" % (len(quar), len(pages)))
        return 2

    bad, entries_marked = [], 0
    for page in pages:
        problems, new = scan(page, quar, a.fix)
        for ln, name, want in problems:
            bad.append("  %s:%d  %s  %s" % (
                page.relative_to(REPO) if page.is_relative_to(REPO) else page, ln, name,
                "is quarantined but not marked" if want
                else "is marked quarantined but is not in the skiplist"))
        if new is not None:
            page.write_text("".join(new))
        # count what now carries the marker, for the summary line
        entries_marked += sum(1 for ln in page.read_text().splitlines() if MARKER in ln)

    if bad and not a.fix:
        print("check-stdlib-doc-quarantine: FAIL -- %d %s with %s:"
              % (len(bad), "entry disagrees" if len(bad) == 1 else "entries disagree",
                 pathlib.Path(a.skiplist).name))
        print("\n".join(bad))
        print("  (run: python3 scripts/check_stdlib_doc_quarantine.py --fix)")
        return 1
    if a.fix and bad:
        print("check-stdlib-doc-quarantine: fixed %d entr%s:" %
              (len(bad), "y" if len(bad) == 1 else "ies"))
        print("\n".join(bad))
    print("check-stdlib-doc-quarantine: OK -- %d page(s), %d quarantined export(s), "
          "%d marked line(s)" % (len(pages), len(quar), entries_marked))
    return 0


if __name__ == "__main__":
    sys.exit(main())
