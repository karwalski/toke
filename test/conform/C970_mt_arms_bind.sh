#!/usr/bin/env bash
# C970_mt_arms_bind.sh -- a binding `let d=mt r {$ok:v a; $err:e b}` carries
# the type its value arms agree on (story 127.327).
#
# THE DEFECT (0f7baf5a).  bind_init_type's NODE_MATCH_STMT case went to
# mt_bind_type (127.180 / 138.2), which answered only codegen's arm in the
# bare `$ok:v v` shape and left every other arm body unknown.  On a map that
# read the wrong representation: `let d=mt g(1){$ok:v mu;$err:e
# @("k":0.0)}; d.getor("k";0.5)` printed 4609434218613702656 (1.5's bits)
# and `let x:str=d` checked clean.  The sibling of 127.326 (C960,
# expression-if).
#
# THE FIX.  types.c mt_arms_agreed_type: semantics.md section 8.1 says all
# arm body types must be equal and the match's type is the common arm type,
# so when every non-diverging arm body's memoised type is known and
# tk_type_identical, the binding takes that type.  The bare `$ok:v v` answer
# is kept.  Anything else (an unknown arm, arms of different types,
# including E4011's tolerated null filler) stays unknown, as before.  No
# join rule.  tail_value_type is shared with 127.326's if_expr_bind_type, so
# an if branch whose tail is such an mt is typed too.
#
# Negative control: C970_TKC=<other tkc>.  At 0f7baf5a r1-r3, r7 and n1-n5
# fail.  r4-r6, n6 and the c* controls pass there: r4/r5 read their value
# through codegen's own arm type, and r6/n6 (an if whose tail is an mt) were
# typed by 127.326 through infer's arm type -- they guard that the stricter
# shared rule keeps that answer.
#
# Story: 127.327
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C970_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c970_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C970: let d=mt r {...} binds the arms' agreed type (127.327)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Every program gets the same fallible g and sum type sh.
HDR='t=$er{c:i64};
t=$sh{$ci:f64;$sq:f64};
f=g(n:i64):i64!$er{ if(n>0){<n}; <$er{c:1} };'

# runs name description expected-output  (program body on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    { printf 'm=%s;i=io:std.io;\n%s\n' "${name}" "${HDR}"; cat; } > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
    if [ "${got}" = "${want}" ]; then pass "${desc} (${want})"
    else fail "${desc}: expected '${want}', got '${got}'"; fi
}

# rejects name description got-type  (program body on stdin): --check must
# exit 1 with E4031 expected str, got <type>.
rejects() {
    local name=$1 desc=$2 want=$3 out rc
    { printf 'm=%s;i=io:std.io;\n%s\n' "${name}" "${HDR}"; cat; } > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -qF "\"error_code\":\"E4031\"" \
       && printf '%s' "${out}" | grep -qF "\"expected\":\"str\",\"got\":\"${want}\""; then
        pass "${desc} is E4031 (got ${want})"
    else fail "${desc}: want exit 1 and E4031 str vs ${want}, got exit ${rc}: ${out}"; fi
}

