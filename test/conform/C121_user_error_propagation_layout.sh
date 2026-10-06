#!/usr/bin/env bash
# C121_user_error_propagation_layout.sh — `!` from one user error type into a
# different one re-raises payload-less; it never hands the callee's box to a
# reader that expects another layout (story 127.210).
#
# THE DEFECT. `f=outer():i64!$eb{let v=inner()!$eb;..}` with `inner` declaring
# `i64!$ea` passed inner's error box through as-is.  A `$ea{$y:"boom"}` was
# then matched as `$eb`: variant 1 of `$eb` is `$q:i64`, so the str pointer
# printed as an integer (`q 4303792896`).  A record error read the other
# record's field at the same offset (`e.n` printed a pointer).  127.182 had
# closed exactly this for STDLIB payloads (stdlib_err_passes_through); user
# callees went round it.
#
# THE RULE (127.182's, llvm.c user_err_reraise): the payload passes through
# only into the same type, or a record whose fields have the same types in
# the same order; anything else is re-raised payload-less (127.56 — tag -1
# for a sum, which a nested `mt` sends to its last arm with payload 0; the
# error flag alone for a record, so reading a field traps RT005 instead of
# returning garbage).
#
# 127.225 (owner decision 2026-10-01 §2.3): cases 1 and 4 -- a re-raise into
# a different sum, or into a record of another layout -- are now the
# check-time error E4076 (C719 covers the rule), so they assert that code
# instead of the payload-less run-time behaviour.  Case 5's ok-path guard
# uses a same-type re-raise, which still compiles.
#
# Negative control: TKC built from main at 8f4a843 — case 1 prints
# "q <pointer>", case 4 prints "rc <pointer>" and does not trap: 3 of 6 red.
#
# Story: 127.210

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
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
codes() { "${TKC}" --check "$1" 2>&1 | grep -o '"error_code":"[A-Z0-9]*"' | sed 's/.*:"//;s/"//' | tr '\n' ' '; }

# 1. sum -> a different sum: re-raised, not read as the other sum's variant
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
t=$ea{$x:i64;$y:str};
t=$eb{$p:str;$q:i64;$r:i64};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};
f=outer(n:i64):i64!$eb{let v=inner(n)!$eb;<v+1};
f=main():i64{
  mt outer(5){$ok:v io.println("ok \(v)");$err:e mt e{$p:s io.println("p \(s)");$q:k io.println("q \(k)");$r:k io.println("r \(k)")}};
  <0
};
EOF
expect "1-sum-to-other-sum-is-E4076" "$(codes "${WORK}/p1.tk")" "E4076 "

# 2. sum -> the same sum: the payload passes through
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
t=$ea{$x:i64;$y:str};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};
f=same(n:i64):i64!$ea{let v=inner(n)!$ea;<v+1};
f=main():i64{
  mt same(5){$ok:v io.println("ok \(v)");$err:e mt e{$x:k io.println("x \(k)");$y:s io.println("y \(s)")}};
  <0
};
EOF
expect "2-same-sum-passes" "$(runs "${WORK}/p2.tk")" "y boom "

# 3. record -> a record of the same field types: passes through
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
t=$ra{msg:str;code:i64};
t=$rb{text:str;n:i64};
f=ri(n:i64):i64!$ra{if(n>0){<$ra{msg:"rm";code:7}};<n};
f=rsame(n:i64):i64!$rb{let v=ri(n)!$rb;<v+1};
f=main():i64{mt rsame(5){$ok:v io.println("ok \(v)");$err:e io.println("rb \(e.text) \(e.n)")};<0};
EOF
expect "3-same-layout-record-passes" "$(runs "${WORK}/p3.tk")" "rb rm 7 "

# 4. record -> a record with the fields in another order: re-raised, and the
#    field read traps rather than returning the other record's bytes
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
t=$ra{msg:str;code:i64};
t=$rc{n:i64;text:str};
f=ri(n:i64):i64!$ra{if(n>0){<$ra{msg:"rm";code:7}};<n};
f=rdiff(n:i64):i64!$rc{let v=ri(n)!$rc;<v+1};
f=main():i64{mt rdiff(5){$ok:v io.println("ok \(v)");$err:e io.println("rc \(e.n)")};<0};
EOF
expect "4-other-layout-record-is-E4076" "$(codes "${WORK}/p4.tk")" "E4076 "

# 5. positive guard: the ok path is untouched
cat > "${WORK}/p5.tk" <<'EOF'
m=t;i=io:std.io;
t=$ea{$x:i64;$y:str};
f=inner(n:i64):i64!$ea{if(n>0){<$ea{$y:"boom"}};<n};
f=outer(n:i64):i64!$ea{let v=inner(n)!$ea;<v+1};
f=main():i64{mt outer(0){$ok:v io.println("ok \(v)");$err:e io.println("err")};<0};
EOF
expect "5-ok-path-unchanged" "$(runs "${WORK}/p5.tk")" "ok 1 "

echo "C121: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
