#!/usr/bin/env bash
# S005_http_route_param_leak.sh — the route matcher must release the key/value
# pairs it binds for each `:param` segment (story 127.72).
#
# THE DEFECT.  match_pattern() malloc()s a key and a value per bound
# parameter and writes them into the caller's StrPair block.  Nothing ever
# freed them, at three separate points:
#
#   1. The first routing pass called match_pattern for EVERY route, and on a
#      route whose path matched but whose method did not it did `pc = 0` —
#      dropping the pointers.  So the cost is not a constant per request: it
#      scales with the number of `:param` routes whose path matches.
#   2. The same pass broke out on the selected route without freeing what it
#      had just bound, and the winner was then matched a SECOND time.
#   3. The bindings the handler actually used were abandoned when the request
#      finished.  free_req() even carried a comment saying req.params was
#      "deliberately NOT touched".
#
# IT EXISTED TWICE.  `tk_match_pattern` in tk_web_glue.c was a verbatim copy,
# leaking identically at five more call sites — and the two had already
# drifted: the copy bounded its writes to 32 entries and the original did not,
# so a route with more than 32 parameters wrote past a stack array.  There is
# now one function, http_match_pattern, with http_free_params beside it.
#
# A plain route allocates nothing, which is why only `:param` routes grew and
# why this survived so long.
#
# MEASURED ON THIS TREE, macOS/arm64, with the C harness this script runs:
#   before   A 257.5 B/request   B 760.0 B/request
#   after    A   4.6 B/request   B   0.0 B/request
# and the residual is not per-request at all — at 12 connections instead of 8
# a scenario's delta was 49152 bytes over 10800 requests, i.e. flat, where
# before the fix it was strictly linear.
#
# WHY THE ASSERTION IS NOT AN RSS DELTA.  RSS cannot see this on a running
# toke server.  The per-connection read buffer is ~1 MB and dwarfs a
# 260-byte-per-request leak, and worse, a worker killed by SIGPIPE is
# respawned by http_serve_workers and takes its leaked memory with it — in
# every 200k-request run attempted here both workers were replaced part way
# through.  A same-process heap count over a fixed number of requests has
# neither problem.
#
# Story: 127.72

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"

PASS=0
FAIL=0

WORK="$(mktemp -d /tmp/tkc_routeleak_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "S005: the route matcher releases the params it binds"
echo "--------------------------------------"

if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror \
        -Wno-misleading-indentation -g \
        -o "${WORK}/test_http_route_param_leak" \
        "${REPO_ROOT}/test/stdlib/test_http_route_param_leak.c" \
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

if "${WORK}/test_http_route_param_leak" >"${WORK}/run.log" 2>&1; then
    sed 's/^/  /' "${WORK}/run.log"
    PASS=$((PASS + 1))
else
    echo "  FAIL: the route matcher is still retaining bound parameters"
    sed 's/^/      /' "${WORK}/run.log"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
