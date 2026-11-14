#!/usr/bin/env bash
# C471_os_strerror_own_copy.sh -- os.strerror returns its own string, not a
# pointer into the C library's shared buffer (story 121.22, audit AMB-11).
#
# THE DEFECT.  tk_os_strerror returned strerror()'s pointer.  For an errno the
# C library has no text for, strerror() formats "Unknown error: N" into one
# static buffer, so a second call rewrote the first call's result.  Measured
# on main (macOS): a = os.strerror(9999); b = os.strerror(9998) and `a` then
# reads "Unknown error: 9998".  Two threads calling it raced on the buffer.
# It now copies strerror_r's text to the heap; os.tki says "ownership":
# "caller", as os.getcwd has since 135.23.
#
# WHAT IS ASSERTED (a toke program):
#   1. a = os.strerror(9999), then b = os.strerror(9998): a is still the
#      C library's text for 9999.  127.439: that text is read from the
#      platform below -- macOS says "Unknown error: 9999", glibc says
#      "Unknown error 9999" (no colon), so a fixed string failed on Linux.
#   2. os.strerror(2) is the ENOENT text, not empty
#   3. os.tki declares os.strerror "ownership": "caller"
#
# Negative control: on main case 1 prints "Unknown error: 9998" for a and
# the program exits 5.
#
# Story: 121.22
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c471_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
echo "C471: os.strerror returns its own copy (121.22, AMB-11)"
echo "------------------------------------------------------------------------"

# The platform C library's own text for the two unknown codes (Python's
# os.strerror is strerror(3)).  The assertion stays exact equality.
E9999="$(python3 -c 'import os; print(os.strerror(9999))')"
E9998="$(python3 -c 'import os; print(os.strerror(9998))')"
case "${E9999}${E9998}" in *'"'*|*'\\'*) echo "  FAIL: unexpected strerror text"; exit 1 ;; esac

cat > t.tk <<EOF
m=t;
i=os:std.os;
i=str:std.str;
i=io:std.io;
f=main():i64{
  let a=os.strerror(9999);
  let b=os.strerror(9998);
  let n=os.strerror(2);
  (io.println(a));
  (io.println(b));
  (io.println(n));
  if(!str.eq(a;"${E9999}")){<5}el{};
  if(!str.eq(b;"${E9998}")){<6}el{};
  if(str.len(n)==0){<7}el{};
  <0
};
EOF
if ! "${TKC}" t.tk --out t >t.cl 2>&1; then
    echo "  FAIL: compile"; sed 's/^/      | /' t.cl | head -5; FAIL=$((FAIL + 1))
else
    rc=0; ./t >t.out 2>t.err || rc=$?
    case "${rc}" in
        0) echo "  PASS: 1 a keeps \"${E9999}\" after a second call"; PASS=$((PASS + 1))
           echo "  PASS: 2 os.strerror(2) = \"$(sed -n 3p t.out)\""; PASS=$((PASS + 1)) ;;
        5) echo "  FAIL: 1 the second call rewrote the first result: a = \"$(sed -n 1p t.out)\""; FAIL=$((FAIL + 1)) ;;
        *) echo "  FAIL: 1/2 exit ${rc}"; sed 's/^/      | /' t.out t.err | head -5; FAIL=$((FAIL + 1)) ;;
    esac
fi
if /usr/bin/grep -a -q '"name": "os.strerror".*"ownership": "caller"' "${REPO_ROOT}/stdlib/os.tki"; then
    echo "  PASS: 3 os.tki declares os.strerror ownership caller"; PASS=$((PASS + 1))
else
    echo "  FAIL: 3 os.tki does not declare os.strerror ownership caller"; FAIL=$((FAIL + 1))
fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
