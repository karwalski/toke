#!/usr/bin/env python3
"""132.140 -- the open-epics counter behind PROJECT_STATUS.md, committed.

PROJECT_STATUS.md's "Open epics" table was counted by a script that lived in a
session scratch directory, so nobody could reproduce it and nothing noticed
when the tracker moved on.  This is that script (open_epics_r1.py, unchanged in
its counting rules), plus the two things a published table needs: a renderer
and a drift check.

What is counted.  Every story row in docs/progress.md, read with the same
parsing rules as scripts/check_progress.py (story tables only; a row whose
cells do not split cleanly is skipped), once per unique ID, under the epic its
ID names -- rows for Epic 127 sit under several headings.  A status is
normalised by its first word: done-like words count as done, closed-out words
(superseded, cancelled, ...) as closed, and the AGENTS.md §8 open states
(in_progress, review, blocked, planned, backlog) as open.  `open`, which the
status list does not define, is counted as open in its own "Other open"
column so that it stays visible (check_progress.py rejects it; 132.139).
"Open P0/P1" counts only open rows whose priority cell states one.

Usage:
  open_epics.py [--repo DIR] [--json]   print the JSON counts (the r1 output)
  open_epics.py --table                 print the markdown table
  open_epics.py --check                 PROJECT_STATUS.md's table matches the
                                        tracker (exit 1 on drift, 2 if the
                                        table or the tracker cannot be read)
  open_epics.py --write                 rewrite PROJECT_STATUS.md's table
  open_epics.py --selftest              the check's negative control

The Subject column is prose: --check ignores it, and --write keeps the subject
already written for an epic and takes the tracker heading for a new one.
"""
import collections
import json
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve()
EPIC = re.compile(r"^#{2,3}\s+Epic\s+(\d+(?:\.\d+)?)\s*[—-]+\s*(.*)$")
OPEN = ["in_progress", "review", "blocked", "planned", "backlog", "open"]
DONE_WORDS = ("done", "complete", "completed", "closed", "merged", "shipped", "fixed",
              "resolved", "pass", "decided", "documented", "deployed")
CLOSED_WORDS = ("superseded", "cancelled", "canceled", "withdrawn", "dropped", "wontfix",
                "rejected", "obsolete", "duplicate", "retired", "abandoned", "folded",
                "absorbed", "moved", "split", "subsumed", "replaced",
                "deferred_superseded", "bypassed", "won't_do", "closed_invalid", "moot")

COLUMNS = ["Epic", "Subject", "Open", "In progress", "Review", "Blocked", "Planned",
           "Backlog", "Other open", "Open P0", "Open P1"]
KEYS = ["nopen", "in_progress", "review", "blocked", "planned", "backlog", "open",
        "p0", "p1"]
TABLE_HEAD = "| " + " | ".join(COLUMNS) + " |"


def norm(s):
    s = s.strip().strip("*").strip().lower()
    w = re.split(r"[\s(→,;:]", s, maxsplit=1)[0].strip("*").replace("-", "_")
    if w in DONE_WORDS:
        return "done"
    if w in CLOSED_WORDS:
        return "closed"
    if w in OPEN:
        return w
    if w in ("in", "inprogress", "wip", "partial", "active"):
        return "in_progress"
    if w in ("deferred", "parked", "on", "paused", "hold"):
        return "blocked"
    return "other:" + w


