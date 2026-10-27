#!/usr/bin/env bash
# C634_every_public_module_has_a_page.sh — check_stdlib_doc_exports.py rule 4:
# every stdlib/*.tki with a public func export has a docs/stdlib page, or is
# in the gate's shrink-only MISSING_PAGES list (121.70 (2)).
#
# THE GAP.  std.os exports 27 functions and had no docs/stdlib/os.md, and
# check-tki (check_stdlib_doc_exports.py, 132.102) said OK: its three rules
# read each PAGE against the interfaces, so a module with no page had nothing
# to disagree with.  std.io was the same.  Thirteen more modules were found
# the same way and are listed, each with why, in MISSING_PAGES.
#
# Cases:
#   1 the gate is OK on this tree
#   2 docs/stdlib without os.md (a copy)  -> FAIL naming stdlib/os.tki
#   3 the gate with MISSING_PAGES['xml'] injected (xml.md exists) -> FAIL:
#     the entry is stale (the list only shrinks).  127.265 emptied the list,
#     so the case loads the gate as a module and injects the entry rather
#     than relying on one the tree carries.
#   4 docs/stdlib without io.md (a copy)  -> FAIL naming stdlib/io.tki
#
# Negative control: main's gate (git show 2aa48c88:scripts/
# check_stdlib_doc_exports.py) over main's docs/stdlib, which has neither
# os.md nor io.md: "check-stdlib-doc-exports: OK -- 54 page(s)" (measured).
#
# Story: 121.70 (2)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
GATE="${C634_GATE:-${REPO_ROOT}/scripts/check_stdlib_doc_exports.py}"
PASS=0; FAIL=0
pass() { echo "PASS $1"; PASS=$((PASS + 1)); }
fail() { echo "FAIL $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

if python3 "${GATE}" > "${WORK}/g1" 2>&1; then pass "1-gate-ok ($(tail -c 80 "${WORK}/g1"))"
else fail "1-gate-ok"; head -8 "${WORK}/g1"; fi

variant() {  # $1 label  $2 command run inside the docs copy  $3 expected line
    rm -rf "${WORK}/docs"; cp -R "${REPO_ROOT}/docs/stdlib" "${WORK}/docs"
    (cd "${WORK}/docs" && eval "$2")
    if python3 "${GATE}" --docs "${WORK}/docs" > "${WORK}/g" 2>&1; then
        fail "$1 (gate stayed OK)"
    elif grep -qF "$3" "${WORK}/g"; then pass "$1"
    else fail "$1 (failed, but not naming it)"; head -6 "${WORK}/g"; fi
}
variant "2-no-os-page"      "rm os.md"             "stdlib/os.tki  27 public exports, no docs/stdlib/os.md"
variant "4-no-io-page"      "rm io.md"             "no docs/stdlib/io.md"

python3 - "${GATE}" > "${WORK}/g3" 2>&1 <<'PYEOF'
import importlib.util, sys
spec = importlib.util.spec_from_file_location("gate", sys.argv[1])
g = importlib.util.module_from_spec(spec); spec.loader.exec_module(g)
g.MISSING_PAGES["xml"] = "injected by C634 case 3"
sys.argv = ["gate"]
sys.exit(g.main())
PYEOF
rc=$?
if [ "${rc}" -eq 1 ] && grep -qF "MISSING_PAGES['xml']: docs/stdlib/xml.md exists now" "${WORK}/g3"; then
    pass "3-stale-entry"
else fail "3-stale-entry (rc=${rc})"; head -6 "${WORK}/g3"; fi
echo "C634: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
