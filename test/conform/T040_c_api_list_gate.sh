#!/usr/bin/env bash
# T040_c_api_list_gate.sh — every tk_* symbol nothing reaches is decided in
# scripts/stdlib_c_api.txt, and no decision there is stale (story 136.73).
#
# docs/reference/stdlib-internal.md classed 34 symbols `dead` that were
# documented C APIs (tk_log_*, tk_webview_*, tk_test_*, ...) or were called
# through a header macro (tk_cap_check / tk_cap_deny via TK_REQUIRE).  The
# generator now has a `c-api` class driven by a cited list, follows header
# macros, and `check_tki_coverage.py --internal-md-check` (make check-tki)
# fails the way check-core-reach does:
#   UNTRIAGED  a dead symbol with no line
#   STALE      a line whose symbol is gone, or is now reached another way
#   BADCITE    a c-api line citing nothing, or a file that does not name it
#
# Case 1 runs the real list; 2-6 run a mutated copy (--c-api-list), never
# the tracked file.  Negative control: on main 7ca0e318 the script has no
# --c-api-list and no c-api class, so 1 and 2-6 are red.
#
# Story: 136.73

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; echo "        $2"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
gate() { (cd "${REPO_ROOT}" && python3 scripts/check_tki_coverage.py --internal-md-check "$@" 2>&1); }

# 1. the tracked list passes, and the page shows the c-api class
OUT="$(gate)"; RC=$?
if [ $RC -eq 0 ] && grep -q '^### `c-api`' "${REPO_ROOT}/docs/reference/stdlib-internal.md"; then
    ok "1-real-list-passes"
else bad "1-real-list-passes" "rc=${RC}: $(echo "${OUT}" | tail -3)"; fi
# ...and TK_REQUIRE's callees are c-internal, not dead
if grep -q '^| `tk_cap_check` | `capabilities.c` | via `TK_REQUIRE`' "${REPO_ROOT}/docs/reference/stdlib-internal.md"; then
    ok "1-macro-callee-c-internal"
else bad "1-macro-callee-c-internal" "tk_cap_check not classed c-internal via TK_REQUIRE"; fi

L="${WORK}/list.txt"
sed -e '/^tk_array_retain /d' \
    -e 's#^tk_webview_set_title c-api src/stdlib/webview.h,#tk_webview_set_title c-api docs/stdlib/infer.md,#' \
    "${REPO_ROOT}/scripts/stdlib_c_api.txt" > "${L}"
printf '%s\n' 'tk_no_such_symbol dead   # gone' \
              'tk_cap_check c-api src/stdlib/capabilities.h   # reached via TK_REQUIRE' \
              'tk_json_print_arr nope   # bad class' >> "${L}"
OUT="$(gate --c-api-list "${L}")"; RC=$?
[ $RC -ne 0 ] && ok "2-mutated-list-fails" || bad "2-mutated-list-fails" "rc=0"
want() { if echo "${OUT}" | grep -q "$2"; then ok "$1"; else bad "$1" "no line matching: $2"; fi; }
want "3-untriaged-dead"        '^UNTRIAGED  tk_array_retain '
want "4-stale-gone"            '^STALE      tk_no_such_symbol .*no longer defined'
want "4-stale-now-reached"     '^STALE      tk_cap_check .*now c-internal'
want "5-badcite-wrong-file"    '^BADCITE    .*tk_webview_set_title cites docs/stdlib/infer.md, which does not name it'
want "6-badline-class"         '^BADLINE    .*tk_json_print_arr nope'

echo ""
echo "T040: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
