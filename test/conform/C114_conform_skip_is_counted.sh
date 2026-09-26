#!/usr/bin/env bash
# C114_conform_skip_is_counted.sh — the conform-sh runner counts a SKIP
# (story 127.166).
#
# THE DEFECT.  C031/C032 skipped for a missing Python library by exiting 1,
# so `make conform` failed on any host without openpyxl/reportlab, and the
# runner had no notion of a skip: a skip was either a FAIL or (had it exited
# 0) an indistinguishable PASS.  A suite now skips by printing
# `CONFORM-SKIP: <reason>` and exiting 0; scripts/conform_isolated.sh counts
# and names it, and TKC_CONFORM_NO_SKIP=1 (CI) turns a skip into a failure.
#
# WHAT THIS DRIVES.  The real runner, copied into a scratch tree holding
# three stub suites — one passes, one skips, one fails — and then only the
# pass and skip stubs:
#   pass+skip        -> exit 0, "1 scripts passed, 0 failed, 1 skipped",
#                       "SKIP ...: <reason>"
#   pass+skip+fail   -> exit 1, "1 passed, 1 failed, 1 skipped"
#   pass+skip, NO_SKIP=1 -> exit 1, the skip reported as FAIL
#
# Negative control: run it against main's conform_isolated.sh — the skip
# stub is counted as a pass ("2 scripts passed, 0 failed", no SKIP line).
#
# Story: 127.166

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
RUNNER="${C114_RUNNER:-${REPO_ROOT}/scripts/conform_isolated.sh}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c114_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C114: conform-sh counts a skip"
echo "------------------------------"

mkdir -p "${WORK}/tree/scripts" "${WORK}/tree/test/conform"
cp "${RUNNER}" "${WORK}/tree/scripts/conform_isolated.sh"
ln -s "${TKC}" "${WORK}/tree/tkc"
printf '#!/usr/bin/env bash\necho "Results: 1 passed, 0 failed"\nexit 0\n' > "${WORK}/tree/test/conform/A_pass.sh"
printf '#!/usr/bin/env bash\necho "SKIP: no widget library"\necho "CONFORM-SKIP: widget library not installed"\nexit 0\n' > "${WORK}/tree/test/conform/B_skip.sh"
printf '#!/usr/bin/env bash\necho "  FAIL: stub"\nexit 1\n' > "${WORK}/tree/test/conform/C_fail.sh"

run() {  # run [ENV=VAL...] -> sets OUT, RC
    OUT="$(cd "${WORK}/tree" && env TKC_GATE_SLOT_HELD=1 TKC_GATE_LOCKDIR="${WORK}/locks" "$@" \
           TKC="${WORK}/tree/tkc" bash scripts/conform_isolated.sh sh 2>&1)"
    RC=$?
}

# 1. pass + fail + skip
run env
if [ "${RC}" -ne 0 ] && echo "${OUT}" | /usr/bin/grep -a -q '^conform-sh: 1 scripts passed, 1 failed, 1 skipped$'; then
    pass "pass+skip+fail -> exit ${RC}, 1 passed / 1 failed / 1 skipped"
else
    fail "pass+skip+fail -> exit ${RC}: $(echo "${OUT}" | tail -1)"
fi

# 2. pass + skip
rm "${WORK}/tree/test/conform/C_fail.sh"
run env
if [ "${RC}" -eq 0 ] && echo "${OUT}" | /usr/bin/grep -a -q '^conform-sh: 1 scripts passed, 0 failed, 1 skipped$'; then
    pass "pass+skip -> exit 0, 1 passed / 0 failed / 1 skipped"
else
    fail "pass+skip -> exit ${RC}: $(echo "${OUT}" | tail -1)"
fi
if echo "${OUT}" | /usr/bin/grep -a -q '^SKIP test/conform/B_skip.sh: widget library not installed$'; then
    pass "the skip is named with its reason"
else
    fail "no named SKIP line: $(echo "${OUT}" | /usr/bin/grep -a B_skip)"
fi

# 3. CI mode: a skip fails
run env TKC_CONFORM_NO_SKIP=1
if [ "${RC}" -ne 0 ] && echo "${OUT}" | /usr/bin/grep -a -q '^FAIL test/conform/B_skip.sh: skipped, and TKC_CONFORM_NO_SKIP=1'; then
    pass "TKC_CONFORM_NO_SKIP=1 -> the skip fails the run"
else
    fail "TKC_CONFORM_NO_SKIP=1 -> exit ${RC}: $(echo "${OUT}" | tail -2 | tr '\n' ' ')"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
