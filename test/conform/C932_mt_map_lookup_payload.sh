#!/usr/bin/env bash
# C932_mt_map_lookup_payload.sh — the `$ok` binding of an `mt` over a map
# lookup carries the map's value type (story 127.322).
#
# THE DEFECT (cf297baa).  mt_ok_payload_type answered an identifier or a
# call scrutinee only, so for `mt n.get("o"){$ok:x ..}` (a NODE_INDEX_EXPR)
# the binding `x` was TY_UNKNOWN: on a map of maps `x.get("i")` read the inner
# f64 untyped and printed its bit pattern (4.61281e+18), and the same through
# `let o=mt n.get("o"){$ok:x x;..}`.  Silent wrong output at exit 0.
#
# THE FIX.  Since 127.244 the lookup is judged `V!$err` with slot_lower V;
# mt_ok_payload_type now answers an index scrutinee with V when
# repr_value_type adopts it (str, bool, f64, a map, an array of those) -- the
# same rule getor uses (127.320).
#
# Negative control: C932_TKC=<other tkc>.  At cf297baa cases 1-3 fail and
# 4 (the control) passes.
#
# Story: 127.322
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C932_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c932_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C932: the ok binding of an mt over a map lookup is typed (127.322)"
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

runs p1 'mt over a map-of-maps lookup, nested mt on the binding' '3.5 ' <<'EOF'
m=p1;i=io:std.io;
f=main():i64{
  let n=@("o":@("i":2.5));
  io.println("\(mt n.get("o"){$ok:x mt x.get("i"){$ok:y y+1.0;$err:e 0.0};$err:e 0.0})");
  <0
};
EOF
runs p2 'the binding let-bound, then looked up' '3.5 3.5 ' <<'EOF'
m=p2;i=io:std.io;
f=main():i64{
  let n=@("o":@("i":2.5));
  let o=mt n.get("o"){$ok:x x;$err:e @("":0.0)};
  io.println("\(mt o.get("i"){$ok:z z+1.0;$err:e 0.0})");
  io.println("\(o.getor("i";0.0)+1.0)");
  <0
};
EOF
runs p3 'mt over a lookup of an f64 array, subscripted' '3.5 ' <<'EOF'
m=p3;i=io:std.io;
f=main():i64{let ar=@("r":@(1.5;2.5;3.5));io.println("\(mt ar.get("r"){$ok:a a.get(2);$err:e 0.0})");<0};
EOF
runs p4 'control: str and i64 payloads (worked before)' '5 21 -1 ' <<'EOF'
m=p4;i=io:std.io;
f=main():i64{
  let sm=@("s":"hello");let im=@("i":7);
  io.println("\(mt sm.get("s"){$ok:s s.len;$err:e 0}) \(mt im.get("i"){$ok:v v*3;$err:e 0}) \(mt im.get("zz"){$ok:v v;$err:e 0-1})");
  <0
};
EOF
echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
