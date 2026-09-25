#!/usr/bin/env bash
# C066_mt_bound_ok_payload.sh — `let x = mt r {$ok:v v; $err:e …}` carries the
# ok payload's type, so a field access on it is checked (story 127.180).
#
# THE DEFECT. An `mt`-bound local stayed TY_UNKNOWN, so after
#
#     let cfg=mt result {$ok:v v;$err:e 0};
#
# `cfg.nosuchfield` passed --check (toke-ooke's tests use this shape eight
# times) while the same field on a directly bound record was E4025, and
# codegen lowered the unknown field to slot 0.
#
# WHAT IS ASSERTED. Codegen types an `mt` by its first non-return arm, and a
# bare `$ok:v v` there is the scrutinee's ok payload (llvm.c NODE_MATCH_STMT).
# The checker now answers that one shape the same way:
#   1. user `T!E` call as the scrutinee, bound through a `let`   -> E4025
#   2. the call itself as the scrutinee                          -> E4025
#   3. an imported (.tki) `T!E` export, the ooke shape           -> E4025
#   4. a real field of the payload checks clean and runs
# and leaves every other shape unknown, as before (no new diagnostic):
#   5. the first arm is `$err`       (codegen types it by that arm)
#   6. the ok arm is not the bare binding
#
# Negative control: TKC built from d25ba32 — 1, 2 and 3 check clean (red).
#
# Story: 127.180

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
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }

HDR='m=t;
i=io:std.io;
t=$rec{port:i64};
f=mk(x:i64):$rec!$str{<$rec{port:x}};'

printf '%s\nf=main():i64{\n  let result=mk(1);\n  let c=mt result {$ok:v v;$err:e 0};\n  <c.nosuchfield\n};\n' "$HDR" > p1.tk
expect "1-let-bound-scrutinee-E4025" "$(errs p1.tk)" "E4025 "

printf '%s\nf=main():i64{\n  let c=mt mk(1) {$ok:v v;$err:e $rec{port:0}};\n  <c.nosuchfield\n};\n' "$HDR" > p2.tk
expect "2-call-scrutinee-E4025" "$(errs p2.tk)" "E4025 "

cat > amod.tk <<'EOF'
m=amod;
t=$cfgrec{name:$str;n:i64};
t=$lerr{$bad:i64};
f=load(x:i64):$cfgrec!$lerr{
  if(x<0){<$lerr{$bad:x}};
  <$cfgrec{name:"widget";n:x}
};
EOF
"${TKC}" --emit-interface --check amod.tk >/dev/null 2>&1
if [ ! -f amod.tki ]; then
    echo "FAIL 3-setup: --emit-interface produced no amod.tki"; FAIL=$((FAIL + 1))
else
    printf 'm=b;\ni=io:std.io;\ni=a:amod;\nf=main():i64{\n  let result=a.load(7);\n  let cfg=mt result {$ok:v v;$err:e 0};\n  io.println(cfg.name);\n  <cfg.nosuchfield\n};\n' > p3.tk
    expect "3-imported-export-E4025" "$(errs p3.tk)" "E4025 "
fi

printf '%s\nf=main():i64{\n  let result=mk(41);\n  let c=mt result {$ok:v v;$err:e 0};\n  io.println("\\(c.port+1)");\n  <0\n};\n' "$HDR" > p4.tk
expect "4-real-field-checks" "$(errs p4.tk)" ""
"${TKC}" p4.tk -o p4 >/dev/null 2>&1
expect "4-real-field-runs" "$(./p4 2>/dev/null)" "42"

printf '%s\nf=main():i64{\n  let c=mt mk(1) {$err:e 0;$ok:v v};\n  <c.nosuchfield\n};\n' "$HDR" > p5.tk
expect "5-err-arm-first-stays-unknown" "$(errs p5.tk)" ""

printf '%s\nf=main():i64{\n  let c=mt mk(1) {$ok:v v.port;$err:e 0};\n  <c\n};\n' "$HDR" > p6.tk
expect "6-non-bare-ok-arm-stays-unknown" "$(errs p6.tk)" ""

echo "C066: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
