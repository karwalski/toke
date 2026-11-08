#!/usr/bin/env bash
# C940_getor_default_type.sh -- the default `d` of `m.getor(k;d)` must be of
# the map's value type V (story 127.324).
#
# THE DEFECT (8a56271d).  Nothing judged getor's default: the receiver-method
# path checked arity only.  The default is what the call yields on a miss,
# through the same i64 slot as a stored value, so a mistyped default lowered
# silently: `@("a":1.5).getor("zz";1)` printed 4.94066e-324 (the int's bits
# read as a double, since 127.320 types the result as f64);
# `@("a":"x").getor("zz";5)` handed 5 to the str reader and exited 139; an
# int default on a map of arrays exited 139 at the first `.len`; a str
# default on an i64 map printed the string's address.
#
# THE FIX.  types.c check_map_getor_default: on a map value (never a module
# alias such as env.getor) whose V is known, a default whose type is known
# must satisfy types_compat(d, V) (integer widening to i64), or be an integer
# literal that narrows (lit_narrows).  No int -> float promotion (ADR-0004);
# an empty `@()` is an empty array, never a map (the empty map is
# `@($k:$v)`, semantics.md 2.1).  E4031 at the default, no `fix` (D305).
#
# Negative control: C940_TKC=<other tkc>.  At 8a56271d, the n* cases fail
# (they check clean) and the p* / e* cases pass.
#
# Story: 127.324
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C940_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c940_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C940: the default of m.getor(k;d) is judged against the map's value type (127.324)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# rejects name description expected got  (main body on stdin)
# The program must fail --check with exactly one E4031 whose expected/got
# name the map's V and the default's type, on the default's line, no fix.
rejects() {
    local name=$1 desc=$2 want_exp=$3 want_got=$4 out rc
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    local n_e n_all
    n_e=$(printf '%s\n' "${out}" | grep -c "\"error_code\":\"E4031\".*default of '.getor'")
    n_all=$(printf '%s\n' "${out}" | grep -c '"error_code"')
    if [ "${rc}" -ne 1 ] || [ "${n_e}" -ne 1 ] || [ "${n_all}" -ne 1 ]; then
        fail "${desc}: want exit 1 and one getor-default E4031, got exit ${rc}, ${n_e}/${n_all}: ${out}"; return
    fi
    if ! printf '%s' "${out}" | grep -qF "\"expected\":\"${want_exp}\",\"got\":\"${want_got}\""; then
        fail "${desc}: expected/got not '${want_exp}'/'${want_got}': ${out}"; return
    fi
    if printf '%s' "${out}" | grep -q '"fix"'; then
        fail "${desc}: carries a fix: ${out}"; return
    fi
    pass "${desc}"
}

# runs name description expected-output  (whole program on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    cat > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
    if [ "${got}" = "${want}" ]; then pass "${desc} (${want})"
    else fail "${desc}: expected '${want}', got '${got}'"; fi
}

# ── rejected: a default of another type ─────────────────────────────────
rejects n1 'int default on an f64 map (read as 4.94e-324)' 'f64' 'i64' <<'EOF'
f=main():i64{let d=@("a":1.5);io.println("\(d.getor("zz";1))");<0};
EOF
rejects n2 'int default on a str map (exit 139)' 'str' 'i64' <<'EOF'
f=main():i64{let sm=@("a":"x");io.println("\(sm.getor("zz";5))");<0};
EOF
rejects n3 'str default on an i64 map (printed an address)' 'i64' 'str' <<'EOF'
f=main():i64{let d=@("a":7);io.println("\(d.getor("zz";"s"))");<0};
EOF
rejects n4 'f64 default on an i64 map' 'i64' 'f64' <<'EOF'
f=main():i64{let d=@("a":7);io.println("\(d.getor("zz";1.5))");<0};
EOF
rejects n5 'int default on a bool map' 'bool' 'i64' <<'EOF'
f=main():i64{let d=@("a":true);io.println("\(d.getor("zz";0))");<0};
EOF
rejects n6 'int default on a map of arrays (exit 139 at .len)' '@(i64)' 'i64' <<'EOF'
f=main():i64{let d=@("a":@(1;2));let x=d.getor("zz";5);io.println("\(x.len)");<0};
EOF
rejects n7 'str array default on a map of i64 arrays' '@(i64)' '@(str)' <<'EOF'
f=main():i64{let d=@("a":@(1;2));io.println("\(d.getor("zz";@("s")).len)");<0};
EOF
rejects n8 'str default on a map of maps' '@(str:i64)' 'str' <<'EOF'
f=main():i64{let d=@("a":@("b":1));io.println("\(d.getor("zz";"s").getor("b";0))");<0};
EOF
rejects n9 'empty @() is an array, not a map (as for an annotated let)' '@(str:i64)' '@(?)' <<'EOF'
f=main():i64{let d=@("a":@("b":1));io.println("\(d.getor("zz";@()).getor("b";0))");<0};
EOF
rejects n10 'map parameter, f64 values, int default' 'f64' 'i64' <<'EOF'
f=g(m:@(str:f64)):f64{<m.getor("k";0)};
f=main():i64{io.println("\(g(@("k":2.5)))");<0};
EOF
rejects n11 'struct field map, i64 values, str default' 'i64' 'str' <<'EOF'
t=$box{tags:@(str:i64)};
f=main():i64{let b=$box{tags:@("k":4)};io.println("\(b.tags.getor("k";"no"))");<0};
EOF
rejects n12 'chained receiver: the inner f64 map, int default' 'f64' 'i64' <<'EOF'
f=main():i64{let mm=@("a":@("b":3.5));io.println("\(mm.getor("a";@($str:$f64)).getor("b";1))");<0};
EOF
rejects n13 'typed default from a binding (an i64 into a str map)' 'str' 'i64' <<'EOF'
f=main():i64{let sm=@("a":"x");let n=3;io.println(sm.getor("q";n));<0};
EOF

