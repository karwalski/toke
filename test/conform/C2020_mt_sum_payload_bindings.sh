#!/usr/bin/env bash
# C2020_mt_sum_payload_bindings.sh -- an `mt` arm binding of a sum-variant
# payload is typed as the variant's payload, for every payload kind
# (story 127.484).
#
# THE DEFECT (v3.2.0 ea7ddf22, base 0558545d).  With
# `t=$shape{$circle:f64;$sq:f64}; let s=$shape{$circle:2.0}`,
# `mt s{$circle:r r;..}` gave 2 but `mt s{$circle:r r*r;..}` gave
# 4616189618054758400, and `f=area(s:$shape):f64{<mt s{$circle:r r*r;..}}`
# returned 4.61619e+18.  The binding itself was lowered right (a double, and
# the product an fmul), but the match's result slot is sized before any arm
# is lowered, so the body was predicted with `r` unbound -- i64 -- and the
# double was stored as its bits and converted back with sitofp.  The checker
# typed every non-`$ok` arm binding as unknown, so it could not say better,
# and an unknown binding also lost the layout of a record or array payload:
# `q.w` failed E4034 when two records declare `w`, an @f64 element was
# multiplied as an integer (RT002), `q.get(1).len()` on an @str read the
# array's length, and a bool payload printed 1.
#
# THE FIX.  types.c sum_variant_payload_type: the binding of an arm naming a
# variant of a declared sum takes that variant's payload type.  llvm.c
# sum_arm_body_llvm_type: the result slot is predicted with the binding in
# force; sum_payload_tag: a record or array payload binding carries its
# layout tag; a bool payload binds an i1 local.
#
# Negative control: C2020_TKC=<other tkc>.  On base 0558545d 18 of 20 checks
# fail: p1-p3 (f64/f32), pb (bool), py (a bare byte binding: SIGSEGV), pr
# (records), pa (arrays), pn (nested sum: E9005 at build), and both rejects
# pass --check.  ps (str) passes there too: it guards that the change keeps
# it.
#
# Story: 127.484
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C2020_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2020_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C2020: mt arm bindings of sum-variant payloads are typed (127.484)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# runs name description expected-stdout  (program on stdin), at -O0 and -O2
runs() {
    local name=$1 desc=$2 want=$3 got o
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    for o in -O0 -O2; do
        if "${TKC}" "${o}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
            got="$(./"${name}" 2>&1)"
        else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
        if [ "${got}" = "${want}" ]; then pass "${desc} ${o} (${want})"
        else fail "${desc} ${o}: expected '${want}', got '${got}'"; fi
    done
}

# rejects name description code expected got  (program on stdin): --check
# exits 1 with that code, those fields and no fix.
rejects() {
    local name=$1 desc=$2 code=$3 exp=$4 got=$5 out rc
    { printf 'm=%s;\n' "${name}"; cat; } > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -qF "\"error_code\":\"${code}\"" \
       && printf '%s' "${out}" | grep -qF "\"expected\":\"${exp}\",\"got\":\"${got}\"" \
       && ! printf '%s' "${out}" | grep -qF '"fix"'; then
        pass "${desc} is ${code} (${exp} vs ${got}, no fix)"
    else fail "${desc}: want exit 1 and ${code} ${exp} vs ${got}, got exit ${rc}: ${out}"; fi
}

# ── f64 / f32: the story's reproducer, locally and through a function ──
runs p1 'f64 payload: bare binding, product, through a function' 'a=2 b=4 c=4 d=5' <<'EOF'
t=$shape{$circle:f64;$sq:f64};
f=area(s:$shape):f64{<mt s{$circle:r r*r;$sq:n n}};
f=main():i64{
  let ls=$shape{$circle:2.0};
  let a=mt ls{$circle:r r;$sq:n n};
  let b=mt ls{$circle:r r*r;$sq:n n};
  io.println("a=\(a) b=\(b) c=\(area(ls)) d=\(area(ls)+1.0)");
  <0};
EOF
runs p2 'f64 payload: sum, negation, binding arm written second' '3 -1.5 2.25' <<'EOF'
t=$v{$n:i64;$f:f64};
f=main():i64{
  let s=$v{$f:1.5};
  let a=mt s{$f:r r+r;$n:q 0.0};
  let b=mt s{$f:r -r;$n:q 0.0};
  let c=mt s{$n:q 0.0;$f:r r*r};
  io.println("\(a) \(b) \(c)");
  <0};
EOF
runs p3 'f32 payload: product' '6.25' <<'EOF'
t=$v{$n:i64;$g:f32};
f=main():i64{
  let s=$v{$g:2.5 as f32};
  let a=mt s{$g:q q*q;$n:q 0.0 as f32};
  io.println("\(a)");
  <0};
