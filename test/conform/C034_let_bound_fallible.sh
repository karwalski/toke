#!/usr/bin/env bash
# C034_let_bound_fallible.sh — a let-bound fallible value answers from the same
# evidence the direct call does (story 135.16).
#
# THE DEFECT.  An ok/err `mt` picks its arm one of three ways: the error
# channel @tk_current_error (114.53/54 for the parse wrappers, 114.55 for user
# `T!$err` functions, 135.12 for std.file), the void status word (127.95), or
# the value-vs-0/null sentinel.  Which one it used was decided by inspecting
# the SCRUTINEE — and only when the scrutinee was itself a NODE_CALL_EXPR.  So
# the two spellings of one call disagreed:
#
#     mt s.toint("0") { $ok:n … }        -> ok, 0
#     let r = s.toint("0"); mt r { … }   -> err
#
# and the let-bound spelling is the one people write.  It is a SILENT wrong
# answer for every fallible function whose ok value can be 0 — str.toint,
# str.tofloat, toml.i64, toml.bool, file.size, and every user-defined
# `T!$err` — and the failure mode is a caller substituting its default for a
# value the data actually supplied.  Same family as 127.123 and 127.67.
#
# IT WAS NEVER JUST THE DISCRIMINANT.  The same scrutinee-is-a-call gate also
# selected the $err arm's typed payload (114.41 / 127.97) and the void status
# word (127.95), so the let-bound spelling additionally:
#
#   * bound `e` to nil, turning `e.msg` into an RT005 trap (case 6), and
#   * where the payload's field name is declared by more than one struct, was
#     rejected at compile time with E4034 on a program that is correct — a
#     false diagnostic, which AGENTS.md §2 ranks as the worst outcome of all
#     (case 7), and
#   * reported a void!$err call that plainly SUCCEEDED as a failure (case 5).
#
# WHY THE FIX SNAPSHOTS, AND WHY THAT IS ASSERTED HERE.  The classification
# travels with the BINDING.  But carrying only the convention and reading
# @tk_current_error at the `mt` would trade one wrong answer for another: the
# channel is live global state, so any later fallible call overwrites it.
# Case 8 binds an ok value, then deliberately fails a call, then matches the
# first binding — it must still be ok.  Cases 9 and 10 are the same property
# under re-assignment and under shadowing.  Without the spill each of those is
# wrong in a way no amount of testing the simple case would reveal.
#
# CASE 11 IS WHAT STOPS THE FIX BEING A CONSTANT.  Every genuine failure must
# still reach $err through a let.  A fix that made `mt r` always take $ok
# would pass cases 1-10 and be far worse than the defect.
#
# C029 pins the std.file half of this (file.size of an empty file, both
# spellings) and is where the defect was found.  This suite covers the rest of
# the family, which is what "much wider than std.file" meant.
#
# Story: 135.16

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c032_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C034: a let-bound fallible value answers like the direct call"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE — compiles AND RUNS.  Only a run proves
# which arm was taken: the wrong arm compiles exactly as happily as the right
# one, which is the whole reason this defect survived since 114.53.
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > run.tk
    local out rc=0
    rm -f run.bin
    out="$("${TKC}" --allow-all --diag-json -o run.bin run.tk 2>&1)" || rc=$?
    if [ ! -x run.bin ]; then
        echo "  FAIL: ${name}: did not compile (exit ${rc})"
        echo "${out}" | grep -v '"severity":"warning"' | sed 's/^/      /' | head -4
        FAIL=$((FAIL + 1))
        return
    fi
    local got
    got="$(./run.bin 2>&1)"
    rm -f run.bin
    if [ "${got}" != "${expected}" ]; then
        echo "  FAIL: ${name}: expected [${expected}], got [${got}]"
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ════════════════════════════════════════════════════════════════════════
# 1-2. THE PARSE WRAPPERS (114.53/54).  The two spellings print the same
#      line or this suite has no subject.  The non-zero values are here so a
#      "fix" that broke the ordinary case cannot hide behind the zero one.
# ════════════════════════════════════════════════════════════════════════
run_case "str.toint: direct and let-bound agree, including on 0" \
    $'direct0=ok|0\nlet0=ok|0\ndirect7=ok|7\nlet7=ok|7' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  mt s.toint("0"){$ok:n io.println(s.concat("direct0=ok|";s.fromint(n)));$err:e io.println("direct0=err")};
  let a=s.toint("0");
  mt a{$ok:n io.println(s.concat("let0=ok|";s.fromint(n)));$err:e io.println("let0=err")};
  mt s.toint("7"){$ok:n io.println(s.concat("direct7=ok|";s.fromint(n)));$err:e io.println("direct7=err")};
  let b=s.toint("7");
  mt b{$ok:n io.println(s.concat("let7=ok|";s.fromint(n)));$err:e io.println("let7=err")};
  <0
};'

