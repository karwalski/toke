#!/usr/bin/env bash
# C672_os_getenv_owned_copy.sh -- os.getenv returns a copy the program owns,
# so a later os.setenv does not change or invalidate it (story 121.75).
#
# THE DEFECT.  tk_os_getenv returned getenv()'s pointer into the process
# environment ("ownership": "borrowed" in os.tki).  A later setenv of the same
# name may overwrite that storage in place or free it: measured on main
# 437dc471 (Darwin), a = os.getenv(X) taken after os.setenv(X; "first-value")
# read "SECOND" once os.setenv(X; "SECOND") had run.
#
# THE FIX.  tk_os_getenv strdup()s the value (an unset name gives an owned
# ""), and os.tki declares "ownership": "caller".
#
# WHAT IS ASSERTED
#   1. a value taken before a setenv of the same name is unchanged after it;
#   2. the second getenv sees the new value;
#   3. an unset name is "";
#   4. a value inherited from the parent environment is read.
#
# Negative control: C672_STDLIB=<a src/stdlib tree> builds the probe against
# it (via TKC_STDLIB_DIR).  With main's tree case 1 fails on Darwin (glibc's
# setenv never frees or reuses a value, so there main passes by luck).
#
# Story: 121.75
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C672_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c672_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C672: os.getenv returns an owned copy (121.75)"
echo "-----------------------------------------------"

cat > ge.tk <<'EOF'
m=ge;
i=os:std.os;
i=io:std.io;
f=main():i64{
  let s1=os.setenv("C672_PROBE";"first-value");
  let a=os.getenv("C672_PROBE");
  let s2=os.setenv("C672_PROBE";"SECOND");
  let s3=os.setenv("C672_OTHER";"x");
  let b=os.getenv("C672_PROBE");
  let u=os.getenv("C672_NEVER_SET");
  let h=os.getenv("C672_INHERITED");
  io.println(a);
  io.println(b);
  io.println("[\(u)]");
  io.println(h);
  <0
};
EOF
if ! TKC_STDLIB_DIR="${S}" "${TKC}" ge.tk --out ge >ge.cl 2>&1; then
    echo "  FAIL: compile ge.tk"; sed 's/^/      | /' ge.cl | head -5; exit 1
fi
rc=0; env -u C672_NEVER_SET C672_INHERITED=from-parent ./ge --allow-env >out 2>err || rc=$?
[ "${rc}" -eq 0 ] || { fail "probe exited ${rc}"; sed 's/^/      | /' err | head -3; }
line() { sed -n "$1p" out; }
[ "$(line 1)" = "first-value" ] && pass "1 value survives a later setenv" \
    || fail "1 value taken before the setenv now reads '$(line 1)'"
[ "$(line 2)" = "SECOND" ] && pass "2 second getenv sees the new value" \
    || fail "2 second getenv read '$(line 2)'"
[ "$(line 3)" = "[]" ] && pass "3 unset name is \"\"" || fail "3 unset name read '$(line 3)'"
[ "$(line 4)" = "from-parent" ] && pass "4 inherited value read" || fail "4 inherited value read '$(line 4)'"

echo "-----------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
