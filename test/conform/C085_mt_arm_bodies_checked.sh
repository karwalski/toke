#!/usr/bin/env bash
# C085_mt_arm_bodies_checked.sh — `mt` arm BODIES are type-checked, with the
# arm binding in force, and E4011 compares arm body types (story 127.195).
#
# THE DEFECT. infer() on NODE_MATCH_STMT inferred arm->children[1] — the
# binding NAME — instead of children[2], the body.  No arm body was ever
# type-checked: `mt g(1) {$ok:v v.nosuch;$err:e 1+"a"}` checked clean, and
# E4011 compared the types of the binding identifiers.
#
# WHAT IS ASSERTED.
#   1-2  a bad body is diagnosed: E4031 (1+"a"), E4025 (unknown field on the
#        $ok payload of a T!E scrutinee — the binding carries the payload type,
#        shadowing a parameter of the same name);
#   3    value-position arms that disagree are E4011, with no fix;
#   4-7  what E4011 deliberately leaves alone, each of which must check clean
#        AND run: a match standing as a statement, an early-return arm, the
#        null filler `$err:e 0` beside a struct arm, an $err arm that is an
#        effect call, u64 beside i64.
#
# Negative control: TKC built from 84e1f4d (main before this batch) — cases 1,
# 2 and 3 check clean (3 of 10 red).
#
# Story: 127.195

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
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
hasfix() { "${TKC}" --check "$1" 2>&1 | awk '/"error_code":"E4011"/ && /"fix":/{f=1} END{print f?"fix":"nofix"}'; }

HDR='m=t;
i=io:std.io;
t=$pt{x:i64;y:i64};
t=$er{$bad:i64};
f=g(x:i64):$pt!$er{ if(x<0){<$er{$bad:x}}; <$pt{x:x;y:1} };
f=n(x:i64):i64!$er{ if(x<0){<$er{$bad:x}}; <x };'

# 1. an arithmetic type error inside an arm body
printf '%s\nf=main():i64{ let r=mt g(1) {$ok:v 1;$err:e 1+"a"}; <r };\n' "$HDR" > "${WORK}/p1.tk"
expect "1-arm-body-checked" "$(errs "${WORK}/p1.tk")" "E4031 "

# 2. the $ok binding is the payload struct, and shadows the parameter `v:str`
printf '%s\nf=h(v:$str):i64{ let r=mt g(1) {$ok:v v.nosuch;$err:e 0}; <0 };\nf=main():i64{<h("q")};\n' "$HDR" > "${WORK}/p2.tk"
expect "2-binding-typed-and-shadows" "$(errs "${WORK}/p2.tk")" "E4025 "

# 3. value-position arms of different types: E4011, no fix (errors.md)
printf '%s\nf=main():i64{ let r=mt g(1) {$ok:v v.x;$err:e "s"}; <0 };\n' "$HDR" > "${WORK}/p3.tk"
expect "3-e4011-value-position" "$(errs "${WORK}/p3.tk")" "E4011 "
expect "3-e4011-no-fix"         "$(hasfix "${WORK}/p3.tk")" "nofix"

# 4. a match standing as a statement: its arms need not agree
printf '%s\nf=main():i64{ mt g(1) {$ok:v io.println("ok \\(v.x)");$err:e 0}; <0 };\n' "$HDR" > "${WORK}/p4.tk"
expect "4-statement-match-checks" "$(errs "${WORK}/p4.tk")" ""
expect "4-statement-match-runs"   "$(runs "${WORK}/p4.tk")" "ok 1 "

# 5. an early-return arm yields no value; 6. the null filler beside a struct
printf '%s\nf=k(a:i64):i64{ let p=mt g(a) {$ok:v v;$err:e 0}; let q=mt n(a) {$ok:w w;$err:e <0-1}; <p.x+q };\nf=main():i64{ io.println("\\(k(4))"); <0 };\n' "$HDR" > "${WORK}/p5.tk"
expect "5-6-return-arm-and-null-filler-check" "$(errs "${WORK}/p5.tk")" ""
expect "5-6-return-arm-and-null-filler-run"   "$(runs "${WORK}/p5.tk")" "8 "

# 7. an $err arm that is an effect call, and u64 beside i64
printf '%s\nf=note(m:$str):$str{ io.println(m); <"" };\nf=main():i64{ let a=@(1;2;3); let s=mt g(1) {$ok:v "p\\(v.y)";$err:e note("failed")}; let l=mt n(2) {$ok:w a.len;$err:e 0}; io.println("\\(s) \\(l)"); <0 };\n' "$HDR" > "${WORK}/p7.tk"
expect "7-err-call-and-u64-check" "$(errs "${WORK}/p7.tk")" ""
expect "7-err-call-and-u64-run"   "$(runs "${WORK}/p7.tk")" "p1 3 "

echo "C085: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
