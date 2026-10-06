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
#   after    A   6.8 B/request   B   2.3 B/request
# and the residual is not per-request at all — at 12 connections instead of 8
# a scenario's delta was 49152 bytes over 10800 requests, i.e. flat, where
# before the fix it was strictly linear.
#
# CASE 2 IS WHAT STOPS THE FIX BEING A LEAK TRADED FOR A DANGLING POINTER.
# A `:param` value belongs to the matcher, but a handler may hand that exact
# pointer back as the response body — `http.resok(http.param(req;"name"))` is
# the obvious way to write a `:param` handler, and the Res constructors store
# the pointer without copying it.  The first version of this fix freed the
# bindings the moment the handler returned, and every such response came back
# EMPTY while the same handler with the value copied into a new string
# answered correctly.  So the bindings a handler saw are deferred and released
# beside free_req(), after the response, the gzip buffer and the log line have
# been written.  Case 2 drives that end to end through a real toke server.
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
trap 'rm -rf "${WORK}"; [ -n "${SRV:-}" ] && kill "${SRV}" 2>/dev/null; [ -n "${SRV:-}" ] && pkill -P "${SRV}" 2>/dev/null' EXIT

echo "S005: the route matcher releases the params it binds"
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
if ! "${CC}" "${CF[@]}" -o "${WORK}/test_http_route_param_leak" \
        "${REPO_ROOT}/test/stdlib/test_http_route_param_leak.c" \
        "${LINK[@]}" \
        -lz -lpthread -lm >"${WORK}/build.log" 2>&1; then
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

# ── Case 2: a handler that returns the :param value AS the response body ──
TKC="${TKC:-${REPO_ROOT}/tkc}"
if [ ! -x "${TKC}" ]; then
    echo "  FAIL: tkc not found at ${TKC} — cannot run the dangling-body case"
    FAIL=$((FAIL + 1))
else
    # 135.24: the server binds port 0 and says which port it got; no probe.
    PORT=""
    cat > "${WORK}/srv.tk" <<EOF
m=srv;
i=http:std.http;
f=greet(req:i64):i64{
  <http.resok(http.param(req;"name"))
};
f=main():i64{
  http.gethandler("/api/greet/:name";&greet);
  http.serveworkers(0;1);
  <0
};
EOF
    if ! (cd "${WORK}" && "${TKC}" --allow-all -o srv.bin srv.tk >srvbuild.log 2>&1) \
       || [ ! -x "${WORK}/srv.bin" ]; then
        echo "  FAIL: the param-as-body server did not build"
        sed 's/^/      /' "${WORK}/srvbuild.log" | head -6
        FAIL=$((FAIL + 1))
    else
        "${WORK}/srv.bin" >/dev/null 2>"${WORK}/srv.err" &
        SRV=$!
        for _ in $(seq 1 80); do
            PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "${WORK}/srv.err" | head -1)"
            [ -n "${PORT}" ] && break
            kill -0 "${SRV}" 2>/dev/null || break
            sleep 0.1
        done
        for _ in $(seq 1 80); do
            python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.3)
sys.exit(0 if s.connect_ex(('127.0.0.1',${PORT}))==0 else 1)" && break
            sleep 0.25
        done
        bad=0
        for w in alpha bravo charlie delta echo; do
            got="$(curl -s --max-time 10 "http://127.0.0.1:${PORT}/api/greet/${w}")"
            if [ "${got}" != "${w}" ]; then
                echo "  FAIL: /api/greet/${w} answered [${got}] — the body was freed before it was written"
                bad=1
            fi
        done
        kill "${SRV}" 2>/dev/null; pkill -P "${SRV}" 2>/dev/null; SRV=""
        if [ "${bad}" -eq 0 ]; then
            echo "  PASS: a handler may return a :param value as the response body"
            PASS=$((PASS + 1))
        else
            FAIL=$((FAIL + 1))
        fi
    fi
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
