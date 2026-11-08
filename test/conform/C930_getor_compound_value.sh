#!/usr/bin/env bash
# C930_getor_compound_value.sh — `m.getor(k;d)` yields the map's value type
# (story 127.320).
#
# THE DEFECT (cf297baa).  The checker left the result of `m.getor(k;d)`
# untyped, so the emitter read it as the i64 it assumes for an untyped value:
# on a map of arrays `a.getor("z";@($str)).get(0)` printed the string's
# address; on an f64-valued map the double's bit pattern
# (4615063718147915776, or 4.61281e+18 after an int-to-float); on a map of
# maps the inner map lost its value type, so its own getor printed bits; and a
# bool interpolated as 1/0.  getor is the documented non-mt repair for a
# lookup (semantics.md 10.1), so 127.244 could not migrate compound-valued
# chains to it.  Silent wrong output at exit 0.
#
# THE FIX.  types.c map_getor_type: a getor on a map value (never a module
# alias such as env.getor) is typed with the map's value type V when V says
# something about the representation (repr_value_type: str, bool, f64, a map,
# an array of those).  Integers and structs stay as they were.  llvm.c reads
# an f64 result back as a double and a bool one as an i1 (emit_expr and
# expr_llvm_type agree on the same rtype).
#
# Negative control: C930_TKC=<other tkc>.  At cf297baa, cases 1-3, 5-8 fail
# and 4 and 9 (the controls) pass.
#
# Story: 127.320
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C930_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c930_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C930: m.getor(k;d) yields the map's value type (127.320)"
echo "---------------------------------------------------------------------------"

# runs name description expected-output  (source on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    cat > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed"; fi
    if [ "${got}" = "${want}" ]; then echo "  PASS: ${desc} (${want})"; PASS=$((PASS + 1))
    else echo "  FAIL: ${desc}: expected '${want}', got '${got}'"; FAIL=$((FAIL + 1)); fi
}

runs g1 'getor on a map of str arrays, subscripted (direct and interpolated)' 'r s=q ' <<'EOF'
m=g1;i=io:std.io;
f=main():i64{
  let a=@("z":@("q";"r"));
  io.println(a.getor("z";@($str)).get(1));
  io.println("s=\(a.getor("z";@($str)).get(0))");
  <0
};
EOF
runs g2 'getor on a map of f64 arrays, subscripted' '3.5 ' <<'EOF'
m=g2;i=io:std.io;
f=main():i64{let d=@("d":@(1.5;2.5));io.println("\(d.getor("d";@($f64)).get(1)+1.0)");<0};
EOF
runs g3 'getor on a map of maps, then getor on the inner f64 map' '3.5 ' <<'EOF'
m=g3;i=io:std.io;
f=main():i64{let mm=@("a":@("b":3.5));io.println("\(mm.getor("a";@($str:$f64)).getor("b";0.0))");<0};
EOF
runs g4 'control: getor on a map of i64 arrays (worked before)' '8 ' <<'EOF'
m=g4;i=io:std.io;
f=main():i64{let n=@("n":@(5;7));io.println("\(n.getor("n";@($i64)).get(1)+1)");<0};
EOF
runs g5 'getor on an f64 map: hit, miss, let-bound, passed to an f64 parameter' 'x2=3 miss=9.25 half=1.25 ' <<'EOF'
m=g5;i=io:std.io;
f=half(x:f64):f64{<x/2.0};
f=main():i64{
  let d=@("a":1.5;"b":2.5);
  let x=d.getor("a";0.0);
  io.println("x2=\(x*2.0) miss=\(d.getor("zz";9.25)) half=\(half(d.getor("b";0.0)))");
  <0
};
EOF
runs g6 'getor on a bool map interpolates true/false (direct, let-bound, miss)' 'true false nb=false miss=true cond ' <<'EOF'
m=g6;i=io:std.io;
f=main():i64{
  let bm=@("a":true;"b":false);
  io.println("\(bm.getor("a";false)) \(bm.getor("b";true))");
  let nb=bm.getor("b";true);
  io.println("nb=\(nb) miss=\(bm.getor("q";true))");
  if(bm.getor("a";false)){io.println("cond")};
  <0
};
EOF
runs g7 'getor on a lookup of a map of maps (n.get(k).getor(..))' '3.5 ' <<'EOF'
m=g7;i=io:std.io;
f=main():i64{let n=@("o":@("i":2.5));io.println("\(n.get("o").getor("i";0.0)+1.0)");<0};
EOF
runs g8 'getor on a map of arrays of maps' '1.25 ' <<'EOF'
m=g8;i=io:std.io;
f=main():i64{let am=@("k":@(@("z":1.25)));io.println("\(am.getor("k";@(@("":0.0))).get(0).getor("z";0.0))");<0};
EOF
runs g9 'control: getor on str and i64 maps (worked before)' '5 hello 8 ' <<'EOF'
m=g9;i=io:std.io;
f=main():i64{
  let sm=@("s":"hello");let im=@("i":7);
  io.println("\(sm.getor("s";"").len) \(sm.getor("s";""))");
  io.println("\(im.getor("i";0)+1)");
  <0
};
EOF

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
