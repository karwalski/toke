#!/usr/bin/env bash
# C830_unannotated_let_keeps_interface_type.sh — an unannotated `let` bound to
# a value whose type an INTERFACE states (a stdlib .tki record, or another
# user module's generated .tki) keeps that type, so its uses lower on it
# (stories 127.297, 127.302, 127.308).
#
# THE DEFECT. The checker typed such a binding only when the interface named
# a struct (127.66 / 127.180).  Everything else stayed TY_UNKNOWN, the
# emitter fell back to the i64 it assumes for an untyped value, and the
# program printed a wrong value at exit 0 with no diagnostic:
#   - `let c=mt json.bool(..){$ok:v v;..}` interpolated as 1 (127.302);
#     the std record spells the union inline ("bool!jsonerr"), so even the
#     ok-payload lookup had nothing to read;
#   - `let a=sl.mk()` with `f=mk():@$str` in another module (`[str]` in its
#     .tki): `\(a.get(0))` printed the string's address (127.308);
#   - `let r=mt toml.f64arr(..){$ok:v v;..}` (a `[f64]!E` std export):
#     `r.get(0)` read the double's bits with sitofp (127.297).
# types.c now adopts a REPRESENTATION-bearing interface type (str, bool and
# f64 scalars; arrays of str/bool/f64 to any depth) for an `mt` ok payload of
# an imported call and for an array returned from another user module.
#
# Negative control: TKC built from main at 7758dcf3 — cases 1-4, 6-9 and
# 11 red (1 / addresses / f64 bit patterns), case 10 (IR) red (sitofp);
# cases 5 and 12 (controls) green on both.
#
# 127.297's only reachable export (df.column) is quarantined and
# toml.f64arr does not exist yet (113.B.4), so case 10 adds the record to a
# COPY of the interfaces and checks the IR: it cannot link without glue.
#
# Story: 127.297, 127.302, 127.308

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
# run <main source> [<library source>] — builds main (with the library
# module `lb` when given) and prints its output on one line.
run() {
    rm -f "${WORK}"/*.tk "${WORK}/b"
    printf '%s\n' "$1" > "${WORK}/mb.tk"
    local files=("${WORK}/mb.tk")
    if [ $# -ge 2 ]; then
        printf '%s\n' "$2" > "${WORK}/lb.tk"
        files=("${WORK}/lb.tk" "${WORK}/mb.tk")
    fi
    # --check takes one file; a multi-file build type-checks every module
    if [ $# -lt 2 ]; then
        "${TKC}" --check "${files[@]}" >/dev/null 2>&1 || { echo "check-failed"; return; }
    fi
    "${TKC}" -o "${WORK}/b" "${files[@]}" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '
}
LB='m=lb;t=$e{msg:$str};
f=strs():@$str{<@("ab";"cd")};
f=strse():@$str!$e{<@("ab";"cd")};
f=fl():@f64{<@(1.5;2.5)};
f=bl():@bool{<@(true;false)};
f=nest():@@$str{<@(@("ab"))};
f=bo():bool!$e{<true};'
H='m=mb;i=io:std.io;i=lb:lb;'

# 127.302 — std bool!E through mt, then copied into another binding
# (assigned first: inside "$(…)" the JSON's `{..,..}` met brace expansion)
P1='m=mb;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"f\":true,\"g\":false}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let c=mt json.bool(o;"f") { $ok:v v; $jsonerr:e false };
  let g=mt json.bool(o;"g") { $ok:v v; $jsonerr:e true };
  let d=c;
  io.println("\(c) \(g) \(d)");
  <0};'
expect "1-mt-std-bool" "$(run "$P1")" "true false true "
# a std [bool]!E and [str]!E payload (same root cause)
expect "2-mt-std-bool-and-str-arrays" \
  "$(run 'm=mb;i=io:std.io;i=toml:std.toml;
f=main():i64{
  let cfg=mt toml.load("b = [true, false]\ns = [\"ab\", \"cd\"]\n") {$ok:v v;$err:e < 1};
  let b=mt toml.boolarr(cfg;"b") {$ok:v v;$err:e < 2};
  let s=mt toml.strarr(cfg;"s") {$ok:v v;$err:e < 3};
  io.println("\(b.get(0)) \(b.get(1)) \(s.get(1))");
  <0};')" \
  "true false cd "
# 127.308 — cross-module @$str, direct and as an mt ok payload
expect "3-cross-module-str-array" \
  "$(run "$H"'f=main():i64{let a=lb.strs();io.println("\(a.get(0)) \(a.get(1))");<0};' "$LB")" \
  "ab cd "
expect "4-cross-module-str-array-mt-and-arm" \
  "$(run "$H"'f=main():i64{
  let a=mt lb.strse() {$ok:v v;$e:e @("x")};
  mt lb.strse() {$ok:v io.println("\(v.get(1))");$e:e io.println("e")};
  io.println("\(a.get(0))");<0};' "$LB")" \
  "cd ab "
expect "5-annotated-control" \
  "$(run "$H"'f=main():i64{let c:@$str=lb.strs();io.println("\(c.get(0))");<0};' "$LB")" \
  "ab "
# the same hole for @f64 / @bool / nested arrays from another module
expect "6-cross-module-f64-array" \
  "$(run "$H"'f=main():i64{let f=lb.fl();let m2=mut.lb.fl();io.println("\(f.get(0)) \(m2.get(1))");<0};' "$LB")" \
  "1.5 2.5 "
expect "7-cross-module-bool-array" \
  "$(run "$H"'f=main():i64{let b=lb.bl();io.println("\(b.get(0)) \(b.get(1))");<0};' "$LB")" \
  "true false "
expect "8-cross-module-nested-str-array" \
  "$(run "$H"'f=main():i64{let n=lb.nest();io.println("\(n.get(0).get(0))");<0};' "$LB")" \
  "ab "
expect "9-cross-module-bool-union-mt" \
  "$(run "$H"'f=main():i64{let q=mt lb.bo() {$ok:v v;$e:e false};let r=lb.bo();let r2=mt r {$ok:v v;$e:e false};io.println("\(q) \(r2)");<0};' "$LB")" \
  "true true "

# 127.297 — a [f64]!E std export: the element is read as a double
FS="${WORK}/fstd"
mkdir -p "${FS}/src/stdlib" "${FS}/stdlib"
for f in "${REPO_ROOT}"/src/stdlib/*; do ln -s "$f" "${FS}/src/stdlib/"; done
cp "${REPO_ROOT}"/stdlib/*.tki "${FS}/stdlib/"
python3 - "${FS}/stdlib/toml.tki" <<'EOF'
import sys
p = sys.argv[1]; s = open(p).read()
a = '    {"kind": "func",   "name": "toml.boolarr",'
assert a in s
s = s.replace(a, '    {"kind": "func",   "name": "toml.f64arr",    "params": ["tomlval", "str"],   "return": "[f64]!tomlerr"},\n' + a, 1)
open(p, 'w').write(s)
EOF
cat > "${WORK}/f.tk" <<'EOF'
m=main;i=io:std.io;i=toml:std.toml;i=s:std.str;
f=main():i64{
  let cfg = mt toml.load("r = [1.5]\n") {$ok:v v;$err:e < 1};
  let r = mt toml.f64arr(cfg; "r") {$ok:v v;$err:e < 4};
  let x = r.get(0);
  io.println(s.fromfloat(x*2.0));
  <0
};
EOF
if TKC_STDLIB_DIR="${FS}/src/stdlib" "${TKC}" --emit-llvm --out "${WORK}/f.ll" "${WORK}/f.tk" >/dev/null 2>&1; then
    got="sitofp=$(grep -c 'sitofp' "${WORK}/f.ll") bitcast=$(grep -c 'bitcast i64 .* to double ; f64 array subscript' "${WORK}/f.ll")"
else
    got="emit-failed"
fi
expect "10-std-f64-array-union-element-is-a-double" "${got}" "sitofp=0 bitcast=1"

# 11: a std bool!E / f64!E scalar through mt (f64 was already right)
expect "11-mt-std-scalars" \
  "$(run 'm=mb;i=io:std.io;i=toml:std.toml;
f=main():i64{
  let cfg=mt toml.load("f = 2.5\nb = true\n") {$ok:v v;$err:e < 1};
  let x=mt toml.f64(cfg;"f") {$ok:v v;$err:e 0.0};
  let z=mt toml.bool(cfg;"b") {$ok:v v;$err:e false};
  let z2=z;
  io.println("\(x) \(z) \(z2)");
  <0};')" \
  "2.5 true true "
# 12: integer controls are unchanged
expect "12-integer-control" \
  "$(run 'm=mb;i=io:std.io;i=toml:std.toml;
f=main():i64{
  let cfg=mt toml.load("n = [3, 4]\ni = 7\n") {$ok:v v;$err:e < 1};
  let n=mt toml.i64arr(cfg;"n") {$ok:v v;$err:e < 2};
  let i=mt toml.i64(cfg;"i") {$ok:v v;$err:e 0};
  io.println("\(n.get(1)) \(i+1)");
  <0};')" \
  "4 8 "

echo "C830: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
