#!/usr/bin/env bash
# C162_bind_binary_field_type.sh — a `let` bound from a binary expression or a
# field read has the type of its initialiser (story 127.224 item 2; 127.223
# item 7).
#
# THE DEFECT. bind_init_type had no NODE_BINARY_EXPR case and its
# NODE_FIELD_EXPR case accepted only a map or struct result.  So on main:
#   * `let s=a+a` on u64 was untyped: `s>a` compared signed and printed false
#     for a sum past 2^63;
#   * `let w=v-1; w as u64` drew no W1001 while `(v-1) as u64` did;
#   * `let ns=b.nums` on an `@i64` field was untyped, so `let x:str=ns.get(0)`
#     checked clean.  (The row's premise that this binding broke the
#     cg_recv_field_receiver witness was measured false: the backend already
#     dispatched `ws.get`/`ws.contains` right; only the interpolated cases,
#     C161, needed the checker.  Case 3 pins that behaviour unchanged.)
#
# Negative control: TKC built from main at a705c93 — cases 1, 2, 4 and 5 red.
#
# Story: 127.224, 127.223

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
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

# 1. a u64 sum bound to a let compares unsigned
cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let a:u64=9000000000000000000 as u64;
  let s=a+a;
  let gt:bool=s>a;
  if(gt){io.println("gt")}el{io.println("le")};
  <0
};
EOF
expect "1-u64-sum-let-compares-unsigned" "$(runs "${WORK}/p1.tk")" "gt "

# 2. the same lossy cast through a let as through the expression
cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  lp(let v=0;v<3;v=v+1){ let w=v-1; let z=w as u64; io.println("\(z)") };
  <0
};
EOF
expect "2-binary-let-cast-warns" "$(codes "${WORK}/p2.tk")" "W1001 "

# 3. an @str bound from a struct field dispatches as @str
cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
t=$bag{words:@str};
f=main():i64{
  let b=$bag{words:@("p";"q")};
  let ws=b.words;
  let c:bool=ws.contains("q");
  io.println("\(ws.get(1)) \(c) \(ws.len)");
  <0
};
EOF
expect "3-field-let-array-checks" "$(codes "${WORK}/p3.tk")" ""
expect "3-field-let-array-runs"   "$(runs "${WORK}/p3.tk")" "q true 2 "

# 4. an @i64 bound from a struct field is typed: its element is not a str
cat > "${WORK}/p4f.tk" <<'EOF'
m=t;
t=$bag{nums:@i64};
f=main():i64{
  let b=$bag{nums:@(1;2)};
  let ns=b.nums;
  let x:str=ns.get(0);
  <0
};
EOF
expect "4-field-let-array-elem-mismatch" "$(codes "${WORK}/p4f.tk")" "E4031 "

# 5. a binary let's type still catches a real mismatch
cat > "${WORK}/p4.tk" <<'EOF'
m=t;
f=main():i64{
  let a:i64=1;
  let s=a+2;
  let b:bool=s;
  <0
};
EOF
expect "5-binary-let-type-mismatch" "$(codes "${WORK}/p4.tk")" "E4031 "

echo "C162: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
