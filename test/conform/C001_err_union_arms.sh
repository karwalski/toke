#!/usr/bin/env bash
# C001_err_union_arms.sh — runtime conformance for user-declared `T!$err`
# error unions (story 127.56).
#
# A `T!$err` function signals failure to its caller through the thread-local
# @tk_current_error flag (114.55).  Every error exit must set it and every ok
# exit must clear it, or the caller's `mt` silently takes the wrong arm — a
# failure read as a success.  These cases compile and RUN real programs, at
# every optimisation level, and assert the observed arm:
#
#   1. `<$E{$variant:payload}`      — the documented error-literal return
#   2. `<$variant(payload)`         — the variant-constructor return form
#   3. ok return                    — the $ok arm, including an ok value of 0
#   4. bool-valued union            — `{$ok:v true;$err:e false}` must be
#                                     false on failure (the inverted guard)
#   5. `expr!$E` propagation        — of a user union AND of a 0/null-sentinel
#                                     stdlib union (json.dec / str.toint)
#   6. nested `mt e {$variants}`    — the $err payload keeps its tag
#
# Story: 127.56

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${REPO_ROOT}/tkc"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_errunion_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C001: user-declared T!\$err unions select the right mt arm"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
#   Compiles SOURCE at -O0/-O1/-O2/-O3 and asserts stdout at each level.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "case.tk"
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f "case_bin"
        if ! "${TKC}" --allow-all "${o}" --out "case_bin" "case.tk" >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log
            FAIL=$((FAIL + 1))
            continue
        fi
        out="$(./case_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected '${expected}', got '${out}' (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── 1+2+3: both arms, literal and constructor error-return forms ────────────
run_case "both arms (literal/ctor/ok)" "err=200 ctor=200 ok=100 okzero=100" '
m=t;
i=io:std.io;
t=$err{$bad:$str};
f=lit(n:i64):i64!$err{if(n<0){<$err{$bad:"neg"}};<n*2};
f=ctor(n:i64):i64!$err{if(n<0){<$bad("neg")};<n*2};
f=zero(n:i64):i64!$err{if(n<0){<$err{$bad:"neg"}};<0};
f=main():i64{
  let a=mt lit(0-5){$ok:v 100;$err:e 200};
  let b=mt ctor(0-5){$ok:v 100;$err:e 200};
  let c=mt lit(5){$ok:v 100;$err:e 200};
  let d=mt zero(5){$ok:v 100;$err:e 200};
  io.println("err=\(a) ctor=\(b) ok=\(c) okzero=\(d)");
  <0
};'

# ── 1b: the SAME cases in struct style (a plain record error type) ─────────
#   Struct-style unions already worked; pinned here so the variant-style fix
#   cannot regress them.
run_case "struct-style both arms" "err=200 ok=100 okzero=100 gfail=0 gok=1" '
m=t;
i=io:std.io;
t=$myerr{msg:$str};
f=lit(n:i64):i64!$myerr{if(n<0){<$myerr{msg:"neg"}};<n*2};
f=zero(n:i64):i64!$myerr{if(n<0){<$myerr{msg:"neg"}};<0};
f=guard(n:i64):bool{<mt lit(n){$ok:v true;$err:e false}};
f=main():i64{
  let a=mt lit(0-5){$ok:v 100;$err:e 200};
  let b=mt lit(5){$ok:v 100;$err:e 200};
  let c=mt zero(5){$ok:v 100;$err:e 200};
  io.println("err=\(a) ok=\(b) okzero=\(c) gfail=\(guard(0-5)) gok=\(guard(5))");
  <0
};'

# ── 4: bool-valued union — the inverted-guard case ──────────────────────────
run_case "bool union guard" "onfail=0 onok=1" '
m=t;
i=io:std.io;
t=$err{$bad:$str};
f=chk(n:i64):i64!$err{if(n<0){<$err{$bad:"neg"}};<n};
f=guard(n:i64):bool{<mt chk(n){$ok:v true;$err:e false}};
f=main():i64{
  io.println("onfail=\(guard(0-5)) onok=\(guard(5))");
  <0
};'

# ── 5a: `!` propagation of a user T!$err callee ─────────────────────────────
run_case "propagate user union" "fail=200 ok=11" '
m=t;
i=io:std.io;
t=$err{$bad:$str};
f=inner(n:i64):i64!$err{if(n<0){<$err{$bad:"neg"}};<n*2};
f=outer(n:i64):i64!$err{let v=inner(n)!$err;<v+1};
f=main():i64{
  let a=mt outer(0-5){$ok:v 100;$err:e 200};
  let b=mt outer(5){$ok:v v;$err:e 200};
  io.println("fail=\(a) ok=\(b)");
  <0
};'

# ── 5b: `!` propagation of a 0/null-sentinel STDLIB callee ──────────────────
#   The callee signals failure with a null/0 return, not @tk_current_error;
#   the propagating T!$err wrapper must still report the failure upward.
run_case "propagate stdlib union" "badjson=200 goodjson=1 badint=200 goodint=42" '
m=t;
i=io:std.io;
i=json:std.json;
i=str:std.str;
t=$err{$bad:$str};
f=dec(s:$str):i64!$err{let d=json.dec(s)!$err;<1};
f=num(s:$str):i64!$err{let v=str.toint(s)!$err;<v};
f=main():i64{
  let a=mt dec("{not json"){$ok:v v;$err:e 200};
  let b=mt dec("{\"a\":1}"){$ok:v v;$err:e 200};
  let c=mt num("zz"){$ok:v v;$err:e 200};
  let d=mt num("42"){$ok:v v;$err:e 200};
  io.println("badjson=\(a) goodjson=\(b) badint=\(c) goodint=\(d)");
  <0
};'

# ── 6: nested `mt e {$variants}` on the bound error payload ─────────────────
run_case "nested mt on err payload" "one=bad:neg two=worse:7 ok=okvalue" '
m=t;
i=io:std.io;
i=s:std.str;
t=$err{$bad:$str;$worse:i64};
f=risky(n:i64):$str!$err{
  if(n<0){<$err{$bad:"neg"}};
  if(n>100){<$worse(7)};
  <"okvalue"
};
f=describe(n:i64):$str{
  <mt risky(n){
    $ok:v v;
    $err:e mt e {
      $bad:m s.concat("bad:";m);
      $worse:k "worse:\(k)"
    }
  }
};
f=main():i64{
  io.println("one=\(describe(0-5)) two=\(describe(200)) ok=\(describe(5))");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
