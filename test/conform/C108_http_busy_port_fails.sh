#!/usr/bin/env bash
# C108_http_busy_port_fails.sh — a second std.http server on a port that is
# already being served fails to bind, and the pre-fork pool still serves
# (story 127.203).
#
# THE DEFECT.  bind_listen() in src/stdlib/http.c set SO_REUSEPORT on every
# listening socket.  Two processes that both set it may bind the same
# address, so a second `http.serveworkers` (or `http.serve`, which reaches the
# same function) on a busy port bound successfully and started silently;
# connections were then split between two unrelated servers.  A bind failure
# only occurred against a listener that had NOT set REUSEPORT.
#
# REUSEPORT was never needed for the pool: http_serve_workers() binds ONCE in
# the supervisor and every forked worker accept()s on the inherited fd, and a
# SIGHUP reload re-forks against that same fd.  SO_REUSEADDR stays, so a
# restarted server can still rebind a port whose old connections sit in
# TIME_WAIT — case 4 pins that.
#
# WHAT IS ASSERTED, on loopback with a free port:
#   1. server A comes up as a pool of >= 2 workers and answers 6 requests
#   2. server B on the same port exits on its own, 1, saying it could not
#      bind and printing the structured start-failure line (135.24 item 1:
#      a plain server that does not start fails the process, as 135.22 made
#      TLS do) — it does not start
#   3. A is still the one answering afterwards
#   4. once A is gone the port can be bound again at once
#
# Negative control: put the SO_REUSEPORT setsockopt back in bind_listen() —
# case 2 fails (B is still running after the wait) and B's log shows no bind
# error.
#
# THE PORT (127.231 item 4, 135.24 item 2).  This used to probe a free port
# with bind(0), close it, and hand the number to server A; under a full
# concurrent conform run another process took the port in between.  127.231
# could not do the literal fix because http.serveworkers mapped port 0 to
# 8080 and reported nothing, and settled for a port drawn below the
# ephemeral range plus retries.  Since 135.24 port 0 binds a kernel-chosen
# port and the server prints `DIAGNOSTIC: kind=server_listening port=N`, so
# A is started on port 0 and the port is read from its log: nothing is
# probed, so nothing can be stolen.  B and C are then built for that port.
#
# THE EXIT STATUS (135.24 item 1).  B's program returns 3 from main after
# serveworkers returns, and this suite used to expect 3 — the only thing
# that told "serveworkers returned" apart from "the process was killed".  A
# start failure now records tk_exitfail(), which makes the process exit
# non-zero however main ends (tk_exitfail.h): 1, or since 135.29 main's own
# status when that is non-zero — so case 2 expects 3 again, now together
# with the structured `server_start_failed` line naming http.serveworkers,
# stage "bind/fork".
#
# Story: 127.203, 127.231 (item 4), 135.24 (items 1 and 2)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c108_XXXXXX)"
A_PID=""; B_PID=""; C_PID=""
stop() {
    [ -n "$1" ] || return 0
    pkill -9 -P "$1" 2>/dev/null
    kill -9 "$1" 2>/dev/null
    wait "$1" 2>/dev/null
}
cleanup() { stop "${A_PID}"; stop "${B_PID}"; stop "${C_PID}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

echo "C108: a busy port is a bind failure; the worker pool still serves"
echo "-----------------------------------------------------------------"

build_srv() {   # build_srv PORT OUT -> OUT serving GET /ok on PORT
cat > srv.tk <<TOKE
m=busyport;
i=http:std.http;
f=main():i64{
  http.getstatic("/ok"; "ok");
  let r=http.serveworkers(${1};2);
  <3
};
TOKE
if ! "${TKC}" --allow-all --out "$2" srv.tk > compile.log 2>&1; then
    echo "  FAIL: server did not compile"
    sed 's/^/      /' compile.log | head -6
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
}

up() {   # up PID -> 0 once something answers GET /ok on PORT; 1 once PID exits
    for _i in $(seq 1 100); do
        kill -0 "$1" 2>/dev/null || return 1
        curl -s -m 1 "http://127.0.0.1:${PORT}/ok" 2>/dev/null | grep -q '^ok$' && return 0
        sleep 0.1
    done
    return 1
}

# 1. A: a real pool on a port the kernel chose, read back from A's log.
build_srv 0 srv_any
./srv_any > a.log 2>&1 &
A_PID=$!
PORT=""
for _i in $(seq 1 100); do
    PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' a.log | head -1)"
    [ -n "${PORT}" ] && break
    kill -0 "${A_PID}" 2>/dev/null || break
    sleep 0.1
done
if [ -z "${PORT}" ] || [ "${PORT}" = 8080 ] || ! up "${A_PID}"; then
    echo "  FAIL: server A on port 0 never reported a port it answers on (got '${PORT}')"
    sed 's/^/      /' a.log | head -4
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
build_srv "${PORT}" srv_bin
NW="$(pgrep -P "${A_PID}" 2>/dev/null | wc -l | tr -d ' ')"
OKS=0
for _i in 1 2 3 4 5 6; do
    curl -s -m 2 "http://127.0.0.1:${PORT}/ok" 2>/dev/null | grep -q '^ok$' && OKS=$((OKS + 1))
done
expect "server A is a pool of >= 2 workers that answered 6 of 6 requests" \
    "$([ "${NW}" -ge 2 ] && echo pool || echo "${NW} workers") ${OKS}" "pool 6"

# 2. B on the same port must not start.
./srv_bin > b.log 2>&1 &
B_PID=$!
for _i in $(seq 1 30); do kill -0 "${B_PID}" 2>/dev/null || break; sleep 0.1; done
if kill -0 "${B_PID}" 2>/dev/null; then
    echo "  FAIL: server B is still running 3s after starting on a busy port"
    echo "        (it bound the port: SO_REUSEPORT is back in bind_listen)"
    FAIL=$((FAIL + 1))
    stop "${B_PID}"
else
    rc=0; wait "${B_PID}" || rc=$?
    expect "server B exits 3 (main's), says it could not bind, and prints the start-failure line" \
        "${rc} $(grep -a -c "could not bind to port ${PORT}" b.log) $(grep -a -c "^DIAGNOSTIC: kind=server_start_failed fn=\"http.serveworkers\" port=${PORT} stage=\"bind/fork\"$" b.log)" "3 1 1"
fi
B_PID=""

# 3. A still owns the port.
expect "server A still answers after B's attempt" \
    "$(curl -s -m 2 "http://127.0.0.1:${PORT}/ok" 2>/dev/null)" "ok"

# 4. SO_REUSEADDR kept: the port is reusable the moment A is gone.
kill -TERM "${A_PID}" 2>/dev/null
for _i in $(seq 1 100); do kill -0 "${A_PID}" 2>/dev/null || break; sleep 0.05; done
stop "${A_PID}"; A_PID=""
./srv_bin > c.log 2>&1 &
C_PID=$!
if up "${C_PID}"; then
    echo "  PASS: a new server binds the port as soon as A has exited"
    PASS=$((PASS + 1))
else
    echo "  FAIL: the port could not be rebound after A exited"
    sed 's/^/      /' c.log | head -3
    FAIL=$((FAIL + 1))
fi

echo "-----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
