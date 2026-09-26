#!/usr/bin/env bash
# C180_byte_concat_and_slice.sh — `+` and `.slice` on a `[byte]` the source
# never annotates (story 135.19 items 1 and 2).
#
# THE DEFECT.  The checker does not type stdlib call results, so a local bound
# to `s.bytes(..)` reached codegen as a bare i64 with no backend tag, and every
# array-vs-scalar dispatch guessed "scalar":
#
#   let b=s.bytes("AB"); let e=b+b;   -> llvm.sadd.with.overflow.i64 on the two
#                                        POINTERS; e.len a wild read (garbage,
#                                        a random RT003, or SIGBUS)
#   b.slice(1;2)                      -> tk_str_slice_w, a string slice over the
#                                        array block: SIGSEGV
#
# The same program with `let b:@byte=...` was correct.  The fix gives a
# declared `[byte]` stdlib return the "@byte" tag that annotation gives
# (llvm.c tki_declared_tag) and lets an array-tagged operand decide `+`.
#
# Negative control: TKC built from main at 7c16729 — section 1 prints a
# garbage length and elements (7804176018 ...), section 2 exits 139 (SIGSEGV).
#
# Story: 135.19

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
runs() {
    "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 || { echo "COMPILE-FAIL"; return; }
    "${WORK}/b" 2>/dev/null | LC_ALL=C tr '\n' ' '; echo "rc=${PIPESTATUS[0]}"
}

# 1. concatenation of two unannotated [byte]s, and of a [byte] with itself
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let b=s.bytes("AB");
  let e=b+b;
  io.println("\(e.len) \(e.get(0)) \(e.get(1)) \(e.get(2)) \(e.get(3))");
  let z=s.bytes("");
  let y=z+b;
  io.println("\(y.len) \(y.get(1))");
  io.println(s.frombytes(b+s.bytes("cd")));
  <0
};
EOF
expect "1-unannotated-byte-concat" "$(runs "${WORK}/p1.tk")" "4 65 66 65 66 2 66 ABcd rc=0"

# 2. slice of an unannotated [byte] is an array slice, not a string slice
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let b=s.bytes("ABCD");
  let c=b.slice(1;3);
  io.println("\(c.len) \(c.get(0)) \(c.get(1))");
  let d=b.slice(0;0);
  io.println("\(d.len)");
  <0
};
EOF
expect "2-unannotated-byte-slice" "$(runs "${WORK}/p2.tk")" "2 66 67 0 rc=0"

# 3. the mixed shapes the checker accepts keep working: an [i64] literal plus
#    a [byte], and a [byte] bound as @i64
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let x=@(1;2)+s.bytes("zz");
  io.println("\(x.len) \(x.get(0)) \(x.get(3))");
  let y:@i64=s.bytes("abc");
  io.println("\(y.len) \(y.get(2))");
  <0
};
EOF
expect "3-mixed-and-rebound" "$(runs "${WORK}/p3.tk")" "4 1 122 3 99 rc=0"

# 4. `+` between two plain integers is still arithmetic (the tag must not
#    turn a scalar sum into a concat)
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let b=s.bytes("AB");
  let n=b.len+b.get(0);
  io.println("\(n)");
  <0
};
EOF
expect "4-scalar-plus-unchanged" "$(runs "${WORK}/p4.tk")" "67 rc=0"

echo "C180: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
