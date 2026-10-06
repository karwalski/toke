#!/usr/bin/env bash
# C704 -- three loose ends of Batch Z34 (127.269).
#
#   1  diag.c copies a diagnostic's source line into a 1024-byte buffer.  A
#      longer line was cut at byte 1023 whatever was there, so a multi-byte
#      UTF-8 character straddling the cut left a partial sequence in the JSON
#      record's source_line -- invalid UTF-8, which a strict JSON reader
#      rejects (Python: UnicodeDecodeError).  Now the cut falls on a character
#      boundary and the record says so with "source_line_truncated": true
#      (absent when the line is whole).  Text mode ends a cut line with " ...".
#      Driven for 2-, 3- and 4-byte characters at every position across the
#      cut, plus the boundaries: 1023 bytes (whole) and 1024 (cut).
#   2  src/stdlib_deps.c's db row did not list time, though db_glue.c calls
#      tk_time_nowms_w; db linked only because the core modules reach time.
#      With the dependency on the row, std.db's set reaches time from db
#      itself: tk_time.c comes right after db's own sources, before str.c.
#   3  conform_isolated printed an empty count beside a passing script whose
#      count line is "Cnnn: N passed, M failed"; scripts/conform_summary.awk
#      now reads that form as well as "Results: ...".
#
# Negative control: on main ce75d43c case 1 fails (the cut records do not
# decode, and no truncation mark), case 2 lists tk_time.c after str.c, and
# case 3 has no awk file and conform_isolated reads "Results:" only.
#
# Story: 127.269

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c704_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT

# ── 1. source_line cut on a character boundary ──────────────────────────────
python3 - "${TKC}" "${WORK}" > "${WORK}/diag.out" 2>&1 <<'PY'
import json, subprocess, sys
tkc, work = sys.argv[1], sys.argv[2]
PREFIX = 'f=main():i64{let s="'
LIMIT = 1023                     # bytes of the line diag.c keeps
bad = []
runs = 0

def diag(line, mode="--diag-json"):
    src = ('m=t;\n' + line + '\n').encode()
    p = work + '/p.tk'
    open(p, 'wb').write(src)
    r = subprocess.run([tkc, mode, '--check', p], capture_output=True)
    return r.stderr

def whole_chars(line, limit):
    """The longest prefix of line, in whole characters, within limit bytes."""
    out, n = [], 0
    for ch in line:
        k = len(ch.encode())
        if n + k > limit: break
        out.append(ch); n += k
    return ''.join(out)

def check(label, line, want_cut):
    global runs
    runs += 1
    raw = diag(line)
    try:
        rec = json.loads(raw.decode('utf-8').splitlines()[0])   # strict UTF-8
    except (UnicodeDecodeError, ValueError, IndexError) as e:
        bad.append('%s: the record does not decode: %s' % (label, str(e)[:80])); return
    sl, cut = rec.get('source_line'), rec.get('source_line_truncated')
    want = whole_chars(line, LIMIT) if want_cut else line
    if sl != want:
        bad.append('%s: source_line is %d bytes, want %d' % (label, len((sl or '').encode()), len(want.encode())))
    if want_cut and cut is not True:
        bad.append('%s: no source_line_truncated mark' % label)
    if not want_cut and 'source_line_truncated' in rec:
        bad.append('%s: source_line_truncated on a whole line' % label)

for ch in ('é', '€', '\U0001F600'):            # 2, 3, 4 bytes
    k = len(ch.encode())
    for lead in range(LIMIT - k + 1, LIMIT + 1):           # every straddling position
        pad = lead - len(PREFIX)
        line = PREFIX + 'a' * pad + ch + 'b' * 40 + '";<zz};'
        check('%d-byte char at byte %d' % (k, lead), line, True)
# boundaries: 1023 bytes is whole, 1024 is cut (ASCII)
tail = '";<zz};'
for total, cut in ((LIMIT, False), (LIMIT + 1, True)):
    line = PREFIX + 'a' * (total - len(PREFIX) - len(tail)) + tail
    assert len(line.encode()) == total
    check('ascii line of %d bytes' % total, line, cut)
# a short line carries no mark
check('short line', PREFIX + 'x' + tail, False)
# text mode: the cut line ends " ..."
t = diag(PREFIX + 'a' * 1100 + tail, '--diag-text').decode('utf-8', 'replace')
row = [l for l in t.splitlines() if l.startswith('2 | ')]
if not row or not row[0].endswith(' ...'):
    bad.append('text mode: the cut line does not end " ..."')
for b in bad[:12]: print('  ' + b)
print('RUNS %d BAD %d' % (runs, len(bad)))
PY
sed -n '/^  /p' "${WORK}/diag.out" | head -12
expect "1 source_line is cut on a UTF-8 boundary and marked; whole lines are not marked" \
    "$(grep '^RUNS' "${WORK}/diag.out" || tail -3 "${WORK}/diag.out")" "RUNS 12 BAD 0"

# ── 2. db reaches time itself ───────────────────────────────────────────────
bash "${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" db 2>/dev/null \
    | sed 's|.*/||' > "${WORK}/db.set"
expect "2 std.db's set reaches tk_time.c from db's own row (right after db_glue.c, before str.c)" \
    "$(awk '{ n[$0] = NR } END { print (n["db_glue.c"] && n["tk_time.c"] == n["db_glue.c"] + 1 && n["tk_time.c"] < n["str.c"]) ? "from-db" : "not-from-db" }' "${WORK}/db.set")" \
    "from-db"

# ── 3. the conform summary reads both count-line forms ──────────────────────
AWKF="${REPO_ROOT}/scripts/conform_summary.awk"
printf 'C123: x\n  PASS: one\n\nC123: 4 passed, 0 failed\n' > "${WORK}/s1"
printf 'Results: 7 passed, 0 failed\n' > "${WORK}/s2"
printf '  PASS: only case lines\n' > "${WORK}/s3"
expect "3 conform_summary.awk reads 'Cnnn: N passed' and 'Results:'; conform_isolated uses it" \
    "$(awk -f "${AWKF}" "${WORK}/s1" 2>&1)|$(awk -f "${AWKF}" "${WORK}/s2" 2>&1)|$(awk -f "${AWKF}" "${WORK}/s3" 2>&1)|$(grep -c 'conform_summary.awk' "${REPO_ROOT}/scripts/conform_isolated.sh")" \
    "C123: 4 passed, 0 failed|Results: 7 passed, 0 failed||1"

echo "C704: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
