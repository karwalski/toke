#!/usr/bin/env bash
# C126_min_by_minus_one_traps.sh — the most negative value `/` or `%` -1 traps
# RT004 (story 121.36 via 127.217, ADR-0012 b).
#
# THE DEFECT.  Only a zero divisor was guarded.  `INT64_MIN / -1` is LLVM
# undefined behaviour (the quotient does not fit), and the build was not
# self-consistent: -O0 printed INT64_MIN, -O2 printed 0.  `srem` has the same
# UB.  Codegen now checks (lhs == MIN && rhs == -1) for sdiv and srem at the
# operand width and calls tk_div_trap with a code that names the overflow.
#
# WHAT THIS DRIVES.  `/` and `%` at i64 with call-produced operands, `/` at
# i8 (MIN = -128), and an all-literal expression; each at -O0 and -O2.  A
# control program pins the neighbours that must NOT trap: MIN/2, 7/-1,
# -7%-1, MIN%3.
#
# Negative control: delete the `dv_trap` guard block in src/llvm.c — the
# four trap cases fail (they exit 0 with an optimiser-dependent quotient).
#
# Story: 121.36 / 127.217

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c126_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C126: MIN / -1 and MIN % -1 trap RT004"
echo "--------------------------------------"

prog() {
    cat > "${WORK}/$1.tk" <<EOF
m=c126;
i=io:std.io;
f=mn():i64{<0-9223372036854775807-1};
f=m1(n:i64):i64{<n-n-1};
f=main():i64{
$2
  <0
};
EOF
}
prog div64 '  let q=mn()/m1(3);
  io.println("FAIL div");'
prog rem64 '  let q=mn()%m1(3);
  io.println("FAIL rem");'
prog div8  '  let a:i8=0-128 as i8;
  let b:i8=m1(0) as i8;
  let c:i8=a/b;
  io.println("FAIL div8");'
prog lit   '  let q=(0-9223372036854775807-1)/(0-1);
  io.println("FAIL literal");'
prog ok    '  io.println("\(mn()/2) \(7/m1(0)) \((0-7)%m1(0)) \(mn()%3)");'

check() {  # NAME WANT_EXIT WANT_ERR WANT_OUT
    local n="$1" we="$2" werr="$3" wout="$4" o
    for o in -O0 -O2; do
        if ! "${TKC}" --allow-all "${o}" --out "${WORK}/${n}" "${WORK}/${n}.tk" >"${WORK}/${n}.b" 2>&1; then
            fail "${n} ${o}: did not build: $(head -c 200 "${WORK}/${n}.b")"; continue
        fi
        "${WORK}/${n}" >"${WORK}/${n}.out" 2>"${WORK}/${n}.err" </dev/null
        local rc=$? out; out="$(cat "${WORK}/${n}.out")"
        if [ "${rc}" = "${we}" ] && [ "${out}" = "${wout}" ] &&
           { [ -z "${werr}" ] || awk -v p="${werr}" 'index($0,p)==1{f=1} END{exit !f}' "${WORK}/${n}.err"; }; then
            pass "${n} ${o}: exit ${rc}${werr:+, ${werr}}"
        else
            fail "${n} ${o}: exit ${rc} (want ${we}), stdout [${out}] (want [${wout}]), stderr [$(head -1 "${WORK}/${n}.err")]"
        fi
    done
}
check div64 1 "RT004: invalid division: overflow"  ""
check rem64 1 "RT004: invalid remainder: overflow" ""
check div8  1 "RT004: invalid division: overflow"  ""
check lit   1 "RT004: invalid division: overflow"  ""
check ok    0 "" "-4611686018427387904 -7 0 -2"

echo ""
echo "C126: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
