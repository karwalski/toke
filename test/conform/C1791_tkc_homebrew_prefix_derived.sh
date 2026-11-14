#!/usr/bin/env bash
# C1791_tkc_homebrew_prefix_derived.sh — on macOS tkc links programs with
# the Homebrew prefix brew reports, not a hard-coded /opt/homebrew
# (story 127.446).
#
# THE DEFECT.  src/llvm.c passed -I/opt/homebrew/include and
# -L/opt/homebrew/lib on every macOS link.  On an Intel Mac Homebrew lives in
# /usr/local, /opt/homebrew does not exist, and every program link printed
# "ld: warning: search path '/opt/homebrew/lib' not found" (C1162 10c on the
# macos-15-intel runner).  The Makefile now asks `brew --prefix` and bakes it
# in as TKC_HOMEBREW_PREFIX; with no brew, no Homebrew flags at all.
#
# CASES
#   1  a program importing std.http (OpenSSL linked) builds with no linker
#      "search path ... not found" warning
#   2  tkc carries brew's own prefix (when brew is installed), and carries
#      /opt/homebrew only when that IS brew's prefix
#
# Story: 127.446
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ "$(uname -s)" != Darwin ]; then
    echo "CONFORM-SKIP: macOS only (Homebrew link flags are not passed elsewhere)"; exit 77
fi
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1791_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
echo "C1791: tkc links with the Homebrew prefix brew reports"
echo "------------------------------------------------------"

cat > "${WORK}/p.tk" <<'TOKE'
m=p;
i=http:std.http;
f=main():i64{
  <0
};
TOKE
if (cd "${WORK}" && "${TKC}" --allow-all -o p p.tk > build.log 2>&1) && [ -x "${WORK}/p" ]; then
    if grep -q "search path .* not found" "${WORK}/build.log"; then
        fail "1 the link warned: $(grep -m1 'search path' "${WORK}/build.log")"
    else
        pass "1 a std.http program links with no missing search path"
    fi
else
    fail "1 the std.http program did not build: $(head -c 300 "${WORK}/build.log")"
fi

# Case 2 reads the flags tkc hands to clang, through a TKC_CC wrapper that
# logs its arguments and then runs the real clang.  (Reading `strings tkc`
# was fragile: the linker may tail-merge the prefix into another string.)
REAL_CC="$(command -v clang || true)"
BP=""
command -v brew >/dev/null 2>&1 && BP="$(brew --prefix 2>/dev/null)"
if [ -z "${REAL_CC}" ]; then
    fail "2 no clang on PATH to wrap"
else
    cat > "${WORK}/cc" <<WRAP
#!/bin/sh
printf '%s\n' "\$@" >> "${WORK}/cc.args"
exec "${REAL_CC}" "\$@"
WRAP
    chmod +x "${WORK}/cc"
    rm -f "${WORK}/cc.args" "${WORK}/p"
    (cd "${WORK}" && TKC_CC="${WORK}/cc" "${TKC}" --allow-all -o p p.tk > build2.log 2>&1)
    ARGS="$(cat "${WORK}/cc.args" 2>/dev/null)"
    if [ -z "${ARGS}" ]; then
        fail "2 tkc did not run TKC_CC: $(head -c 300 "${WORK}/build2.log")"
    else
        if [ -n "${BP}" ]; then
            if grep -qxF -- "-L${BP}/lib" <<< "${ARGS}" && grep -qxF -- "-I${BP}/include" <<< "${ARGS}"; then
                pass "2 tkc passes brew's prefix to clang (${BP})"
            else
                fail "2 tkc does not pass -I/-L for brew's prefix ${BP}"
            fi
        fi
        if [ "${BP}" != /opt/homebrew ] && grep -qF -- "/opt/homebrew" <<< "${ARGS}"; then
            fail "2 tkc still passes /opt/homebrew, which is not this host's prefix (${BP:-no brew})"
        elif [ "${BP}" != /opt/homebrew ]; then
            pass "2 no /opt/homebrew passed on a host whose prefix is ${BP:-(no brew)}"
        fi
    fi
fi

echo "------------------------------------------------------"
echo "C1791: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
