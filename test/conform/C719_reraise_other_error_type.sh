#!/usr/bin/env bash
# C719_reraise_other_error_type.sh — `!` may not re-raise a user error into a
# different error type: E4076 at check time, RT010 as the run-time backstop
# (story 127.225, owner decision 2026-10-01 §2.3).
#
# THE DEFECT.  `let v=inner(n)!$eb` inside `f=outer(n:i64):i64!$eb{..}`, with
# `inner` declaring `i64!$ea`, compiled clean.  127.210 had made codegen
# re-raise the error with no payload (tag -1), and a nested
# `mt e{$p..;$q..;$r..}` then took its LAST arm with payload 0: the caller
# was told `r 0`, a variant and value nobody raised.
#
# THE RULE (codegen's pass-through rule, llvm.c stdlib_err_passes_through):
# the error passes through `!` into the same type, or into a record whose
# fields have the same types in the same order.  Any other pair of named
# sum / record types is E4076, for a local callee, a closure body, a tail
# `<f()!`, and `alias.f` from a user module's interface.  Where the checker
# cannot judge the pair (here: a record field typed `$none`, which it does not
# resolve) and the layouts still differ, the re-raise traps RT010 naming both
# types instead of inventing a variant.
#
# CASES.
#   1. sum -> different sum (local callee): exactly one E4076, stage
#      type_check, expected "$eb", got "$ea", no `fix` field.
#   2. record -> record with another layout: E4076.
#   3. sum -> record, and 4. record -> sum: E4076.
#   5. inside a closure body: E4076.
#   6. tail position `<inner(n)!$eb`: E4076.
#   7. `l.inner(n)!$eb` across a user-module boundary (-I, .tki): E4076.
#   8. backstop: a pair the checker cannot judge compiles, and the error path
#      traps "RT010: error of type $ra ... $rb" with exit 1; the ok path of
#      the same program runs normally.
#   Negative controls (must still compile and run as before):
#   9.  same sum re-raised: the payload passes ("y boom").
#   10. same-layout records: the payload passes ("rb rm 7").
#   11. explicit mapping with `mt`: compiles, prints the mapped variant.
#   12. a std.* callee (`str.toint(s)!$err`) into a user sum: compiles, the
#       failure still reaches the caller's $err arm (127.56/C001 behaviour).
#
# Red on main (996c367c): cases 1-8 compile clean (case 1 prints "r 0"; case
# 8's error path prints "err" and exits 0), so 10 of 19 assertions fail.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc not found at ${TKC}"; exit 1; fi
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
codes() { (cd "${WORK}" && "${TKC}" --check -I . "$1" 2>&1) | grep -o '"error_code":"[A-Z0-9]*"' | sed 's/.*:"//;s/"//' | tr '\n' ' '; }
runs()  { (cd "${WORK}" && "${TKC}" -I . "$@" -o b >/dev/null 2>&1 && ./b 2>/dev/null) | tr '\n' ' '; }

HDR='m=t;i=io:std.io;
t=$ea{$x:i64;$y:str};
t=$eb{$p:str;$q:i64;$r:i64};
t=$ra{msg:str;code:i64};
t=$rb{text:str;n:i64};
t=$rc{n:i64;text:str};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};
f=ri(n:i64):i64!$ra{if(n>0){<$ra{msg:"rm";code:7}};<n};'
MAIN_SUM='f=main():i64{
  mt outer(5){$ok:v io.println("ok \(v)");$err:e mt e{$p:s io.println("p \(s)");$q:k io.println("q \(k)");$r:k io.println("r \(k)")}};
  <0
};'
MAIN_ANY='f=main():i64{mt outer(5){$ok:v io.println("ok \(v)");$err:e io.println("err")};<0};'

# ── 1. sum -> different sum ───────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$eb{let v=inner(n)!$eb;<v+1};\n%s\n' "${HDR}" "${MAIN_SUM}" > "${WORK}/p1.tk"
expect "1-sum-to-other-sum" "$(codes p1.tk)" "E4076 "
rec="$(cd "${WORK}" && "${TKC}" --check p1.tk 2>&1 | grep E4076)"
fields=""
case "${rec}" in *'"stage":"type_check"'*) fields="${fields}stage ";; esac
case "${rec}" in *'"expected":"$eb"'*) fields="${fields}expected ";; esac
case "${rec}" in *'"got":"$ea"'*) fields="${fields}got ";; esac
case "${rec}" in *'"fix"'*) fields="${fields}HAS-FIX ";; esac
expect "1-record-fields-no-fix" "${fields}" "stage expected got "

# ── 2. record -> record of another layout ─────────────────────────────
printf '%s\nf=outer(n:i64):i64!$rc{let v=ri(n)!$rc;<v+1};\n%s\n' "${HDR}" "${MAIN_ANY}" > "${WORK}/p2.tk"
expect "2-record-other-layout" "$(codes p2.tk)" "E4076 "

# ── 3. sum -> record ──────────────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ra{let v=inner(n)!$ra;<v+1};\n%s\n' "${HDR}" "${MAIN_ANY}" > "${WORK}/p3.tk"
expect "3-sum-to-record" "$(codes p3.tk)" "E4076 "

