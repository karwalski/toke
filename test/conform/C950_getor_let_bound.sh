#!/usr/bin/env bash
# C950_getor_let_bound.sh -- a binding `let d=mm.getor(k;D)` carries the
# map's value type V, as the same call does in place (story 127.325).
#
# THE DEFECT (67113149).  127.320 typed `m.getor(k;d)` in place only
# (infer, map_getor_type).  bind_init_type's NODE_CALL_EXPR case consulted
# call_decl_ret_type alone, so a let-bound getor result was TY_UNKNOWN.  On a
# map of maps that lost the inner value type: `let d=mu.getor("a";D);
# d.getor("k";0.5)` printed 4609434218613702656 (1.5's bits) and the miss
# 4602678819172646912, a bool inner map printed 1, a map of str arrays
# printed an address, and `let x:str=d` checked clean.  The same through
# `let x=mut.`, through `let e=d`, and through a reassigned mut binding.
#
# THE FIX.  types.c bind_init_type: after call_decl_ret_type, adopt
# map_getor_type (the repr_value_type family) for `let` and `let mut`.
#
# Negative control: C950_TKC=<other tkc>.  At 67113149 the r1-r6, r8 and n1
# cases fail; r7 and the c* controls pass.
#
# Story: 127.325
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C950_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c950_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C950: let d=m.getor(k;D) binds the map's value type (127.325)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# runs name description expected-output  (main body on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
    if [ "${got}" = "${want}" ]; then pass "${desc} (${want})"
    else fail "${desc}: expected '${want}', got '${got}'"; fi
}

# ── let-bound getor on a map of maps ────────────────────────────────────
runs r1 'let-bound inner f64 map, hit and miss' '1.5 0.5 ' <<'EOF'
f=main():i64{let mu=@("a":@("k":1.5));let d=mu.getor("a";@($str:$f64));
io.println("\(d.getor("k";0.5)) \(d.getor("zz";0.5))");<0};
EOF
runs r2 'let-bound inner bool map' 'true ' <<'EOF'
f=main():i64{let ms=@("a":@("k":true));let e=ms.getor("a";@($str:$bool));
io.println("\(e.getor("k";false))");<0};
EOF
runs r3 'let mut binding' '1.5 ' <<'EOF'
f=main():i64{let mu=@("a":@("k":1.5));let x=mut.mu.getor("a";@($str:$f64));
io.println("\(x.getor("k";0.5))");<0};
EOF
runs r4 'through a second binding let e=d' '1.5 ' <<'EOF'
f=main():i64{let mu=@("a":@("k":1.5));let d=mu.getor("a";@($str:$f64));let e=d;
io.println("\(e.getor("k";0.5))");<0};
EOF
runs r5 'three levels, each let-bound' '2.5 ' <<'EOF'
f=main():i64{let m3=@("a":@("b":@("c":2.5)));
let l1=m3.getor("a";@($str:@($str:$f64)));let l2=l1.getor("b";@($str:$f64));
io.println("\(l2.getor("c";0.5))");<0};
EOF
runs r6 'map of maps of str arrays' 'q ' <<'EOF'
f=main():i64{let ma=@("a":@("x":@("p";"q")));let la=ma.getor("a";@("y":@("w")));
let lb=la.getor("x";@("z"));io.println("\(lb.get(1))");<0};
EOF
runs r7 'getor result to and from a user function' '1.5 true ' <<'EOF'
f=show(d:@($str:$f64)):i64{io.println("\(d.getor("k";0.5))");<0};
f=pick(d:@($str:$bool)):bool{<d.getor("k";false)};
f=main():i64{let mu=@("a":@("k":1.5));let d=mu.getor("a";@($str:$f64));show(d);
let ms=@("a":@("k":true));io.println("\(pick(ms.getor("a";@($str:$bool))))");<0};
EOF
runs r8 'mut binding reassigned from another getor' '9.5 ' <<'EOF'
f=main():i64{let mu=@("a":@("k":1.5));let lf=mut.mu.getor("a";@($str:$f64));
lf=mu.getor("zz";@("k":9.5));io.println("\(lf.getor("k";0.5))");<0};
EOF

# ── controls: scalar values were right before and stay right ────────────
runs c1 'let-bound f64 / str / bool scalars' '2.5 3.5 hi true ' <<'EOF'
f=main():i64{let fm=@("a":2.5);let g=fm.getor("a";0.0);
let s=@("a":"hi");let t=s.getor("a";"no");let bm=@("a":true);let b=bm.getor("a";false);
io.println("\(g) \(g+1.0) \(t) \(b)");<0};
EOF
runs c2 'let-bound inner i64 map' '7 3 ' <<'EOF'
f=main():i64{let mi=@("a":@("k":7));let d=mi.getor("a";@($str:$i64));
io.println("\(d.getor("k";3)) \(d.getor("zz";3))");<0};
EOF

# ── the binding is judged as the V it now has ───────────────────────────
printf 'm=n1;i=io:std.io;\nf=main():i64{let mu=@("a":@("k":1.5));let d=mu.getor("a";@($str:$f64));let x:str=d;io.println(x);<0};\n' > n1.tk
out="$("${TKC}" --check n1.tk 2>&1)"; rc=$?
if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031".*"expected":"str","got":"@(str:f64)"'; then
    pass 'let x:str=d on a map-valued getor binding is E4031'
else fail "n1: want exit 1 and E4031 str vs @(str:f64), got exit ${rc}: ${out}"; fi

echo "---------------------------------------------------------------------------"
echo "C950: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
