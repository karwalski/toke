#!/usr/bin/env bash
# F003_fmt_roundtrip.sh — `--fmt` output must be a faithful program (131.37).
#
# For test/standalone/test_expr_if.tk plus five frozen v0.4 corpus records
# (test/conform/fixtures/roundtrip/*.tk, all using expression-form `if`):
#   1. `tkc --fmt X` exits 0.
#   2. The formatted text passes `tkc --check` (no E2003 from a dropped `;`,
#      no bare `<if`, no legacy `[..]`, `==` kept, `$T` kept).
#   3. `tkc --min` of the formatted text equals `tkc --min` of the original,
#      modulo the OPTIONAL `;` before a closing `}` and at end of file — --min
#      is token-faithful to the input and --fmt emits ';' as a separator, so
#      `x;}` and `x}` are the same program.
#   4. --fmt is idempotent: fmt(fmt(X)) == fmt(X).
#
# Story: 131.37

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${REPO_ROOT}/tkc"

PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "${expected}" = "${actual}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "    expected:"; echo "${expected}" | sed 's/^/      /' | cut -c1-200
        echo "    actual:";   echo "${actual}"   | sed 's/^/      /' | cut -c1-200
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

echo "F003: --fmt round-trip (check clean, --min identical, idempotent)"
echo "--------------------------------------"

WORK="$(mktemp -d /tmp/tkc_fmtrt_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

# normalise a --min line: optional ';' before '}' and at end of input
norm_min() { sed -e 's/;}/}/g' -e 's/;$//'; }

roundtrip() {
    local src="$1" name
    name="$(basename "${src}")"
    local rc=0
    "${TKC}" --fmt "${src}" > "${WORK}/fmt.tk" 2>/dev/null || rc=$?
    check "${name}: --fmt exit 0" "0" "${rc}"
    rc=0
    "${TKC}" --check "${WORK}/fmt.tk" >/dev/null 2>"${WORK}/err" || rc=$?
    check "${name}: formatted output --check clean" "0" "${rc}"
    [ "${rc}" -eq 0 ] || head -3 "${WORK}/err" | sed 's/^/      /'
    check "${name}: --min(fmt) == --min(orig)" \
          "$("${TKC}" --min "${src}" 2>/dev/null | norm_min)" \
          "$("${TKC}" --min "${WORK}/fmt.tk" 2>/dev/null | norm_min)"
    "${TKC}" --fmt "${WORK}/fmt.tk" > "${WORK}/fmt2.tk" 2>/dev/null || true
    check "${name}: --fmt idempotent" "$(cat "${WORK}/fmt.tk")" "$(cat "${WORK}/fmt2.tk")"
}

roundtrip "${REPO_ROOT}/test/standalone/test_expr_if.tk"
for rec in "${SCRIPT_DIR}"/fixtures/roundtrip/*.tk; do
    roundtrip "${rec}"
done

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
