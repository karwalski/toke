#!/usr/bin/env bash
# Verify LLVM IR emitted by tkc using llvm-as and opt -verify.
# Runs --emit-llvm on every e2e test file and validates the IR.
set -uo pipefail

PASS=0 FAIL=0
TKC="$(cd "$(dirname "$0")/.." && pwd)/tkc"
DIR="$(cd "$(dirname "$0")" && pwd)/e2e"
TMPDIR="${TMPDIR:-/tmp}"

# Find llvm tools — try versioned names first (Ubuntu packages)
LLVM_AS=""
OPT=""
for suffix in "" "-18" "-17" "-16" "-15" "-14"; do
  if command -v "llvm-as${suffix}" >/dev/null 2>&1; then
    LLVM_AS="llvm-as${suffix}"
    break
  fi
done
for suffix in "" "-18" "-17" "-16" "-15" "-14"; do
  if command -v "opt${suffix}" >/dev/null 2>&1; then
    OPT="opt${suffix}"
    break
  fi
done

if [ -z "$LLVM_AS" ]; then
  echo "ERROR: llvm-as not found in PATH"
  exit 1
fi
if [ -z "$OPT" ]; then
  echo "ERROR: opt not found in PATH"
  exit 1
fi

echo "Using: $LLVM_AS, $OPT"

for tk in "${DIR}"/*.tk; do
  name=$(basename "$tk" .tk)
  ll_file="${TMPDIR}/tkc_ir_${name}.ll"
  bc_file="${TMPDIR}/tkc_ir_${name}.bc"

  # Emit LLVM IR
  if ! "$TKC" --emit-llvm --out "$ll_file" "$tk" 2>/dev/null; then
    echo "SKIP $name (tkc --emit-llvm failed)"
    continue
  fi

  # Assemble IR to bitcode (validates syntax)
  if ! "$LLVM_AS" "$ll_file" -o "$bc_file" 2>"${TMPDIR}/tkc_ir_err_${name}"; then
    echo "FAIL $name (llvm-as rejected IR)"
    cat "${TMPDIR}/tkc_ir_err_${name}"
    FAIL=$((FAIL+1))
    rm -f "$ll_file" "$bc_file" "${TMPDIR}/tkc_ir_err_${name}"
    continue
  fi

  # Verify IR semantics (new pass manager syntax for LLVM 16+)
  if ! "$OPT" -passes=verify "$bc_file" -o /dev/null 2>"${TMPDIR}/tkc_ir_err_${name}"; then
    echo "FAIL $name (opt -verify rejected IR)"
    cat "${TMPDIR}/tkc_ir_err_${name}"
    FAIL=$((FAIL+1))
    rm -f "$ll_file" "$bc_file" "${TMPDIR}/tkc_ir_err_${name}"
    continue
  fi

  echo "PASS $name"
  PASS=$((PASS+1))
  rm -f "$ll_file" "$bc_file" "${TMPDIR}/tkc_ir_err_${name}"
done

echo "---"
echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
