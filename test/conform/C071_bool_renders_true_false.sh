#!/usr/bin/env bash
# C071_bool_renders_true_false.sh — a bool renders as `true`/`false`, the same
# way on every path, and `b as str` is valid IR.
#
# Stories: 127.94, 127.15 (owner decision 2026-09-22: "`\(x)` on a bool prints
# `true`/`false`").
#
# THE DEFECT.  Two spellings of one value disagreed inside one program:
#
#     io.println("\(true)")        -> 1
#     io.println(fmt.bool(true))   -> true
#
# and the third spelling did not compile at all: `b as str` passed the i1
# straight to `tk_str_fromi64_w(i64 ...)`, clang rejected the module
# ("'%t9' defined with type 'i1' but expected 'i64'"), and the user saw E9003
# at line 0 with an empty fix.
#
# THE RULE.  Interpolation, `as str` and fmt.bool all render a bool as
# `true`/`false`.  That includes a bool the backend only sees as an i64
# because it crossed the stdlib ABI (a `bool` return declared in a `.tki`) or
# sits in a struct slot (a field declared `bool`); both are read from their
# declarations.  Integers are the control: they must still print digits.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_boolstr_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C071: a bool renders as true/false on every path"
echo "-------------------------------------------------"

# runs NAME EXPECTED SOURCE — build, run, compare stdout exactly.
runs() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "b.tk"
    local out rc=0
    rm -f b_bin
    if ! "${TKC}" --allow-all --out b_bin "b.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        grep -o '"error_code":"[A-Z0-9]*"' compile.log | head -1 | sed 's/^/      /'
        grep -v '^{' compile.log | head -2 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(./b_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "        expected: $(printf '%s' "${expected}" | tr '\n' '|')"
        echo "        got:      $(printf '%s' "${out}" | tr '\n' '|')  (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

echo ""
echo "A. interpolation"
echo ""

runs "literals, a local, a comparison, a negation" "true false true true false" \
'm=t;
i=io:std.io;
f=main():i64{
  let b=true;
  io.println("\(true) \(false) \(b) \(1<2) \(!b)");
  <0
};'

runs "a user function returning bool" "true false" \
'm=t;
i=io:std.io;
f=big(n:i64):bool{<n>10};
f=main():i64{
  io.println("\(big(20)) \(big(5))");
  <0
};'

# A bool that crossed the stdlib i64 ABI: stdlib/str.tki declares
# str.contains -> bool.
runs "a stdlib call whose interface declares bool" "true false" \
'm=t;
i=io:std.io;
i=str:std.str;
f=main():i64{
  io.println("\(str.contains("abc";"b")) \(str.contains("abc";"z"))");
  <0
};'

runs "a struct field declared bool" "true 3" \
'm=t;
i=io:std.io;
t=$p{flag:bool;n:i64};
f=main():i64{
  let p=$p{flag:true;n:3};
  io.println("\(p.flag) \(p.n)");
  <0
};'

echo ""
echo "B. the three spellings agree"
echo ""

# `b as str` did not compile before 127.94 (invalid IR, E9003).
runs "b as str, fmt.bool(b) and \\(b) print the same thing" "true|true|true
false|false|false" \
'm=t;
i=io:std.io;
i=fmt:std.fmt;
f=main():i64{
  let b=true;
  let s=b as str;
  io.println("\(s)|\(fmt.bool(b))|\(b)");
  let c=1>2;
  let d=c as str;
  io.println("\(d)|\(fmt.bool(c))|\(c)");
  <0
};'

runs "a stdlib bool through as str" "false" \
'm=t;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let s=str.contains("abc";"z") as str;
  io.println(s);
  <0
};'

echo ""
echo "C. controls — integers still print digits"
echo ""

runs "integers, an integer as str, str.len" "1 0 42 5
7" \
'm=t;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let one=1;
  let zero=0;
  io.println("\(one) \(zero) \(40+2) \(str.len("hello"))");
  let n=7;
  io.println(n as str);
  <0
};'

echo "-------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
