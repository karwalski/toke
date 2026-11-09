#!/usr/bin/env bash
# C1130_std_i64_result_mt_arms_agree.sh — an `mt` over a std call whose .tki
# return is `i64!E` (`str.toint` is `"return": "i64!parseerr"`) types its
# `$ok` arm binding as i64 while the arms are judged, so an arm of another
# type is E4011, as it already was over a user `i64!$er` function (story
# 127.337).
#
# THE DEFECT. mt_ok_payload_type adopts only representation-bearing payloads
# (str/bool/f64/arrays/records), so the `$ok:n` binding of an `i64!E` std call
# was TY_UNKNOWN, the `$ok` arm was skipped as unresolved, and with one known
# arm left nothing was compared: `let v=mt s.toint("7") {$ok:n n;$err:e "bad"}`
# passed --check and the binary segfaulted (rc 139).  The i64 typing is used
# for the arm binding only (arm_bind_type); the bound `let`'s type is
# unchanged.  E4011's tolerated shapes stay clean: `$err:e 0`, a `<` return
# arm, a statement match, an `$err` arm that is a call.  No `fix` field.
#
# Negative control: the 131.103 pin (toke 1f39c2998110) -- cases 1-6 red
# (--check rc 0), 7-13 green on both.
#
# Story: 127.337

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
H='m=sr;i=io:std.io;i=s:std.str;'
# chk <body> — `--check` a main whose body is <body>; prints
# "rc=<n> <first error_code> <first message> fix=<0|1>".
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/sr.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/sr.tk" 2>&1)"; rc=$?
    local code msg fix=0
    code="$(printf '%s' "$out" | grep -o '"error_code":"E[^"]*"' | head -1 | cut -d'"' -f4)"
    msg="$(printf '%s' "$out" | grep '"error_code":"E' | grep -o '"message":"[^"]*"' | head -1 | cut -d'"' -f4)"
    printf '%s' "$out" | grep -q '"fix":' && fix=1
    echo "rc=${rc} ${code} ${msg} fix=${fix}"
}
# run <body> — build and run; output on one line.
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/sr.tk"
    "${TKC}" -o "${WORK}/b" "${WORK}/sr.tk" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '
}
E11="rc=1 E4011 match arms have inconsistent types:"

# 1 — the filed repro (Z127 m1.tk): a str err arm beside the i64 payload
expect "1-str-err-arm" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n;$err:e "bad"}; io.println("\(v)");')" \
  "${E11} 'i64' vs 'str' fix=0"
# 2 — Z127 m3.tk: the payload in an expression
expect "2-expr-ok-arm" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n+0;$err:e "bad"};')" \
  "${E11} 'i64' vs 'str' fix=0"
# 3 — arms written err-first
expect "3-err-first" \
  "$(chk 'let v=mt s.toint("7") {$err:e "bad";$ok:n n};')" \
  "${E11} 'str' vs 'i64' fix=0"
# 4 — an f64 filler is not the i64's
expect "4-f64-err-arm" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n;$err:e 0.0};')" \
  "${E11} 'i64' vs 'f64' fix=0"
# 5 — the scrutinee through a let
expect "5-let-bound-scrutinee" \
  "$(chk 'let r=s.toint("7"); let v=mt r {$ok:n n;$err:e "bad"};')" \
  "${E11} 'i64' vs 'str' fix=0"
# 6 — a failing input string: the judgment is static
expect "6-failing-input" \
  "$(chk 'let v=mt s.toint("x") {$ok:n n;$err:e "bad"}; let w=0;')" \
  "${E11} 'i64' vs 'str' fix=0"

# Controls (green on both sides).
# 7 — `$err:e 0`: E4011's tolerated placeholder
expect "7-zero-err-arm-clean" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n;$err:e 0};')" "rc=0   fix=0"
# 8 — a negative sentinel is an i64 too
expect "8-neg-err-arm-clean" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n;$err:e -1};')" "rc=0   fix=0"
# 9 — a `<` return arm yields no value
expect "9-return-arm-clean" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n;$err:e <1};')" "rc=0   fix=0"
# 10 — a statement match discards its value
expect "10-statement-match-clean" \
  "$(chk 'mt s.toint("7") {$ok:n io.println("\(n)");$err:e io.println("bad")};')" "rc=0   fix=0"
# 11 — both arms str
expect "11-str-both-arms-clean" \
  "$(chk 'let v=mt s.toint("7") {$ok:n "\(n)";$err:e "bad"};')" "rc=0   fix=0"
# 12 — a conversion in the ok arm agreeing with an f64 err arm
expect "12-converted-clean" \
  "$(chk 'let v=mt s.toint("7") {$ok:n n as f64;$err:e 0.0};')" "rc=0   fix=0"
# 13 — runtime: both sides of the sentinel form
expect "13-run" \
  "$(run 'let a=mt s.toint("42") {$ok:n n;$err:e -1}; let b=mt s.toint("x") {$ok:n n;$err:e -1}; io.println("\(a) \(b)");')" \
  "42 -1 "

echo "C1130: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
