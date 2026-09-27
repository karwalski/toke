#!/usr/bin/env bash
# C361_tki_record_layout_gate.sh — the check that a C struct handed to toke as
# a record has that record's layout is live, and fails when it should
# (story 138.18).
#
# THE DEFECT IT GUARDS.  tkc reads `x.f` as slot <index of f in the .tki>.
# std.http's `$req` was handed to handlers as the server's C Req, whose
# fields are in another order and whose pair lists are 16-byte {data, len}
# pairs, so `req.body` read the headers pointer (C360).  Nothing compared the
# two.  scripts/check_tki_layout.py (make check-tki-layout, run by make
# check-tki) now compares every struct marked `/* tki-record: mod.type */`
# in src/stdlib/*.h with its .tki record: names and order in Python, offsets
# and sizes by the C compiler.
#
# WHAT IS ASSERTED (each on a copy of the tree, never the tree itself):
#   1. the tree as it is: OK, and it checked the four http records
#   2. two fields swapped in the C struct: FAIL naming the struct
#   3. two fields swapped in the .tki: FAIL naming the record
#   4. an array field declared as the inline {data, len} pair: FAIL
#   5. a u16 field declared uint16_t (a 2-byte slot): FAIL from the compiler
#   6. the pre-138.18 state -- the server's Req marked as http.req: FAIL
#   7. no marker anywhere: FAIL (a check with nothing to check is not green)
#
# Story: 138.18
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c361_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
echo "C361: .tki record vs C struct layout gate (138.18)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

# fresh COPY: a private tree with the script, the headers and the .tki files
fresh() {
    rm -rf "${WORK}/t"; mkdir -p "${WORK}/t/scripts" "${WORK}/t/src/stdlib" "${WORK}/t/stdlib"
    cp "${REPO_ROOT}/scripts/check_tki_layout.py" "${WORK}/t/scripts/"
    cp "${REPO_ROOT}"/src/stdlib/*.h "${WORK}/t/src/stdlib/"
    cp "${REPO_ROOT}"/stdlib/*.tki "${WORK}/t/stdlib/"
}
# edit FILE PY-EXPR : apply a Python str transform; fail loudly if it changed nothing
edit() {
    python3 - "$1" "$2" <<'PY' || { echo "  FAIL: injection into $1 did not apply"; exit 1; }
import sys
p, expr = sys.argv[1], sys.argv[2]
s = open(p).read()
t = eval(expr, {"s": s, "json": __import__("json")})
if t == s:
    sys.exit(1)
open(p, "w").write(t)
PY
}
gate() {  # "<rc> <last line>"
    local out rc
    out="$(python3 "${WORK}/t/scripts/check_tki_layout.py" 2>&1)"; rc=$?
    echo "${out}" > "${WORK}/last.out"
    echo "${rc} $(printf '%s\n' "${out}" | tail -1 | cut -d' ' -f2-3)"
}
H="${WORK}/t/src/stdlib/http.h"
T="${WORK}/t/stdlib/http.tki"

fresh
expect "1  the tree: OK" "$(gate)" "0 OK --"
expect "1b it checked the four http records" \
    "$(python3 "${WORK}/t/scripts/check_tki_layout.py" --list | grep -c 'http\.h:.*= http\.\(req\|res\|httpresp\|httperr\) ')" "4"

fresh
edit "${H}" 's.replace("    int64_t     headers;    /* [[str]]: each element @(name; value) */\n    const char *body;\n", "    const char *body;\n    int64_t     headers;\n", 1)'
expect "2  C struct fields swapped: FAIL" "$(gate)" "1 FAIL --"
expect "2b ... naming TkReqRec" "$(grep -c 'TkReqRec mirrors http.req but its fields are' "${WORK}/last.out")" "1"

fresh
edit "${T}" '(lambda d: (d["exports"][0]["fields"].insert(2, d["exports"][0]["fields"].pop(3)), json.dumps(d, indent=2))[1])(json.loads(s))'
expect "3  .tki fields swapped: FAIL" "$(gate)" "1 FAIL --"

fresh
edit "${H}" 's.replace("    int64_t     headers;    /* [[str]]: each element @(name; value) */\n    const char *body;\n", "    StrPairArray headers;\n    const char *body;\n", 1)'
expect "4  array field as inline {data, len}: FAIL" "$(gate)" "1 FAIL --"
expect "4b ... compiler asserts the offsets" "$(grep -c 'C layout: TkReqRec body is_slot_3 -- false' "${WORK}/last.out")" "1"

fresh
edit "${H}" 's.replace("    int64_t     status;     /* u16 in the .tki: one whole slot */", "    uint16_t    status;", 1)'
expect "5  u16 field as uint16_t: FAIL" "$(gate)" "1 FAIL --"

fresh
edit "${H}" 's.replace("/* tki-record: http.req */\n", "", 1).replace("typedef struct {\n    const char  *method;\n    const char  *path;\n    const char  *body;\n    StrPairArray headers;", "/* tki-record: http.req */\ntypedef struct {\n    const char  *method;\n    const char  *path;\n    const char  *body;\n    StrPairArray headers;", 1)'
expect "6  the pre-138.18 C Req as http.req: FAIL" "$(gate)" "1 FAIL --"

fresh
for h in "${WORK}"/t/src/stdlib/*.h; do sed -i.bak 's/tki-record:/tki-rec0rd:/' "$h"; done
rm -f "${WORK}"/t/src/stdlib/*.bak
expect "7  no marker at all: FAIL" "$(gate)" "1 FAIL --"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
