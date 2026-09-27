#!/usr/bin/env bash
# C093_no_main_has_its_own_code.sh — "no main function" and "failed to write
# .tkir file" are two diagnostics with two codes.
#
# Story: 127.208.
#
# THE DEFECT.  main.c reported a build of a program with no `f=main()` as
# E9020 — the code whose documented meaning is "failed to write .tkir file"
# (src/tkir.h).  AGENTS.md §3.1: never change the meaning of an existing code.
# It also carried a fix field that was prose ("... (or a body that calls your
# helper functions)"), not a mechanical fix.
#
# NOW "no main" is E9023 with no fix field, and E9020 is emitted only by
# tkir.c when the .tkir output cannot be written.
#
# This is a shell case rather than a D-series YAML because the YAML runner
# always passes --check, and neither code is reachable under --check.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_nomain_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C093: no-main is E9023; E9020 means only a .tkir write failure"
echo "--------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); sed 's/^/      /' "$2" | head -3; }

printf 'm=t;\nf=helper():i64{<1};\n' > nomain.tk
printf 'm=t;\nf=main():i64{<0};\n' > withmain.tk

# ── 1. build with no main: exactly one diagnostic, E9023, no fix key ──────
rc=0; "${TKC}" --diag-json nomain.tk -o nm > nm.log 2>&1 || rc=$?
n_diag=$(grep -c '"error_code"' nm.log)
if [ "${rc}" -ne 0 ] && [ "${n_diag}" -eq 1 ] \
   && grep -q '"error_code":"E9023"' nm.log \
   && ! grep -q '"fix"' nm.log \
   && grep -q 'no main function defined' nm.log; then
    pass "no main: E9023, one record, no fix key"
else
    fail "no main: rc=${rc} records=${n_diag}" nm.log
fi
if grep -q '"E9020"' nm.log; then fail "no main still reports E9020" nm.log
else pass "no main does not report E9020"; fi

# ── 2. --check on the same module is clean (the message's advice holds) ──
rc=0; "${TKC}" --check --diag-json nomain.tk > chk.log 2>&1 || rc=$?
if [ "${rc}" -eq 0 ]; then pass "--check accepts a module with no main"
else fail "--check on a module with no main: rc=${rc}" chk.log; fi

# ── 3. E9020 keeps its meaning: an unwritable .tkir path ──────────────────
rc=0; "${TKC}" --diag-json --emit-tkir --out "${WORK}/no/such/dir/x.tkir" withmain.tk > tk.log 2>&1 || rc=$?
if [ "${rc}" -ne 0 ] && grep -q '"error_code":"E9020"' tk.log && grep -q 'tkir' tk.log; then
    pass "unwritable .tkir output: E9020"
else
    fail "unwritable .tkir output: rc=${rc}" tk.log
fi

# ── 4. and a program with main builds ─────────────────────────────────────
rc=0; "${TKC}" withmain.tk -o wm > wm.log 2>&1 || rc=$?
if [ "${rc}" -eq 0 ] && [ -x wm ]; then pass "a program with main builds"
else fail "program with main: rc=${rc}" wm.log; fi

echo ""
echo "C093: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
