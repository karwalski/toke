#!/usr/bin/env python3
"""132.59a — prove a repaired tracker differs from the original only as allowed.

Independent of ``repair_progress.py`` and of its change log: it reads the two
files and nothing else.  Every line of the repaired copy must pair with exactly
one line of the original, and each pair may differ only by

* a ``\\`` inserted before a previously unescaped ``|``;
* one `` |`` appended (an empty cell);
* a rename: the ID cell's token replaced and one rename note appended, where
  the note itself names the old ID (so the check restores it from the note);

plus two whole-line classes: a table header and its separator gaining a
``Notes`` column, and rows moved (reported by ID).  Any other byte difference
fails.  Exit 0 proven, 1 not.

Usage: verify_progress_repair.py ORIGINAL REPAIRED
"""
import bisect
import collections
import re
import sys

NOTE = re.compile(
    r"^(?P<body>.*) \*\*Renumbered \d{4}-\d\d-\d\d \(132\.59a\):\*\* was `(?P<old>[^`]+)`, "
    r"which the row \".*?\" also used; [^|]*\. \|$")
IDCELL = re.compile(r"^\|(\s*)(\S+)(\s*)\|")


def unrename(line):
    m = NOTE.match(line)
    if not m:
        return line, None
    body = m.group("body") + " |"
    idm = IDCELL.match(body)
    new = idm.group(2)
    if not new.startswith(m.group("old")) or not re.fullmatch(r"r\d*", new[len(m.group("old")):]):
        return line, "bad-rename"
    restored = "|" + idm.group(1) + m.group("old") + idm.group(3) + body[idm.end() - 1:]
    return restored, (m.group("old"), new)


def key(s):
    return re.sub(r"[\s|]+$", "", s.replace("\\|", "|"))


def walk(o, r):
    """Return (escapes_added, appended) if r == o plus allowed insertions, else None."""
    i = j = esc = 0
    while i < len(o) and j < len(r):
        if r[j] == "\\" and j + 1 < len(r) and r[j + 1] == "|" and o[i] == "|" and (i == 0 or o[i - 1] != "\\"):
            esc += 1
            j += 1
            continue
        if o[i] != r[j]:
            return None
        i += 1
        j += 1
    if i != len(o):
        return None
    tail = r[j:]
    if tail not in ("", " |"):
        return None
    return esc, tail == " |"


def main():
    O = open(sys.argv[1]).read().split("\n")
    R = open(sys.argv[2]).read().split("\n")
    pool = collections.defaultdict(collections.deque)
    for i, o in enumerate(O):
        pool[key(o)].append(i)
    pairs, unpaired, bad = [], [], []
    stats = collections.Counter()
    for j, r in enumerate(R):
        r1, ren = unrename(r)
        if ren == "bad-rename":
            bad.append(f"R{j+1}: malformed rename")
            continue
        q = pool.get(key(r1))
        if not q:
            unpaired.append(j)
            continue
        i = q.popleft()
        w = walk(O[i], r1)
        if w is None:
            bad.append(f"R{j+1} vs O{i+1}: differs beyond escapes/appended cell/rename")
            continue
        stats["escapes"] += w[0]
        stats["rows-escaped"] += w[0] > 0
        stats["cells-appended"] += w[1]
        if ren:
            stats["renames"] += 1
        pairs.append((i, j))
    left = sorted(i for q in pool.values() for i in q)
    # Header lines: allowed only as original + " Notes |" / "-------|".
    for j in list(unpaired):
        for i in list(left):
            if R[j] in (O[i].rstrip() + " Notes |", O[i].rstrip() + "-------|"):
                stats["header-lines"] += 1
                unpaired.remove(j)
                left.remove(i)
                pairs.append((i, j))
                break
    for j in unpaired:
        bad.append(f"R{j+1}: no original line: {R[j][:100]!r}")
    for i in left:
        bad.append(f"O{i+1}: dropped: {O[i][:100]!r}")
    # Moves: pairs sorted by repaired position; original indices out of order.
    pairs.sort(key=lambda p: p[1])
    # Non-row lines (prose, headers, blank lines) are interchangeable when
    # identical, so their pairing proves nothing about order; instead the
    # sequence of non-row lines must be the same in both files, header edits
    # aside.  Order is then judged over rows only.
    def isrow(s):
        m = IDCELL.match(s)
        return bool(m and re.match(r"\d+\.", m.group(2)))
    hdr_new = {j for i, j in pairs if R[j] != O[i] and not isrow(O[i]) and key(R[j]) != key(O[i])}
    o_nonrow = [O[i] for i in range(len(O)) if not isrow(O[i])]
    r_nonrow = [O[i] if j in hdr_new else R[j] for i, j in pairs if not isrow(O[i])]
    if [x for x in o_nonrow if x.strip()] != [x for x in r_nonrow if x.strip()]:
        bad.append("the sequence of non-blank non-row lines changed")
    stats["blank-lines-delta"] = sum(1 for x in r_nonrow if not x.strip()) - sum(1 for x in o_nonrow if not x.strip())
    pairs = [(i, j) for i, j in pairs if isrow(O[i])]
    seq = [i for i, _ in pairs]
    # longest increasing subsequence = lines that kept their order
    tails, idx, prev = [], [], [-1] * len(seq)
    for k, v in enumerate(seq):
        p = bisect.bisect_left([seq[t] for t in tails], v)
        if p == len(tails):
            tails.append(k)
        else:
            tails[p] = k
        prev[k] = tails[p - 1] if p else -1
    keep, k = set(), tails[-1] if tails else -1
    while k != -1:
        keep.add(k)
        k = prev[k]
    moved = [seq[k] for k in range(len(seq)) if k not in keep]
    stats["rows-moved"] = len(moved)
    for b in bad:
        print("FAIL", b)
    print("verify-progress-repair:", dict(stats), "moved:",
          ", ".join(IDCELL.match(O[i]).group(2) for i in moved if IDCELL.match(O[i])))
    print(f"verify-progress-repair: {len(O)} original lines, {len(R)} repaired, {len(bad)} violation(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
