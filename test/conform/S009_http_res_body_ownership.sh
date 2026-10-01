#!/usr/bin/env bash
# S009_http_res_body_ownership.sh — a Res does not own its body, and the
# handlers that allocate one must say who releases it (story 127.148).
#
# THE RULE.  http_Res_ok / _json / _bad / _err store the `body` pointer as
# given; make_res() copies nothing.  That is deliberate: it is what lets a
# handler return a string literal, and it is what makes
# `http.resok(http.param(req;"name"))` — the obvious `:param` handler —
# work at all, since a binding is request-lifetime memory released after the
# response (story 127.72, suite S005).  So a body is exactly one of:
#
#   (a) static storage;
#   (b) memory owned by the request, which outlives the response by
#       construction;
#   (c) heap the handler hands to http_defer_free() in the same breath.
#
# There is no fourth case.  It was never written down and never enforced,
# and all three handlers that built a body on the heap simply dropped it:
# staticdir_handler leaked the whole file plus a StrPair block,
# tk_page_dispatch leaked the rendered HTML plus a block, and
# vhost_catchall_handler ignored router_static_serve's own written contract
# and leaked the file, four strdup'd header values and two pointer arrays.
# (136.69 removed the vhost surface -- http.vhost / servevhosts /
# servevhoststls and that handler -- which had no caller anywhere, so the
# measurement below, a real http.servedir mount, is what this suite covers.)
#
# MEASURED ON THIS TREE with the harness this script builds, macOS/arm64, a
# real http.servedir mount serving a 64 KiB file over keep-alive:
#
#   before   81920.0 bytes per request   (delta 65536000 over 800 requests)
#   after        0.0 bytes per request   (delta 0 over 800 requests)
#
# WHY HEAP ACCOUNTING AND NOT RSS.  RSS cannot see this on a running toke
# server: the per-connection read buffer is ~1 MB and swamps it, and worse, a
# worker killed by SIGPIPE — which until 127.144 happened on every abandoned
# request — is respawned and carries its leaked memory away with it, which is
# exactly how a 200,000-request measurement was destroyed under 127.72.  A
# same-process heap count over a fixed number of requests has neither
# problem.
#
# WHY THE ASSERTION IS LOAD-INDEPENDENT.  It is a per-request average with a
# 64-byte threshold against a defect worth 81920 bytes per request — a factor
# of 1280.  Allocator noise and machine contention cannot move a number that
# far in either direction, and the residual after the fix was measured at
# exactly zero, not merely under threshold.
#
# Story: 127.148

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"

PASS=0
FAIL=0

WORK="$(mktemp -d /tmp/tkc_s009_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "S009: a heap response body is released with the request"
echo "--------------------------------------"

# 127.268: the stdlib link set is the one a program importing std.http is
# linked with, read from tkc's own dependency table (src/stdlib_deps.c via
# --emit-deps) -- runtime, capabilities, http's cores and glue, and their
# dependencies.  It used to be listed here by hand, and a stdlib dependency
# change (121.74: toml.c -> file.c) broke the build at merge.
# tk_web_glue.c carries several helpers that are unused unless the whole glue
# surface is linked, so -Wno-unused-function here; the production build of
# this file is unchanged and still -Werror.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${REPO_ROOT}/src/stdlib" http)" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror \
        -Wno-misleading-indentation -Wno-unused-function -g \
        -iquote "${REPO_ROOT}/src/stdlib" -I"${REPO_ROOT}/stdlib/vendor/tomlc99" \
        -o "${WORK}/test_http_res_body_ownership" \
        "${REPO_ROOT}/test/stdlib/test_http_res_body_ownership.c" \
        "${LINK[@]}" \
        -lz -lpthread -lm >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: harness did not build"
    sed 's/^/      /' "${WORK}/build.log" | head -20
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

if "${WORK}/test_http_res_body_ownership" >"${WORK}/run.log" 2>&1; then
    sed 's/^/  /' "${WORK}/run.log"
    PASS=$((PASS + 1))
else
    echo "  FAIL: a heap response body is still being dropped on the floor"
    sed 's/^/      /' "${WORK}/run.log"
    FAIL=$((FAIL + 1))
fi

# ── The rule has to be written down where a handler author will meet it ──
# This is not decoration: the defect above existed BECAUSE the rule was only
# implicit in make_res().  If the statement disappears, the next heap-bodied
# handler repeats it.
for f in "${REPO_ROOT}/src/stdlib/http.h" "${REPO_ROOT}/docs/stdlib/http.md"; do
    if awk '/http_defer_free/ { found = 1 } END { exit !found }' "${f}"; then
        echo "  PASS: $(basename "${f}") states the response-body ownership rule"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $(basename "${f}") no longer states the response-body ownership rule"
        FAIL=$((FAIL + 1))
    fi
done

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