def count(repo):
    sys.path.insert(0, str(repo / "scripts"))
    import check_progress as cp
    epics, cur, in_story = collections.OrderedDict(), None, False
    seen = set()
    for line in (repo / "docs" / "progress.md").read_text().splitlines():
        m = EPIC.match(line)
        if m:
            cur = m.group(1).split(".")[0]
            e = epics.setdefault(cur, {"title": m.group(2).strip(),
                                       "c": collections.Counter()})
            if e["title"] == "(no heading)":
                e["title"] = m.group(2).strip()
            continue
        if not line.startswith("|"):
            in_story = False
            continue
        if not cp.ROW.match(line):
            if cp.SEP.match(line):
                continue
            in_story, _ = cp.parse_header(line)
            continue
        if not in_story or cur is None:
            continue
        cells = cp.split_cells(line)
        if len(cells) < 5:
            continue
        eid = cells[1].strip().split(".")[0]
        epics.setdefault(eid, {"title": "(no heading)", "c": collections.Counter()})
        key = cells[1].strip()
        if key in seen:
            continue
        seen.add(key)
        st = norm(cells[3])
        epics[eid]["c"][st] += 1
        if st in OPEN:
            pm = re.search(r"\bP([0-3])\b", cells[4]) if len(cells) > 4 else None
            if pm:
                epics[eid]["c"]["openP" + pm.group(1)] += 1
    out = []
    for k, v in epics.items():
        c = v["c"]
        out.append({"epic": k, "title": v["title"], "nopen": sum(c[s] for s in OPEN),
                    **{s: c[s] for s in OPEN},
                    "done": c["done"], "closed": c["closed"],
                    "p0": c["openP0"], "p1": c["openP1"],
                    "other": {kk: vv for kk, vv in c.items() if kk.startswith("other:")}})
    return out


def open_rows(counts):
    rows = [e for e in counts if e["nopen"]]
    return sorted(rows, key=lambda e: int(e["epic"]))


def find_table(lines):
    """(start, end) line indices of the open-epics table in PROJECT_STATUS.md."""
    for i, line in enumerate(lines):
        if line.strip() == TABLE_HEAD:
            j = i + 1
            while j < len(lines) and lines[j].startswith("|"):
                j += 1
            return i, j
    return None


def parse_table(lines, span):
    """{epic: (subject, [numbers...])} and the total row's numbers."""
    rows, total = {}, None
    for line in lines[span[0] + 2:span[1]]:
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        nums = [int(c.strip("*")) if c.strip("*").isdigit() else None for c in cells[2:]]
        if cells[0].strip("*").lower() == "total":
            total = nums
        else:
            rows[cells[0]] = (cells[1], nums)
    return rows, total


def render(counts, subjects=None):
    subjects = subjects or {}
    rows = open_rows(counts)
    out = [TABLE_HEAD, "|" + "---|" * len(COLUMNS)]
    total = [0] * len(KEYS)
    for e in rows:
        nums = [e[k] for k in KEYS]
        total = [a + b for a, b in zip(total, nums)]
        subject = subjects.get(e["epic"]) or e["title"]
        out.append("| %s | %s | %s |" % (e["epic"], subject,
                                          " | ".join(str(n) for n in nums)))
    out.append("| **Total** | | %s |" % " | ".join("**%d**" % n for n in total))
    return out


def main(argv):
    repo = HERE.parent.parent.parent
    if "--repo" in argv:
        repo = pathlib.Path(argv[argv.index("--repo") + 1])
    status = repo / "PROJECT_STATUS.md"
    tracker = repo / "docs" / "progress.md"
    if not tracker.exists():
        print("open_epics: tracker missing: %s" % tracker, file=sys.stderr)
        return 2
    counts = count(repo)

    if "--selftest" in argv:
        return selftest(counts)
    if "--table" in argv:
        print("\n".join(render(counts)))
        return 0
    if "--check" not in argv and "--write" not in argv:
        json.dump(counts, sys.stdout, indent=0)
        print()
        return 0

    lines = status.read_text().split("\n") if status.exists() else []
    span = find_table(lines)
    if span is None:
        print("open_epics: no open-epics table (header %r) in %s" % (TABLE_HEAD, status),
              file=sys.stderr)
        return 2
    published, _ = parse_table(lines, span)

    if "--write" in argv:
        subjects = {k: v[0] for k, v in published.items()}
        lines[span[0]:span[1]] = render(counts, subjects)
        status.write_text("\n".join(lines))
        print("open_epics: wrote %d open epics to %s" % (len(open_rows(counts)), status.name))
        return 0

    problems = compare(lines, span, counts)
    if problems:
        print("check-open-epics: PROJECT_STATUS.md's open-epics table does not match "
              "docs/progress.md:")
        for p in problems:
            print("  " + p)
        print("Refresh it with: python3 scripts/status/open_epics.py --write")
        return 1
    have = open_rows(counts)
    print("check-open-epics: %d open epics, %d open stories -- table matches the tracker"
          % (len(have), sum(e["nopen"] for e in have)))
    return 0


