#!/usr/bin/env bash
# C684 -- the parser keeps the alias of a qualified type `alias.$name`
# (131.81).
#
# parse_type_expr lowered `alias.$rec` to one NODE_TYPE_IDENT carrying the
# type token and dropped the alias, so nothing downstream could tell which
# module a type came from.  Lint's unused-import and the formatter each
# recovered it by re-reading the source bytes before the type token, which
# breaks as soon as anything but spaces sits between the tokens:
#   - lint reported `unused-import` -- with a --fix deleting the import --
#     for an import the program uses, and
#   - --fmt printed `r:.$csvreader`, source that no longer parses.
# The alias now lives on the node (qual_start/qual_len); lint and fmt read
# it there, and --dump-ast shows it as "qualifier".
#
# Negative control: TKC=<437dc471's toke> -- cases 1-3 fail.
#
# Story: 131.81

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
printf 'm=h;\ni=c:std.csv;\nf=g(r:c (* the alias *) .$csvreader):i64{<0};\nf=main():i64{<0};\n' > q.tk
printf 'm=h;\ni=c:std.csv;\nf=g(r:c.$csvreader):i64{<0};\nf=main():i64{<0};\n' > p.tk

expect "1 --dump-ast carries the qualifier" \
    "$("${TKC}" --dump-ast q.tk 2>/dev/null | grep -o '"qualifier": "[a-z]*"' | tr '\n' ' ')" '"qualifier": "c" '
expect "2 lint: a type-only use through a comment still uses the import" \
    "$("${TKC}" --lint q.tk 2>&1 | grep -c unused-import)" "0"
"${TKC}" --fmt q.tk > f.tk 2>&1
expect "3 --fmt keeps the qualifier (and its output checks)" \
    "$(grep -c 'r:c\.\$csvreader' f.tk) $("${TKC}" --check f.tk >/dev/null 2>&1; echo $?)" "1 0"
expect "4 the plain form: no unused-import, fmt unchanged" \
    "$("${TKC}" --lint p.tk 2>&1 | grep -c unused-import) $("${TKC}" --fmt p.tk 2>&1 | grep -c 'r:c\.\$csvreader')" "0 1"
printf 'm=h;\ni=c:std.csv;\nf=main():i64{<0};\n' > u.tk
expect "5 an import used nowhere is still reported" \
    "$("${TKC}" --lint u.tk 2>&1 | grep -c unused-import)" "1"

echo "C684: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
