#!/usr/bin/env bash
# C1000_opaque_std_handle_judged.sh — an OPAQUE std record (`"fields": []`:
# std.process's `handle`, std.vec's vechandle, ...) is judged by its declared
# .tki type: a str, float or array where one is declared is E4031, a handle
# where a str is declared is E4031, and an `mt` over process.spawn types its
# `$ok` binding as `handle`, so an arm of another kind is E4011 (story 138.21).
#
# THE DEFECT. `process.wait("abc")` passed --check: check_tki_arg_types puts
# an opaque record in representation class 0 (never judged), because an
# integer is a legitimate handle (136.36).  And `let h=mt process.spawn(..)
# {$ok:h h; $err:e "oops"}` was clean because the inline `handle!processerr`
# payload was left unknown (the 127.66 rule adopts no struct there), so a
# failed spawn bound the string and `process.wait(h)` printed w=1702063201 --
# the string's bytes read as a ProcessHandle (Z108 probe hstr2).  Now an
# opaque record is judged against the OTHER side's kind (an integer either
# way, or the same record, stays clean), and an opaque record is adopted as
# the ok payload of an inline `T!E` -- it has no fields for codegen to read
# and is integer-compatible (types_equal), so the emitted IR is unchanged.
#
# Negative control: TKC built from toke main c26cbc1b -- cases 1-7 red
# (--check rc 0), 8-12 green on both.
#
# Story: 138.21

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
H='m=ph;i=io:std.io;i=s:std.str;i=process:std.process;i=vec:std.vec;'
# chk <body> — `--check` a main whose body is <body>; prints
# "rc=<n> <first error_code> <expected> <got>".
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/ph.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/ph.tk" 2>&1)"; rc=$?
    local code exp got
    code="$(printf '%s' "$out" | grep -o '"error_code":"[^"]*"' | head -1 | cut -d'"' -f4)"
    exp="$(printf '%s' "$out" | grep -o '"expected":"[^"]*"' | head -1 | cut -d'"' -f4)"
    got="$(printf '%s' "$out" | grep -o '"got":"[^"]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${code} ${exp} ${got}"
}
# run <body> — build and run with --allow-run; output on one line.
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/ph.tk"
    "${TKC}" -o "${WORK}/b" "${WORK}/ph.tk" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" --allow-run 2>/dev/null | tr '\n' ' '
}
SPAWN_OK='mt process.spawn(@("/bin/echo";"hi")) {$ok:v v;$err:e 0}'
SPAWN_BAD='process.spawn(@("/nonexistent/c1000"))'

# 1 — the filed repro (Z108 probe hw): a str where a handle is declared
expect "1-str-into-wait" \
  "$(chk 'let w=process.wait("abc");')" \
  "rc=1 E4031 handle str"
# 2 — a float
expect "2-f64-into-wait" \
  "$(chk 'let w=process.wait(1.5);')" \
  "rc=1 E4031 handle f64"
# 3 — an array
expect "3-array-into-kill" \
  "$(chk 'let k=process.kill(@(1;2));')" \
  "rc=1 E4031 handle @(i64)"
# 4 — the filed repro (Z108 probe hstr2): a str err arm beside the handle
expect "4-str-err-arm" \
  "$(chk "let h=mt ${SPAWN_BAD} {\$ok:h h; \$err:e \"oops\"}; io.println(\"w=\\(process.wait(h))\");")" \
  "rc=1 E4011 handle str"
# 5 — Z108 probe armmix: an empty-array err arm
expect "5-array-err-arm" \
  "$(chk "let h=mt ${SPAWN_BAD} {\$ok:h h; \$err:e @()};")" \
  "rc=1 E4011 handle @(?)"
# 6 — a handle where a str is declared
expect "6-handle-into-str" \
  "$(chk "let h=${SPAWN_OK}; io.println(s.upper(h));")" \
  "rc=1 E4031 str handle"
# 7 — another opaque record: a str where vec.len declares vechandle
expect "7-str-into-vechandle" \
  "$(chk 'let n=vec.len("x");')" \
  "rc=1 E4031 vechandle str"
# 8 — control: the ooke idiom (`$err:e 0` filler), wait + stdout, is clean
expect "8-control-ooke-idiom-check" \
  "$(chk "let h=${SPAWN_OK}; let w=process.wait(h); let o=mt process.stdout(h) {\$ok:v v;\$err:e \"\"};")" \
  "rc=0   "
expect "9-control-ooke-idiom-runs" \
  "$(run "let h=${SPAWN_OK}; let w=process.wait(h); let o=mt process.stdout(h) {\$ok:v v;\$err:e \"\"}; io.println(\"w=\\(w) o=\\(o)\");")" \
  "w=0 o=hi  "
# 10 — control: process.badhandle() as the err arm; wait answers -1
expect "10-control-badhandle-runs" \
  "$(run "let b=mt ${SPAWN_BAD} {\$ok:v v;\$err:e process.badhandle()}; io.println(\"wb=\\(process.wait(b))\");")" \
  "wb=-1 "
# 11 — control: the literal 0 (the null handle) where a handle is declared
#      stays clean (136.36; any other integer is E4031 since 138.35, C1905)
expect "11-control-int-handle" \
  "$(chk 'let w=process.wait(0); let n=vec.len(0);')" \
  "rc=0   "
# 12 — control: the same opaque record is clean
expect "12-control-same-record" \
  "$(chk 'let v=vec.new(); let n=vec.len(v); let m=vec.len(vec.new());')" \
  "rc=0   "

echo "C1000: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
