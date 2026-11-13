#!/usr/bin/env bash
# C1700_result_match_local_record.sh -- a result match (`$ok`/`$err` arms)
# over a LOCAL product record is not a sum match: the record's fields are not
# variants, so no E4010 "missing variant(s)" (story 127.426).
#
# WHAT WAS WRONG (base 6911273a, and toke main):
#   `t=$pt{x:i64}; let ps=@($pt{x:1}); let p=mt ps.get(0) {$ok:r r;$err:e
#   $pt{x:0}};` failed --check with E4010 "non-exhaustive match: missing
#   variant(s) 'x'": the sum exhaustiveness check in NODE_MATCH_STMT read
#   any record scrutinee's fields as variants (the checker's Type carries no
#   sum/product flag).  127.424 (C1690) skipped it for an IMPORTED record;
#   a local one now consults its TYPE_DECL (local_record_decl): a
#   default-syntax `t=$nm{..}` with no `$variant` field and no
#   uppercase-initial field is a product.
#
# WHAT IS ASSERTED
#   1. run: result matches over `.get` of an array of a one-field and a
#      two-field local record compile and print the values
#   2. --check: E4010 is NOT weakened for a real sum -- a default sum
#      `t=$sh{$ok:i64;$err:i64;$other:i64}` matched with `$ok`/`$err` only
#      still reports missing 'other'; a declaration without the `$` before
#      its name (`t=r{ok:..;err:..;other:..}`) is not judged a product and
#      keeps the check
#   3. --legacy --check: record and sum share a spelling (G012/G045,
#      127.380) -- `T=R{Ok:i64;Err:i64;Other:i64}` and
#      `T=R{ok:i64;err:i64;other:i64}` matched with Ok/Err keep E4010; an
#      exhaustive legacy match stays clean
#   4. --check: the arms of a local-record result match are still compared
#      -- an `$ok` arm yielding `$pt` beside an `$err` arm yielding a str is
#      E4011 (the `$ok` binding itself is untyped here: 127.428)
#
# Negative control: C1700_TKC=<path> runs another compiler.  Measured: base
# 6911273a tkc 5 passed, 3 failed (both run cases and the E4011 case also report E4010); this branch 8
# passed.
#
# Story: 127.426
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1700_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1700_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1700: result match over a local record is not a sum match (127.426)"
echo "--------------------------------------------------------------------"

run_tk() {  # $1 name, $2 expected stdout; program in $1.tk
    if ! "${TKC}" --out "$1" "$1.tk" > "$1.log" 2>&1; then
        fail "$1: compile"; head -c 400 "$1.log" | sed 's/^/        /'; echo; return
    fi
    got="$(TK_STR_VERIFY=1 "./$1" 2>&1)"
    if [ "${got}" = "$2" ]; then pass "$1: ${2//$'\n'/ | }"
    else fail "$1: got '${got//$'\n'/ | }', want '${2//$'\n'/ | }'"; fi
}
codes() {  # $@ tkc args: the sorted error codes --check reports
    "${TKC}" --check "$@" 2>&1 | grep -o '"error_code":"E[0-9]*"' | cut -d'"' -f4 | sort -u | tr '\n' ' '
}
chk() {  # $1 label, $2 expected codes ("" = clean), rest: tkc args
    local label=$1 want=$2; shift 2
    got="$(codes "$@")"
    if [ "${got}" = "${want}" ]; then pass "--check ${label}: '${got}'"
    else fail "--check ${label}: got '${got}', want '${want}'"; fi
}

cat > one.tk <<'TOKE'
m=one;
i=io:std.io;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:7});
  let p=mt ps.get(0) {$ok:r r;$err:e $pt{x:0}};
  io.println("\(p.x)");
  <0
};
TOKE
run_tk one "7"

cat > two.tk <<'TOKE'
m=two;
i=io:std.io;
t=$pt{x:i64;name:str};
f=main():i64{
  let ps=@($pt{x:1;name:"a"};$pt{x:2;name:"bc"});
  let p=mt ps.get(1) {$ok:r r;$err:e $pt{x:0;name:""}};
  io.println("\(p.x) \(p.name) \(p.name.len)");
  <0
};
TOKE
run_tk two "2 bc 2"

cat > sum.tk <<'TOKE'
m=sum;
t=$sh{$ok:i64;$err:i64;$other:i64};
f=g(s:$sh):i64{
  <mt s {$ok:v v;$err:e e}
};
f=main():i64{ <g($sh{$ok:1}) };
TOKE
chk "default sum missing \$other" "E4010 " sum.tk

cat > nodollar.tk <<'TOKE'
m=nodollar;
t=r{ok:i64;err:i64;other:i64};
f=g(s:$r):i64{
  <mt s {$ok:v v;$err:e e}
};
TOKE
chk "decl without \$ keeps the check" "E4010 " nodollar.tk

cat > lup.tk <<'TOKE'
M=lup;
T=R{Ok:i64;Err:i64;Other:i64};
F=g(s:R):i64{
  <mt s{Ok:v v;Err:e e}
};
TOKE
chk "legacy uppercase sum" "E4010 " --legacy lup.tk

cat > llo.tk <<'TOKE'
M=llo;
T=R{ok:i64;err:i64;other:i64};
F=g(s:R):i64{
  <mt s{Ok:v v;Err:e e}
};
TOKE
chk "legacy lowercase decl" "E4010 " --legacy llo.tk

cat > lex.tk <<'TOKE'
M=lex;
T=Color{Red:i64;Green:i64;Blue:i64};
F=name(c:Color):i64{
  <mt c{Red:r r;Green:g g;Blue:b b}
};
TOKE
chk "legacy exhaustive (G045)" "" --legacy lex.tk

cat > arm.tk <<'TOKE'
m=arm;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:1});
  let p=mt ps.get(0) {$ok:r $pt{x:1};$err:e "no"};
  <0
};
TOKE
chk "err arm of another type" "E4011 " arm.tk

echo "--------------------------------------------------------------------"
echo "C1700: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
