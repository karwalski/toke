#!/usr/bin/env bash
# C003_shadowed_match_arm.sh — an `mt` arm binding maintains the codegen
# pointer registry for the name it binds (story 127.71).
#
# Codegen keeps a per-function registry of pointer locals, keyed by the RAW
# source name, recording which locals hold a string (and which struct/map type
# a pointer has).  `==`/`<`/interpolation/`.len`/field access consult it.  A
# shadowing binding is allocated under a UNIQUIFIED name (`v.1`), so unless the
# site also updates the raw name's entry the registry keeps describing the
# OUTER binding.
#
# `let` (126.7) and `lp(let …)` (127.64) clear the stale tag.  The three
# `mt`-arm binding sites did not:
#
#   A  sum-variant arm   `mt val {$num:v …}`   — payload may be i64/f64
#   B  string multi-arm  `mt k {$aa:v …}`      — binds the string scrutinee
#   C  ok/err arm        `mt call() {$ok:v …}` — binds the result / err payload
#
# The failure mode is a `strcmp` against an integer: SIGSEGV when it is lucky
# (A and C below crashed on the pre-fix build) and a garbage answer when it is
# not (B returned the bytes of the scrutinee as an i64 and exited 0).  Site B
# also never registered its binding at all, so `.len` inside the arm fell
# through to the array-header load even with no shadowing.
#
# Every case compiles and RUNS at -O0..-O3 and asserts stdout.
#
# Story: 127.71

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_matcharm_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C003: an mt arm binding owns its name in the pointer registry"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > case.tk
    local o
    for o in -O0 -O1 -O2 -O3; do
        local out rc=0
        rm -f case_bin
        if ! "${TKC}" --allow-all "${o}" --out case_bin case.tk >compile.log 2>&1; then
            echo "  FAIL: ${name} ${o}: compile failed"
            sed 's/^/      /' compile.log | head -6
            FAIL=$((FAIL + 1))
            continue
        fi
        # A stale tag can also spin (strcmp on two garbage pointers never
        # matches), so cap each run.  macOS ships no coreutils `timeout`;
        # perl's alarm is always present.
        out="$(perl -e 'alarm 20; exec @ARGV' ./case_bin 2>&1)" || rc=$?
        if [ "${out}" = "${expected}" ]; then
            echo "  PASS: ${name} ${o}"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${name} ${o}: expected [${expected}], got [${out}] (rc ${rc})"
            FAIL=$((FAIL + 1))
        fi
    done
}

# ── C: ok/err arm binding shadows a str local.  `v == 5` in the arm body read
#      the outer `v`'s "$str" tag and lowered to strcmp(inttoptr i64 5, …). ──
run_case "ok arm binding shadows a str local" $'a\n1' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a b");
  let v=toks.get(0);
  io.println(v);
  let n=mt s.toint("5") {$ok:v (if(v==5){1}el{0});$err:e 0};
  io.println(s.fromint(n));
  <0
};'

# ── A: sum-variant arm binding shadows a str local; the payload is an i64. ──
run_case "sum arm binding shadows a str local" $'a\n1' '
m=t;
i=io:std.io;
i=s:std.str;
t=Val{$num:i64;$txt:str};
f=mk():Val{
  <$num(7)
};
f=main():i64{
  let toks=s.fields("a b");
  let v=toks.get(0);
  io.println(v);
  let r=mt mk() {$num:v (if(v==7){1}el{0});$txt:v 0};
  io.println(s.fromint(r));
  <0
};'

# ── B: string multi-arm binding shadows a @str local.  `v.len` inside the arm
#      used the outer "@str" tag and loaded the array header word. ──
run_case "string arm binding shadows a str-array local" $'3\n2' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let v=s.fields("pp qq rr");
  io.println(s.fromint(v.len as i64));
  let k="bb";
  let n=mt k {$aa:v 1;$bb:v (v.len as i64);$cc:v 3};
  io.println(s.fromint(n));
  <0
};'

# ── B (no shadowing): the arm binding was never registered as a string at
#      all, so `.len` was wrong even on a fresh name. ──
run_case "string arm binding is a string with no shadowing" $'2' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let k="bb";
  let n=mt k {$aa:w 1;$bb:w (w.len as i64);$cc:w 3};
  io.println(s.fromint(n));
  <0
};'

# ── Positive guard: a str->str shadow must KEEP content comparison.  Clearing
#      the tag without re-recording the new binding would pointer-compare two
#      equal-content heap strings and report a miss. ──
run_case "str arm binding over a str local still strcmp" $'a\neq' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let toks=s.fields("a a");
  let v=toks.get(0);
  io.println(v);
  let r=mt s.toint("1") {$ok:n (if(v==toks.get(1)){1}el{0});$err:e 0};
  if(r==1){io.println("eq")}el{io.println("ne")};
  <0
};'

# ── Positive guard: an $err arm binding that shadows an outer name must carry
#      its OWN error payload type, so `e.msg` resolves against the error
#      struct and not against whatever the outer binding was. ──
# ── Positive guard: the $ok arm binds the scrutinee's value, so it carries
#      the scrutinee's tag.  It was never registered at all, so `v.len` on a
#      str result (returned through the i64 ABI, so bind_ty does not reveal it)
#      loaded the array header word — here with an outer struct binding of the
#      same name to pin the shadowing direction too. ──
run_case "ok arm binding over a struct local is the scrutinee's string" $'11\n2' '
m=t;
i=io:std.io;
i=s:std.str;
t=Pt{x:i64;y:i64};
t=$myerr{msg:$str};
f=mkpt():Pt{
  <Pt{x:11;y:22}
};
f=getname():str!$myerr{
  <"hi"
};
f=main():i64{
  let v=mkpt();
  io.println(s.fromint(v.x));
  let n=mt getname() {$ok:v (v.len as i64);$err:e 0};
  io.println(s.fromint(n));
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
