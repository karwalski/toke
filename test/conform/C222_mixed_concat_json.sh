#!/usr/bin/env bash
# C222_mixed_concat_json.sh — an array concat whose operands' element types
# differ (an i64 literal array and a `[byte]`) is still an array to the JSON
# encoder, inline or through a `let` (story 135.20 item 3).
#
# THE DEFECT. The checker leaves `@(256;0)+s.bytes(..)` untyped, and the
# emitter's fallback tag inference (expr_struct_type_legacy) had no case for
# a binary `+`, so the value carried no array tag.  tk_json_enc_w — the str
# encoder — was picked, and walked the block as a C string:
#   j.enc(@(256;0)+s.bytes("hi"))   -> ""        (256 = 0x100: a NUL first byte)
#   j.enc(s.bytes("hi")+@(256))     -> "h"
# The concat now takes its array operand's tag (llvm.c, 135.20 item 3).
#
# Negative control: TKC built from main at c8575da — cases 1, 2 and 3 red
# ('[""]', '[""]', '["h"]').
#
# Story: 135.20

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

cat > "${WORK}/p.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;i=j:std.json;
f=main():i64{
  io.println("[\(j.enc(@(256;0)+s.bytes("hi")))]");
  let a=@(256;0)+s.bytes("hi");
  io.println("[\(j.enc(a))] \(a.len) \(a.get(2))");
  let b=s.bytes("hi")+@(256);
  io.println("[\(j.enc(b))]");
  io.println("[\(j.enc(@(1;2)+@(3)))]");
  <0
};
EOF
if "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" >/dev/null 2>&1; then
    out=$("${WORK}/b" 2>/dev/null)
else
    out="compile-failed"
fi
line() { printf '%s\n' "${out}" | sed -n "$1p"; }
expect "1-inline-mixed-concat"   "$(line 1)" '[[256,0,104,105]]'
expect "2-let-bound-mixed-concat" "$(line 2)" '[[256,0,104,105]] 4 104'
expect "3-byte-left-operand"     "$(line 3)" '[[104,105,256]]'
expect "4-same-type-concat"      "$(line 4)" '[[1,2,3]]'

echo "C222: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
