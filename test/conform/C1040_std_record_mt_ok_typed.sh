#!/usr/bin/env bash
# C1040_std_record_mt_ok_typed.sh — an `mt` over a std call whose .tki return
# is an inline `record!err` with a fielded record (http.postheaders's
# `httpresp!httperr`, json.dec's `jsonval!jsonerr`, time.parseduration's
# `timeduration!timeerr`, ...) types its `$ok` binding as that record, so an
# arm of another type is E4011, a field it does not declare is E4025, and the
# bound value where a str is declared is E4031 (story 138.33).
#
# THE DEFECT. mt_ok_payload_type adopted no fielded struct for the inline
# `T!E` spelling (the 127.66 rule; 138.21 adopted only OPAQUE records), so
# `let r=mt http.postheaders(..) {$ok:x x; $err:e "oops"}` passed --check,
# as did `r.nosuch` and `let s:str=r`.  At run time `let j=mt json.dec("{bad")
# {$ok:v v; $err:e "oops"}; io.println(j.raw)` segfaulted (rc 139): the
# string was read as a jsonval.  The record is now adopted as it is for the
# "error"-key spelling; codegen already lowered the payload by the record's
# .tki layout, so the emitted IR is unchanged for programs that compiled.
# `$err:e 0` stays clean (E4011's null filler beside a record).
#
# Negative control: TKC built from fix/z109-opaque-handle-typing aa6f65e7 --
# cases 1-7 red (--check rc 0), 8-11 green on both.
#
# Story: 138.33

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
H='m=sr;i=io:std.io;i=http:std.http;i=json:std.json;i=time:std.time;'
# chk <body> — `--check` a main whose body is <body>; prints
# "rc=<n> <first error_code> <first message>".
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/sr.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/sr.tk" 2>&1)"; rc=$?
    local code msg
    code="$(printf '%s' "$out" | grep -o '"error_code":"E[^"]*"' | head -1 | cut -d'"' -f4)"
    msg="$(printf '%s' "$out" | grep '"error_code":"E' | grep -o '"message":"[^"]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${code} ${msg}"
}
# run <body> — build and run; output on one line.
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/sr.tk"
    "${TKC}" -o "${WORK}/b" "${WORK}/sr.tk" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '
}
POST='http.postheaders("http://x/";"b";@(@("a";"b")))'

# 1 — the filed repro (Z109 probe b1): a str err arm beside httpresp
expect "1-str-err-arm" \
  "$(chk "let r=mt ${POST} {\$ok:x x; \$err:e \"oops\"};")" \
  "rc=1 E4011 match arms have inconsistent types: 'httpresp' vs 'str'"
# 2 — Z109 probe b2: a non-zero int err arm is not the null filler
expect "2-int-err-arm" \
  "$(chk "let r=mt ${POST} {\$ok:x x; \$err:e 5};")" \
  "rc=1 E4011 match arms have inconsistent types: 'httpresp' vs 'i64'"
# 3 — the bound record where a str is declared
expect "3-record-into-str" \
  "$(chk "let r=mt ${POST} {\$ok:x x; \$err:e 0}; let s:str=r;")" \
  "rc=1 E4031 type mismatch: expected 'str', got 'httpresp'"
# 4 — a field httpresp does not declare, on the bound value
expect "4-unknown-field-bound" \
  "$(chk "let r=mt ${POST} {\$ok:x x; \$err:e 0}; let n=r.nosuch;")" \
  "rc=1 E4025 struct 'httpresp' has no field 'nosuch'"
# 5 — a field it does not declare, on the arm binding itself
expect "5-unknown-field-arm" \
  "$(chk "let n=mt ${POST} {\$ok:x x.nosuch; \$err:e 0};")" \
  "rc=1 E4025 struct 'httpresp' has no field 'nosuch'"
# 6 — json.dec: the str err arm that segfaulted at run time (rc 139)
expect "6-jsonval-str-arm" \
  "$(chk "let j=mt json.dec(\"{bad\") {\$ok:v v; \$err:e \"oops\"}; io.println(\"raw=\\(j.raw)\");")" \
  "rc=1 E4011 match arms have inconsistent types: 'jsonval' vs 'str'"
# 7 — another module: time.parseduration's timeduration
expect "7-timeduration-str-arm" \
  "$(chk "let d=mt time.parseduration(\"5s\") {\$ok:v v; \$err:e \"x\"};")" \
  "rc=1 E4011 match arms have inconsistent types: 'timeduration' vs 'str'"

# Controls (green on both sides).
# 8 — `$err:e 0` beside the record stays clean (E4011 null filler)
expect "8-zero-err-arm-clean" \
  "$(chk "let r=mt ${POST} {\$ok:x x; \$err:e 0};")" \
  "rc=0  "
# 9 — declared fields read cleanly, on the binding and the bound value
expect "9-declared-fields-clean" \
  "$(chk "let r=mt ${POST} {\$ok:x x; \$err:e 0}; let st=r.status; let b=mt ${POST} {\$ok:x x.body; \$err:e @()};")" \
  "rc=0  "
# 10 — the bound record passed where the .tki declares it
expect "10-record-as-std-arg-clean" \
  "$(chk "let j=mt json.dec(\"{}\") {\$ok:v v; \$err:e 0}; let a=json.get(j;\"a\");")" \
  "rc=0  "
# 11 — runtime: the typed payload's field reads the decoded value
expect "11-run-jsonval-raw" \
  "$(run "let j=mt json.dec(\"{\\\"a\\\":1}\") {\$ok:v v; \$err:e 0}; io.println(\"raw=\\(j.raw)\");")" \
  'raw={"a":1} '

echo "C1040: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
