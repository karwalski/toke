#!/usr/bin/env bash
# C051_tail_propagate_is_not_a_return.sh — a function body whose tail is a
# `!` propagation with no `<` is E4071 even when the propagated call's type
# does not resolve (story 127.127).
#
# THE DEFECT. ooke's store.tk ends two lookups in
#
#     (file.listall("")!$storeerr)
#
# as a way to "return an error". On the success path the function falls off
# its end and the caller receives the null filler as a value. At 39b6cfd:
# --check 0, build 0, and the caller's first `v.len` on the ok path ran
# rc=139. E4071 (127.113) already fires on a tail propagation whose type
# resolves (C021); for an unresolved one the fallback in check_tail_expr only
# admitted pure value expressions, and a propagation is not one.
#
# WHAT IS ASSERTED:
#   1. Unresolved tail propagation (a stdlib call) is E4071 with an empty fix
#      — `<` is not a certain repair: the author may have meant the error.
#   2. The resolved shape (a user fn) is still E4071 (C021's case).
#   3. Correct programs stay clean: `<x!$e` at the tail, a propagation
#      statement followed by `<`, a void function ending in a propagation,
#      and a non-void function whose tail propagation is of a void callee
#      (a missing return — out of this code's scope, as a void tail call is).
#   4. The fixed program runs and prints on its ok path.
#
# Negative control: TKC built from 39b6cfd — case 1 has no E4071 (red).
#
# Story: 127.127

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# errors only: `void` draws the lexer's W1020 style warning, not our business
codes() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

HDR='m=t;
i=io:std.io;
i=file:std.file;
t=$storeerr{$io:str};'

# ── 1. unresolved tail propagation ───────────────────────────────────────
printf '%s\nf=find(p:str):@(str)!$storeerr{\n  (file.listall(p)!$storeerr)\n};\n' "$HDR" > "${WORK}/u1.tk"
expect "1-paren-stdlib-propagate-is-E4071" "$(codes "${WORK}/u1.tk")" "E4071 "
expect "1-fix-field-empty" \
    "$("${TKC}" --check "${WORK}/u1.tk" 2>&1 | grep '"error_code"' | grep -vc '"fix":' || true)" "1"
printf '%s\nf=find(p:str):@(str)!$storeerr{\n  file.listall(p)!$storeerr\n};\n' "$HDR" > "${WORK}/u2.tk"
expect "1-bare-stdlib-propagate-is-E4071" "$(codes "${WORK}/u2.tk")" "E4071 "

# ── 2. resolved tail propagation (C021's shape) ──────────────────────────
cat > "${WORK}/r1.tk" <<'EOF2'
m=t;
t=$e{$bad:i64};
f=g(x:i64):i64!$e{ if(x<0){<$e{$bad:x}}; <x*2 };
f=h(x:i64):i64!$e{
  (g(x)!$e)
};
EOF2
expect "2-user-fn-propagate-is-E4071" "$(codes "${WORK}/r1.tk")" "E4071 "

# ── 3. correct programs stay clean ───────────────────────────────────────
printf '%s\nf=find(p:str):@(str)!$storeerr{\n  <file.listall(p)!$storeerr\n};\n' "$HDR" > "${WORK}/g1.tk"
expect "3-returned-propagate-clean" "$(codes "${WORK}/g1.tk")" ""
printf '%s\nf=count(p:str):i64!$storeerr{\n  let v=(file.listall(p)!$storeerr);\n  <v.len as i64\n};\n' "$HDR" > "${WORK}/g2.tk"
expect "3-propagate-then-return-clean" "$(codes "${WORK}/g2.tk")" ""
cat > "${WORK}/g3.tk" <<'EOF2'
m=t;
t=$e{$bad:i64};
f=chk(x:i64):void!$e{ if(x<0){<$e{$bad:x}} };
f=run(x:i64):void!$e{
  chk(x)!$e
};
f=run2(x:i64):i64!$e{
  chk(x)!$e
};
EOF2
expect "3-void-propagate-tail-clean" "$(codes "${WORK}/g3.tk")" ""

# ── 4. the corrected program runs ────────────────────────────────────────
printf '%s\nf=find(p:str):@(str)!$storeerr{\n  <file.listall(p)!$storeerr\n};\nf=main():i64{\n  mt find(".") {$ok:v io.println("OK");$err:e io.println("ERR")};\n  <0\n};\n' "$HDR" > "${WORK}/run.tk"
"${TKC}" "${WORK}/run.tk" -o "${WORK}/run" >/dev/null 2>&1
expect "4-fixed-program-runs" "$(cd "${WORK}" && ./run --allow-read 2>&1)" "OK"

echo "C051: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
