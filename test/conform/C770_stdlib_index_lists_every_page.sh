#!/usr/bin/env bash
# C770_stdlib_index_lists_every_page.sh — check_stdlib_doc_exports.py rule 5:
# every docs/stdlib/<stem>.md is linked from docs/stdlib/index.md, and every
# index link names a page that exists (132.52).
#
# THE GAP.  The index was maintained by hand.  check-docs compiled the pages
# and check-tki read them against the interfaces, but nothing asked whether a
# reader could find one: std.pdf, std.xlsx, std.ocr and std.fmt were linked
# from no entry until they were added by hand on 2026-09-23.
#
# Cases:
#   1 the gate is OK on this tree
#   2 index.md without the std.fmt entry (a copy) -> FAIL naming fmt.md
#   3 index.md with a link to a page that is not there -> FAIL naming it
#   4 a docs copy with no index.md -> exit 2 (refuses to pass), not 0
#
# Negative control: main's gate before 132.52 (C770_GATE=<copy>) passes case 1
# and fails cases 2-4 -- it exits 0 on all three copies (measured).
#
# Story: 132.52
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GATE="${C770_GATE:-${REPO_ROOT}/scripts/check_stdlib_doc_exports.py}"
PASS=0; FAIL=0
pass() { echo "PASS $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

if python3 "${GATE}" > "${WORK}/g1" 2>&1; then pass "1-gate-ok"
else fail "1-gate-ok"; head -8 "${WORK}/g1"; fi

fresh() { rm -rf "${WORK}/docs"; cp -R "${REPO_ROOT}/docs/stdlib" "${WORK}/docs"; }

fresh
grep -v '/docs/stdlib/fmt/' "${WORK}/docs/index.md" > "${WORK}/i" && mv "${WORK}/i" "${WORK}/docs/index.md"
python3 "${GATE}" --docs "${WORK}/docs" > "${WORK}/g" 2>&1; rc=$?
if [ "${rc}" -eq 1 ] && grep -qF "docs/stdlib/fmt.md is linked from no entry" "${WORK}/g"; then pass "2-unlisted-page"
else fail "2-unlisted-page (rc=${rc})"; head -6 "${WORK}/g"; fi

fresh
printf -- '- [std.nosuchmod](/docs/stdlib/nosuchmod/) -- x\n' >> "${WORK}/docs/index.md"
python3 "${GATE}" --docs "${WORK}/docs" > "${WORK}/g" 2>&1; rc=$?
if [ "${rc}" -eq 1 ] && grep -qF "links /docs/stdlib/nosuchmod/, which has no page" "${WORK}/g"; then pass "3-dangling-link"
else fail "3-dangling-link (rc=${rc})"; head -6 "${WORK}/g"; fi

fresh
rm "${WORK}/docs/index.md"
python3 "${GATE}" --docs "${WORK}/docs" > "${WORK}/g" 2>&1; rc=$?
if [ "${rc}" -eq 2 ]; then pass "4-no-index-refuses"
else fail "4-no-index-refuses (rc=${rc})"; head -6 "${WORK}/g"; fi

echo "C770: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
