#!/usr/bin/env bash
# C300_cross_module_return_types.sh — a value returned from another toke
# module arrives intact for every scalar return type (story 138.5, loke T-20).
#
# THE DEFECT. The importer registered every non-void function of another
# module as returning i64 ("floats via bitcast" said the comment), but the
# DEFINING module returns f64 as double, f32 as float, bool as i1 and narrow
# integers as i8/i16/i32.  So `declare fastcc i64 @lib_f64abs(double)` read
# the integer register: loke's T-20 exited 5 instead of 55, `lib.neg(2.5)`
# printed 4381514752, and a u8/i32 return was never truncated (`lib.b8(456)`
# gave 456, `lib.w(4294967301)` gave 4294967301).  llvm.c now declares the
# definer's type and zero-extends an unsigned narrow return at the call.
#
# Negative control: TKC built from main at c15dd11 — cases 1, 2, 3 red
# ("5", garbage f64/f32, "456 4294967301"); case 4 (str, i64) green on both.
#
# Story: 138.5

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
cd "${WORK}" || exit 1
cat > lib.tk <<'EOF'
m=lib;
f=f64abs(n:f64):f64{ if(n<0.0){ <0.0-n }; <n };
f=half(n:f32):f32{ <n/(2.0 as f32) };
f=big(n:i64):bool{ <n>100 };
f=w(n:i64):i32{ <n as i32 };
f=sw(n:i64):i16{ <n as i16 };
f=b8(n:i64):u8{ <n as u8 };
f=s(n:i64):str{ <"s\(n)" };
f=dbl(n:i64):i64{ <n*2 };
EOF
"${TKC}" --emit-interface --check -I . lib.tk >/dev/null 2>&1
build() { rm -f b; "${TKC}" -I . --out b "$1" lib.tk >/dev/null 2>&1 && ./b 2>/dev/null | tr '\n' ' '; echo "rc=$?"; }

# 1. loke T-20: an f64 across the boundary (exit 55)
cat > t20.tk <<'EOF'
m=main;
i=lib:lib;
f=localabs(n:f64):f64{ if(n<0.0){ <0.0-n }; <n };
f=main():i64{
  let x=lib.f64abs(0.0-50.0) as i64;
  let y=localabs(0.0-50.0) as i64;
  <(x/10)*10+(y/10)
};
EOF
rm -f b; "${TKC}" -I . --out b t20.tk lib.tk >/dev/null 2>&1; ./b; expect "1-T20-f64-return" "$?" "55"

# 2. f64 and f32 values
cat > p2.tk <<'EOF'
m=main;
i=lib:lib;
i=io:std.io;
f=main():i64{
  let a=lib.f64abs(0.0-2.5);
  let h=lib.half(5.0 as f32) as f64;
  io.println("\(a) \(h)");
  <0
};
EOF
expect "2-f64-f32-values" "$(build p2.tk)" "2.5 2.5 rc=0"

# 3. bool and narrow integers are truncated and extended by their type
cat > p3.tk <<'EOF'
m=main;
i=lib:lib;
i=io:std.io;
f=main():i64{
  let a=mut.456;
  io.println("\(lib.b8(a)) \(lib.w(4294967301)) \(lib.w(0-5)) \(lib.sw(40000)) \(lib.big(500)) \(lib.big(5))");
  let d=lib.b8(200);
  if(lib.big(1)){io.println("WRONG")};
  io.println("\(d)");
  <0
};
EOF
expect "3-bool-and-narrow" "$(build p3.tk)" "200 5 -5 -25536 true false 200 rc=0"

# 4. control: str and i64 returns (unchanged)
cat > p4.tk <<'EOF'
m=main;
i=lib:lib;
i=io:std.io;
f=main():i64{ io.println("\(lib.s(3)) \(lib.dbl(21))"); <0 };
EOF
expect "4-str-i64-control" "$(build p4.tk)" "s3 42 rc=0"

echo "C300: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
