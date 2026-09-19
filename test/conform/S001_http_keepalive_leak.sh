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

if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror \
        -Wno-misleading-indentation -g \
        -o "${WORK}/test_http_keepalive_leak" \
        "${REPO_ROOT}/test/stdlib/test_http_keepalive_leak.c" \
        "${REPO_ROOT}/src/stdlib/http.c" \
        "${REPO_ROOT}/src/stdlib/encoding.c" \
        "${REPO_ROOT}/src/stdlib/str.c" \
        "${REPO_ROOT}/src/stdlib/log.c" \
        "${REPO_ROOT}/src/stdlib/capabilities.c" \
        -lz -lpthread >"${WORK}/build.log" 2>&1; then
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