run_case "str.tofloat: 0.0 through a let is a value, not a failure" \
    $'direct=ok\nlet=ok' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  mt s.tofloat("0.0"){$ok:v io.println("direct=ok");$err:e io.println("direct=err")};
  let a=s.tofloat("0.0");
  mt a{$ok:v io.println("let=ok");$err:e io.println("let=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 3. THE CONFIG ACCESSORS (127.67).  `minify = false` read as true is the
#    concrete harm that story was filed for; through a let it came straight
#    back.  Both value types whose domain includes the sentinel are here.
# ════════════════════════════════════════════════════════════════════════
run_case "toml: false and 0 survive a let binding" \
    $'boolD=ok|0\nboolL=ok|0\nintD=ok|0\nintL=ok|0\nmissL=err' '
m=t;
i=io:std.io;
i=s:std.str;
i=toml:std.toml;
f=main():i64{
  let src="minify = false\nretries = 0\n";
  let cfg=mt toml.load(src){$ok:v v;$err:e <1};
  mt toml.bool(cfg;"minify"){$ok:v io.println(s.concat("boolD=ok|";s.fromint(v)));$err:e io.println("boolD=err")};
  let a=toml.bool(cfg;"minify");
  mt a{$ok:v io.println(s.concat("boolL=ok|";s.fromint(v)));$err:e io.println("boolL=err")};
  mt toml.i64(cfg;"retries"){$ok:v io.println(s.concat("intD=ok|";s.fromint(v)));$err:e io.println("intD=err")};
  let b=toml.i64(cfg;"retries");
  mt b{$ok:v io.println(s.concat("intL=ok|";s.fromint(v)));$err:e io.println("intL=err")};
  let c=toml.i64(cfg;"nosuchkey");
  mt c{$ok:v io.println("missL=ok");$err:e io.println("missL=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 4. USER-DEFINED `T!$err` (114.55).  Not a stdlib quirk: any user function
#    whose ok value can be 0 had the same hole, which is the half that makes
#    this "much wider than std.file".
# ════════════════════════════════════════════════════════════════════════
run_case "a user T!\$err returning 0 reaches \$ok through a let" \
    $'direct=ok|0\nlet=ok|0\nlet8=ok|4' '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64};
f=half(n:i64):i64!$myerr{
  if(n<0){<$myerr{code:1}};
  <n/2
};
f=main():i64{
  mt half(0){$ok:v io.println(s.concat("direct=ok|";s.fromint(v)));$err:e io.println("direct=err")};
  let a=half(0);
  mt a{$ok:v io.println(s.concat("let=ok|";s.fromint(v)));$err:e io.println("let=err")};
  let b=half(8);
  mt b{$ok:v io.println(s.concat("let8=ok|";s.fromint(v)));$err:e io.println("let8=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 5. THE VOID STATUS WORD (127.95).  Inverted sentinel: 0 is SUCCESS.  The
#    let-bound form fell back to "non-zero means ok" and so reported every
#    successful fallible void call as a failure — note the program prints
#    "did it" and then used to print let=ERR.
# ════════════════════════════════════════════════════════════════════════
run_case "a successful void!\$err call reaches \$ok through a let" \
    $'did it\ndirect-OK\ndid it\nlet-OK\nletneg-ERR' '
m=t;
i=io:std.io;
t=$myerr{code:i64};
f=doit(n:i64):void!$myerr{
  if(n<0){<$myerr{code:1}};
  io.println("did it")
};
f=main():i64{
  mt doit(1){$ok:v io.println("direct-OK");$err:e io.println("direct-ERR")};
  let a=doit(1);
  mt a{$ok:v io.println("let-OK");$err:e io.println("let-ERR")};
  let b=doit(0-1);
  mt b{$ok:v io.println("letneg-OK");$err:e io.println("letneg-ERR")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 6. THE TYPED ERROR PAYLOAD (114.41 / 127.97).  The same gate chose the
#    $err arm's binding, so through a let `e` was nil and `e.zzcode` trapped
#    RT005 at runtime.  A trap is loud, but it is still a correct program
#    that does not run.
# ════════════════════════════════════════════════════════════════════════
run_case "the \$err payload is readable through a let" \
    $'direct-err=7\nlet-err=7' '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{zzcode:i64;zzmsg:$str};
f=half(n:i64):i64!$myerr{
  if(n<0){<$myerr{zzcode:7;zzmsg:"neg"}};
  <n/2
};
f=main():i64{
  mt half(0-4){$ok:v io.println("direct=ok");$err:e io.println(s.concat("direct-err=";s.fromint(e.zzcode)))};
  let c=half(0-4);
  mt c{$ok:v io.println("let=ok");$err:e io.println(s.concat("let-err=";s.fromint(e.zzcode)))};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 7. THE FALSE DIAGNOSTIC.  `msg` is declared by more than one struct in
#    scope, so without the payload's type the type checker could not place
#    the field and rejected this CORRECT program with E4034 — and E4034's
#    own `fix` ("annotate the value") is not writable for an $err binding,
#    so the repair loop had nowhere to go.  §2: a wrong error message is
#    worse than a missing feature.
# ════════════════════════════════════════════════════════════════════════
run_case "a payload field shared with another struct still compiles" \
    $'direct-err-msg=neg\nlet-err-msg=neg' '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64;msg:$str};
f=half(n:i64):i64!$myerr{
  if(n<0){<$myerr{code:7;msg:"neg"}};
  <n/2
};
f=main():i64{
  mt half(0-4){$ok:v io.println("direct=ok");$err:e io.println(s.concat("direct-err-msg=";e.msg))};
  let c=half(0-4);
  mt c{$ok:v io.println("let=ok");$err:e io.println(s.concat("let-err-msg=";e.msg))};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 8. THE CHANNEL IS SPILLED, NOT RE-READ.  @tk_current_error is live global
#    state.  `b` FAILS between the binding of `a` and the match on `a`, so a
#    fix that merely read the global at the `mt` reports a=err here — a
#    different wrong answer, and one that only shows up in real programs.
#    `c` proves the channel is not simply pinned to the first call either.
# ════════════════════════════════════════════════════════════════════════
run_case "an intervening failure does not steal an earlier binding's answer" \
    $'a=ok|0\nb=err\nc=ok|0' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let a=s.toint("0");
  let b=s.toint("zz");
  let c=s.toint("0");
  mt a{$ok:n io.println(s.concat("a=ok|";s.fromint(n)));$err:e io.println("a=err")};
  mt b{$ok:n io.println(s.concat("b=ok|";s.fromint(n)));$err:e io.println("b=err")};
  mt c{$ok:n io.println(s.concat("c=ok|";s.fromint(n)));$err:e io.println("c=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 9-10. RE-BINDING AND SHADOWING.  The channel must follow the call that is
#       IN the binding now, not the one that was there first.  Getting this
#       wrong would be a regression introduced BY the fix.
# ════════════════════════════════════════════════════════════════════════
run_case "an assignment replaces the binding's error channel" \
    $'d=ok|0\ne=err' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let d=mut.s.toint("zz");
  d=s.toint("0");
  mt d{$ok:n io.println(s.concat("d=ok|";s.fromint(n)));$err:e io.println("d=err")};
  let e=mut.s.toint("0");
  e=s.toint("zz");
  mt e{$ok:n io.println(s.concat("e=ok|";s.fromint(n)));$err:e io.println("e=err")};
  <0
};'

run_case "a shadowing re-let does not inherit the outer binding's channel" \
    $'x=err\ny=ok|0' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let x=s.toint("0");
  let x=s.toint("zz");
  mt x{$ok:n io.println(s.concat("x=ok|";s.fromint(n)));$err:e io.println("x=err")};
  let y=s.toint("zz");
  let y=s.toint("0");
  mt y{$ok:n io.println(s.concat("y=ok|";s.fromint(n)));$err:e io.println("y=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 11. NOT A CONSTANT.  Every genuine failure still reaches $err through a
#     let — a "fix" that always took $ok would pass everything above and be
#     strictly worse than the defect it replaced.
# ════════════════════════════════════════════════════════════════════════
run_case "real failures still reach \$err through a let" \
    $'p=err\nq=err\nr=err' '
m=t;
i=io:std.io;
i=s:std.str;
t=$myerr{code:i64};
f=half(n:i64):i64!$myerr{
  if(n<0){<$myerr{code:1}};
  <n/2
};
f=main():i64{
  let p=s.toint("zz");
  mt p{$ok:n io.println("p=ok");$err:e io.println("p=err")};
  let q=s.tofloat("nope");
  mt q{$ok:v io.println("q=ok");$err:e io.println("q=err")};
  let r=half(0-2);
  mt r{$ok:v io.println("r=ok");$err:e io.println("r=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 12. THE BINDING IS INSIDE A LOOP.  The spill slot is allocated once in the
#     IR text and written on every iteration; a per-iteration answer that
#     came out constant would mean the store was hoisted out of the body.
# ════════════════════════════════════════════════════════════════════════
run_case "a fallible let inside a loop answers per iteration" \
    $'ok|0\nerr\nok|7\nok|0' '
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let vs=@("0";"zz";"7";"0");
  lp(let i=0;i<vs.len;i=i+1){
    let r=s.toint(vs.get(i));
    mt r{$ok:n io.println(s.concat("ok|";s.fromint(n)));$err:e io.println("err")}
  };
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 13. THE OTHER DISCRIMINATION SITE COMPOSES WITH THIS ONE.  `!$err` is the
#     compiler's second place for deciding ok-vs-err, and it reads the
#     channel at the call — it was never affected by this defect and must
#     still work.  Here an ok value of 0 travels OUT through a propagate and
#     back IN through a let: pre-135.16 the propagate got it right and the
#     let then threw the answer away, which is what made the defect so easy
#     to mistake for a stdlib bug.
# ════════════════════════════════════════════════════════════════════════
run_case "a 0 propagated out through !\$err survives being let-bound" \
    $'prop=ok|0\nprop=err' '
m=t;
i=io:std.io;
i=s:std.str;
t=$perr{code:i64};
f=parse(x:str):i64!$perr{
  let v=s.toint(x)!$perr;
  <v
};
f=main():i64{
  let a=parse("0");
  mt a{$ok:n io.println(s.concat("prop=ok|";s.fromint(n)));$err:e io.println("prop=err")};
  let b=parse("zz");
  mt b{$ok:n io.println(s.concat("prop=ok|";s.fromint(n)));$err:e io.println("prop=err")};
  <0
};'

# ════════════════════════════════════════════════════════════════════════
# 14. THE SPILL SLOT IS ONLY EVER MINTED AT A `let`.  137.6 hoists every
#     alloca to the entry block, so a slot minted inside a branch would be
#     READ, undef, on the path that skipped the branch — an answer that
#     varies with whatever was on the stack.  An assignment therefore only
#     ever updates a slot a `let` already created, where the store is the
#     next instruction and dominates every use.  Here the `let` is fallible,
#     so the slot exists and both paths are defined: the branch not taken
#     must answer for the let's call, the branch taken for the assignment's.
# ════════════════════════════════════════════════════════════════════════
run_case "a conditional re-assign answers for whichever call actually ran" \
    $'notaken=ok|7\ntaken=ok|0\nnotakenerr=err' '
m=t;
i=io:std.io;
i=s:std.str;
f=probe(label:str;c:i64;seed:str):i64{
  let r=mut.s.toint(seed);
  if(c==1){ r=s.toint("0") };
  mt r{$ok:n io.println(s.concat(s.concat(label;"=ok|");s.fromint(n)));$err:e io.println(s.concat(label;"=err"))};
  <0
};
f=main():i64{
  probe("notaken";0;"7");
  probe("taken";1;"7");
  probe("notakenerr";0;"zz");
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
# Zero PASS and zero FAIL means it did not run, which is not a success.
[ "$((PASS + FAIL))" -gt 0 ] || { echo "ERROR: no assertion ran"; exit 1; }
[ "${FAIL}" -eq 0 ]
