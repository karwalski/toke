#!/usr/bin/env bash
# C901_strlen_judges_mut_split.sh — a `mut` binding of a std `[T]` return,
# passed to a `str` parameter, is E4031 when every assignment to it is the
# same std `[T]` (story 127.315).
#
# THE DEFECT. 127.314 (C890) made `let a=s.split(..); s.len(a)` E4031
# through std_array_ret, which -- like stdlib_str_bound -- followed only an
# immutable `let`.  `let a=mut.s.split("a,b";","); s.len(a)` still passed
# --check (rc 0) and printed 1: the glue read the array handle as a string.
#
# NOW std_array_ret follows a `mut` binding too (std_array_mut_ret), but
# only when it is the binding lexically in force at the use and EVERY
# assignment to it in the function is a std [T] identical to the
# initialiser's.  The checker leaves a `mut` binding untyped, so
# `a="xyz"` is accepted and afterwards `s.len(a)` is correct (prints 3):
# judging by the initialiser alone would be a false E4031 (cases 5-7).
# The 127.230 item 5 user-call path reads the same function (case 4).
#
# Negative control: TKC built from 06f2b0b4 + 127.314 (10f5a079) --
# cases 1-4 red (rc 0), 5-8 green on both.
#
# Story: 127.315

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
RUN="${REPO_ROOT}/test/run_test.sh"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
H='m=mb;i=s:std.str;i=io:std.io;f=g(x:str):i64{<s.len(x) as i64};'
# chk <body> -- `--check` a main whose body is <body>: "rc=<n> <first code>"
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/mb.tk"
    local out rc code
    out="$("${TKC}" --check "${WORK}/mb.tk" 2>&1)"; rc=$?
    code="$(printf '%s' "$out" | grep -o '"error_code":"[A-Z0-9]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${code:-none}"
}
# run <body> -- build and run the same program: its output
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/rb.tk"
    "${RUN}" 30 "${TKC}" "${WORK}/rb.tk" -o "${WORK}/rb" >/dev/null 2>&1 || { echo build-failed; return; }
    "${RUN}" 20 "${WORK}/rb" 2>&1
}

expect "1 the 127.315 repro: mut split into s.len" \
  "$(chk 'let a=mut.s.split("a,b";","); io.println(s.fromint(s.len(a) as i64));')" "rc=1 E4031"
expect "2 every assignment is the same std [str]" \
  "$(chk 'let a=mut.s.split("a,b";","); a=s.split("c";","); io.println(s.fromint(s.len(a) as i64));')" "rc=1 E4031"
expect "3 through an immutable alias of the mut binding" \
  "$(chk 'let b=mut.s.bytes("x"); let c=b; io.println(s.fromint(s.len(c) as i64));')" "rc=1 E4031"
expect "4 user str parameter (127.230 item 5 path)" \
  "$(chk 'let b=mut.s.bytes("x"); io.println(s.fromint(g(b)));')" "rc=1 E4031"
# Re-assigned to a str: the call is correct, so it must stay clean.
B5='let a=mut.s.split("a,b";","); a="xyz"; io.println(s.fromint(s.len(a) as i64));'
expect "5 re-assigned a str: not judged"            "$(chk "$B5")" "rc=0 none"
expect "5b ...and it prints the str's length"       "$(run "$B5")" "3"
B6='let a=mut.s.split("a,b";","); if(1<2){a="xyz"}; io.println(s.fromint(s.len(a) as i64));'
expect "6 re-assigned in a nested block: not judged" "$(chk "$B6")" "rc=0 none"
expect "7 re-assigned another array type: not judged" \
  "$(chk 'let a=mut.s.split("a,b";","); a=s.bytes("q"); io.println(s.fromint(a.len as i64));')" "rc=0 none"
expect "8 the array used as an array: clean" \
  "$(chk 'let a=mut.s.split("a,b";","); io.println(s.fromint(a.len as i64));')" "rc=0 none"

echo "C901: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
