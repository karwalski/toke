#!/usr/bin/env bash
# C220_byte_element_widening.sh — a byte (u8) stored into an array by
# .append/.push/.set widens by ITS OWN signedness, on both lowerings
# (story 135.20, found while making item 2 in-place).
#
# THE DEFECT. Array elements are i64.  A `byte` local lives in an i8 slot, and
#   * the receiver-method lowering (tk_array_append_w / tk_array_set_w, the
#     copying path) passed the raw i8 to an i64 parameter — clang rejected the
#     IR, E9003 "'%tN' defined with type 'i8' but expected 'i64'";
#   * the in-place lowering (114.18) widened through coerce_value, whose
#     narrow-int rule is sext — a byte of 250 was stored as -6.
# The checker cannot say which: it leaves a `let v:byte` local untyped (its
# resolve_type knows `Byte`, not `byte`), so the emitter reads the binding's
# declared type itself (llvm.c widen_elem_i64 / ubind_find).
#
# Negative control: TKC built from main at c8575da — all four red (E9003 at
# clang; case 3 compiled only on the copying path).  With item 2 in-place and
# this widening absent, case 3 printed "3 -6 -4".
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
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1. copying append (the array is aliased, so not linear) of a byte local
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let w=mut.@(1);
  let k=w;
  let b:byte=200 as byte;
  w=w.append(b);
  io.println("\(w.get(1)) \(k.len)");
  <0
};
EOF
expect "1-copying-append-byte" "$(runs "${WORK}/p1.tk")" "200 1 "

# 2. copying set of a byte local
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let q=mut.@(1;2);
  let b:byte=250 as byte;
  q=q.set(0;b);
  let q2=q;
  io.println("\(q.get(0)) \(q2.len)");
  <0
};
EOF
expect "2-copying-set-byte" "$(runs "${WORK}/p2.tk")" "250 2 "

# 3. in-place append into a [byte] accumulator, values >= 128
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;i=s:std.str;
f=main():i64{
  let big=mut.s.bytes("");
  lp(let i=250;i<253;i=i+1){
    let v:byte=i as byte;
    big=big.append(v)
  };
  io.println("\(big.len) \(big.get(0)) \(big.get(2))");
  <0
};
EOF
expect "3-inplace-append-byte" "$(runs "${WORK}/p3.tk")" "3 250 252 "

# 4. a byte parameter, and a cast operand, appended
cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
f=ap(a:@i64;b:byte):@i64{<a.append(b)};
f=main():i64{
  let r=ap(@(0);200 as byte);
  let a=@(1).append(251 as byte);
  io.println("\(r.get(1)) \(a.get(1))");
  <0
};
EOF
expect "4-param-and-cast-byte" "$(runs "${WORK}/p4.tk")" "200 251 "

echo "C220: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
