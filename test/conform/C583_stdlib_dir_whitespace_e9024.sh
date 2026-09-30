#!/usr/bin/env bash
# C583_stdlib_dir_whitespace_e9024.sh — a stdlib or runtime directory whose
# path contains whitespace is refused with E9024, not split into clang
# arguments (story 121.28, audit-120 compiler-toolchain COM-02).
#
# THE DEFECT.  121.1b replaced system() with fork+execvp, so shell
# metacharacters in TKC_STDLIB_DIR / TKC_RUNTIME_DIR are inert.  But the
# stdlib source paths and vendor -I flags still reach clang's argv through
# whitespace-joined strings, so a directory with a space in it was split:
# measured, TKC_STDLIB_DIR='<dir>/std -v lib' handed `-v` to clang as a flag
# and the build failed on the path fragments with E9003.  The link step now
# refuses such a directory by name, E9024, before clang runs.
#
# WHAT THIS DRIVES.  (1) TKC_STDLIB_DIR naming a real directory "std -v lib"
# (it holds tk_runtime.c, so E9022 does not fire) → E9024, one record, and
# clang is never run (no "clang version" banner).  (2) TKC_RUNTIME_DIR with a
# space → E9024.  (3) A symlink WITH a space pointing at the real stdlib still
# builds — the compiler resolves TKC_STDLIB_DIR to its real path first, so the
# workaround the message names works.
#
# Negative control: disable the E9024 block at the top of compile_binary_multi
# in src/llvm.c (make its test `continue` unconditionally) — cases 1 and 2
# fail: no E9024, the build exits 0 (this stub directory holds no str.c, so
# selective linking quietly falls back to the built-in stdlib path; a real
# stdlib copy under "std -v lib" instead fails E9003 with clang's -v banner).
#
# Story: 121.28

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
STD="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c583_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C583: whitespace in the stdlib/runtime directory is E9024"
echo "--------------------------------------------------------"

cat > "${WORK}/p.tk" <<'EOF'
m=c583;
i=io:std.io;
f=main():i64{
  io.println("ok");
  <0
};
EOF

mkdir -p "${WORK}/std -v lib"
: > "${WORK}/std -v lib/tk_runtime.c"

# 1. TKC_STDLIB_DIR with a space.
rc=0
TKC_STDLIB_DIR="${WORK}/std -v lib" "${TKC}" --out "${WORK}/p1" "${WORK}/p.tk" \
    >"${WORK}/o1" 2>&1 || rc=$?
n="$(grep -c '"error_code":"E9024"' "${WORK}/o1")"
if [ "${rc}" != 0 ] && [ "${n}" = 1 ] && ! grep -q "clang version" "${WORK}/o1" &&
   grep -q "TKC_STDLIB_DIR='[^']*/std -v lib' contains whitespace" "${WORK}/o1"; then
    pass "TKC_STDLIB_DIR with a space: one E9024, clang not run"
else
    fail "TKC_STDLIB_DIR with a space: rc ${rc}, ${n} E9024 record(s): $(head -c 240 "${WORK}/o1")"
fi

# 2. TKC_RUNTIME_DIR with a space.
rc=0
TKC_STDLIB_DIR="${STD}" TKC_RUNTIME_DIR="${WORK}/rt dir" "${TKC}" --out "${WORK}/p2" "${WORK}/p.tk" \
    >"${WORK}/o2" 2>&1 || rc=$?
if [ "${rc}" != 0 ] && grep -q '"error_code":"E9024"' "${WORK}/o2" &&
   grep -q "TKC_RUNTIME_DIR=" "${WORK}/o2"; then
    pass "TKC_RUNTIME_DIR with a space: E9024"
else
    fail "TKC_RUNTIME_DIR with a space: rc ${rc}: $(head -c 240 "${WORK}/o2")"
fi

# 3. A symlink with a space to the real stdlib is resolved and builds.
ln -s "${STD}" "${WORK}/link to std"
rc=0
TKC_STDLIB_DIR="${WORK}/link to std" "${TKC}" --out "${WORK}/p3" "${WORK}/p.tk" \
    >"${WORK}/o3" 2>&1 || rc=$?
if [ "${rc}" = 0 ] && [ "$("${WORK}/p3" 2>/dev/null)" = "ok" ]; then
    pass "symlink with a space to the real stdlib: builds and runs"
else
    fail "symlink with a space: rc ${rc}: $(head -c 240 "${WORK}/o3")"
fi

echo ""
echo "C583: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
