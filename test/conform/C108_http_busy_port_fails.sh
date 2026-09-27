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
#   2. server B on the same port exits on its own, non-zero, saying it could
#      not bind — it does not start
#   3. A is still the one answering afterwards
#   4. once A is gone the port can be bound again at once
#
# Negative control: put the SO_REUSEPORT setsockopt back in bind_listen() —
# case 2 fails (B is still running after the wait) and B's log shows no bind
# error.
#
# THE PORT (127.231 item 4).  This used to probe a free port with bind(0),
# close it, and hand the number to server A.  Under a full concurrent conform
# run A once failed with "could not bind to port 54308": between the probe and
# A's bind, another process took the port.  Every suite that probes the same
# way draws from the kernel's ephemeral range, and a released port is handed
# straight back out, so two probing suites can be given the same number.
# The literal fix — A binds port 0 and the test reads the port back — is not
# available: http.serveworkers maps port 0 to 8080 and reports nothing.  So:
#   - the port is drawn from 20000-29999, BELOW every ephemeral range (macOS
#     49152+, Linux 32768+): bind(0) probes and client source ports never
#     land there, which removes the thief that was seen;
#   - if A still cannot bind (some process listens there), that is visible in
#     its log, and A is retried on a fresh port, up to 5 times.  A retry can
#     only turn a bind failure into a start; it cannot turn a real defect into
#     a pass, because cases 2-4 all run against the port A actually holds.
#   Control for the retry: C108_STEAL_FIRST=1 puts a listener on the first
#   port drawn before A starts (the thief, on demand).  With the default 5
#   attempts the suite passes and says it retried; with C108_ATTEMPTS=1 (the
#   old single shot) it fails "server A never answered", as it did under load.
#
# Story: 127.203, 127.231 (item 4)

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
cleanup() { stop "${A_PID}"; stop "${B_PID}"; stop "${C_PID}"; stop "${THIEF:-}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

echo "C108: a busy port is a bind failure; the worker pool still serves"
echo "-----------------------------------------------------------------"

# A port below the ephemeral ranges that is bindable right now (see header).
pick_port() {
    python3 -c '
import random, socket
for _ in range(200):
    p = random.randint(20000, 29999)
    s = socket.socket()
    try:
        s.bind(("", p)); print(p); break
    except OSError:
        pass
    finally:
        s.close()'
}

build_srv() {   # build_srv PORT -> srv_bin serving GET /ok on PORT
cat > srv.tk <<TOKE
m=busyport;
i=http:std.http;
f=main():i64{
  http.getstatic("/ok"; "ok");
  let r=http.serveworkers(${1};2);
  <3
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > compile.log 2>&1; then
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

# 1. A: a real pool, answering.  Retried on a fresh port only when A itself
#    reports that it could not bind (see header).
A_UP=""
THIEF=""
for attempt in $(seq 1 "${C108_ATTEMPTS:-5}"); do
    PORT="$(pick_port)"
    [ -n "${PORT}" ] || continue
    build_srv "${PORT}"
    if [ "${attempt}" = 1 ] && [ "${C108_STEAL_FIRST:-0}" = 1 ]; then
        python3 -c 'import socket,sys,time
s=socket.socket(); s.bind(("", int(sys.argv[1]))); s.listen(1); time.sleep(60)' "${PORT}" &
        THIEF=$!
        sleep 0.5
    fi
    ./srv_bin > a.log 2>&1 &
    A_PID=$!
    if up "${A_PID}"; then A_UP=1; break; fi
    if grep -a -q "could not bind to port ${PORT}" a.log; then
        echo "  info: port ${PORT} was taken before server A bound it; attempt ${attempt} of ${C108_ATTEMPTS:-5}"
        stop "${A_PID}"; A_PID=""
        continue
    fi
    break
done
if [ -z "${A_UP}" ]; then
    echo "  FAIL: server A never answered on port ${PORT}"
    sed 's/^/      /' a.log | head -4
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
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
    expect "server B exits 3 (serveworkers returned) and says it could not bind" \
        "${rc} $(grep -a -c "could not bind to port ${PORT}" b.log)" "3 1"
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

stop "${THIEF}"; THIEF=""
echo "-----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
