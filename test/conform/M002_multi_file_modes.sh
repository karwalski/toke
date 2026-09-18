#!/usr/bin/env bash
# M002_multi_file_modes.sh — every non-compile mode applies to EVERY positional
# file (131.37; generalises the --min-only fix of 131.36).
#
# Verifies, for --fmt --pretty --expand --dump-ast --lint (+ --fix --dry-run)
# --migrate --check --min:
#   1. `tkc MODE a.tk b.tk` exits 0 and its stdout carries file a's output
#      BEFORE file b's output (per-file dispatch, in argument order).
#   2. No stray binary (`a` / `b`) is left in the cwd — file 2 never falls
#      through to the compile+link pipeline.
#   3. The default (no-flag) invocation still compiles BOTH files to binaries.
#
# Story: 131.37

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${REPO_ROOT}/tkc"
RUN="${REPO_ROOT}/test/run_test.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "${expected}" = "${actual}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "    expected:"; echo "${expected}" | sed 's/^/      /'
        echo "    actual:";   echo "${actual}"   | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

echo "M002: non-compile modes apply to every positional file"
echo "--------------------------------------"

WORK="$(mktemp -d /tmp/tkc_multi_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

# Two distinct, valid programs. `x` in a.tk is a single-use let so --lint has
# something to report for file a; b.tk is deliberately multi-line so --min /
# --fmt visibly rewrite it.
printf 'm=alpha;f=main():i64{let x=1;<x};\n' > a.tk
printf 'm=beta;\nf=main():i64{\n  <2\n};\n' > b.tk

# mode_check NAME MARKER_A MARKER_B FLAGS...
#   Runs `tkc FLAGS a.tk b.tk`, asserts rc 0, that MARKER_A appears before
#   MARKER_B in stdout, and that no binary was produced.
mode_check() {
    local name="$1" ma="$2" mb="$3"; shift 3
    local out rc=0
    out="$("${RUN}" 20 "${TKC}" "$@" a.tk b.tk </dev/null 2>/dev/null)" || rc=$?
    check "${name}: exit 0" "0" "${rc}"
    local pa pb
    pa="$(printf '%s' "${out}" | grep -nF -m1 -- "${ma}" | cut -d: -f1)"
    pb="$(printf '%s' "${out}" | grep -nF -m1 -- "${mb}" | cut -d: -f1)"
    check "${name}: output for a.tk present" "present" "$([ -n "${pa}" ] && echo present || echo missing)"
    check "${name}: output for b.tk present" "present" "$([ -n "${pb}" ] && echo present || echo missing)"
    if [ -n "${pa}" ] && [ -n "${pb}" ]; then
        check "${name}: a.tk output precedes b.tk output" "yes" "$([ "${pa}" -lt "${pb}" ] && echo yes || echo no)"
    fi
    check "${name}: no stray binary" "absent" "$([ -e a ] || [ -e b ] && echo present || echo absent)"
    rm -f a b
}

mode_check "--fmt"      'm=alpha;' 'm=beta;'  --fmt
mode_check "--pretty"   'm=alpha;' 'm=beta;'  --pretty
mode_check "--expand"   'm=alpha;' 'm=beta;'  --expand
mode_check "--dump-ast" '"alpha"'  '"beta"'   --dump-ast
mode_check "--migrate"  'm=alpha;' 'm=beta;'  --migrate
mode_check "--min"      'm=alpha;' 'm=beta;'  --min

# --lint: diagnostics go to stdout as JSON when not a tty; a.tk has one hint,
# b.tk has none — so check file a's hint, exit 0, and no binary. Then --fix
# --dry-run must print a diff for a.tk and still leave b.tk untouched/no binary.
LINT_OUT="$("${RUN}" 20 "${TKC}" --lint a.tk b.tk </dev/null 2>/dev/null)" && LRC=0 || LRC=$?
check "--lint: exit 0" "0" "${LRC}"
check "--lint: reports a.tk" "1" "$(printf '%s\n' "${LINT_OUT}" | grep -c '"file":"a.tk"' | tr -d ' ')"
check "--lint: no stray binary" "absent" "$([ -e a ] || [ -e b ] && echo present || echo absent)"
DRY_OUT="$("${RUN}" 20 "${TKC}" --lint --fix --dry-run a.tk b.tk </dev/null 2>&1)" || true
check "--lint --fix --dry-run: diff for a.tk" "1" "$(printf '%s\n' "${DRY_OUT}" | grep -c '^+++ a.tk' | tr -d ' ')"
check "--lint --fix --dry-run: b.tk untouched" "m=beta;" "$(head -1 b.tk)"
check "--lint --fix --dry-run: no stray binary" "absent" "$([ -e a ] || [ -e b ] && echo present || echo absent)"

# --check: silent on success; the proof is exit 0 + no binary, and that an
# error in file b is still reported (both files are checked).
CRC=0; "${RUN}" 20 "${TKC}" --check a.tk b.tk </dev/null >/dev/null 2>&1 || CRC=$?
check "--check: exit 0" "0" "${CRC}"
check "--check: no stray binary" "absent" "$([ -e a ] || [ -e b ] && echo present || echo absent)"
printf 'm=bad;f=main():i64{<}\n' > bad.tk
BRC=0; "${RUN}" 20 "${TKC}" --check a.tk bad.tk </dev/null >/dev/null 2>&1 || BRC=$?
check "--check: error in file 2 is reported (exit 1)" "1" "${BRC}"

# Default mode still compiles every file.
"${RUN}" 60 "${TKC}" a.tk b.tk </dev/null >/dev/null 2>&1 || true
check "default mode compiles both files" "a b" "$(ls a b 2>/dev/null | tr '\n' ' ' | sed 's/ $//')"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
