#!/usr/bin/env bash
# C084_let_bound_error_keeps_its_payload.sh — a let-bound error keeps its OWN
# payload after a later call fails (story 127.103).
#
# THE DEFECT.  135.16 spilled @tk_current_error into a per-binding slot at the
# `let`, which fixed WHICH ARM a later `mt` takes.  But the slot holds a
# pointer, and since 127.109 every raise on a thread reuses ONE error box.  So
# the spill captured a pointer into a box the next raise rewrote:
#
#     let r3=g(0);  let z=g(-1);  mt r3{…$err:e …}    -> big 1099  (want small 77)
#
# Measured at 2e932d6 for a sum error (wrong variant AND wrong payload), a
# record error (both bindings read the second record), and a std.toml
# {msg} error (reads the later message).
#
# THE FIX.  At the capture the box is copied into a buffer the binding owns
# (tk_err_keep), and the $err arm copies it back into the thread box
# (tk_err_rebox), so `e` has the direct spelling's lifetime and never points
# into a frame.  runtime-abi.md §7.5 restriction 1.
#
# ALSO ASSERTED: a re-assigned binding answers for its NEW call; a binding
# inside a loop answers for its own iteration; the direct spelling is
# unchanged; and the error-box allocation count stays flat (C019's property):
# the copy lives in the frame, not on the heap.
#
# Story: 127.103

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c084_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C084: a let-bound error keeps its own payload"
echo "--------------------------------------"

cat > p.tk <<'EOF'
m=p;
i=io:std.io;
i=str:std.str;
i=toml:std.toml;
t=$e2{$small:i64;$big:i64};
t=$rec{code:i64;msg:$str};
f=g(x:i64):i64!$e2{
  if(x==0){<$e2{$small:77}};
  if(x<0){<$e2{$big:1099}};
  <x
};
f=h(x:i64):i64!$rec{
  if(x==0){<$rec{code:5;msg:"first"}};
  if(x<0){<$rec{code:9;msg:"second"}};
  <x
};
f=sumtxt(e:$e2):$str{
  <mt e{$small:n str.concat("small ";str.fromi64(n));$big:n str.concat("big ";str.fromi64(n))}
};
f=main():i64{
  let r3=g(0);
  let z=g(-1);
  mt r3{$ok:v io.println("sum let: ok");$err:e io.println(str.concat("sum let: ";sumtxt(e)))};
  mt z{$ok:v io.println("sum later: ok");$err:e io.println(str.concat("sum later: ";sumtxt(e)))};
  let a=h(0);
  let b=h(-1);
  mt a{$ok:v io.println("rec let: ok");$err:e io.println(str.concat("rec let: ";str.concat(str.fromi64(e.code);e.msg)))};
  mt b{$ok:v io.println("rec later: ok");$err:e io.println(str.concat("rec later: ";str.concat(str.fromi64(e.code);e.msg)))};
  let t1=toml.load("x = = 1");
  let tv=mt toml.load("a = 1"){$ok:v v;$err:e 0};
  let t2=toml.i64(tv;"nope");
  mt t1{$ok:v io.println("toml let: ok");$err:e io.println(str.concat("toml let: ";e.msg))};
  let m=mut.h(0);
  m=h(-1);
  let q=h(0);
  mt m{$ok:v io.println("reassigned: ok");$err:e io.println(str.concat("reassigned: ";e.msg))};
  mt g(0){$ok:v io.println("direct: ok");$err:e io.println(str.concat("direct: ";sumtxt(e)))};
  let acc=mut.0;
  lp(let i=0;i<50;i=i+1){
    let r=h(0-i%2);
    let clob=h(-1);
    let c=mt r{$ok:v 0;$err:e e.code};
    acc=acc+c
  };
  io.println(str.concat("loop: ";str.fromi64(acc)));
  <0
};
EOF

# loop: i even -> r=h(0) code 5, i odd -> r=h(-1) code 9; 25*5 + 25*9 = 350.
EXPECTED='sum let: small 77
sum later: big 1099
rec let: 5first
rec later: 9second
toml let: line 1: syntax error
reassigned: second
direct: small 77
loop: 350'

if ! "${TKC}" --allow-all --out p_bin p.tk >compile.log 2>&1; then
    echo "  FAIL: program did not compile"
    grep -o '"error_code":"[^"]*","severity":"[^"]*","stage":"[^"]*","message":"[^"]*"' compile.log | head -2 | sed 's/^/      /'
    FAIL=$((FAIL + 1))
else
    out="$(./p_bin 2>&1)"; rc=$?
    while IFS= read -r want; do
        key="${want%%: *}: "
        if printf '%s\n' "${out}" | grep -qxF "${want}"; then
            echo "  PASS: ${want}"
            PASS=$((PASS + 1))
        else
            got="$(printf '%s\n' "${out}" | awk -v p="${key}" 'index($0,p)==1' | head -1)"
            echo "  FAIL: expected [${want}], got [${got}]"
            FAIL=$((FAIL + 1))
        fi
    done <<< "${EXPECTED}"
    [ "${rc}" -ne 0 ] && { echo "  FAIL: program exited ${rc}"; FAIL=$((FAIL + 1)); }

    # The copy is a frame buffer, not a heap allocation: the emitted IR keeps
    # the box with tk_err_keep into an alloca and calls no malloc for it.
    if "${TKC}" --allow-all --emit-llvm p.tk >p.ll 2>/dev/null && [ -s p.ll ]; then
        if grep -q 'call i64 @tk_err_keep(' p.ll && grep -q '\.tkerr\.keep = alloca \[' p.ll; then
            echo "  PASS: the capture copies the box into a frame buffer (tk_err_keep into an alloca)"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: no tk_err_keep into a frame buffer in the emitted IR"
            FAIL=$((FAIL + 1))
        fi
    else
        echo "  FAIL: --emit-llvm produced no IR"
        FAIL=$((FAIL + 1))
    fi
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
