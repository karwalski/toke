#!/usr/bin/env bash
# C067_mut_assigned_fallible.sh — a binding first bound to a plain value and
# later ASSIGNED an error-channel call is matched on that call's channel
# (story 135.16a).
#
# THE DEFECT. `let r=mut.0; r=s.toint("0"); mt r` printed `err`: 135.16 mints
# a binding's error slot only at a fallible `let`, because a slot minted at an
# assignment inside a branch reads undef on the other path once 137.6 hoists
# its alloca to the entry block. So the assignment updated nothing and the
# `mt` fell back to the value sentinel, which calls an ok 0 a failure.
#
# THE FIX. When a later assignment to the name is an error-channel call, the
# slot is minted at the `let` holding 0 ("no error"). That store dominates
# every use, so a path that skips the assignment reads ok — which is what the
# plain value is — and the assignment overwrites it with its captured channel.
#
# WHAT IS ASSERTED (each program is built and run):
#   1. straight-line assignment of an ok 0              -> ok|0   (was err)
#   2. assignment in an if that is NOT taken             -> ok|7   (unchanged)
#   3. assignment in an if that IS taken, call fails     -> err    (unchanged)
#   4. assignment in an if that IS taken, ok 0           -> ok|0   (was err)
#   5. user T!$e function, ok 0, typed $err arm intact   -> ok|0 / bad|-1
#   6. a later plain assignment returns to the sentinel  -> ok|5
# Residual, deliberately unchanged: an `mt` emitted BEFORE the assignment
# (the top of a loop body reading last iteration's value) still uses the
# sentinel; 7 pins that it is no worse than before.
#
# Negative control: TKC built from d25ba32 — both programs red: 1, 4 and the
# ok 0 of 5 print `err`, and 5's `$err` arm binds an untyped payload, so the
# nested `mt e` takes `$worse` for a `$bad` (prints `worse`, not `bad|-1`).
#
# Story: 135.16a

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

cat > s.tk <<'EOF'
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let r=mut.0;
  r=s.toint("0");
  io.println(mt r {$ok:v "ok|\(v)";$err:e "err"});
  let q=mut.7;
  if(q>100){q=s.toint("zz")};
  io.println(mt q {$ok:v "ok|\(v)";$err:e "err"});
  let w=mut.7;
  if(w>1){w=s.toint("zz")};
  io.println(mt w {$ok:v "ok|\(v)";$err:e "err"});
  let z=mut.5;
  if(z>1){z=s.toint("0")};
  io.println(mt z {$ok:v "ok|\(v)";$err:e "err"});
  <0
};
EOF
if "${TKC}" s.tk -o s >/dev/null 2>&1; then
    out="$(./s 2>/dev/null | tr '\n' ' ')"
    expect "1-4-stdlib-channel" "${out}" "ok|0 ok|7 err ok|0 "
else
    echo "FAIL 1-4-build"; FAIL=$((FAIL + 1))
fi

cat > u.tk <<'EOF'
m=t;
i=io:std.io;
t=$e{$bad:i64;$worse:i64};
f=g(x:i64):i64!$e{
  if(x<0){<$e{$bad:x}};
  <x
};
f=main():i64{
  let r=mut.3;
  lp(let i=0;i<3;i=i+1){
    io.println(mt r {$ok:v "top ok|\(v)";$err:e "top err"});
    r=g(i-1);
  };
  r=g(0);
  io.println(mt r {$ok:v "ok|\(v)";$err:e "err"});
  r=g(-1);
  io.println(mt r {$ok:v "ok|\(v)";$err:e mt e {$bad:n "bad|\(n)";$worse:n "worse"}});
  r=5;
  io.println(mt r {$ok:v "ok|\(v)";$err:e "err"});
  <0
};
EOF
if "${TKC}" u.tk -o u >/dev/null 2>&1; then
    out="$(./u 2>/dev/null | tr '\n' ' ')"
    # 7: the loop-top reads are the sentinel, as before (an ok 0 there still
    #    reads `top err` on the third iteration — the documented residual).
    expect "5-7-user-fallible" "${out}" "top ok|3 top err top err ok|0 bad|-1 ok|5 "
else
    echo "FAIL 5-7-build"; FAIL=$((FAIL + 1))
fi

echo "C067: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
