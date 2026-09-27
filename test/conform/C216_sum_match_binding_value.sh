#!/usr/bin/env bash
# C216_sum_match_binding_value.sh — `let v=mt r{$tag:v v; ...}` over a
# SUM-type value binds the variant's payload with the payload's type
# (story 127.220 item 1).
#
# PREMISE CORRECTED.  The row blamed `arr.get(0).get(1)` for
# fixtures/roundtrip/struct_literal.tk stopping at E9002.  It is typed and
# lowers correctly on its own (`<arr.get(0).get(1)+1` -> 3).  The E9002 came
# from the line before it: `let v=mt r {$ok:v v; $err:e <0-1}` with
# `t=$res{$ok:i64;$err:str}`.  Both of the backend's result-type inferences
# for a match took a bare-binding arm's type from the SCRUTINEE, which for a
# sum type is the box pointer, so the i64 payload sat in a pointer slot and
# `v+1` was "'+' ... with a pointer operand" (E9002), or E4031 at codegen.
# Now a bare-binding arm of a sum-type match has its variant's slot type
# (i64 for integers/bool, double for f64/f32, i8* for str), the same slot
# the arm binding itself gets.
#
# Negative control: TKC=<main's toke> -- cases 1 and 2 fail (no binary).
#
# Story: 127.220 (item 1)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() { rm -f "${WORK}/b"; "${TKC}" --out "${WORK}/b" "$1" > "${WORK}/build.log" 2>&1
        if [ -x "${WORK}/b" ]; then "${WORK}/b" 2>&1 | tr '\n' '|'; else echo "no binary"; fi; }

expect "1 fixtures/roundtrip/struct_literal.tk builds and runs" \
    "$(run "${SCRIPT_DIR}/fixtures/roundtrip/struct_literal.tk")" "3 8 3|"

cat > "${WORK}/sm.tk" <<'EOF'
m=q;
i=io:std.io;
t=$res{$ok:i64;$err:str};
t=$fr{$val:f64;$none:i64};
t=$sr{$txt:str;$num:i64};
f=a(r:$res):i64{let v=mt r {$ok:v v; $err:e <0-1}; <v+1};
f=b(r:$fr):f64{let v=mt r {$val:v v; $none:n <0.5}; <v*2.0};
f=c(r:$sr):str{let v=mt r {$txt:v v; $num:n <"num"}; <"\(v)!"};
f=d(r:$res):i64{let v=mt r {$err:e 7; $ok:v v}; <v+1};
f=main():i64{
  io.println("\(a($res{$ok:41})) \(a($res{$err:"x"}))");
  io.println("\(b($fr{$val:1.25}))");
  io.println("\(c($sr{$txt:"hi"})) \(c($sr{$num:3}))");
  io.println("\(d($res{$ok:1})) \(d($res{$err:"y"}))");
  <0
};
EOF
expect "2 i64, f64 and str payloads, first arm or not" "$(run "${WORK}/sm.tk")" "42 -1|2.5|hi! num|2 8|"

cat > "${WORK}/ctl.tk" <<'EOF'
m=q;
f=main():i64{let arr:@(@i64)=@(@(1;2)); <arr.get(0).get(1)+1};
EOF
"${TKC}" --out "${WORK}/c" "${WORK}/ctl.tk" >/dev/null 2>&1; "${WORK}/c"
expect "3 control: arr.get(0).get(1) (the row's premise) was never the defect" "$?" "3"

echo "C216: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