# ── 4. record -> sum ──────────────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$ea{let v=ri(n)!$ea;<v+1};\n%s\n' "${HDR}" "${MAIN_ANY}" > "${WORK}/p4.tk"
expect "4-record-to-sum" "$(codes p4.tk)" "E4076 "

# ── 5. inside a closure body ──────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64{let h=fn(k:i64):i64!$eb{let v=inner(k)!$eb;<v+1};<0};\nf=main():i64{<outer(1)};\n' "${HDR}" > "${WORK}/p5.tk"
expect "5-closure-body" "$(codes p5.tk)" "E4076 "

# ── 6. tail position ──────────────────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$eb{<inner(n)!$eb};\n%s\n' "${HDR}" "${MAIN_SUM}" > "${WORK}/p6.tk"
expect "6-tail-position" "$(codes p6.tk)" "E4076 "

# ── 7. across a user-module boundary ──────────────────────────────────
cat > "${WORK}/lib.tk" <<'EOF'
m=lib;
t=$ea{$x:i64;$y:str};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};
EOF
(cd "${WORK}" && "${TKC}" --emit-interface --check lib.tk >/dev/null 2>&1)
cat > "${WORK}/p7.tk" <<'EOF'
m=main;i=io:std.io;i=l:lib;
t=$eb{$p:str;$q:i64;$r:i64};
f=outer(n:i64):i64!$eb{let v=l.inner(n)!$eb;<v+1};
f=main():i64{mt outer(5){$ok:v io.println("ok \(v)");$err:e io.println("err")};<0};
EOF
expect "7-cross-module" "$(codes p7.tk)" "E4076 "

# ── 8. run-time backstop RT010 ────────────────────────────────────────
cat > "${WORK}/p8.tk" <<'EOF'
m=t;i=io:std.io;
t=$ra{a:$none};
t=$rb{a:i64};
f=inner(n:i64):i64!$ra{if(n>0){<$ra{a:7}};<n};
f=outer(n:i64):i64!$rb{let v=inner(n)!$rb;<v+1};
f=main():i64{
  mt outer(0){$ok:v io.println("ok \(v)");$err:e io.println("err")};
  mt outer(5){$ok:v io.println("ok \(v)");$err:e io.println("err")};
  <0
};
EOF
expect "8-backstop-checks-clean" "$(codes p8.tk)" ""
(cd "${WORK}" && "${TKC}" p8.tk -o b8 >/dev/null 2>&1 && ./b8 >out8 2>err8); rc8=$?
expect "8-backstop-ok-path-runs" "$(tr '\n' '|' < "${WORK}/out8" 2>/dev/null)" "ok 1|"
expect "8-backstop-traps-RT010" "$(cat "${WORK}/err8" 2>/dev/null) rc=${rc8}" \
    "RT010: error of type \$ra re-raised with '!' into a function whose error type is \$rb rc=1"

# ── 9. control: same sum ──────────────────────────────────────────────
cat > "${WORK}/p9.tk" <<'EOF'
m=t;i=io:std.io;
t=$ea{$x:i64;$y:str};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};
f=same(n:i64):i64!$ea{let v=inner(n)!$ea;<v+1};
f=main():i64{
  mt same(5){$ok:v io.println("ok \(v)");$err:e mt e{$x:k io.println("x \(k)");$y:s io.println("y \(s)")}};
  <0
};
EOF
expect "9-same-sum-checks" "$(codes p9.tk)" ""
expect "9-same-sum-passes" "$(runs p9.tk)" "y boom "

# ── 10. control: same-layout records ──────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$rb{let v=ri(n)!$rb;<v+1};\nf=main():i64{mt outer(5){$ok:v io.println("ok \\(v)");$err:e io.println("rb \\(e.text) \\(e.n)")};<0};\n' "${HDR}" > "${WORK}/p10.tk"
expect "10-same-layout-checks" "$(codes p10.tk)" ""
expect "10-same-layout-passes" "$(runs p10.tk)" "rb rm 7 "

# ── 11. control: explicit mapping ─────────────────────────────────────
printf '%s\nf=outer(n:i64):i64!$eb{<mt inner(n){$ok:v v+1;$err:e <$eb{$q:9}}};\n%s\n' "${HDR}" "${MAIN_SUM}" > "${WORK}/p11.tk"
expect "11-explicit-mapping-checks" "$(codes p11.tk)" ""
expect "11-explicit-mapping-runs" "$(runs p11.tk)" "q 9 "

# ── 12. control: std.* callee into a user sum ─────────────────────────
cat > "${WORK}/p12.tk" <<'EOF'
m=t;i=io:std.io;i=str:std.str;
t=$err{$bad:$str};
f=num(s:$str):i64!$err{let v=str.toint(s)!$err;<v};
f=main():i64{
  let c=mt num("zz"){$ok:v v;$err:e 200};
  let d=mt num("42"){$ok:v v;$err:e 200};
  io.println("badint=\(c) goodint=\(d)");
  <0
};
EOF
expect "12-std-callee-checks" "$(codes p12.tk)" ""
expect "12-std-callee-runs" "$(runs p12.tk)" "badint=200 goodint=42 "

echo ""
echo "C719: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
