#!/usr/bin/env bash
# S002_http_sigterm_shutdown.sh — a pre-fork HTTP worker pool must honour
# SIGTERM immediately, not after the drain timeout (story 127.69).
#
# http_serve_workers() installed its shutdown handler with signal(), which on
# macOS/BSD (and glibc) implies SA_RESTART.  A worker parked in accept() was
# therefore resumed by the kernel instead of getting EINTR, so it never
# reached its `if (g_shutdown_requested) break;` check and only noticed the
# SIGTERM when the next connection happened to arrive.  The supervisor then
# waited the full HTTP_DRAIN_TIMEOUT_SECS (10 s) and SIGKILLed its own idle
# workers.  Measured on this tree before the fix: 9.36 / 9.45 / 10.02 s.
#
# With sigaction() and sa_flags = 0 the accept() returns EINTR at once and the
# pool is gone in well under a tenth of a second.  This test asserts the
# supervisor is reaped inside SHUTDOWN_BUDGET_S — comfortably under the drain
# timeout, so it cannot pass by accident if the regression returns.
#
# Story: 127.69

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

# Budget in seconds.  HTTP_DRAIN_TIMEOUT_SECS is 10; the fixed path measures
# 0.08-0.23 s.  2 s leaves room for a loaded machine without ever admitting
# the SA_RESTART behaviour.
SHUTDOWN_BUDGET_S=2.0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_sigterm_XXXXXX)"
cleanup() {
    if [ -n "${SRV_PID:-}" ]; then
        pkill -9 -P "${SRV_PID}" 2>/dev/null
        kill -9 "${SRV_PID}" 2>/dev/null
    fi
    rm -rf "${WORK}"
}
trap cleanup EXIT
cd "${WORK}"

echo "S002: http.serveworkers honours SIGTERM without waiting out the drain timeout"
echo "--------------------------------------"

# Pick a free high port.
PORT=0
for _try in 1 2 3 4 5 6 7 8 9 10; do
    CAND=$(( 20000 + RANDOM % 20000 ))
    if ! (exec 3<>"/dev/tcp/127.0.0.1/${CAND}") 2>/dev/null; then
        PORT="${CAND}"
        break
    fi
done
if [ "${PORT}" -eq 0 ]; then
    echo "  FAIL: could not find a free port"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

cat > srv.tk <<TOKE
m=sigtermsrv;
i=http:std.http;

f=main():i64{
  http.getstatic("/ok"; "ok");
  http.serveworkers(${PORT};4);
  < 0
};
TOKE

if ! "${TKC}" --allow-all --out srv_bin srv.tk >compile.log 2>&1; then
    echo "  FAIL: server did not compile"
    sed 's/^/      /' compile.log
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

./srv_bin >server.log 2>&1 &
SRV_PID=$!

# Wait for the pool to come up and for the workers to settle into accept().
UP=0
for _i in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${PORT}") 2>/dev/null; then UP=1; break; fi
    sleep 0.1
done
if [ "${UP}" -ne 1 ]; then
    echo "  FAIL: server never accepted a connection on port ${PORT}"
    sed 's/^/      /' server.log
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

NWORKERS=$(pgrep -P "${SRV_PID}" 2>/dev/null | wc -l | tr -d ' ')
if [ "${NWORKERS}" -lt 2 ]; then
    echo "  FAIL: expected a pre-fork pool, saw ${NWORKERS} worker(s)"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi
echo "  workers forked: ${NWORKERS} (supervisor ${SRV_PID}, port ${PORT})"

# Every worker is now blocked in accept() with nothing in flight — exactly the
# state the regression could not interrupt.
sleep 0.5

T0=$(python3 -c 'import time;print(time.time())')
kill -TERM "${SRV_PID}"
for _i in $(seq 1 1500); do
    kill -0 "${SRV_PID}" 2>/dev/null || break
    sleep 0.01
done
T1=$(python3 -c 'import time;print(time.time())')
ELAPSED=$(python3 -c "print('%.2f' % (${T1} - ${T0}))")

if kill -0 "${SRV_PID}" 2>/dev/null; then
    echo "  FAIL: supervisor still alive ${ELAPSED}s after SIGTERM"
    FAIL=$((FAIL + 1))
elif python3 -c "import sys; sys.exit(0 if ${ELAPSED} < ${SHUTDOWN_BUDGET_S} else 1)"; then
    echo "  PASS: pool exited ${ELAPSED}s after SIGTERM (budget ${SHUTDOWN_BUDGET_S}s)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: pool took ${ELAPSED}s to honour SIGTERM (budget ${SHUTDOWN_BUDGET_S}s)"
    echo "        workers blocked in accept() are not seeing the shutdown flag —"
    echo "        the handler is back on signal()/SA_RESTART (story 127.69)"
    FAIL=$((FAIL + 1))
fi

STRAGGLERS=$(pgrep -P "${SRV_PID}" 2>/dev/null | wc -l | tr -d ' ')
if [ "${STRAGGLERS}" -eq 0 ]; then
    echo "  PASS: no worker left behind"
    PASS=$((PASS + 1))
else
    echo "  FAIL: ${STRAGGLERS} worker(s) outlived the supervisor"
    FAIL=$((FAIL + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
