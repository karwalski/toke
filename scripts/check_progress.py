#!/usr/bin/env python3
"""132.58a — structural validation of docs/progress.md, the canonical tracker.

`docs/progress.md` carries 1,094 story rows and is the single source of truth
for every epic in this project.  Until this script existed **nothing checked
it at all** -- it appears in no `make` target and in no CI job -- which is how
twelve structurally malformed rows and nineteen reused IDs accumulated
unnoticed.

Two things are checked, and both have bitten this project:

1. **Cell structure.**  A story row is `| ID | Story | Status | Priority |
   Notes |`, so splitting on unescaped `|` must give exactly 7 fields.  A raw
   `|` inside a note silently shifts every later column: the status of such a
   row is whatever text happened to land in that position.  Pipes inside notes
   must be written `\\|`.

2. **ID uniqueness within story tables.**  A duplicated ID means a scripted
   update matching `| 132.47 |` edits two unrelated stories, and a human
   following a cross-reference cannot tell which one is meant.  This was found
   when exactly that happened while recording a batch of work.

**Why the table header matters, and why this is not a skiplist.**  progress.md
also contains *delivery-record* tables -- `| ID | New status | Resolution |`
and similar -- which legitimately restate an ID that a story table already
owns, and legitimately have a different column count.  The 114.10-114.22 block
under "Codegen/type-bug sweep (2026-06-24)" is one of these.  Keying off the
table header tells the two apart structurally, so no dated exemption list is
needed and none is offered: a future delivery record is recognised
automatically, and a future duplicate story ID still fails.

3. **A defined status (132.139).**  AGENTS.md §8 defines the statuses:
   `backlog | planned | in_progress | blocked | review | done`.  A story row's
   status cell must begin with one of those, or with one of the closed-out
   words the tracker has always used for a story that ends without being done
   (STATUS_CLOSED: superseded, cancelled, withdrawn, ...).  Emphasis and a
   trailing note are allowed -- `**DONE 2026-09-19** -- premise wrong` is a
   done row -- but the FIRST word decides.  `open`, which two Epic 128 rows
   carried, is in neither list: nothing that filters the tracker by status
   (PROJECT_STATUS's open-epics count, the Gantt reader) knows what it means.

Exit 0 clean, 1 on a finding, 2 if the anchor is missing (no story table
found, or fewer rows than the floor) -- a check that cannot find its input
must fail rather than pass vacuously.
"""
import collections
import pathlib
import re
import sys

# Story IDs in this tracker are not uniformly `N.N`.  Alongside `127.141` and
# `132.58a` there are `124.4-testcaps`, `124.4-fu`, `123.11-fu`, `124.2f-d`,
# `124.4c2` and `113.B.9`.  A narrower pattern does not merely skip those rows
# -- it mistakes them for table headers, which silently resets the parser's
# idea of what table it is in and takes the rows after them with it.  That is
# how a first draft of this script saw 608 of 1,094 rows.
ROW = re.compile(r"^\|\s*\d+\.[0-9A-Za-z][0-9A-Za-z.\-]*\s*\|")
SEP = re.compile(r"^\|[\s:-]+\|[\s:|-]*$")
# A tracker that has lost most of its rows is a broken read, not a clean tree.
MIN_ROWS = 2500

# Why there is a baseline, and why it is a ratchet rather than an exemption
# list.  When this check was first run it reported 287 findings, every one of
# them in a historical epic (7, 48-52, 70, 74-82, 126) written before the
# escaping convention settled.  Fixing all 287 is a separate, reviewable job
# -- some of those "duplicates" are a roadmap index restating a detail row and
# are correct as they stand.  Gating on them immediately would only mean the
# gate gets switched off.
#
# So: findings recorded here are tolerated, anything new fails, and a baseline
# entry that stops being produced ALSO fails so the file cannot rot into a
# blanket amnesty.  Baseline keys deliberately exclude line numbers -- every
# row added to the tracker shifts them, and a baseline that churns on every
# edit is one nobody will keep accurate.
BASELINE = pathlib.Path(__file__).resolve().parent / "progress-baseline.txt"

# 132.139: AGENTS.md §8 -- the defined status values.
STATUS_DEFINED = ("backlog", "planned", "in_progress", "blocked", "review", "done")
# ...and the closed-out words a story table has always used for a story that
# ends without being done.  Each is terminal and unambiguous to a reader; none
# is a synonym for an open state.  Add to this list deliberately, never to
# quiet a row: an open story takes one of STATUS_DEFINED.
STATUS_CLOSED = ("superseded", "cancelled", "withdrawn", "won't-do", "moved",
                 "deferred", "bypassed", "closed", "closed-invalid", "fixed",
                 "moot", "decided", "deployed")
STATUS_WORD = re.compile(r"[a-z][a-z'_-]*")


