#!/usr/bin/env bash
# F003_fmt_roundtrip.sh — `--fmt` output must be a faithful program (131.37).
#
# For test/standalone/test_expr_if.tk plus the fixtures under
# test/conform/fixtures/roundtrip/ (five frozen v0.4 corpus records using
# expression-form `if`; 131.46 adds struct_literal.tk — `$Name{..}`, `mt`,
# `t=$name{$variant:..}`, `@(@i64)`, `let x:T=`, source parens —
# qualified_type.tk — `mod.$type` — and comments.tk):
#   1. `tkc --fmt X` exits 0.
#   2. The formatted text passes `tkc --check` (no E2003 from a dropped `;`,
#      no bare `<if`, no legacy `[..]`, `==` kept, `$T` kept).
#   3. `tkc --min` of the formatted text equals `tkc --min` of the original,
#      modulo the OPTIONAL `;` before a closing `}` and at end of file — --min
#      is token-faithful to the input and --fmt emits ';' as a separator, so
#      `x;}` and `x}` are the same program.
#   4. --fmt is idempotent: fmt(fmt(X)) == fmt(X).
#   5. (131.46) every `(* … *)` comment survives: the count of `(*` in the
#      formatted text equals the count in the original.
#   6. (131.46) `--pretty` output is a program too: --check clean and the same
#      --min (covers the expression-`if` case the pretty walker lacked).
#
# 131.58 adds two targeted regressions on top of the generic round-trip, for
# the two defects the 131.8 macro-check wave hit before 131.46a landed.  Both
# slipped past --check, so a --check-clean round-trip does not catch them:
#   A. typed_empty_array.tk -- `--fmt` dropped the type sigil from an empty
#      typed array literal (`mut.@($str)` -> `@(str)`, `@($i64)` -> `@(i64)`).
#      The result passes --check and then fails codegen with
#      `use of undefined value '%str'`.  Asserted by counting `@($` and by
#      COMPILING the formatted text, not just checking it.
#   B. match_arms.tk -- `--fmt` rendered `mt` as the removed v0.2 pipe form
#      `x | {ok:v ..}`, which does not re-parse.  Asserted by requiring `mt `
#      in the output and forbidding a `| {` arm block.
#
# Story: 131.37, 131.46, 131.58

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
    check "${name}: comments preserved" \
          "$(grep -o '(\*' "${src}" | wc -l | tr -d ' ')" \
          "$(grep -o '(\*' "${WORK}/fmt.tk" | wc -l | tr -d ' ')"
    rc=0
    "${TKC}" --pretty "${src}" > "${WORK}/pretty.tk" 2>/dev/null || rc=$?
    "${TKC}" --check "${WORK}/pretty.tk" >/dev/null 2>"${WORK}/perr" || rc=$?
    check "${name}: --pretty output --check clean" "0" "${rc}"
    [ "${rc}" -eq 0 ] || head -3 "${WORK}/perr" | sed 's/^/      /'
    check "${name}: --min(pretty) == --min(orig)" \
          "$("${TKC}" --min "${src}" 2>/dev/null | norm_min)" \
          "$("${TKC}" --min "${WORK}/pretty.tk" 2>/dev/null | norm_min)"
}

# 131.58: the formatted text must also CODEGEN, and must keep the two surface
# forms --fmt used to destroy.  `--check` passed on both defects, so these go
# beyond the generic round-trip above.
regression_131_58() {
    local src="$1" name
    name="$(basename "${src}")"
    "${TKC}" --fmt "${src}" > "${WORK}/r.tk" 2>/dev/null || true

    # A: every `@($T)` type sigil survives (dropping it yields `@(i64)`, a
    #    one-element literal naming an undefined value).
    check "${name}: 131.58 empty typed-array sigils kept" \
          "$(grep -o '@(\$' "${src}" | wc -l | tr -d ' ')" \
          "$(grep -o '@(\$' "${WORK}/r.tk" | wc -l | tr -d ' ')"

    # B: `mt` keeps the v0.4 head and never becomes the v0.2 `x | {ok:v ..}`.
    check "${name}: 131.58 mt heads kept" \
          "$(grep -o 'mt ' "${src}" | wc -l | tr -d ' ')" \
          "$(grep -o 'mt ' "${WORK}/r.tk" | wc -l | tr -d ' ')"
    check "${name}: 131.58 no v0.2 pipe-match form" "0" \
          "$(grep -o '| *{' "${WORK}/r.tk" | wc -l | tr -d ' ')"

    # The defect both shared: --check clean, codegen broken.  Compile it.
    local rc=0
    "${TKC}" -o "${WORK}/r.bin" "${WORK}/r.tk" >/dev/null 2>"${WORK}/cerr" || rc=$?
    check "${name}: 131.58 formatted output codegens" "0" "${rc}"
    [ "${rc}" -eq 0 ] || head -3 "${WORK}/cerr" | sed 's/^/      /'

    # Same program: the formatted build must print what the original prints.
    rc=0
    "${TKC}" -o "${WORK}/o.bin" "${src}" >/dev/null 2>&1 || rc=$?
    if [ "${rc}" -eq 0 ]; then
        check "${name}: 131.58 formatted build output matches original" \
              "$("${WORK}/o.bin" 2>&1 || true)" "$("${WORK}/r.bin" 2>&1 || true)"
    fi
}

roundtrip "${REPO_ROOT}/test/standalone/test_expr_if.tk"
for rec in "${SCRIPT_DIR}"/fixtures/roundtrip/*.tk; do
    roundtrip "${rec}"
done

regression_131_58 "${SCRIPT_DIR}/fixtures/roundtrip/typed_empty_array.tk"
regression_131_58 "${SCRIPT_DIR}/fixtures/roundtrip/match_arms.tk"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
