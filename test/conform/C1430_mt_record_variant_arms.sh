#!/usr/bin/env bash
# C1430_mt_record_variant_arms.sh -- a match on a RECORD value whose arms
# name variants is E4080 at check time (story 127.367).
#
# `t=$s{a:i64;b:i64}; let m=mt $s{a:1;b:2} {$a:n n+10;$b:n n+20}` passed
# --check: the sum exhaustiveness check treats any struct's field names as
# its variants, so arms named after the fields covered "every variant".  The
# build then failed at each arm (E4031 from codegen).  A record has fields,
# not variants, so variant arms on it are now E4080, located at the first
# variant arm, with no `fix` (a sum may have been meant, or a field read).
# Arm names that are not the record's fields drew E4010 "missing variant(s)"
# with a `fix` to add arms -- a wrong repair for a record; they are E4080 too.
#
# Unchanged: a sum scrutinee (exhaustive, and non-exhaustive -> E4010), a
# result match over a call whose ok type is a record (`$ok`/`$err`, and a
# flat error sum's variants beside `$ok`), a record field read.
#
# Negative control: TKC built from integrate/z-wave-1113b (c0a8bee7) --
# cases 1-5 red (1, 4 rc=0; 2 is E4010 with a fix; 5 no diagnostic); 6-10 green on both.
#
# Story: 127.367, 127.559

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
H='m=a;i=io:std.io;t=$s{a:i64;b:i64};t=$u{$a:i64;$b:i64};t=$er{msg:str};t=$es{$x:i64;$y:i64};
f=g(n:i64):$s!$er{<$s{a:n;b:2}};f=h(n:i64):$s!$es{<$s{a:n;b:2}};'
# chk <body> -- "rc=<rc> <line>:<code>... fix=<0|1>"
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/c.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/c.tk" 2>&1)"; rc=$?
    local cs; cs="$(printf '%s\n' "$out" | grep -o '"error_code":"[A-Z0-9]*".*"line":[0-9]*' \
      | sed -E 's/"error_code":"([A-Z0-9]*)".*"line":([0-9]*)/\2:\1/' | tr '\n' ' ')"
    local fx=0; printf '%s' "$out" | grep -q '"fix"' && fx=1
    echo "rc=${rc} ${cs}fix=${fx}"
}
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/r.tk"
    tk_build "${TKC}" "${WORK}/r.tk" -o "${WORK}/r" >/dev/null 2>&1 || { tk_build_failed $?; return; }
    tk_run "${WORK}/r" 2>&1 | tr '\n' ' '
}
expect "1 record subject, arms named after its fields: E4080" \
  "$(chk 'let m=mt $s{a:1;b:2} {$a:n n+10;$b:n n+20};io.println("\(m)");')" "rc=1 4:E4080 fix=0"
expect "2 record subject, other arm names: E4080, not E4010 with a fix" \
  "$(chk 'let r=$s{a:1;b:2};let m=mt r {$c:n n+10;$d:n n+20};io.println("\(m)");')" "rc=1 4:E4080 fix=0"
expect "3 message names the record" \
  "$(printf '%s\n' "$("${TKC}" --check "${WORK}/c.tk" 2>&1)" | grep -c "'\$s' is a record, not a sum type")" "1"
expect "4 record subject, match as a statement: E4080" \
  "$(chk 'let r=$s{a:1;b:2};mt r {$a:n io.println("a");$b:n io.println("b")};')" "rc=1 4:E4080 fix=0"
expect "5 record parameter subject" \
  "$(printf '%s\nf=k(r:$s):i64{<mt r {$a:n n;$b:n n}};\nf=main():i64{<0};\n' "$H" > "${WORK}/p.tk"; "${TKC}" --check "${WORK}/p.tk" 2>&1 | grep -o '"error_code":"[A-Z0-9]*"' | tr '\n' ' ')" '"error_code":"E4080" '
expect "6 control: sum subject, exhaustive, runs" \
  "$(run 'let u=$u{$a:1};let m=mt u {$a:n n+10;$b:n n+20};io.println("\(m)");')" "11 "
expect "7 control: sum subject, non-exhaustive: E4010 as before" \
  "$(chk 'let u=$u{$a:1};let m=mt u {$a:n n+10};io.println("\(m)");')" "rc=1 4:E4010 fix=1"
expect "8 control: result match over a record-returning call runs" \
  "$(run 'let m=mt g(1) {$ok:v v.a;$err:e 0};io.println("\(m)");')" "1 "
expect "9 control: \$ok beside a flat error sum's variants runs" \
  "$(run 'let k=mt h(3) {$ok:v v.a;$x:e e;$y:e 0};io.println("\(k)");')" "3 "
expect "10 control: field read on a record checks clean" \
  "$(chk 'let r=$s{a:1;b:2};io.println("\(r.a)");')" "rc=0 fix=0"

echo "C1430: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
