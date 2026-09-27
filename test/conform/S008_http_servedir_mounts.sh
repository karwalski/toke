#!/usr/bin/env bash
# S008_http_servedir_mounts.sh — http.servedir is a MOUNT, and a server may
# have several (story 127.75).
#
# THE DEFECT.  The prefix and the root were two single globals —
# g_staticdir_prefix[512] and g_staticdir_root[1024] in tk_web_glue.c —
# overwritten by every call, with http_GET("*", staticdir_handler)
# re-registered on every call as well.  Two mounts therefore collapsed to the
# last one, silently.  Confirmed on the wire before the fix, with /a and /b
# registered in that order:
#
#   GET /a/f.txt  ->  404  body=[Not Found]
#   GET /b/f.txt  ->  200  body=[FILE-B]
#
# Nothing warned and nothing logged.  The first wildcard route still matched;
# it just ran the handler against the second mount's prefix, which no longer
# matched the path, so it fell through to 404.  And because a route table
# slot was consumed per call, a server with many mounts also ate its 512-slot
# route table for no benefit.
#
# WHAT THIS PINS.  Behaviour on the wire: two mounts registered in one
# server, both served, from their own roots, with a path under neither
# answering 404.  Plus the ordering case the single global could never
# express — a more specific mount nested inside a broader one wins, which is
# the only sensible reading of overlapping prefixes.
#
# LOAD-INDEPENDENT BY CONSTRUCTION: three fixed request/response pairs with
# exact expected bodies.  No counting, no timing, no thresholds.
#
# Story: 127.75

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

WORK="$(mktemp -d /tmp/tkc_s008_XXXXXX)"
SRV=""
cleanup() {
    [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null
    [ -n "${SRV}" ] && pkill -P "${SRV}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "S008: several http.servedir mounts coexist"
echo "--------------------------------------"

# A mount resolves <root><full request path>, so the file for /a/f.txt under
# root A lives at A/a/f.txt.  That is the shipped behaviour and is not what
# this story changes.
mkdir -p "${WORK}/rootA/a" "${WORK}/rootB/b" "${WORK}/rootB/b/deep"
echo "FILE-A" > "${WORK}/rootA/a/f.txt"
echo "FILE-B" > "${WORK}/rootB/b/f.txt"
echo "FILE-DEEP" > "${WORK}/rootB/b/deep/f.txt"
# The nested mount points at a THIRD root, so "the longest prefix won" and
# "the broader mount happened to hold the file too" cannot be confused.
mkdir -p "${WORK}/rootC/b/deep"
echo "FILE-C" > "${WORK}/rootC/b/deep/f.txt"

# 135.24: the server binds port 0 and prints the port it got; no probe.
PORT=""

cat > "${WORK}/srv.tk" <<EOF
m=srv;
i=http:std.http;
f=main():i64{
  http.servedir("/a";"${WORK}/rootA");
  http.servedir("/b";"${WORK}/rootB");
  http.servedir("/b/deep";"${WORK}/rootC");
  http.serveworkers(0;1);
  <0
};
EOF

if ! (cd "${WORK}" && "${TKC}" --allow-all -o srv.bin srv.tk >build.log 2>&1) \
   || [ ! -x "${WORK}/srv.bin" ]; then
    echo "  FAIL: the multi-mount server did not build"
    sed 's/^/      /' "${WORK}/build.log" | head -8
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

"${WORK}/srv.bin" >/dev/null 2>"${WORK}/srv.err" &
SRV=$!
for _ in $(seq 1 80); do
    PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "${WORK}/srv.err" | head -1)"
    [ -n "${PORT}" ] && break
    kill -0 "${SRV}" 2>/dev/null || break
    sleep 0.1
done
up=0
for _ in $(seq 1 80); do
    python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.3)
sys.exit(0 if s.connect_ex(('127.0.0.1',${PORT}))==0 else 1)" && { up=1; break; }
    sleep 0.25
done
if [ "${up}" -ne 1 ]; then
    echo "  FAIL: the multi-mount server never came up"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

check() {   # $1 = url path, $2 = expected body ("" means expect a 404)
    local path="$1" want="$2"
    local code body
    code="$(curl -s -o /dev/null -w '%{http_code}' --max-time 10 "http://127.0.0.1:${PORT}${path}")"
    body="$(curl -s --max-time 10 "http://127.0.0.1:${PORT}${path}" | tr -d '\n')"
    if [ -z "${want}" ]; then
        if [ "${code}" = "404" ]; then
            echo "  PASS: GET ${path} -> 404, as it should be"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: GET ${path} -> ${code} body=[${body}], expected 404"
            FAIL=$((FAIL + 1))
        fi
    elif [ "${code}" = "200" ] && [ "${body}" = "${want}" ]; then
        echo "  PASS: GET ${path} -> 200 [${body}]"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: GET ${path} -> ${code} body=[${body}], expected 200 [${want}]"
        echo "        (before 127.75 the FIRST mount registered answered 404 —"
        echo "         every mount but the last collapsed into the last root)"
        FAIL=$((FAIL + 1))
    fi
}

# The first mount registered — the one the single global used to lose.
check /a/f.txt      FILE-A
# The last mount registered — the one that used to win everything.
check /b/f.txt      FILE-B
# A more specific mount nested inside a broader one: longest prefix wins,
# and it resolves against its OWN root, not the broader mount's.
check /b/deep/f.txt FILE-C
# Under no mount at all.
check /z/f.txt      ""

kill "${SRV}" 2>/dev/null; pkill -P "${SRV}" 2>/dev/null; SRV=""

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
