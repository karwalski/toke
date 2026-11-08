#!/usr/bin/env bash
# C960_if_expr_bind.sh -- a binding `let d=if(c){a}el{b}` carries the type
# its two branch tails agree on (story 127.326).
#
# THE DEFECT (8dff5994).  bind_init_type (types.c) had no case for an
# expression-`if` initialiser, so the binding was TY_UNKNOWN whatever its
# branches held.  On a map that read the wrong representation:
# `let d=if(c){mu}el{@("k":0.0)}; d.getor("k";0.5)` printed
# 4609434218613702656 (1.5's bits), a bool map printed 1, and `let x:str=d`
# checked clean.  The same through an `el if` chain.
#
# THE FIX.  types.c if_expr_bind_type: spec v0.4 section C says the two
# tails of an expression-if must have the same type, so when both tails'
# memoised types are known and tk_type_identical, the binding takes that
# type.  Anything else (an unknown tail, a diverging `<` branch, tails of
# different types) stays unknown, as before.  No join rule.
#
# Negative control: C960_TKC=<other tkc>.  At 8dff5994 r1, r2, r3, r7 and
# n1-n6 fail; r4-r6 and the c* controls pass.
#
# Story: 127.326
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C960_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c960_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C960: let d=if(c){a}el{b} binds the branches' agreed type (127.326)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# runs name description expected-output  (program body on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
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
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -qF "\"error_code\":\"E4031\"" \
       && printf '%s' "${out}" | grep -qF "\"expected\":\"str\",\"got\":\"${want}\""; then
        pass "${desc} is E4031 (got ${want})"
    else fail "${desc}: want exit 1 and E4031 str vs ${want}, got exit ${rc}: ${out}"; fi
}

# ── the value is read with the right representation ─────────────────────
runs r1 'if-bound f64 map, hit and miss' '1.5 0.5 ' <<'EOF'
f=main():i64{let mu=@("k":1.5);let c=true;let d=if(c){mu}el{@("k":0.0)};
io.println("\(d.getor("k";0.5)) \(d.getor("zz";0.5))");<0};
EOF
runs r2 'if-bound bool map' 'true ' <<'EOF'
f=main():i64{let mb=@("k":true);let c=true;let e=if(c){mb}el{@("k":false)};
io.println("\(e.getor("k";false))");<0};
EOF
runs r3 'el-if chain of f64 maps' '2.5 ' <<'EOF'
f=main():i64{let c=false;let b=true;
let d=if(c){@("k":1.5)}el if(b){@("k":2.5)}el{@("k":0.0)};
io.println("\(d.getor("k";0.5))");<0};
EOF
runs r4 'if-bound f64 array' '2.5 ' <<'EOF'
f=main():i64{let c=true;let a=if(c){@(1.5;2.5)}el{@(0.5)};io.println("\(a.get(1))");<0};
EOF
runs r5 'if-bound struct' '4 ' <<'EOF'
t=$pt{x:i64;y:i64};
f=main():i64{let c=true;let p=if(c){$pt{x:3;y:4}}el{$pt{x:0;y:0}};io.println("\(p.y)");<0};
EOF
runs r6 'if-bound map handed to a typed parameter' '1.5 ' <<'EOF'
f=show(d:@($str:$f64)):i64{io.println("\(d.getor("k";0.5))");<0};
f=main():i64{let c=true;let d=if(c){@("k":1.5)}el{@("k":0.0)};show(d);<0};
EOF
runs r7 'let mut binding of an if over maps' '1.5 ' <<'EOF'
f=main():i64{let mu=@("k":1.5);let c=true;let d=mut.if(c){mu}el{@("k":0.0)};
io.println("\(d.getor("k";0.5))");<0};
EOF

# ── controls: scalars and str were right before and stay right ──────────
runs c1 'if-bound f64 / bool / str / i64 scalars' '1.5 3.5 true a 7 ' <<'EOF'
f=main():i64{let c=true;let g=if(c){1.5}el{2.5};let b=if(c){true}el{false};
let s=if(c){"a"}el{"b"};let n=if(c){7}el{8};
io.println("\(g) \(g+2.0) \(b) \(s) \(n)");<0};
EOF
runs c2 'a diverging branch leaves the binding as before' '1.5 ' <<'EOF'
f=pick(c:bool):f64{let g=if(c){1.5}el{<0.0};<g};
f=main():i64{io.println("\(pick(true))");<0};
EOF

# ── the binding is judged as the type it now has ────────────────────────
rejects n1 'let x:str=d on an if over f64 maps' '@(str:f64)' <<'EOF'
f=main():i64{let mu=@("k":1.5);let c=true;let d=if(c){mu}el{@("k":0.0)};let x:str=d;io.println(x);<0};
EOF
rejects n2 'let x:str=a on an if over f64 arrays' '@(f64)' <<'EOF'
f=main():i64{let c=true;let a=if(c){@(1.5)}el{@(0.5)};let x:str=a;io.println(x);<0};
EOF
rejects n3 'let x:str=g on an if over f64s' 'f64' <<'EOF'
f=main():i64{let c=true;let g=if(c){1.5}el{2.5};let x:str=g;io.println(x);<0};
EOF
rejects n4 'let x:str=b on an if over bools' 'bool' <<'EOF'
f=main():i64{let c=true;let b=if(c){true}el{false};let x:str=b;io.println(x);<0};
EOF
rejects n5 'let x:str=p on an if over structs' 'pt' <<'EOF'
t=$pt{x:i64;y:i64};
f=main():i64{let c=true;let p=if(c){$pt{x:3;y:4}}el{$pt{x:0;y:0}};let x:str=p;io.println(x);<0};
EOF
rejects n6 'let x:str=d on an el-if chain of maps' '@(str:f64)' <<'EOF'
f=main():i64{let c=true;let d=if(c){@("k":1.5)}el if(c){@("k":2.0)}el{@("k":0.0)};let x:str=d;io.println(x);<0};
EOF
# A str if-expression is a str: assigning it to a str is clean.
printf 'm=c3;i=io:std.io;\nf=main():i64{let c=true;let s=if(c){"a"}el{"b"};let x:str=s;io.println(x);<0};\n' > c3.tk
out="$("${TKC}" --check c3.tk 2>&1)"; rc=$?
if [ "${rc}" -eq 0 ]; then pass 'let x:str=s on an if over strs is clean'
else fail "c3: want a clean --check, got exit ${rc}: ${out}"; fi

echo "---------------------------------------------------------------------------"
echo "C960: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