# ── accepted: a default of V, for every value type, hit and miss ───────────
runs p1 'str, bool, f64, i64 maps: hit then miss' 'str x d bool true false f64 1.5 2.5 i64 7 -4 ' <<'EOF'
m=p1;i=io:std.io;
f=main():i64{
  let ms=@("a":"x");let mb=@("a":true);let mf=@("a":1.5);let mi=@("a":7);
  io.println("str \(ms.getor("a";"d")) \(ms.getor("zz";"d"))");
  io.println("bool \(mb.getor("a";false)) \(mb.getor("zz";false))");
  io.println("f64 \(mf.getor("a";2.5)) \(mf.getor("zz";2.5))");
  io.println("i64 \(mi.getor("a";0)) \(mi.getor("zz";-4))");
  <0
};
EOF
runs p2 'array maps (i64 and str elements; literal, typed-empty and @() defaults): hit then miss' 'arr 3 3 2 6 sarr 2 y 0 0 ' <<'EOF'
m=p2;i=io:std.io;
f=main():i64{
  let ma=@("a":@(1;2;3));
  let ah=ma.getor("a";@(0));let am=ma.getor("zz";@(5;6));
  io.println("arr \(ah.len) \(ah.get(2)) \(am.len) \(am.get(1))");
  let ms=@("a":@("x";"y"));
  let sh=ms.getor("a";@($str));
  io.println("sarr \(sh.len) \(sh.get(1)) \(ms.getor("zz";@($str)).len) \(ms.getor("zz";@()).len)");
  <0
};
EOF
runs p3 'map-valued maps (literal and typed-empty defaults): hit then miss' 'map 9 4 0 f 3.5 0.5 ' <<'EOF'
m=p3;i=io:std.io;
f=main():i64{
  let mm=@("a":@("b":9));
  let mh=mm.getor("a";@("q":1));let mx=mm.getor("zz";@("q":4));
  io.println("map \(mh.getor("b";0)) \(mx.getor("q";0)) \(mm.getor("zz";@($str:$i64)).getor("b";0))");
  let mf=@("a":@("b":3.5));
  io.println("f \(mf.getor("a";@($str:$f64)).getor("b";0.0)) \(mf.getor("zz";@($str:$f64)).getor("b";0.5))");
  <0
};
EOF
runs p4 'let-bound defaults and results; a u8 value widens into an i64 map' 'let x 2.5 true 12 ' <<'EOF'
m=p4;i=io:std.io;
f=main():i64{
  let ms=@("a":"x");let mf=@("a":1.5);let mb=@("a":true);let mi=@("a":7);
  let ds="d";let df=2.5;let small:u8=12;
  let sh=ms.getor("a";ds);let fm=mf.getor("zz";df);let bm=mb.getor("zz";true);
  io.println("let \(sh) \(fm) \(bm) \(mi.getor("zz";small))");
  <0
};
EOF

# ── unaffected: module calls named getor ─────────────────────────────────
runs e1 'env.getor (a module call) keeps its str default' 'fallback ' <<'EOF'
m=e1;i=io:std.io;i=env:std.env;
f=main():i64{io.println(env.getor("TK_C940_DEFINITELY_UNSET";"fallback"));<0};
EOF

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
