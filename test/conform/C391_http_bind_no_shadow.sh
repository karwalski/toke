#!/usr/bin/env bash
# C391_http_bind_no_shadow.sh — a std.http server cannot start beside a live
# listener it would shadow, and still restarts over TIME_WAIT (story 135.32).
#
# THE DEFECT.  bind_listen() (src/stdlib/http.c) set SO_REUSEADDR before
# every bind.  On Darwin that lets 127.0.0.1:P bind beside a live *:P
# listener (and *:P beside a live 127.0.0.1:P); the more specific socket then
# takes that address's traffic.  So a second toke server on a busy port
# started "successfully" and silently took loopback traffic from the first.
# Linux refuses both binds with or without SO_REUSEADDR.
#
# THE FIX (bind_exclusive): bind without SO_REUSEADDR; on EADDRINUSE connect
# to the address (loopback for a wildcard bind) -- a listener answers -> the
# bind fails; nothing answers -> TIME_WAIT, retry with SO_REUSEADDR.
#
# WHAT IS ASSERTED (servers on loopback; ports read from server_listening):
#   1. A on *:P (serveworkerson "0.0.0.0"); B = serveon("127.0.0.1";P)
#      exits 1, "could not bind to port P", the server_start_failed line;
#      127.0.0.1:P still reaches A
#   2. A on 127.0.0.1:P (serveon); B = serveworkers(P;1) on *:P exits 1 the
#      same way; 127.0.0.1:P still reaches A
#   3. C on 127.0.0.1:P is killed while a connection is open (so the server
#      side closes first), leaving a TIME_WAIT socket whose local port is P (asserted from netstat/ss, so the case
#      cannot pass vacuously); D = serveon("127.0.0.1";P) starts and answers
#   4. the same for a wildcard restart (serveworkers(P;1))
#
# Negative control: C391_STDLIB=<main's src/stdlib> -- cases 1 and 2 fail
# (B starts and takes loopback traffic: "B"), 3 and 4 pass (measured on
# Darwin 25.6).  On Linux cases 1 and 2 pass either way: that is the point.
#
# Story: 135.32
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C391_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if command -v netstat >/dev/null 2>&1 && [ "$(uname -s)" = Darwin ]; then TW=netstat
elif command -v ss >/dev/null 2>&1; then TW=ss
else echo "CONFORM-SKIP: neither netstat (Darwin) nor ss to see TIME_WAIT sockets"; exit 0; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c391_XXXXXX")"
PIDS=()
# stop PID -- kill a server and its workers.  Freeze the supervisor FIRST:
# serveon/serveworkers respawn a worker that dies by a signal, so killing the
# workers while the supervisor still runs can fork a fresh worker between the
# pkill and the kill of the parent.  That orphan keeps listening on P, and the
# restart case's D then (correctly) refuses to bind beside a live listener --
# the intermittent "could not bind ... Address already in use" of 121.52's
# follow-up.  A stopped process cannot fork, and SIGKILL still reaches it.
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { for p in "${PIDS[@]+"${PIDS[@]}"}"; do stop "$p"; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C391: a bind cannot shadow a live listener; TIME_WAIT still rebinds"
echo "--------------------------------------------------------------------"

# build NAME TAG CALL -> ./NAME serves GET /who -> TAG, started by CALL
build() {
    cat > "$1.tk" <<TOKE
m=srv;
i=http:std.http;
f=main():i64{
  http.getstatic("/who"; "$2");
  let r=$3;
  <0
};
TOKE
    if ! "${TKC}" --allow-all --out "$1" "$1.tk" > "$1.build" 2>&1; then
        echo "  FAIL: $1 does not build"; sed 's/^/      /' "$1.build" | head -6
        echo "Results: ${PASS} passed, $((FAIL + 1)) failed"; exit 1
    fi
}

# start NAME -> PID in SPID, port in SPORT (empty if it never listened)
start() {
    "./$1" > "$1.out" 2> "$1.err" &
    SPID=$!; PIDS+=("${SPID}"); SPORT=""
    for _ in $(seq 1 80); do
        SPORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "$1.err" | head -1)"
        [ -n "${SPORT}" ] && break
        kill -0 "${SPID}" 2>/dev/null || break
        sleep 0.1
    done
}

who() { curl -s --max-time 2 "http://127.0.0.1:$1/who" 2>/dev/null; }

answers() {   # answers PORT TAG -> 0 once GET /who says TAG
    for _ in $(seq 1 50); do [ "$(who "$1")" = "$2" ] && return 0; sleep 0.1; done
    return 1
}

