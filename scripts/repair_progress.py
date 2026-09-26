#!/usr/bin/env python3
"""132.59a — repair the baselined structural findings in docs/progress.md.

This script never edits the tracker in place.  It reads it, writes a repaired
copy to the path given with ``--out``, and writes a report of every change
(``--report``, markdown) plus a machine-readable change log (``--log``, JSON)
that ``scripts/verify_progress_repair.py`` checks independently.

What `scripts/check_progress.py` reports, and what is actually wrong
--------------------------------------------------------------------
The 132.58a baseline counted 234 ``CELLS`` rows and described them as rows
"whose status column is not what it appears (an unescaped ``|`` shifted it)".
Measured on 2026-09-26 that premise holds for very few of them:

* **199 rows carry an extra placeholder cell** -- ``| done | 2026-04-26 | — |
  **P0** notes |`` under a five-column header.  The status cell is correct;
  what a markdown renderer drops is the *notes*, because the sixth cell has no
  header.  Repair: escape the pipe between the date/branch cell and the ``—``
  placeholder, so the placeholder joins the date cell and the notes land in
  the Notes column again.  No character other than the added ``\\`` changes.
* **~30 rows have a pipe in their text** -- ``&&``/``||``, ``|{`` (the v0.2
  match syntax), ``windowBits=15|16``, a pipe inside a code span.  Repair:
  escape exactly the pipes that are text.  A pipe is text when it sits inside
  a backtick span or is not space-delimited on both sides; when that leaves a
  row still too wide (a space-delimited pipe in prose, or a second notes cell),
  the surplus pipes after the last pre-notes column are escaped, so the extra
  text is merged into the Notes cell rather than any earlier column.
* **4 rows are one cell short** -- a missing trailing ``|`` or a missing
  Notes cell.  Repair: append `` |`` (an empty cell); no text changes.
* **One table's header is narrower than all of its rows** (``| Story |
  Description | Status | Priority |`` above rows that carry an empty fifth
  cell).  Repair: add the missing ``Notes`` column to the header and its
  separator; the rows are untouched.

``ORPHAN`` rows are rows cut off from their table by a blank line.  Each is
moved to the end of the nearest table that already holds a sibling row (same
ID prefix); its text is unchanged.

``DUPID`` rows share a story ID with an earlier story-table row.  They are
computed on the cell-repaired text, because a row the checker could not split
was never entered into its duplicate map -- the repaired tracker has more
duplicates than the baseline shows.  For each pair, the **later-dated** row is
renamed to the first free ``<id>r``/``<id>r2``... suffix and gets a note
appended to its last cell saying what it was and which row kept the ID.  The
exception is a pair that is the **same story restated** (the same title, or a
decided override below): renaming the current row would hand the ID to the
stale one, so the stale row is renamed and the note says so.

Usage::

    python3 scripts/repair_progress.py --out REPAIRED.md --report R.md --log L.json
"""
import argparse
import collections
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_progress as cp  # noqa: E402  (shared parsing rules)

STORY = "132.59a"
DATE = "2026-09-26"
DASH = {"—", "–", "-"}
DATE_RE = re.compile(r"20\d\d-\d\d-\d\d")

# Pairs that are one story written down twice rather than two stories that
# collided on a number.  Keyed by ID; the value is a substring of the row that
# is STALE (superseded by the other row) and is therefore the one renamed.
SAME_STORY_STALE = {
    "111.10": "| planned |",          # 4520 still says planned; 4515 records it done
    "111.11": "| planned |",          # 4509 planned; 4512 records it done
    "113.B.9": "Compiler/spec: confirm canonical result-constructor casing",
    # 4621 "deferred (deep ABI)" is superseded by the 2026-09-22 owner decision row
    "113.B.13": "**deferred (deep ABI)**",
    # 4643 "CLOSED (not repro)" is superseded by the later FIXED (7fbe4de) row
    "114.13": "**CLOSED (not repro)**",
}


def pipes(line):
    """Offsets of unescaped pipes in ``line``."""
    return [i for i, ch in enumerate(line) if ch == "|" and (i == 0 or line[i - 1] != "\\")]