def status_word(cell):
    """The status a cell declares: its first word, lower case, emphasis removed."""
    m = STATUS_WORD.match(re.sub(r"[*`]", "", cell).strip().lower())
    return m.group(0).rstrip("-") if m else ""


def status_defined(cell):
    """True when the cell's first word is a defined or closed-out status."""
    return status_word(cell) in STATUS_DEFINED + STATUS_CLOSED


def split_cells(line):
    """Split a markdown row on unescaped pipes, as the tracker's own convention."""
    return re.split(r"(?<!\\)\|", line)


def parse_header(line):
    """Return (is_story_table, expected_cell_count) for a table header row.

    The expected width comes from the header itself rather than from a fixed
    constant, because story tables in this file spell column four four
    different ways -- `Priority`, `Prio`, `Date`, `Branch` -- and there is a
    narrower four-column variant as well.  Pinning one shape would have marked
    486 sound rows as malformed.

    A table is a *story* table -- the kind whose IDs must be unique -- when it
    is keyed by ID and carries a status.  Delivery records such as
    `| ID | New status | Resolution |` legitimately restate an ID a story
    table already owns, so they are width-checked but not uniqueness-checked.
    """
    raw = split_cells(line)
    cells = [c.strip().lower() for c in raw]
    inner = cells[1:-1] if len(cells) >= 2 and cells[0] == "" and cells[-1] == "" else cells
    is_story = (
        len(inner) >= 4
        and inner[0] == "id"
        and "status" in inner
        and "notes" in inner
    )
    return is_story, len(raw)


def status_column(line):
    """Index, in split_cells(row), of the header's Status column (or None)."""
    cells = [c.strip().lower() for c in split_cells(line)]
    return cells.index("status") if "status" in cells else None


def main():
    # 132.59a: `--path FILE` checks another copy of the tracker (a repaired
    # candidate) against the same rules; `--baseline FILE` swaps the ratchet
    # (use an empty file to see every finding as new).  `--record` is refused
    # with either, so the checked-in baseline is only ever recorded from the
    # checked-in tracker.
    path = pathlib.Path(__file__).resolve().parent.parent / "docs" / "progress.md"
    baseline = BASELINE
    argv = sys.argv[1:]
    for flag in ("--path", "--baseline"):
        if flag in argv:
            i = argv.index(flag)
            if i + 1 >= len(argv):
                print(f"check-progress: {flag} needs a file argument", file=sys.stderr)
                return 2
            val = pathlib.Path(argv[i + 1])
            if flag == "--path":
                path = val
            else:
                baseline = val
    if "--record" in argv and ("--path" in argv or "--baseline" in argv):
        print("check-progress: --record only records the checked-in tracker", file=sys.stderr)
        return 2
    if not path.exists():
        print(f"check-progress: anchor missing: {path}", file=sys.stderr)
        return 2

    findings, total_rows, story_rows, seen = collect(path.read_text(), path.name)

    if total_rows < MIN_ROWS:
        print(
            f"check-progress: only {total_rows} rows found (floor "
            f"{MIN_ROWS}) -- the parser is not reading the tracker it should be.",
            file=sys.stderr,
        )
        return 2
    return report(findings, total_rows, story_rows, seen, baseline)


def collect(text, name):
    """Every finding in a tracker's text: (findings, rows, story rows, ids)."""
    findings = []          # (key, message)
    counts = collections.Counter()

    def add(kind, sid, detail, message):
        """Record a finding under a line-number-free baseline key."""
        base = f"{kind}\t{sid}\t{detail}"
        counts[base] += 1
        findings.append((f"{base}\t#{counts[base]}", message))

    seen = {}
    story_rows = 0
    total_rows = 0
    is_story = False
    width = None  # expected cell count of the table we are inside
    status_at = None

    for n, raw in enumerate(text.split("\n"), 1):
        line = raw.rstrip()
        if not line.startswith("|"):
            is_story, width = False, None
            continue
        if SEP.match(line):
            continue
        if not ROW.match(line):
            is_story, width = parse_header(line)
            status_at = status_column(line)
            continue

        total_rows += 1
        cells = split_cells(line)
        sid = cells[1].strip()

        if width is None:
            add(
                "ORPHAN", sid, "",
                f"ORPHAN {name}:{n}: row {sid} is not under any table "
                f"header, so nothing defines its columns."
            )
            continue

        if len(cells) != width:
            add(
                "CELLS", sid, f"{len(cells)}/{width}",
                f"CELLS {name}:{n}: row {sid} splits into {len(cells)} "
                f"fields; its table header declares {width} -- an unescaped "
                f"`|` in a note shifts every later column, so this row's "
                f"status is not what it appears. Write pipes as `\\|`."
            )
            continue

        if not is_story:
            continue  # a delivery record: width-checked, not uniqueness-checked
        story_rows += 1

        if status_at is not None and not status_defined(cells[status_at]):
            st = cells[status_at].strip()
            add(
                "STATUS", sid, status_word(st) or st[:20],
                f"STATUS {name}:{n}: row {sid} has status `{st[:60]}`, which is "
                f"not a defined status. Use one of "
                f"{' | '.join(STATUS_DEFINED)} (AGENTS.md §8), or a closed-out "
                f"word: {', '.join(STATUS_CLOSED)}."
            )

        if sid in seen:
            add(
                "DUPID", sid, "",
                f"DUPID {name}:{n}: story ID {sid} is already used at "
                f"line {seen[sid]}. Two different stories share one ID: a "
                f"scripted update matching `| {sid} |` hits both, and a "
                f"cross-reference to {sid} is ambiguous. Renumber the later one."
            )
        else:
            seen[sid] = n
    return findings, total_rows, story_rows, seen


