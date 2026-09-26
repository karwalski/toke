#!/usr/bin/env bash
# C087_result_match_on_scalar.sh — a `$ok`/`$err` match over a value KNOWN to
# be a non-union scalar is E4031 (story 127.202).
#
# THE DEFECT. Codegen lowers a result match as `value != 0 ? $ok : $err`.  Over
# a plain integer that is a zero test: `mt process.wait(h) {$ok:v v;$err:e 1}`
# — process.wait returns an i32 exit status — takes $err exactly when the
# child succeeded (printed 1 for an exit of 0), which broke `ooke compile`.
#
# WHAT IS ASSERTED.
#   1  a stdlib call whose interface states a scalar return (process.wait,
#      i32): E4031, no fix;
#   2  a user function returning i64, and a let bound to it: E4031 each;
#   3  a real T!E scrutinee checks clean and runs;
#   4  NOT judged, and must check clean and run: a bool (the zero test is the
#      truth test — `mt file.exists(p) {$ok:v v;$err:e false}`), and a str
#      (the zero test is the null test fallible pointer returns use).
#
# Negative control: TKC built from 84e1f4d — cases 1 and 2 check clean
# (2 of 7 red).  (The bool prints 0: the match's result slot is i64.)
#
# Story: 127.202

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
runs() { local f="$1"; shift; "${TKC}" "$f" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" "$@" 2>/dev/null | tr '\n' ' '; }
fixes() { "${TKC}" --check "$1" 2>&1 | awk '/"error_code":"E4031"/ && /"fix":/{f=1} END{print f?"fix":"nofix"}'; }

cat > "${WORK}/p1.tk" <<'EOF'
m=t;
i=io:std.io;
i=process:std.process;
f=main():i64{
  let h=process.spawn(@("true"));
  let c=mt h {$ok:v mt process.wait(v) {$ok:x x;$err:e 1};$err:e 2};
  io.println("\(c)");
  <0
};
EOF
expect "1-process-wait-errors" "$(errs "${WORK}/p1.tk")" "E4031 "
expect "1-process-wait-no-fix" "$(fixes "${WORK}/p1.tk")" "nofix"

cat > "${WORK}/p2.tk" <<'EOF'
m=t;
i=io:std.io;
f=cnt(x:i64):i64{<x};
f=main():i64{
  let a=mt cnt(0) {$ok:v v;$err:e 9};
  let n=cnt(0);
  let b=mt n {$ok:v v;$err:e 9};
  <a+b
};
EOF
expect "2-user-scalar-and-binding" "$(errs "${WORK}/p2.tk")" "E4031 E4031 "

cat > "${WORK}/p3.tk" <<'EOF'
m=t;
i=io:std.io;
t=$er{$bad:i64};
f=cnt(x:i64):i64!$er{ if(x<0){<$er{$bad:x}}; <x };
f=main():i64{
  let a=mt cnt(0) {$ok:v v;$err:e 9};
  let b=mt cnt(0-1) {$ok:v v;$err:e 9};
  io.println("\(a) \(b)");
  <0
};
EOF
expect "3-error-union-checks" "$(errs "${WORK}/p3.tk")" ""
expect "3-error-union-runs"   "$(runs "${WORK}/p3.tk")" "0 9 "

cat > "${WORK}/p4.tk" <<'EOF'
m=t;
i=io:std.io;
i=file:std.file;
f=nm(x:i64):$str{<"n"};
f=main():i64{
  let e=mt file.exists("/nonexistent/aa") {$ok:v v;$err:e false};
  let s=mt nm(1) {$ok:v v;$err:e "z"};
  io.println("\(e) \(s)");
  <0
};
EOF
expect "4-bool-and-str-not-judged-check" "$(errs "${WORK}/p4.tk")" ""
expect "4-bool-and-str-not-judged-run"   "$(runs "${WORK}/p4.tk" --allow-read)" "0 n "

echo "C087: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
