#!/usr/bin/env bash
# C052_variant_ctor_return.sh — `<$variant(x)` whose sum type is neither the
# function's error type nor its return type is E4031; one declared by several
# sum types, none the error type, is E4072 (story 127.59, part b).
#
# THE DEFECT. The emitter resolves `$v(x)` to the enclosing function's error
# type if it declares `v`, else to the UNIQUE sum type declaring `v`, else it
# passes the payload through untagged. Measured at 39b6cfd, --check 0 and
# build 0 in every case:
#
#   t=$other{$neg:i64}; t=$myerr{$bad:i64};
#   f=g(x:i64):i64!$myerr{ <$neg(x) }   run: "ok 4315257600" — the $other
#                                        box's heap address as the ok value
#   two sums declare $neg; f=k():$other{ <$neg(x) }   run: rc=139
#
# The literal form `<$other{$neg:x}` already drew E4031; the call form was
# untyped.
#
# WHAT IS ASSERTED:
#   1. Foreign single owner, with and without an error type: E4031, no fix.
#   2. Several owners, none the error type: E4072, no fix.
#   3. Correct programs stay clean AND run right: an error return of the
#      error type's variant, an ok return of the declared sum return type,
#      several owners where one is the error type, a function named like the
#      variant, and an undeclared-by-any-sum name is left to E3011.
#
# Negative control: TKC built from 39b6cfd — cases 1 and 2 are clean (red).
#
# Story: 127.59

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
nofix() { "${TKC}" --check "$1" 2>&1 | grep '"severity":"error"' | grep -vc '"fix":""' || true; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
one='m=t;
i=io:std.io;
t=$other{$neg:i64;$zz:i64};
t=$myerr{$bad:i64};'
two="${one}
t=\$third{\$neg:str};"

# ── 1. a foreign single owner ────────────────────────────────────────────
printf '%s\nf=g(x:i64):i64!$myerr{ if(x<0){<$neg(x)}; <x*2 };\n' "$one" > "${WORK}/f1.tk"
expect "1-foreign-owner-err-fn-E4031" "$(codes "${WORK}/f1.tk")" "E4031 "
expect "1-no-fix" "$(nofix "${WORK}/f1.tk")" "0"
printf '%s\nf=g(x:i64):i64{ <$neg(x) };\n' "$one" > "${WORK}/f2.tk"
expect "1-foreign-owner-plain-fn-E4031" "$(codes "${WORK}/f2.tk")" "E4031 "
printf '%s\nf=g(x:i64):$myerr{ <$neg(x) };\n' "$one" > "${WORK}/f3.tk"
expect "1-foreign-owner-other-sum-E4031" "$(codes "${WORK}/f3.tk")" "E4031 "
printf '%s\nf=g(x:i64):i64!$myerr{ if(x>0){<x}el{<$neg(x)} };\n' "$one" > "${WORK}/f4.tk"
expect "1-foreign-owner-in-branch-E4031" "$(codes "${WORK}/f4.tk")" "E4031 "

# ── 2. several owners, none the error type ───────────────────────────────
printf '%s\nf=g(x:i64):i64!$myerr{ <$neg(x) };\n' "$two" > "${WORK}/a1.tk"
expect "2-ambiguous-err-fn-E4072" "$(codes "${WORK}/a1.tk")" "E4072 "
expect "2-no-fix" "$(nofix "${WORK}/a1.tk")" "0"
printf '%s\nf=k(x:i64):$other{ <$neg(x) };\n' "$two" > "${WORK}/a2.tk"
expect "2-ambiguous-even-if-return-type-owns-E4072" "$(codes "${WORK}/a2.tk")" "E4072 "

# ── 3. correct programs: clean, and they run right ───────────────────────
cat > "${WORK}/g1.tk" <<'EOF2'
m=t;
i=io:std.io;
t=$other{$neg:i64;$zz:i64};
t=$myerr{$bad:i64;$neg:i64};
f=g(x:i64):$other!$myerr{ if(x<0){<$bad(x)}; <$zz(x) };
f=h(x:i64):$other{ <$zz(x) };
f=e(x:i64):i64!$myerr{ <$neg(x) };
f=main():i64{
  mt g(5){$ok:v mt v {$neg:n io.println("neg \(n)");$zz:z io.println("zz \(z)")};$err:e io.println("err")};
  mt g(-1){$ok:v io.println("ok");$err:e io.println("err")};
  mt e(3){$ok:v io.println("ok \(v)");$err:x io.println("err")};
  mt h(7){$neg:n io.println("neg \(n)");$zz:z io.println("zz \(z)")};
  <0
};
EOF2
expect "3-correct-returns-clean" "$(codes "${WORK}/g1.tk")" ""
"${TKC}" "${WORK}/g1.tk" -o "${WORK}/g1" >/dev/null 2>&1
expect "3-correct-returns-run" "$("${WORK}/g1" 2>&1 | tr '\n' '|')" "zz 5|err|err|zz 7|"
printf '%s\nf=neg(x:i64):i64{<x};\nf=g(x:i64):i64!$myerr{ <$neg(x) };\n' "$one" > "${WORK}/g2.tk"
expect "3-function-named-like-variant-clean" "$(codes "${WORK}/g2.tk")" ""
printf '%s\nf=g(x:i64):i64!$myerr{ <$nosuch(x) };\n' "$one" > "${WORK}/g3.tk"
expect "3-undeclared-name-is-E3011-only" "$(codes "${WORK}/g3.tk")" "E3011 "

echo "C052: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