EOF
# ── bool ───────────────────────────────────────────────────────────────
runs pb 'bool payload: bare binding, negation, interpolated in the arm' 'true false true' <<'EOF'
t=$v{$n:i64;$b:bool};
f=main():i64{
  let s=$v{$b:true};
  let a=mt s{$n:q false;$b:q q};
  let b=mt s{$n:q false;$b:q !q};
  let c=mt s{$n:q "";$b:q "\(q)"};
  io.println("\(a) \(b) \(c)");
  <0};
EOF
# ── str / byte (kept) ──────────────────────────────────────────────────
runs ps 'str payload: length and interpolation' '3 hey!' <<'EOF'
t=$v{$n:i64;$s:str};
f=main():i64{
  let s=$v{$s:"hey"};
  let a=mt s{$n:q 0;$s:q q.len()};
  let b=mt s{$n:q "";$s:q "\(q)!"};
  io.println("\(a) \(b)");
  <0};
EOF
runs py 'byte payload: widened, widened sum, bare binding' '7 8 7' <<'EOF'
t=$v{$n:i64;$e:byte};
f=main():i64{
  let s=$v{$e:7 as byte};
  let a=mt s{$n:q 0;$e:q q as i64};
  let b=mt s{$n:q 0;$e:q (q as i64)+1};
  let c=mt s{$e:q q;$n:q 0 as byte};
  io.println("\(a) \(b) \(c)");
  <0};
EOF
# ── records: two records declare `w` and `nm` at different offsets ────
runs pr 'record payloads: field reads resolve against the payload layout' 'pn 5 21 qn 2.5' <<'EOF'
t=$pa{nm:str;w:f64};
t=$pb{w:i64;nm:str};
t=$v{$n:i64;$p:$pa;$q:$pb};
f=main():i64{
  let p=$v{$p:$pa{nm:"pn";w:2.5}};
  let q=$v{$q:$pb{w:7;nm:"qn"}};
  let a=mt p{$n:q "";$p:q q.nm;$q:q ""};
  let b=mt p{$n:q 0.0;$p:q q.w*2.0;$q:q 0.0};
  let c=mt q{$n:q 0;$p:q 0;$q:q q.w*3};
  let d=mt q{$n:q "";$p:q "";$q:q q.nm};
  let e=mt p{$n:q 0.0;$p:q q.w;$q:q 0.0};
  io.println("\(a) \(b) \(c) \(d) \(e)");
  <0};
EOF
# ── arrays ─────────────────────────────────────────────────────────────
runs pa 'array payloads: @f64 product, @str element, @bool element, @record element' '4.5 2 true r0 2' <<'EOF'
t=$pa{nm:str;w:f64};
t=$pb{w:i64;nm:str};
t=$v{$n:i64;$a:@f64;$s:@str;$b:@bool;$r:@$pa};
f=main():i64{
  let a=$v{$a:@(1.5;3.0)};
  let s=$v{$s:@("x";"yy")};
  let b=$v{$b:@(true;false)};
  let r=$v{$r:@($pa{nm:"r0";w:0.5})};
  let x1=mt a{$n:q 0.0;$a:q q.get(0)*q.get(1);$s:q 0.0;$b:q 0.0;$r:q 0.0};
  let x2=mt s{$n:q 0;$a:q 0;$s:q q.get(1).len();$b:q 0;$r:q 0};
  let x3=mt b{$n:q false;$a:q false;$s:q false;$b:q q.get(0);$r:q false};
  let x4=mt r{$n:q "";$a:q "";$s:q "";$b:q "";$r:q q.get(0).nm};
  let x5=mt r{$n:q 0.0;$a:q 0.0;$s:q 0.0;$b:q 0.0;$r:q q.get(0).w*4.0};
  io.println("\(x1) \(x2) \(x3) \(x4) \(x5)");
  <0};
EOF
# ── a sum payload matched in the arm; a map payload ───────────────────
runs pn 'nested sum and map payloads' '1.5625 5' <<'EOF'
t=$in{$x:f64;$y:i64};
t=$v{$n:i64;$s:$in;$m:@(str:f64)};
f=main():i64{
  let s=$v{$s:$in{$x:1.25}};
  let mm=$v{$m:@("k":2.5)};
  let b=mt s{$n:q 0.0;$s:q mt q{$x:z z*z;$y:z 0.0};$m:q 0.0};
  let c=mt mm{$n:q 0.0;$s:q 0.0;$m:q q.getor("k";0.0)*2.0};
  io.println("\(b) \(c)");
  <0};
EOF

# ── the binding is judged as the type it now has ────────────────────────
rejects n1 'let b:str of an f64 product arm' E4031 str f64 <<'EOF'
t=$shape{$circle:f64;$sq:f64};
f=main():i64{let ls=$shape{$circle:2.0};let b:str=mt ls{$circle:r r*r;$sq:n n};<0};
EOF
rejects n2 'an f64 payload arm beside an i64 payload arm' E4011 f64 i64 <<'EOF'
t=$shape{$circle:f64;$sq:i64};
f=main():i64{let ls=$shape{$circle:2.0};let b=mt ls{$circle:r r*r;$sq:n n};<0};
EOF

echo "---------------------------------------------------------------------------"
echo "C2020: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
