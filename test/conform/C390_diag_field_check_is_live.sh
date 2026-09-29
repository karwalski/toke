#!/usr/bin/env bash
# C390_diag_field_check_is_live.sh — run_conform.sh's expected_diag_field
# check actually reads the block it is given (story 127.239).
#
# THE DEFECT.  run_conform.sh asked yaml_get for "expected_diag_field.field"
# as one flat key.  The D-series files write it as a nested mapping
#   expected_diag_field:
#     field: "schema_version"
#     value: "1.0"
# so the lookup returned "", which the runner read as "no check requested",
# and all 13 files that carry the block asserted nothing.  D006 with its value
# changed to "9.9" still passed; D011 had asserted a field ("code") the
# emitter has never written, unnoticed since the suite was scaffolded.
#
# WHAT THIS DRIVES.  The real runner, copied into a scratch tree beside one
# case at a time:
#   D006 as shipped (schema_version 1.0)          -> PASS
#   D006 with value "9.9"                         -> FAIL, names the field
#   a nested field (pos.line) with a wrong value  -> FAIL
#   the block declared with a misspelt child key  -> FAIL (not a silent skip)
#
# Negative control: point C390_RUNNER at main's test/run_conform.sh from
# before 127.239 — the "9.9" case passes, as do the other two wrong cases.
#
# Story: 127.239

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
RUNNER="${C390_RUNNER:-${REPO_ROOT}/test/run_conform.sh}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c390_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C390: expected_diag_field is checked, not skipped"
echo "-------------------------------------------------"

mkdir -p "${WORK}/test/diagnostics"
cp "${RUNNER}" "${WORK}/test/run_conform.sh"

case_file() {  # case_file <field> <value> [childkey] -> writes the one case
    local ck="${3:-field}"
    rm -f "${WORK}/test/diagnostics/"*.yaml
    cat > "${WORK}/test/diagnostics/DX.yaml" <<EOF
id: DX
series: D
description: "C390 probe"
input: |
  m=test;
  f=bad():Str{<"\\q"};
expected_exit_code: 1
expected_error_codes: ["E1001"]
expected_diag_field:
  ${ck}: "$1"
  value: "$2"
EOF
}

run() {  # -> OUT, RC
    OUT="$(TKC="${TKC}" bash "${WORK}/test/run_conform.sh" 2>&1)"
    RC=$?
}

case_file schema_version 1.0
run
if [ "${RC}" -eq 0 ] && printf '%s\n' "${OUT}" | grep -q '^PASS DX'; then
    pass "schema_version 1.0 passes"
else
    fail "schema_version 1.0 should pass (rc=${RC}): ${OUT}"
fi

case_file schema_version 9.9
run
if [ "${RC}" -ne 0 ] && printf '%s\n' "${OUT}" | grep -q "diag field 'schema_version' expected '9.9' not found"; then
    pass "schema_version 9.9 fails and names the field"
else
    fail "schema_version 9.9 should fail (rc=${RC}): ${OUT}"
fi

case_file pos.line 7
run
if [ "${RC}" -ne 0 ] && printf '%s\n' "${OUT}" | grep -q "diag field 'pos.line' expected '7' not found"; then
    pass "nested pos.line 7 (actual 2) fails"
else
    fail "nested pos.line 7 should fail (rc=${RC}): ${OUT}"
fi

case_file severity error feild
run
if [ "${RC}" -ne 0 ] && printf '%s\n' "${OUT}" | grep -q "expected_diag_field is declared but its field/value could not be read"; then
    pass "a misspelt child key fails instead of skipping"
else
    fail "a misspelt child key should fail (rc=${RC}): ${OUT}"
fi

echo "-------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
