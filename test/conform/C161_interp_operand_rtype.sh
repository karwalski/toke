#!/usr/bin/env bash
# C161_interp_operand_rtype.sh — an interpolated operand carries the checker's
# type, so it renders and dispatches exactly as the same expression written
# outside the string does (story 127.224, item 1).
#
# THE DEFECT. The parser attaches each `\(..)` expression to its STR_LIT as a
# child (127.24) and the emitter lowers THAT node, reading its rtype to pick
# unsigned rendering (127.217) and receiver glue.  The checker never inferred
# the child: check_interp_composites typed a private re-parse of the segment
# instead.  So on main:
#   * `let e:u8=200 as u8; "\(e)"` printed -56, and `"\(e>small)"` compared
#     signed and printed false;
#   * `"\(b.words.get(1))"` on a struct's `@str` field printed a heap address
#     and `"\(b.idx.len)"` on a map field printed 0.
# The checker now infers the attached child itself (types.c); the re-parse is
# only the fallback for a literal the parser did not attach.
#
# Case 5 guards the other direction: a diagnostic inside an interpolation is
# still reported, once, at its real position.
#
# Negative control: TKC built from main at a705c93 — cases 2 and 4 red
# (printed "-56 -294967296 false" and "<address> 0").
#
# Story: 127.224

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
diags() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){c=substr($0,RSTART+14,RLENGTH-15); match($0,/"line":[0-9]+,"col":[0-9]+/); printf "%s@%s ",c,substr($0,RSTART,RLENGTH)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1-2. unsigned operands render and compare unsigned inside an interpolation
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let e:u8=200 as u8;
  let h:u32=4000000000 as u32;
  let small:u8=100 as u8;
  io.println("\(e) \(h) \(e>small)");
  <0
};
EOF
expect "1-unsigned-interp-checks" "$(diags "${WORK}/p1.tk")" ""
expect "2-unsigned-interp-runs"   "$(runs "${WORK}/p1.tk")" "200 4000000000 true "

# 3-4. a struct-field receiver inside an interpolation dispatches by its type
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
t=$bag{words:@str;idx:@(str:i64)};
f=main():i64{
  let b=$bag{words:@("p";"q");idx:@("k":9)};
  io.println("\(b.words.get(1)) \(b.idx.len)");
  <0
};
EOF
expect "3-field-receiver-interp-checks" "$(diags "${WORK}/p3.tk")" ""
expect "4-field-receiver-interp-runs"   "$(runs "${WORK}/p3.tk")" "q 1 "

# 5. errors inside an interpolation: each reported once, at the operand
cat > "${WORK}/p5.tk" <<'EOF'
m=t;i=io:std.io;
t=$p{x:i64};
f=main():i64{
  let p=$p{x:1};
  io.println("a \(p.y) b");
  io.println("\(1+true)");
  <0
};
EOF
expect "5-interp-diags-once-located" "$(diags "${WORK}/p5.tk")" 'E4025@"line":5,"col":20 E4031@"line":6,"col":18 '

echo "C161: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
