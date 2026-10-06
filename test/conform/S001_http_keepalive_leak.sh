#!/usr/bin/env bash
# S001_http_keepalive_leak.sh — the HTTP keep-alive loop must not retain the
# parsed request across iterations (story 127.65).
#
# handle_connection() called parse_request() once per request and never freed
# the result: strdups for method, path and body, two per header, and a flat
# malloc(64 * sizeof(StrPair)) header block (1024 B) regardless of how many
# headers arrived.  Measured on this tree before the fix: 1222.3 bytes per
# request, linear, no plateau — and reproducible on a 404 path where no toke
# code runs at all, which is what rules out the runtime's never-free arena.
#
# The assertion lives in a C harness that drives the real server loop through
# http_handle_fd() over a loopback keep-alive connection and reads the
# process heap's in-use byte count either side of it.  This script builds and
# runs it the way the Makefile's test-stdlib-http-leak target does, so
# `make conform` covers the regression.
#
# Story: 127.65

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"

PASS=0
FAIL=0

WORK="$(mktemp -d /tmp/tkc_httpleak_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "S001: the HTTP keep-alive loop frees the parsed request each iteration"
echo "--------------------------------------"

# 127.268: the stdlib link set is the one a program importing std.http is
# linked with, read from tkc's own dependency table (src/stdlib_deps.c via
# --emit-deps), not a hand list a stdlib dependency change silently breaks.
# tk_web_glue.c carries helpers unused outside the full glue surface, hence
# -Wno-unused-function.
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -Wno-unused-function -g -I"${REPO_ROOT}/stdlib/vendor/tomlc99")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${REPO_ROOT}/src/stdlib" --cc "${CC}" --objects http -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/test_http_keepalive_leak" \
        "${REPO_ROOT}/test/stdlib/test_http_keepalive_leak.c" \
        "${LINK[@]}" \
        -lz -lpthread -lm >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: harness did not build"
    sed 's/^/      /' "${WORK}/build.log"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

if "${WORK}/test_http_keepalive_leak" >"${WORK}/run.log" 2>&1; then
    sed 's/^/  /' "${WORK}/run.log"
    PASS=$((PASS + 1))
else
    echo "  FAIL: the keep-alive loop is still retaining memory per request"
    sed 's/^/      /' "${WORK}/run.log"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
