#!/usr/bin/env bash
# C086_stdlib_arg_kinds.sh — a module-form stdlib call's arguments are
# checked against the representation the GLUE reads each parameter as
# (story 127.201).
#
# THE DEFECT. `s.join(p;"-")` — `(@(str),str)` against `str.join(sep;arr)` —
# passed --check, built, and died with SIGSEGV (exit 139): arity was checked
# against the glue, types never, because a std module's .tki parameter types
# are "gen":"abi" placeholders.  scripts/gen_stdlib_decls.py now writes
# src/stdlib_paramkind_gen.h from the glue bodies (S = read as a C string,
# A = read as an array), and the checker fires E4031, with no fix, on an
# established clash.
#
# WHAT IS ASSERTED.
#   1  the swapped join: E4031 on both arguments, no fix;
#   2  the canonical join checks clean and prints a-b;
#   3  json.enc of an array is NOT judged — codegen redirects it to
#      tk_json_encarr_w — and prints [1,2];
#   4  a str where the glue reads an array (`s.join("-";"x")`): E4031;
#   5  the generated table records join as S then A (grounded, not guessed).
#
# Negative control: TKC built from 84e1f4d — cases 1 and 4 check clean
# (2 of 8 red; 1-no-fix is vacuous there, 5 reads the table directly).
#
# Story: 127.201

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
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
fixes() { "${TKC}" --check "$1" 2>&1 | awk '/"error_code":"E4031"/ && /"fix":/{f=1} END{print f?"fix":"nofix"}'; }

HDR='m=t;
i=io:std.io;
i=s:std.str;
i=j:std.json;'

printf '%s\nf=main():i64{ let p=@("a";"b"); io.println(s.join(p;"-")); <0 };\n' "$HDR" > "${WORK}/p1.tk"
expect "1-swapped-join-errors" "$(errs "${WORK}/p1.tk")" "E4031 E4031 "
expect "1-swapped-join-no-fix" "$(fixes "${WORK}/p1.tk")" "nofix"

printf '%s\nf=main():i64{ let p=@("a";"b"); io.println(s.join("-";p)); <0 };\n' "$HDR" > "${WORK}/p2.tk"
expect "2-join-checks" "$(errs "${WORK}/p2.tk")" ""
expect "2-join-runs"   "$(runs "${WORK}/p2.tk")" "a-b "

printf '%s\nf=main():i64{ let a=@(1;2); io.println(j.enc(a)); <0 };\n' "$HDR" > "${WORK}/p3.tk"
expect "3-json-enc-array-checks" "$(errs "${WORK}/p3.tk")" ""
expect "3-json-enc-array-runs"   "$(runs "${WORK}/p3.tk")" "[1,2] "

printf '%s\nf=main():i64{ let x="x"; io.println(s.join("-";x)); <0 };\n' "$HDR" > "${WORK}/p4.tk"
expect "4-str-in-array-slot" "$(errs "${WORK}/p4.tk")" "E4031 "

expect "5-table-grounds-join" \
    "$(awk -F'"' '$2=="tk_str_join_w"{print $4}' "${REPO_ROOT}/src/stdlib_paramkind_gen.h")" "SA"

echo "C086: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
