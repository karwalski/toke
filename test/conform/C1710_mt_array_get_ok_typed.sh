#!/usr/bin/env bash
# C1710_mt_array_get_ok_typed.sh -- the `$ok` binding of a result match over
# an array lookup (`mt ps.get(i) {$ok:r ..;$err:e ..}`) has the array's
# element type, so field reads and arm agreement are judged as for the
# direct form `ps.get(i)` (story 127.428).
#
# WHAT WAS WRONG (base b9a39109 = integrate/z-wave-1113g):
#   mt_ok_payload_type answered an index-expression scrutinee only when it
#   was a map lookup (`V!$err`, 127.322).  An array `.get` is typed by the
#   checker as the element itself, so the `$ok` binding stayed TY_UNKNOWN:
#   `mt ps.get(0) {$ok:r r.nosuch;$err:e 0}` checked clean while
#   `ps.get(0).nosuch` is E4025, and E4011 skipped a bare `$ok:r r` arm
#   (`{$ok:r r;$err:e "no"}` was clean).  The run-time value was already
#   right (codegen types the element); only the judgment was missing.
#
# WHAT IS ASSERTED
#   1. run: result matches over `.get` of arrays of a local record, str,
#      i64 and nested arrays compile and print the values
#   2. --check: `$ok:r r.nosuch` is E4025 (direct scrutinee, let-bound
#      scrutinee, and a field read on the `let` the match binds)
#   3. --check: a bare `$ok:r r` beside an `$err` arm of another type is
#      E4011 (record vs str, str vs i64)
#   4. --check: correct programs stay clean -- a known field, a same-typed
#      `$err` arm, E4011's `0` null filler, and a map `.get` match
#
# Negative control: C1710_TKC=<path> runs another compiler.  Measured: base
# b9a39109 tkc 3 passed, 6 failed (every case in 2 and 3); this branch 9
# passed.
#
# Story: 127.428
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1710_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1710_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1710: \$ok binding of a match over an array .get is typed (127.428)"
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

cat > val.tk <<'TOKE'
m=val;
i=io:std.io;
t=$pt{x:i64;name:str};
f=main():i64{
  let ps=@($pt{x:3;name:"bc"});
  let p=mt ps.get(0) {$ok:r r;$err:e $pt{x:0;name:""}};
  let n=mt ps.get(0) {$ok:r r.name.len;$err:e 0};
  let ss=@("hey";"you");
  let s=mt ss.get(1) {$ok:v v;$err:e ""};
  let is=@(4;5);
  let k=mt is.get(1) {$ok:v v*2;$err:e 0};
  let aa=@(@(1;2);@(3;4));
  let a=mt aa.get(1) {$ok:r r;$err:e @(0)};
  io.println("\(p.x) \(p.name) \(n) \(s) \(k) \(a.len)");
  <0
};
TOKE
run_tk val "3 bc 2 you 10 2"

cat > fld.tk <<'TOKE'
m=fld;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:3});
  let p=mt ps.get(0) {$ok:r r.nosuch;$err:e 0};
  <0
};
TOKE
chk "\$ok field read, unknown field" "E4025 " fld.tk

cat > fldlet.tk <<'TOKE'
m=fldlet;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:3});
  let g=ps.get(0);
  let p=mt g {$ok:r r.nosuch;$err:e 0};
  <0
};
TOKE
chk "let-bound scrutinee, unknown field" "E4025 " fldlet.tk

cat > bound.tk <<'TOKE'
m=bound;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:3});
  let p=mt ps.get(0) {$ok:r r;$err:e $pt{x:0}};
  let q=p.nosuch;
  <0
};
TOKE
chk "field read on the bound let" "E4025 " bound.tk

cat > armrec.tk <<'TOKE'
m=armrec;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:1});
  let p=mt ps.get(0) {$ok:r r;$err:e "no"};
  <0
};
TOKE
chk "record \$ok vs str \$err" "E4011 " armrec.tk

cat > armstr.tk <<'TOKE'
m=armstr;
f=main():i64{
  let ss=@("hey");
  let s=mt ss.get(0) {$ok:v v;$err:e 7};
  <0
};
TOKE
chk "str \$ok vs i64 \$err" "E4011 " armstr.tk

cat > armfld.tk <<'TOKE'
m=armfld;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:1});
  let p=mt ps.get(0) {$ok:r r.x;$err:e "no"};
  <0
};
TOKE
chk "i64 field \$ok vs str \$err" "E4011 " armfld.tk

cat > ok.tk <<'TOKE'
m=ok;
t=$pt{x:i64};
f=main():i64{
  let ps=@($pt{x:1});
  let a=mt ps.get(0) {$ok:r r.x;$err:e 0};
  let b=mt ps.get(0) {$ok:r r;$err:e $pt{x:0}};
  let c=mt ps.get(0) {$ok:r r;$err:e 0};
  let ss=@("hey");
  let d=mt ss.get(0) {$ok:v v;$err:e ""};
  let e=mt ss.get(0) {$ok:v v.len;$err:e 0};
  <a+b.x+c.x+(e as i64)+d.len as i64
};
TOKE
chk "correct matches stay clean" "" ok.tk

cat > mp.tk <<'TOKE'
m=mp;
f=main():i64{
  let mm=@("a":1;"b":2);
  let v=mt mm.get("a") {$ok:x x;$err:e 0};
  <v
};
TOKE
chk "map .get match unchanged" "" mp.tk

echo "--------------------------------------------------------------------"
echo "C1710: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
