#!/usr/bin/env bash
# C1132_returned_match_error_arm_not_e4011.sh — a match RETURNED from a `T!E`
# function may yield the function's error value from one arm and the ok value
# from another (127.245, C401); E4011 does not compare the error-valued arm,
# as it does not compare a `<` arm (story 127.340).
#
# THE DEFECT. Over a user `i64!$er` scrutinee the ok arm is typed, so
# `f=g(x:i64):i64!$e{ <mt h(x) {$ok:v v+100;$err:e $e{$bad:1}} }` drew a false
# E4011 'i64' vs 'e'.  The std-call form (C401) passed only because its ok arm
# was unknown -- 127.337 typing that arm turned C401 red the same way.
# The tolerance is narrow: only the function's own error type, only when the
# match is the direct value of a `<`.  A let-bound match with the same arms is
# still E4011, as is a returned match whose odd arm is not the error type.
#
# Negative control: toke main ccc09753 -- cases 1-2 red (E4011; the build
# fails), 3-5 green.
#
# Story: 127.340

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
H='m=re;
i=io:std.io;
t=$e{$bad:i64;$msg:str};
t=$er{$x:bool};
f=h(x:i64):i64!$er{ if(x<0){<$er{$x:true}}; <x };'
# chk <decls> — `--check` <H><decls> plus a main; "rc=<n> <first code>"
chk() {
    printf '%s\n%s\nf=main():i64{<0};\n' "$H" "$1" > "${WORK}/re.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/re.tk" 2>&1)"; rc=$?
    echo "rc=${rc} $(printf '%s' "$out" | grep -o '"error_code":"E[^"]*"' | head -1 | cut -d'"' -f4)"
}

# 1 — the returned match over a user T!E call (was a false E4011)
expect "1-returned-error-arm-clean" \
  "$(chk 'f=g(x:i64):i64!$e{ <mt h(x) {$ok:v v+100;$err:e $e{$bad:1}} };')" "rc=0 "
# 2 — runtime: each path returns as what it is
printf '%s\n%s\n' "$H" 'f=g(x:i64):i64!$e{ <mt h(x) {$ok:v v+100;$err:e $e{$bad:1}} };
f=main():i64{
  let a=mt g(5) {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  let b=mt g(0-1) {$ok:v v;$err:e mt e {$bad:k 0-k;$msg:m 0-99}};
  io.println("\(a) \(b)");
  <0
};' > "${WORK}/run.tk"
if "${TKC}" -o "${WORK}/b" "${WORK}/run.tk" >/dev/null 2>&1; then got="$("${WORK}/b" | tr '\n' ' ')"; else got="build-failed"; fi
expect "2-run" "${got}" "105 -1 "
# 3 — the same arms bound by a let are still E4011
expect "3-let-bound-still-e4011" \
  "$(chk 'f=g(x:i64):i64!$e{ let r=mt h(x) {$ok:v v+100;$err:e $e{$bad:1}}; <r };')" "rc=1 E4011"
# 4 — a returned match whose odd arm is a str (not the error type)
expect "4-returned-str-arm-e4011" \
  "$(chk 'f=g(x:i64):i64!$e{ <mt h(x) {$ok:v v+100;$err:e "bad"} };')" "rc=1 E4011"
# 5 — a struct that is not the function's error type, returned
expect "5-other-struct-arm-e4011" \
  "$(chk 'f=g(x:i64):i64!$e{ <mt h(x) {$ok:v v+100;$err:e $er{$x:true}} };')" "rc=1 E4011"

echo "C1132: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
