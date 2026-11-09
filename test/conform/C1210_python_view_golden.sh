#!/usr/bin/env bash
# C1210_python_view_golden.sh — `tkc --python-view` renders a representative
# set of programs exactly as the checked-in golden files say (story 96.7).
#
# The set covers every construct the design note maps
# (docs/decisions/96.7-python-view.md): module/import, struct and sum types,
# constants, module-level `let`, functions with `T!E`, `if`/`el if`/`el`,
# both `lp` forms and `br`, `mt` (statement, value and in an argument, with a
# `<` arm), expression-`if` as a conditional expression and as a routed
# statement, closures as `def`, lifted `_fnN`/`_ifN`/`_mtN` helpers, casts,
# `!` propagation, `.get` indexing, array/map/struct/variant literals,
# interpolated and multi-line strings, `&f` references, `sc`/`spawn`, and
# identifiers that are Python keywords — plus four of the shipped examples/.
#
# Golden files live in test/conform/fixtures/python_view/expected/.  After a
# deliberate rendering change, regenerate them with
#   PYVIEW_BLESS=1 bash test/conform/C1210_python_view_golden.sh
# and review the diff before committing it.
#
# Negative control: TKC built from toke main 031d72fd has no --python-view
# ("tkc: unknown flag: --python-view", rc 3), so all 7 cases are red there.
#
# Story: 96.7

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
FIX="${SCRIPT_DIR}/fixtures/python_view"
EXP="${FIX}/expected"
PASS=0; FAIL=0
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# source path (relative to the repo) -> golden file name
CASES=(
    "test/conform/fixtures/python_view/errors.tk:errors.py"
    "test/conform/fixtures/python_view/lifting.tk:lifting.py"
    "test/conform/fixtures/python_view/edges.tk:edges.py"
    "examples/mortgage/calc.tk:examples_mortgage_calc.py"
    "examples/tkgrep/main.tk:examples_tkgrep_main.py"
    "examples/bookmarks-api/api.tk:examples_bookmarks_api_api.py"
    "examples/datapipe/main.tk:examples_datapipe_main.py"
)

for c in "${CASES[@]}"; do
    src="${c%%:*}"; gold="${c##*:}"
    out="${WORK}/${gold}"
    "${TKC}" --python-view "${REPO_ROOT}/${src}" > "${out}" 2>/dev/null
    rc=$?
    if [ "${PYVIEW_BLESS:-}" = "1" ] && [ "${rc}" -eq 0 ]; then
        cp "${out}" "${EXP}/${gold}"
    fi
    if [ "${rc}" -ne 0 ]; then
        echo "FAIL ${src}: --python-view exited ${rc}"; FAIL=$((FAIL + 1))
    elif [ ! -f "${EXP}/${gold}" ]; then
        echo "FAIL ${src}: no golden ${gold}"; FAIL=$((FAIL + 1))
    elif diff -u "${EXP}/${gold}" "${out}" > "${WORK}/d" 2>&1; then
        echo "PASS ${src}"; PASS=$((PASS + 1))
    else
        echo "FAIL ${src}: differs from ${gold}"; head -40 "${WORK}/d"; FAIL=$((FAIL + 1))
    fi
done

echo "C1210: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
