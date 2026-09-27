#!/usr/bin/env bash
# C213_keyword_module_name.sh — a keyword as a module name is ONE diagnostic,
# and a stray statement keyword at top level cannot stall the parser
# (story 127.219 item 5).
#
# THE DEFECT, on main at c8575da: `m=rt;` produced twenty diagnostics, 18 of
# them the same E2002 "unexpected token" at the same offset.  The module-path
# parser rejected the keyword without consuming it (and called it a "type
# declaration"), and the top-level recovery, sync(), stops unconsumed on a
# statement keyword -- so every pass of the declaration loop reported the
# same token until MAX_PARSE_ERRORS.  Any keyword did it (lp, if, mt, ...),
# and so did a stray `rt 5;` between declarations.
#
# NOW: one E2002 naming the keyword; the top-level loop steps over a token
# sync() could not move past.
#
# Negative control: TKC=<main's toke> -- cases 1-4 fail (20 diagnostics each).
#
# Story: 127.219 (item 5)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
msg1() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"message":"[^"]*"/){print substr($0,RSTART+11,RLENGTH-12); exit}'; }

echo "C213: keyword module names and top-level recovery (127.219 item 5)"

printf 'm=rt;\nf=main():i64{<0};\n' > a.tk
expect "1 m=rt: one diagnostic" "$(codes a.tk)" "E2002 "
expect "1 m=rt: it names the keyword" "$(msg1 a.tk)" "a module path segment cannot be a keyword, got 'rt'"

printf 'm=lp;\nf=main():i64{<0};\n' > b.tk
expect "2 m=lp: one diagnostic" "$(codes b.tk)" "E2002 "

printf 'm=app.mt;\nf=main():i64{<0};\n' > c.tk
expect "3 m=app.mt (keyword in a later segment): one diagnostic" "$(codes c.tk)" "E2002 "

printf 'm=main;\nrt 5;\nf=main():i64{<0};\n' > d.tk
n="$("${TKC}" --check d.tk 2>&1 | awk '/"error_code"/{c++} END{print c+0}')"
if [ "${n}" -ge 1 ] && [ "${n}" -le 3 ]; then got="bounded"; else got="${n} diagnostics"; fi
expect "4 stray 'rt 5;' at top level: at most three diagnostics, not twenty" "${got}" "bounded"

printf 'm=main;\nf=main():i64{<0};\n' > e.tk
expect "5 control: a valid module still checks clean" "$(codes e.tk)" ""

echo "C213: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
