#!/usr/bin/env bash
# C581_narrow_int_wraps.sh — sized integers wrap in two's complement; i64 and
# u64 trap RT002 (story 121.36, ADR-0012 c).
#
# THE CLAIM.  ADR-0012 (c) chose "document narrow-int wrap as defined".  Until
# 121.36 nothing documented it, and docs/known-limitations.md §8 said the
# opposite ("All integer arithmetic is checked").  docs/spec/memory-model.md
# §6.6 now states the measured behaviour; this script is what keeps that
# sentence true.  Plain LLVM add/sub/mul (no nsw/nuw) is defined to wrap, so
# the result must be identical at -O0 and -O2.
#
# WHAT THIS DRIVES.  +, -, * past the edge of i8, i16, i32, u8, u16, u32
# (wrap, exit 0, exact value) and of i64, u64 (RT002, exit 1), at -O0 and
# -O2.  Operands come from a function call so nothing is folded.
#
# Negative control: in src/llvm.c, widen the checked-arithmetic gate
# `!strcmp(ity, "i64")` to `(!strcmp(ity, "i64") || !strcmp(ity, "i32"))` —
# the i32 cases fail (the intrinsic call does not type-check at i32, so the
# program no longer builds).
#
# Story: 121.36

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
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c581_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C581: narrow integers wrap, i64/u64 trap RT002"
echo "----------------------------------------------"

# case NAME TYPE A B OP WANT_EXIT WANT (stdout, or the stderr prefix on a trap)
case_() {
    local n="$1" ty="$2" a="$3" b="$4" op="$5" we="$6" want="$7" o
    cat > "${WORK}/${n}.tk" <<EOF
m=c581;
i=io:std.io;
f=ix(n:i64):i64{<n};
f=main():i64{
  let a:${ty}=ix(${a}) as ${ty};
  let b:${ty}=ix(${b}) as ${ty};
  let c:${ty}=a${op}b;
  io.println("\(c)");
  <0
};
EOF
    for o in -O0 -O2; do
        if ! "${TKC}" --allow-all "${o}" --out "${WORK}/${n}" "${WORK}/${n}.tk" >"${WORK}/${n}.b" 2>&1; then
            fail "${n} ${o}: did not build: $(head -c 200 "${WORK}/${n}.b")"; continue
        fi
        "${WORK}/${n}" >"${WORK}/${n}.out" 2>"${WORK}/${n}.err" </dev/null
        local rc=$? out; out="$(cat "${WORK}/${n}.out")"
        if [ "${we}" = 0 ] && [ "${rc}" = 0 ] && [ "${out}" = "${want}" ]; then
            pass "${n} ${o}: ${want}"
        elif [ "${we}" != 0 ] && [ "${rc}" = "${we}" ] && [ -z "${out}" ] &&
             awk -v p="${want}" 'index($0,p)==1{f=1} END{exit !f}' "${WORK}/${n}.err"; then
            pass "${n} ${o}: exit ${rc}, ${want}"
        else
            fail "${n} ${o}: exit ${rc} (want ${we}), stdout [${out}], stderr [$(head -1 "${WORK}/${n}.err")] (want ${want})"
        fi
    done
}

case_ i8_add   i8  127        1     + 0 "-128"
case_ i8_sub   i8  "0-128"    1     - 0 "127"
case_ i8_mul   i8  100        2     '*' 0 "-56"
case_ i16_add  i16 32767      1     + 0 "-32768"
case_ i32_add  i32 2147483647 1     + 0 "-2147483648"
case_ i32_mul  i32 65536      65536 '*' 0 "0"
case_ u8_add   u8  255        1     + 0 "0"
case_ u8_sub   u8  0          1     - 0 "255"
case_ u16_add  u16 65535      1     + 0 "0"
case_ u32_add  u32 4294967295 1     + 0 "0"
case_ i64_add  i64 9223372036854775807 1 + 1 "RT002: integer overflow in addition"
case_ u64_sub  u64 0          1     - 1 "RT002: integer overflow in subtraction"

echo ""
echo "C581: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