# second NAME PORT FN LABEL -- NAME on a busy PORT must exit 1 with the bind
# failure, and the first server must still own 127.0.0.1:PORT.
second() {
    local name="$1" port="$2" fn="$3" label="$4" rc="running"
    "./${name}" > "${name}.out" 2> "${name}.err" &
    local pid=$!; PIDS+=("${pid}")
    for _ in $(seq 1 40); do kill -0 "${pid}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${pid}" 2>/dev/null; then
        rc="running(loopback->$(who "${port}"))"; stop "${pid}"
    else
        rc=0; wait "${pid}" || rc=$?
    fi
    local got="${rc} $(grep -a -c "could not bind to port ${port}: Address already in use" "${name}.err") $(grep -a -c "^DIAGNOSTIC: kind=server_start_failed fn=\"${fn}\" port=${port} stage=\"bind/fork\"$" "${name}.err") $(who "${port}")"
    if [ "${got}" = "1 1 1 A" ]; then
        echo "  PASS: ${label}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${label}"; echo "        expected: 1 1 1 A"; echo "        got:      ${got}"
        sed 's/^/        | /' "${name}.err" | head -3; FAIL=$((FAIL + 1))
    fi
}

tw_count() {  # TIME_WAIT sockets whose LOCAL port is $1
    if [ "${TW}" = netstat ]; then
        netstat -an -p tcp 2>/dev/null | awk -v p="$1" '$6=="TIME_WAIT" && $4 ~ ("\\." p "$") {n++} END{print n+0}'
    else
        ss -tanH state time-wait "sport = :$1" 2>/dev/null | wc -l | tr -d ' '
    fi
}

# restart LABEL CALL -- C on 127.0.0.1 serves requests and is killed with its
# connections in TIME_WAIT; D (started by CALL on the same port) must answer.
restart() {
    local label="$1" call="$2"
    build c_srv C 'http.serveon("127.0.0.1";0)'
    start c_srv
    local port="${SPORT}" pid="${SPID}"
    if [ -z "${port}" ] || ! answers "${port}" C; then
        echo "  FAIL: ${label}: C never answered"; FAIL=$((FAIL + 1)); stop "${pid}"; return
    fi
    # The server must close first for ITS side to sit in TIME_WAIT: hold a
    # connection open, kill the server (its FIN goes first), then close ours.
    # Ask for /who on it and read the status line, keeping it alive: a worker
    # has then accepted it.  A connection still in the listen backlog when the
    # server dies is reset instead, leaving no TIME_WAIT -- under CPU load the
    # 0.2 s sleep alone did not guarantee the accept (0 TIME_WAIT, 1 in 20).
    exec 3<>"/dev/tcp/127.0.0.1/${port}"
    printf 'GET /who HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n' >&3
    local status=""; IFS= read -r -t 5 status <&3 || true
    sleep 0.2
    stop "${pid}"
    sleep 0.2
    # Drain to the server's FIN before closing: closing with the response's
    # unread bytes would send a reset and take the server's TIME_WAIT with it.
    cat <&3 >/dev/null 2>&1
    exec 3<&-
    sleep 0.2
    local tw; tw="$(tw_count "${port}")"
    build d_srv D "${call//PORT/${port}}"
    "./d_srv" > d_srv.out 2> d_srv.err &
    local dpid=$!; PIDS+=("${dpid}")
    local ok=no; answers "${port}" D && ok=yes
    stop "${dpid}"
    if [ "${tw}" -ge 1 ] && [ "${ok}" = yes ]; then
        echo "  PASS: ${label} (${tw} TIME_WAIT on :${port})"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${label}"; echo "        expected: >=1 TIME_WAIT on :${port}, D answers"
        echo "        got:      ${tw} TIME_WAIT, D answers: ${ok}"; sed 's/^/        | /' d_srv.err | head -3
        FAIL=$((FAIL + 1))
    fi
}

# 1. loopback beside a live wildcard listener
build a_any A 'http.serveworkerson("0.0.0.0";0;1)'
start a_any
A1="${SPID}"
if [ -z "${SPORT}" ] || ! answers "${SPORT}" A; then
    echo "  FAIL: server A on *:0 never answered"; FAIL=$((FAIL + 1))
else
    build b_lo B "http.serveon(\"127.0.0.1\";${SPORT})"
    second b_lo "${SPORT}" http.serveon "serveon(\"127.0.0.1\";P) beside a live *:P fails at bind; A keeps loopback"
fi
stop "${A1}"

# 2. wildcard beside a live loopback listener
build a_lo A 'http.serveon("127.0.0.1";0)'
start a_lo
A2="${SPID}"
if [ -z "${SPORT}" ] || ! answers "${SPORT}" A; then
    echo "  FAIL: server A on 127.0.0.1:0 never answered"; FAIL=$((FAIL + 1))
else
    build b_any B "http.serveworkers(${SPORT};1)"
    second b_any "${SPORT}" http.serveworkers "serveworkers(P;1) beside a live 127.0.0.1:P fails at bind; A keeps loopback"
fi
stop "${A2}"

# 3, 4. restarts over TIME_WAIT
restart 'restart on 127.0.0.1:P over TIME_WAIT binds and answers' 'http.serveon("127.0.0.1";PORT)'
restart 'wildcard restart on *:P over a 127.0.0.1:P TIME_WAIT binds and answers' 'http.serveworkers(PORT;1)'

echo "--------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
