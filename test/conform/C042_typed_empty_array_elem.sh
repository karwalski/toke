#!/usr/bin/env bash
# C042_typed_empty_array_elem.sh — a typed-empty array literal `@($T)` has
# element type T, not void.
#
# THE DEFECT. A typed-empty literal carries its element type as a TYPE node
# (`@($f64)` parses as ARRAY_LIT[TYPE_IDENT f64]). The checker's ARRAY_LIT
# case called infer() on that child; infer() has no case for a type node and
# its default returns TY_VOID, so `@($f64)` typed as `@(void)`. That was
# dormant while un-annotated array bindings stayed TY_UNKNOWN; 127.129
# (886bdb0) typed them, and every `let a=mut.@($T); a=a+@(x)` became a FALSE
# E4031 ("expected '@(void)', got '@(f64)'") on a correct program. Measured
# at main 444a55c: test/standalone/test_f64_array.tk stopped compiling
# (diff-codegen BROKEN), and --check over the tree newly rejected
# typed_empty_array.tk plus EDU-085, EDU-098, GAZ-012 and MFG-051 in
# bench/patterns/macro (orig and canon).
#
# WHAT IS ASSERTED:
#   1. The shapes that were falsely rejected --check clean and run with the
#      right output, for $f64, $str, $i64 and a user struct type.
#   2. The element type is really T, not a wildcard: pushing the wrong element
#      type onto a typed-empty array is still E4031. Without this case, a fix
#      that typed the literal @(unknown) would pass case 1 while switching
#      the element check off.
#
# Negative control: run with TKC pointing at a compiler built from 444a55c;
# cases 1a-1c go red (E4031 '@(void)').
#
# Story: regression of 127.129, found under 127.157 (Batch E2)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/f64.tk" <<'EOF'
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let a=mut.@($f64);
  a=a+@(3.0);
  a=a+@(6.0);
  let p=a.get(0)*a.get(1);
  io.println(s.format(p;"%.4f"));
  <0
};
EOF
cat > "${WORK}/str.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let xs=mut.@($str);
  let w=mut."b";
  xs=xs+@("a");
  xs=xs+@(w);
  io.println("\(xs.len()) \(xs.get(1))");
  <0
};
EOF
cat > "${WORK}/i64.tk" <<'EOF'
m=t;
i=io:std.io;
t=$pair{a:i64;b:i64};
f=main():i64{
  let ys=mut.@($i64);
  lp(let i=0;i<3;i=i+1){ys=ys+@(i)};
  let ps=mut.@($pair);
  ps=ps+@($pair{a:1;b:2});
  io.println("\(ys.len()) \(ys.get(2)) \(ps.len())");
  <0
};
EOF
cat > "${WORK}/wrong.tk" <<'EOF'
m=t;
f=main():i64{
  let a=mut.@($f64);
  a=a+@("x");
  <0
};
EOF

run() { "${TKC}" "$1" -o "${WORK}/bin" >/dev/null 2>&1 && "${WORK}/bin" 2>/dev/null; }

expect "1a-f64-typed-empty-checks" "$(codes "${WORK}/f64.tk")" ""
expect "1a-f64-runs"               "$(run "${WORK}/f64.tk")" "18.0000"
expect "1b-str-typed-empty-checks" "$(codes "${WORK}/str.tk")" ""
expect "1b-str-runs"               "$(run "${WORK}/str.tk")" "2 b"
expect "1c-i64-and-struct-checks"  "$(codes "${WORK}/i64.tk")" ""
expect "1c-i64-and-struct-runs"    "$(run "${WORK}/i64.tk")" "3 2 1"
expect "2-wrong-element-still-E4031" "$(codes "${WORK}/wrong.tk")" "E4031 "

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