def report(findings, total_rows, story_rows, seen, baseline):
    if "--record" in sys.argv:
        BASELINE.write_text(
            "# 132.58a baseline -- pre-existing structural and (132.139) status\n"
            "# findings in docs/progress.md. Anything NOT listed\n"
            "# here fails the gate; an entry here that stops being produced\n"
            "# also fails, so this file cannot rot into a blanket amnesty.\n"
            "# Regenerate deliberately with --record, never to quiet a gate.\n"
            + "".join(f"{k}\n" for k, _ in sorted(findings))
        )
        print(f"check-progress: recorded {len(findings)} baseline entries")
        return 0

    if not baseline.exists():
        print(f"check-progress: baseline missing: {baseline}", file=sys.stderr)
        return 2

    known = {
        l for l in baseline.read_text().split("\n")
        if l.strip() and not l.startswith("#")
    }
    produced = {k for k, _ in findings}

    new = [(k, m) for k, m in findings if k not in known]
    stale = sorted(known - produced)

    for _, m in sorted(new, key=lambda x: x[1]):
        print(m)
    for k in stale:
        print(
            f"STALE baseline entry no longer produced: {k.split(chr(9))[0]} "
            f"{k.split(chr(9))[1]} -- the row was fixed or renumbered. Remove "
            f"it from {baseline.name} (--record) so the ratchet tightens."
        )

    print(
        f"check-progress: {total_rows} rows ({story_rows} in story tables), "
        f"{len(seen)} unique IDs, {len(findings)} finding(s) "
        f"({len(known)} baselined, {len(new)} new, {len(stale)} stale)"
    )
    return 1 if (new or stale) else 0


# 132.139 -- the negative control.  A gate nobody has watched fail is not a
# gate: each REJECT row below must be reported, each ACCEPT row must not.
SELFTEST_ROWS = [
    # (accepted?, status cell)
    (True, "done"), (True, "backlog"), (True, "planned"), (True, "in_progress"),
    (True, "blocked"), (True, "review"), (True, "**DONE 2026-09-19** — premise wrong"),
    (True, "in_progress — **plumbing DONE 2026-09-18**"), (True, "superseded → 116.9"),
    (True, "won't-do"), (True, "**CLOSED-INVALID** (this pass)"),
    (True, "done — closed by 127.41, verified 2026-09-22"),
    (False, "open"), (False, "**open**"), (False, "todo"), (False, "wip"),
    (False, "in progress"), (False, ""), (False, "PREMISE WRONG — close with no work"),
    (False, "**harness DONE 2026-09-18**"),
]


def selftest():
    head = "| ID | Story | Status | Priority | Notes |\n|---|---|---|---|---|\n"
    bad = 0
    for i, (accept, cell) in enumerate(SELFTEST_ROWS):
        text = head + "| 9.%d | a story | %s | P1 | note |\n" % (i, cell)
        findings = collect(text, "selftest.md")[0]
        flagged = any(k.startswith("STATUS\t") for k, _ in findings)
        if flagged == accept:
            bad += 1
            print("SELFTEST FAIL [%s] status %r was %s"
                  % ("accept" if accept else "reject", cell,
                     "rejected" if flagged else "accepted"))
    # a delivery record restates statuses freely and is not status-checked
    text = ("| ID | New status | Resolution |\n|---|---|---|\n"
            "| 9.99 | open | x |\n")
    if collect(text, "selftest.md")[0]:
        bad += 1
        print("SELFTEST FAIL [accept] a delivery-record row was status-checked")
    n = len(SELFTEST_ROWS) + 1
    if bad:
        print("check-progress selftest: %d of %d cases wrong" % (bad, n))
        return 1
    print("check-progress selftest: %d cases OK (%d statuses accepted, %d rejected, "
          "1 delivery record left alone)"
          % (n, sum(1 for a, _ in SELFTEST_ROWS if a),
             sum(1 for a, _ in SELFTEST_ROWS if not a)))
    return 0


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