# ── the value is read with the right representation ─────────────────────
runs r1 'mt-bound f64 map, ok side, hit and miss' '1.5 0.5 ' <<'EOF'
f=main():i64{let mu=@("k":1.5);let d=mt g(1){$ok:v mu;$err:e @("k":0.0)};
io.println("\(d.getor("k";0.5)) \(d.getor("zz";0.5))");<0};
EOF
runs r2 'mt-bound f64 map, err side' '2.5 ' <<'EOF'
f=main():i64{let mu=@("k":1.5);let d=mt g(0){$ok:v mu;$err:e @("k":2.5)};
io.println("\(d.getor("k";0.5))");<0};
EOF
runs r3 'mt-bound bool map, err arm written first' 'true ' <<'EOF'
f=main():i64{let mb=@("k":true);let d=mt g(1){$err:e @("k":false);$ok:v mb};
io.println("\(d.getor("k";false))");<0};
EOF
runs r4 'mt-bound f64 array' '2.5 ' <<'EOF'
f=main():i64{let d=mt g(1){$ok:v @(1.5;2.5);$err:e @(0.5;0.0)};io.println("\(d.get(1))");<0};
EOF
runs r5 'mt over a sum type, f64 arms, map handed on' '1.5 ' <<'EOF'
f=show(d:@($str:$f64)):i64{io.println("\(d.getor("k";0.5))");<0};
f=main():i64{let s=$sh{$ci:1.0};let d=mt s{$ci:r @("k":1.5);$sq:w @("k":0.0)};show(d);<0};
EOF
runs r6 'an if branch whose tail is such an mt' '1.5 ' <<'EOF'
f=main():i64{let c=true;let mu=@("k":1.5);
let d=if(c){mt g(1){$ok:v mu;$err:e @("k":0.0)}}el{@("k":0.0)};
io.println("\(d.getor("k";0.5))");<0};
EOF
runs r7 'a diverging arm is skipped' '1.5 ' <<'EOF'
f=pick(n:i64):f64{let mu=@("k":1.5);let d=mt g(n){$ok:v mu;$err:e <0.0};<d.getor("k";0.5)};
f=main():i64{io.println("\(pick(1))");<0};
EOF

# ── controls: shapes that were right before stay right ──────────────────
runs c1 'bare $ok:v v, scalars, bool and str arms' '1 1.5 true a ' <<'EOF'
f=main():i64{let a=mt g(1){$ok:v v;$err:e 0};let b=mt g(1){$ok:v 1.5;$err:e 2.5};
let t=mt g(1){$ok:v true;$err:e false};let s=mt g(1){$ok:v "a";$err:e "b"};
io.println("\(a) \(b) \(t) \(s)");<0};
EOF
runs c2 'null filler beside a map arm' '1.5 ' <<'EOF'
f=main():i64{let mu=@("k":1.5);let d=mt g(1){$ok:v mu;$err:e 0};
io.println("\(mu.getor("k";0.5))");<0};
EOF

# ── the binding is judged as the type it now has ────────────────────────
rejects n1 'let x:str=d on an mt over f64 maps' '@(str:f64)' <<'EOF'
f=main():i64{let mu=@("k":1.5);let d=mt g(1){$ok:v mu;$err:e @("k":0.0)};let x:str=d;io.println(x);<0};
EOF
rejects n2 'let x:str=d on an mt over f64 arrays' '@(f64)' <<'EOF'
f=main():i64{let d=mt g(1){$ok:v @(1.5);$err:e @(0.5)};let x:str=d;io.println(x);<0};
EOF
rejects n3 'let x:str=d on an mt over f64s' 'f64' <<'EOF'
f=main():i64{let d=mt g(1){$ok:v 1.5;$err:e 2.5};let x:str=d;io.println(x);<0};
EOF
rejects n4 'let x:str=d on an mt over bools' 'bool' <<'EOF'
f=main():i64{let d=mt g(1){$ok:v true;$err:e false};let x:str=d;io.println(x);<0};
EOF
rejects n5 'let x:str=d on a sum-type mt over f64 maps' '@(str:f64)' <<'EOF'
f=main():i64{let s=$sh{$ci:1.0};let d=mt s{$ci:r @("k":1.5);$sq:w @("k":0.0)};let x:str=d;io.println(x);<0};
EOF
rejects n6 'let x:str=d on an if whose tail is such an mt' '@(str:f64)' <<'EOF'
f=main():i64{let c=true;let d=if(c){mt g(1){$ok:v @("k":1.5);$err:e @("k":0.0)}}el{@("k":0.0)};let x:str=d;io.println(x);<0};
EOF
# A str match is a str: assigning it to a str is clean.
printf 'm=c3;i=io:std.io;\n%s\nf=main():i64{let s=mt g(1){$ok:v "a";$err:e "b"};let x:str=s;io.println(x);<0};\n' "${HDR}" > c3.tk
out="$("${TKC}" --check c3.tk 2>&1)"; rc=$?
if [ "${rc}" -eq 0 ]; then pass 'let x:str=s on an mt over strs is clean'
else fail "c3: want a clean --check, got exit ${rc}: ${out}"; fi

echo "---------------------------------------------------------------------------"
echo "C970: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
