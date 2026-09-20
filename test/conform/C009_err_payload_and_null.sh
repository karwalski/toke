#!/usr/bin/env bash
# C009_err_payload_and_null.sh — runtime conformance for the error-union
# payload (story 127.97 / 127.78) and for the NULL-safety of comparison
# (story 127.83).  Both are runtime-abi.md §7.
#
# 127.97: a `T!E` error return stashes a malloc'd box of the declared error
# type in @tk_current_error and returns the 0/null value sentinel.  The $err
# arm binds that box.  Before the fix the arm only read the box back when the
# error type was a discriminated sum, so a record-style error type — which is
# what almost every stdlib module and all of ooke declares — bound nil and any
# field read trapped RT005.  The payload was written and then dropped, which is
# why zip, file and csv each grew an out-of-band lasterr accessor.
#
# 127.83: `==`/`!=`/`<`/`>` on $str lowered straight to strcmp(), which
# dereferences both operands, so comparing the `?(T)` miss sentinel — the
# documented way to check for a missing key — exited 139.
#
# Cases 1 and 2 are the "distinguish two different errors" assertions: each
# asserts a VALUE carried out of the error arm, not merely that it compiled.
# A test that only checked compilation would pass against the defect, because
# binding nil compiles exactly as happily as binding the box.
# Cases 5 and 6 are the controls: ordinary string comparison must be
# untouched, and a payload-less error must still take the $err arm.
#
# Stories: 127.97, 127.78, 127.83

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_errpayload_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C009: error-union payload is readable, and null comparison is safe"
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

# ── 1: TWO DIFFERENT ERRORS, record-style error type (127.97 / 127.78) ──────
#   The whole point.  Both calls fail; the caller must be able to say WHICH
#   rule rejected it.  Before the fix `e.code` was RT005 on both.
run_case "record-style: two errors told apart" \
"neg code=10 msg=negative
big code=20 msg=too-big
ok value=7" '
m=t;
i=io:std.io;
t=$myerr{code:i64;msg:$str};
f=check(n:i64):i64!$myerr{
  if(n<0){<$myerr{code:10;msg:"negative"}};
  if(n>100){<$myerr{code:20;msg:"too-big"}};
  <n
};
f=main():i64{
  io.println(mt check(0-5){$ok:v "ok value=\(v)";$err:e "neg code=\(e.code) msg=\(e.msg)"});
  io.println(mt check(200){$ok:v "ok value=\(v)";$err:e "big code=\(e.code) msg=\(e.msg)"});
  io.println(mt check(7){$ok:v "ok value=\(v)";$err:e "err code=\(e.code)"});
  <0
};'

# ── 2: TWO DIFFERENT ERRORS, sum-style error type ───────────────────────────
#   The variant path (114.41) already worked; pinned here so the 127.97 change
#   to the same binding site cannot regress it.
run_case "sum-style: two errors told apart" \
"bad:negative
worse:20
ok:7" '
m=t;
i=io:std.io;
t=$err{$bad:$str;$worse:i64};
f=check(n:i64):i64!$err{
  if(n<0){<$err{$bad:"negative"}};
  if(n>100){<$worse(20)};
  <n
};
f=describe(n:i64):$str{
  <mt check(n){
    $ok:v "ok:\(v)";
    $err:e mt e {$bad:m "bad:\(m)";$worse:k "worse:\(k)"}
  }
};
f=main():i64{
  io.println(describe(0-5));
  io.println(describe(200));
  io.println(describe(7));
  <0
};'

# ── 3: A NULL COMPARISON MUST NOT CRASH (127.83) ────────────────────────────
#   keychain.get returns the `?(str)` miss sentinel for an absent entry on a
#   supported platform AND on an unsupported one, so this is portable.  Every
#   operator below used to dereference the sentinel; `v==""` exited 139.
#   The sentinel is NOT the empty string — a missing key and a key stored
#   empty must stay distinguishable (stdlib/securemem.md, test C005).
run_case "null sentinel survives every comparison" \
"len=0 eqempty=0 eqval=0 nefull=1 ltA=1 gtA=0
survived" '
m=t;
i=io:std.io;
i=kc:std.keychain;
i=s:std.str;
f=main():i64{
  let v=kc.get("toke-conform-absent-service";"toke-conform-absent-account");
  io.println("len=\(s.len(v)) eqempty=\(v=="") eqval=\(v=="secret") nefull=\(v!="secret") ltA=\(v<"a") gtA=\(v>"a")");
  io.println("survived");
  <0
};'

# ── 4: CONTROL — mt string-tag dispatch still selects the right arm ─────────
#   The `mt s {tag:b …}` lowering (80.2.7) compared the scrutinee to each tag
#   with the same unguarded strcmp and was switched to tk_str_cmp with the
#   rest; this pins that the ordinary dispatch is unchanged.
#   A NULL scrutinee cannot be driven through this path yet: `mt` over a
#   fallible call returning $str emits invalid IR ("Terminator found in the
#   middle of a basic block"), identically on the pinned tkc-131.73 build, so
#   that is a standing defect rather than anything introduced here. → story.
run_case "control: string-tag dispatch picks the right arm" \
"first=a second=b other=missing" '
m=t;
i=io:std.io;
f=pick(v:$str):$str{<mt v{alpha:x "a";beta:x "b";rest:x "missing"}};
f=main():i64{
  io.println("first=\(pick("alpha")) second=\(pick("beta")) other=\(pick("zulu"))");
  <0
};'

# ── 5: CONTROL — ordinary string comparison is unchanged ────────────────────
#   tk_str_cmp replaced strcmp at every toke-level comparison site; equal
#   content in separately allocated strings must still compare equal, and
#   ordering must still be lexicographic.
run_case "control: non-null string comparison" \
"eq=1 ne=1 selfeq=1 lt=1 gt=1 le=1 ge=1 emptyeq=1 emptylt=1" '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let a=s.concat("he";"llo");
  let b="hello";
  io.println("eq=\(a==b) ne=\(a!="world") selfeq=\(b==b) lt=\("apple"<"banana") gt=\("banana">"apple") le=\("a"<="a") ge=\("b">="a") emptyeq=\(""=="") emptylt=\(""<"a")");
  <0
};'

# ── 6: CONTROL — a payload-less error still takes the $err arm ──────────────
#   The C glue signals failure by storing the bare flag 1 in @tk_current_error
#   and carries no box (runtime-abi §7).  127.97 binds the slot to the arm, so
#   the flag must be normalised to nil rather than handed over as the address
#   1 — and the $ok/$err discrimination itself must be unaffected.
run_case "control: payload-less error still discriminates" \
"badjson=err goodjson=ok badint=err goodint=42 none=miss" '
m=t;
i=io:std.io;
i=json:std.json;
i=s:std.str;
f=maybe(n:i64):i64!$none{if(n<0){<$none{}};<n};
f=main():i64{
  let a=mt json.dec("{not json"){$ok:v "ok";$err:e "err"};
  let b=mt json.dec("{\"a\":1}"){$ok:v "ok";$err:e "err"};
  let c=mt s.toint("zz"){$ok:v "\(v)";$err:e "err"};
  let d=mt s.toint("42"){$ok:v "\(v)";$err:e "err"};
  let e=mt maybe(0-1){$ok:v "\(v)";$none:z "miss"};
  io.println("badjson=\(a) goodjson=\(b) badint=\(c) goodint=\(d) none=\(e)");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
