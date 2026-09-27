#!/usr/bin/env bash
# C223_struct_accum_field_interp.sh — a string element reached through a
# struct field of an element of a `mut.@()` accumulator interpolates as the
# string, not its address (story 135.20 item 4).
#
# THE DEFECT. `let q=mut.@(); q=q.append($row{fields:@("e";"f")})` leaves q's
# element type unknown to the checker, so `"\(q.get(0).fields.get(1))"` — and
# the same read through `let w=q.get(0)` — had no type at the interpolation
# and printed a heap address.  Two gaps in the emitter's fallback tags, both
# closed (llvm.c, 135.20 item 4):
#   * an empty accumulator every self-append of which is a literal of one
#     struct is tagged "@<Struct>", as an array literal of structs is (136.28);
#   * a struct field DECLARED `@str` tags "@str", so its element loads are
#     strings (the $str field rule, 126.6, had no array counterpart).
# Case 4 guards the other direction: an integer-array field of an accumulated
# struct still renders its elements as integers.
#
# Negative control: TKC built from main at c8575da — cases 1, 2 and 3 red
# (each printed a decimal address).
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
m=t;i=io:std.io;
t=$row{fields:@$str;n:i64};
t=$other{items:@i64;k:i64};
f=main():i64{
  let q=mut.@();
  lp(let i=0;i<2;i=i+1){ q=q.append($row{fields:@("e";"f");n:i}) };
  io.println("1 \(q.get(1).fields.get(1)) \(q.get(1).n)");
  let w=q.get(0);
  io.println("2 \(w.fields.get(0))");
  let x=q.get(1).fields.get(0);
  io.println("3 \(x)");
  let o=mut.@();
  o=o.append($other{items:@(7;8);k:1});
  io.println("4 \(o.get(0).items.get(1))");
  <0
};
EOF
if "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" >/dev/null 2>&1; then
    out=$("${WORK}/b" 2>/dev/null)
else
    out="compile-failed"
fi
line() { printf '%s\n' "${out}" | sed -n "$1p"; }
expect "1-chained-str-field-element" "$(line 1)" "1 f 1"
expect "2-via-let-bound-element"     "$(line 2)" "2 e"
expect "3-let-bound-chain"           "$(line 3)" "3 e"
expect "4-int-field-element"         "$(line 4)" "4 8"

echo "C223: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