def in_code(line):
    """Set of offsets that lie inside a backtick code span."""
    inside, out = False, set()
    for i, ch in enumerate(line):
        if ch == "`":
            inside = not inside
        elif inside:
            out.add(i)
    return out


def escape_at(line, offsets):
    for off in sorted(offsets, reverse=True):
        line = line[:off] + "\\" + line[off:]
    return line


def fix_width(line, width):
    """Return (new_line, op) making ``line`` split into ``width`` cells, or None."""
    cells = cp.split_cells(line)
    k = len(cells) - width
    if k == 0:
        return None
    if k < 0:
        # One cell short: either the trailing pipe is missing or a trailing
        # cell is.  Appending ` |` restores the count without touching text.
        new = line + " |" * (-k)
        return new, {"op": "append-cell", "n": -k}
    ps = pipes(line)
    inner = ps[1:-1] if line.rstrip().endswith("|") else ps[1:]
    code = in_code(line)

    def spaced(i):
        return i > 0 and i + 1 < len(line) and line[i - 1] == " " and line[i + 1] == " "

    textual = [i for i in inner if i in code or not spaced(i)]
    chosen = []
    if len(textual) == k:
        chosen = textual
    elif not textual and k == 1 and cells[5].strip() in DASH and width == 7:
        # The placeholder column: `| date | — | notes |`.  Join `—` to the date.
        chosen = [ps[4]]
    else:
        chosen = list(textual[:k]) if len(textual) > k else list(textual)
        if width == 7 and cells[5].strip() in DASH and len(chosen) < k and ps[4] not in chosen:
            chosen.append(ps[4])  # same placeholder rule as above, then the rest
        # Surplus space-delimited pipes: escape those after the last
        # pre-notes separator, so extra text merges into Notes only.
        keep = width - 3  # index (in ps) of the separator that opens Notes
        rest = [i for i in ps[keep + 1:] if i not in chosen]
        if line.rstrip().endswith("|"):
            rest = rest[:-1]
        need = k - len(chosen)
        chosen += rest[:need]
    new = escape_at(line, chosen)
    if len(cp.split_cells(new)) != width:
        return None
    return new, {"op": "escape", "offsets": sorted(chosen)}


def row_date(line):
    """Best date for a row: its Date column if it has one, else the first date in it."""
    cells = cp.split_cells(line)
    for c in cells[3:5]:
        m = DATE_RE.fullmatch(c.strip())
        if m:
            return m.group(0)
    m = DATE_RE.search(line)
    return m.group(0) if m else "0000-00-00"


def tables(lines):
    """Yield (header_index, [row indices]) for every table, as check_progress reads them."""
    out, cur = [], None
    for i, raw in enumerate(lines):
        line = raw.rstrip()
        if not line.startswith("|"):
            cur = None
            continue
        if cp.SEP.match(line):
            continue
        if not cp.ROW.match(line):
            cur = [i, []]
            out.append(cur)
            continue
        if cur is not None:
            cur[1].append(i)
    return out


def orphans(lines):
    """Indices of rows under no table header."""
    res, width = [], None
    for i, raw in enumerate(lines):
        line = raw.rstrip()
        if not line.startswith("|"):
            width = None
            continue
        if cp.SEP.match(line):
            continue
        if not cp.ROW.match(line):
            width = 1
            continue
        if width is None:
            res.append(i)
    return res


def sid_of(line):
    return cp.split_cells(line)[1].strip()


