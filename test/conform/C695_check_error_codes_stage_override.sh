#!/usr/bin/env bash
# C695 -- check_error_codes.py reports only real drift in the spec table
# (127.267).
#
# The stage a diagnostic carries is decided per (number, severity): W1001
# (lossy cast) is raised by the type checker, E1001 by the lexer (127.223).
# The gate's table renderer passed the severity; its drift LISTING did not,
# so whenever the generated table in docs/spec/errors.md disagreed for any
# other reason the listing added a false
#     W1001: spec Stage 'type_check', the compiler emits 'lex'
# next to the real drift, pointing whoever ran the gate at a row that was
# right.
#
# Each case copies the gate and the files it reads into a scratch tree,
# plants one drift in docs/spec/errors.md, and requires the listing to be
# exactly that drift plus the "run: ... --write-spec" hint.
#
# Negative control: C695_CHECKER=<main 13f1d3ae's scripts/check_error_codes.py>
# -- cases 2 and 4 fail (the false W1001 line is listed beside the real
# drift) and case 3 fails the other way: a REAL W1001 stage drift was never
# named, only "generated table is out of date".  Case 1 passes on both.
#
# Story: 127.267

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CHECKER="${C695_CHECKER:-${REPO_ROOT}/scripts/check_error_codes.py}"
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# A scratch tree holding exactly what the gate reads.
T="${WORK}/t"
mkdir -p "${T}/scripts" "${T}/docs/reference" "${T}/docs/spec"
cp "${CHECKER}" "${T}/scripts/check_error_codes.py"
cp "${REPO_ROOT}/docs/reference/errors.md" "${T}/docs/reference/"
cp "${REPO_ROOT}/docs/spec/errors.md" "${T}/docs/spec/errors.md.orig"
( cd "${REPO_ROOT}" && find src -name '*.[ch]' -print ) | while IFS= read -r f; do
    mkdir -p "${T}/$(dirname "${f}")"; cp "${REPO_ROOT}/${f}" "${T}/${f}"
done

# run SED-EXPR: plant the drift, run the gate, print "rc=N" and the spec
# listing (the indented lines under the docs/spec/errors.md header).
run() {
    sed "$1" "${T}/docs/spec/errors.md.orig" > "${T}/docs/spec/errors.md"
    python3 "${T}/scripts/check_error_codes.py" > "${WORK}/out" 2>&1; rc=$?
    echo "rc=${rc}"
    awk '/^ERROR: docs\/spec\/errors.md disagrees/ { on = 1; next }
         on && /^  / { sub(/^  /, ""); print; next } on { on = 0 }' "${WORK}/out"
}

expect "1 the unmodified table: in sync, rc 0" \
    "$(run 's/x/x/' | head -1)" "rc=0"

expect "2 a planted title drift on E1003 lists only E1003" \
    "$(run 's/^| E1003 | error | lex | Character outside permitted character set |/| E1003 | error | lex | Planted title |/')" \
    "rc=1
E1003: spec says 'Planted title', docs/reference/errors.md says 'Character outside permitted character set'
run: python3 scripts/check_error_codes.py --write-spec"

expect "3 a real W1001 stage drift is still caught, against the per-severity stage" \
    "$(run 's/^| W1001 | warning | type_check |/| W1001 | warning | lex |/')" \
    "rc=1
W1001: spec Stage 'lex', the compiler emits 'type_check'
run: python3 scripts/check_error_codes.py --write-spec"

expect "4 a planted stage drift on E1003 lists only E1003" \
    "$(run 's/^| E1003 | error | lex |/| E1003 | error | parse |/')" \
    "rc=1
E1003: spec Stage 'parse', the compiler emits 'lex'
run: python3 scripts/check_error_codes.py --write-spec"

echo "C695: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
