#!/usr/bin/env bash
# C166_cast_to_bool.sh — `x as bool` is `x != 0` for every integer and float
# source (story 127.224 item 6).
#
# THE DEFECT.  The integer path truncated to the LOW BIT (`2 as bool` would
# be false) and re-extended to i64 for storage; the float path converted with
# fptosi and did the same.  Either way an i64 reached the i1 slot, clang
# rejected the module, and `--check`-clean programs failed to build with
# E9003 (`let q=2.5 as bool` was C095's fixture for exactly that).
# NaN is true (fcmp une), as in C.
#
# Negative control: TKC built from main at a705c93 — all three red (build
# fails E9003 "defined with type 'i64' but expected 'i1'").
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
runs() { "${TKC}" ${2:-} "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let x:f64=2.5;
  let b=x as bool;
  let z:f64=0.0;
  let c=z as bool;
  let n:f32=0.5 as f32;
  io.println("\(b) \(c) \(n as bool)");
  <0
};
EOF
expect "1-float-as-bool"     "$(runs "${WORK}/p1.tk")" "true false true "

cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=show(b:bool):str{ if(b){<"T"}; <"F" };
f=main():i64{
  let x:i64=2;
  let b=x as bool;
  let u:u8=0 as u8;
  let w:u8=4 as u8;
  let q=(0-7) as bool;
  if(w as bool){io.println("w")};
  io.println("\(b) \(u as bool) \(w as bool) \(q) \(show(0 as bool)) \(true as bool)");
  <0
};
EOF
expect "2-int-as-bool-O2" "$(runs "${WORK}/p2.tk")" "w true false true true F true "
expect "3-int-as-bool-O0" "$(runs "${WORK}/p2.tk" -O0)" "w true false true true F true "

echo "C166: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