def prefix(sid):
    return sid.rsplit(".", 1)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=str(pathlib.Path(__file__).resolve().parent.parent / "docs" / "progress.md"))
    ap.add_argument("--out", required=True)
    ap.add_argument("--report", required=True)
    ap.add_argument("--log", required=True)
    args = ap.parse_args()

    text = pathlib.Path(args.src).read_text()
    lines = text.split("\n")
    # Each entry tracks its original 1-based line number through the moves.
    L = [{"orig": n + 1, "text": t, "ops": []} for n, t in enumerate(lines)]
    report = collections.defaultdict(list)

    # ---- 1. orphans: move each to the nearest table holding a sibling ----
    idx = orphans([e["text"] for e in L])
    moving = [L[i] for i in idx]
    for e in moving:
        L.remove(e)
    for e in moving:
        sid = sid_of(e["text"])
        cur = [x["text"] for x in L]
        best = None
        ow = len(cp.split_cells(e["text"].rstrip()))
        for h, rows in tables(cur):
            # Only a table of the orphan's own width: moving a row into a
            # narrower delivery-record table would fold its columns together.
            if len(cp.split_cells(cur[h].rstrip())) != ow:
                continue
            if any(prefix(sid_of(cur[r])) == prefix(sid) for r in rows):
                dist = abs(h - e["orig"])
                if best is None or dist < best[0]:
                    best = (dist, rows[-1])
        if best is None:
            report["orphan-unplaced"].append(f"line {e['orig']}: {sid} -- no table holds a sibling row")
            continue
        at = best[1] + 1
        L.insert(at, e)
        e["ops"].append({"op": "move"})
        report["orphan-placed"].append(
            f"line {e['orig']}: `{sid}` moved to follow `{sid_of(L[best[1]]['text'])}` "
            f"(originally line {L[best[1]]['orig']})")

    # ---- 2. headers narrower than every row: add the missing Notes column ----
    cur = [x["text"] for x in L]
    for h, rows in tables(cur):
        hw = len(cp.split_cells(cur[h].rstrip()))
        widths = {len(cp.split_cells(cur[r].rstrip())) for r in rows}
        if rows and widths == {hw + 1} and all(cp.split_cells(cur[r].rstrip())[-2].strip() == "" for r in rows):
            L[h]["text"] = cur[h].rstrip() + " Notes |"
            L[h]["ops"].append({"op": "header-add-column"})
            sep = L[h + 1]
            assert cp.SEP.match(sep["text"].rstrip()), "header without separator"
            sep["text"] = sep["text"].rstrip() + "-------|"
            sep["ops"].append({"op": "header-add-column"})
            report["header"].append(f"line {L[h]['orig']}: `{cur[h].strip()}` gains a Notes column "
                                    f"({len(rows)} rows already carry an empty fifth cell)")

    # ---- 3. cell counts ----
    width = None
    for e in L:
        line = e["text"].rstrip()
        if not line.startswith("|"):
            width = None
            continue
        if cp.SEP.match(line):
            continue
        if not cp.ROW.match(line):
            _, width = cp.parse_header(line)
            continue
        if width is None:
            continue
        cells = cp.split_cells(line)
        if len(cells) == width:
            continue
        status_before = cells[3].strip()
        r = fix_width(line, width)
        if r is None:
            report["cells-unfixed"].append(f"line {e['orig']}: {cells[1].strip()} ({len(cells)}/{width})")
            continue
        new, op = r
        trail = e["text"][len(line):]
        e["text"] = new + trail
        op["before"] = f"{len(cells)}/{width}"
        e["ops"].append(op)
        status_after = cp.split_cells(new)[3].strip()
        kind = op["op"] if op["op"] != "escape" else (
            "escape-placeholder" if len(op["offsets"]) == 1 and cells[5].strip() in DASH
            and line[op["offsets"][0] + 1:].lstrip().startswith(cells[5].strip()) else "escape-text")
        op["kind"] = kind
        report[kind].append(
            f"line {e['orig']}: `{cells[1].strip()}` {len(cells)}->{width} cells"
            + (f"; status cell was `{status_before[:40]}`, now `{status_after[:40]}`"
               if status_before != status_after else "; status cell unchanged"))

    # ---- 4. duplicate IDs (on the repaired cells) ----
    all_text = "\n".join(x["text"] for x in L)
    taken = set(re.findall(r"(?<![0-9A-Za-z.])\d+\.[0-9A-Za-z][0-9A-Za-z.\-]*", all_text))
    tdate = {}
    for h, rows in tables([x["text"] for x in L]):
        d = min((row_date(L[r]["text"]) for r in rows), default="0000-00-00")
        for r in rows:
            tdate[id(L[r])] = d
    seen = {}
    width, is_story = None, False
    renames = []
    for e in L:
        line = e["text"].rstrip()
        if not line.startswith("|"):
            width, is_story = None, False
            continue
        if cp.SEP.match(line):
            continue
        if not cp.ROW.match(line):
            is_story, width = cp.parse_header(line)
            continue
        cells = cp.split_cells(line)
        if width is None or len(cells) != width or not is_story:
            continue
        sid = cells[1].strip()
        if sid not in seen:
            seen[sid] = e
            continue
        first = seen[sid]
        stale_mark = SAME_STORY_STALE.get(sid)
        same_title = cp.split_cells(first["text"])[2].strip() == cells[2].strip()
        if stale_mark:
            victim = e if stale_mark in e["text"] else first if stale_mark in first["text"] else None
            assert victim is not None, f"override for {sid} matches neither row"
            why = "same story restated; this row is the stale one"
        elif same_title:
            victim = e if row_date(e["text"]) <= row_date(first["text"]) else first
            why = "same story restated; this row is the older one"
        else:
            # "Later-dated" is judged by the table first: a Date column holds
            # the day a story was *finished*, so a section written weeks after
            # another can hold one row finished earlier than its namesake
            # (76.1.10: Epic 76's row done 2026-05-24, the Learn/Guide
            # section's done 2026-05-10).  The table whose earliest date is
            # later is the one that reused the numbers.
            ke, kf = (tdate[id(e)], row_date(e["text"])), (tdate[id(first)], row_date(first["text"]))
            victim = e if ke >= kf else first
            why = "a different story that reused the number; this row is the later-dated one"
        keeper = first if victim is e else e
        if victim is first:
            seen[sid] = e
        renames.append((sid, victim, keeper, why))

    for sid, victim, keeper, why in renames:
        n = 1
        new = f"{sid}r"
        while new in taken:
            n += 1
            new = f"{sid}r{n}"
        taken.add(new)
        line = victim["text"].rstrip()
        trail = victim["text"][len(line):]
        ps = pipes(line)
        # ID cell is between ps[0] and ps[1]; replace the ID token only.
        idcell = line[ps[0] + 1:ps[1]]
        assert idcell.strip() == sid
        line = line[:ps[0] + 1] + idcell.replace(sid, new, 1) + line[ps[1]:]
        keeper_title = cp.split_cells(keeper["text"])[2].strip()
        # The note quotes the keeper's title; backticks are dropped so a cut
        # cannot leave an unbalanced code span, and the cut never splits `\|`.
        keeper_title = keeper_title.replace("`", "")
        if len(keeper_title) > 70:
            keeper_title = keeper_title[:67].rstrip("\\") + "..."
        note = (f" **Renumbered {DATE} ({STORY}):** was `{sid}`, which the row "
                f"\"{keeper_title}\" also used; {why}.")
        assert line.endswith("|")
        body = line[:-1].rstrip()
        line = body + note + " |"
        victim["text"] = line + trail
        victim["ops"].append({"op": "rename", "old": sid, "new": new, "note": note})
        report["rename"].append(
            f"line {victim['orig']}: `{sid}` -> `{new}` ({why}); `{sid}` stays with line {keeper['orig']}")

    out = "\n".join(e["text"] for e in L)
    pathlib.Path(args.out).write_text(out)
    log = [{"orig": e["orig"], "new": i + 1, "ops": e["ops"]} for i, e in enumerate(L)]
    pathlib.Path(args.log).write_text(json.dumps(log, indent=0))

    order = ["escape-placeholder", "escape-text", "append-cell", "header", "orphan-placed",
             "orphan-unplaced", "cells-unfixed", "rename"]
    with open(args.report, "w") as f:
        f.write(f"# {STORY} progress.md repair report\n\nSource: `{args.src}`\n\n")
        for k in order:
            f.write(f"## {k} ({len(report[k])})\n\n")
            for m in report[k]:
                f.write(f"- {m}\n")
            f.write("\n")
    for k in order:
        print(f"{k}: {len(report[k])}")
    return 1 if report["cells-unfixed"] or report["orphan-unplaced"] else 0


if __name__ == "__main__":
    sys.exit(main())