def compare(lines, span, counts):
    """--check: numbers, epic by epic, and the total row. Subject is prose."""
    published, _ = parse_table(lines, span)
    want = {k: v[1] for k, v in published.items()}
    have = {e["epic"]: [e[k] for k in KEYS] for e in open_rows(counts)}
    problems = []
    for epic in sorted(set(want) | set(have), key=int):
        if epic not in have:
            problems.append("Epic %s is in the table but has no open story" % epic)
        elif epic not in want:
            problems.append("Epic %s has %d open stories but is not in the table"
                            % (epic, have[epic][0]))
        elif want[epic] != have[epic]:
            diff = ", ".join("%s %s -> %s" % (COLUMNS[i + 2], a, b)
                             for i, (a, b) in enumerate(zip(want[epic], have[epic]))
                             if a != b)
            problems.append("Epic %s: %s" % (epic, diff))
    rendered_total = render(counts)[-1]
    if lines[span[1] - 1].strip() != rendered_total and not problems:
        problems.append("the Total row does not add up: %s, expected %s"
                        % (lines[span[1] - 1].strip(), rendered_total))
    return problems


def selftest(counts):
    """The negative control: a table rendered from the tracker passes, and each
    kind of drift a hand edit or a stale table produces is caught."""
    good = ["intro", ""] + render(counts) + ["", "outro"]
    span = find_table(good)
    rows = open_rows(counts)
    first = rows[0]["epic"]

    def mutate(fn):
        lines = list(good)
        fn(lines)
        return lines

    def bump(lines):                       # one count off by one
        i = next(i for i, l in enumerate(lines) if l.startswith("| %s |" % first))
        cells = lines[i].split("|")
        cells[3] = " %d " % (int(cells[3]) + 1)
        lines[i] = "|".join(cells)

    def drop(lines):                       # an open epic missing from the table
        i = next(i for i, l in enumerate(lines) if l.startswith("| %s |" % first))
        del lines[i]

    def extra(lines):                      # a closed epic still listed
        lines.insert(span[0] + 2, "| 9999 | gone | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 |")

    def total(lines):                      # rows right, total wrong
        lines[span[1] - 1] = lines[span[1] - 1].replace("| | **", "| | **1", 1)

    def subject(lines):                    # prose is not drift
        i = next(i for i, l in enumerate(lines) if l.startswith("| %s |" % first))
        cells = lines[i].split("|")
        cells[2] = " a re-worded subject "
        lines[i] = "|".join(cells)

    cases = [(True, "rendered table", good), (True, "re-worded subject", mutate(subject)),
             (False, "a count off by one", mutate(bump)),
             (False, "an open epic missing", mutate(drop)),
             (False, "an epic with no open story listed", mutate(extra)),
             (False, "a wrong total", mutate(total))]
    bad = 0
    for accept, name, lines in cases:
        sp = find_table(lines)
        problems = compare(lines, sp, counts) if sp else ["no table"]
        if bool(problems) == accept:
            bad += 1
            print("SELFTEST FAIL [%s] %s: %s" % ("accept" if accept else "reject", name,
                                                 problems or "not flagged"))
    if bad:
        print("open_epics selftest: %d of %d cases wrong" % (bad, len(cases)))
        return 1
    print("open_epics selftest: %d cases OK (2 accepted, 4 drifts caught)" % len(cases))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
