#!/usr/bin/env bash
# C603_http_sighup_reopens_access_log.sh -- logrotate's "rename, then
# kill -HUP" leaves a toke HTTP server running and writing the new access log,
# in every serving mode (story 121.53 item 1).
#
# THE PREMISE, measured at main (f605aec9) before any change: the row said
# workers-mode SIGHUP never reopens log.c's access log, so after logrotate the
# server "keeps writing the rotated file".  For an access log in a writable
# directory that is REFUTED: since 127.191 every write takes the log's sidecar
# lock and reopens the file when the inode at its path has changed, so a
# renamed log is replaced on the next request with or without SIGHUP.  What
# the rotation DID break, measured with this script against main:
#   * single-process mode (serveworkers(port;1)) had no SIGHUP handler at all:
#     logrotate's `kill -HUP` killed the server (exit by signal 1);
#   * the TLS pre-fork supervisor had none either: SIGHUP killed it and left
#     its workers orphaned, still listening on the port;
#   * a log opened WITHOUT the sidecar lock (127.191 x 134.40: no lock file
#     can be made beside it, e.g. a directory the server cannot write) never
#     follows a rename, and nothing reopened it: after rename + SIGHUP every
#     line went on into the rotated file.
#
# NOW: SIGHUP means "reopen the logs" in every serving process.  The plain
# pre-fork supervisor reopens them before it forks the fresh workers its
# SIGHUP reload starts (59.4.8); the single-process server and each worker
# (plain or TLS) reopen them at the next accept(); the TLS supervisor reopens
# its own and passes SIGHUP to its workers.  tk_log_reopen_files() (log.c)
# reopens the global access and error logs at their paths, and leaves one on a
# tty, FIFO or /dev/stderr alone.
#
# WHAT IS ASSERTED, per mode: start with an access log, two requests, rename
# the log, SIGHUP, two more requests; the server is still alive, the requests
# after the rename are in the NEW file and not the rotated one.  Modes: plain
# workers (2), plain single (1), TLS workers (2), TLS single (1), and plain
# workers + single with a lockless log.  Each server is stopped the C391 way
# (supervisor frozen first) and checked for a leftover listener.
#
# Negative control: C603_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib: single and TLS-workers die on SIGHUP and both
# lockless cases write the rotated file; the two locked pre-fork modes pass
# (they followed the rename already).
#
# Story: 121.53, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C603_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v curl >/dev/null 2>&1 || { echo "CONFORM-SKIP: curl not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c603_XXXXXX")"
SRV=""
# stop PID -- freeze the supervisor first (C391): a supervisor still running
# respawns a worker killed under it, and that orphan keeps listening.
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C603: logrotate + SIGHUP -- the server lives and writes the new access log (121.53)"
echo "-----------------------------------------------------------------------------------"

HAVE_TLS=0
if command -v openssl >/dev/null 2>&1 && openssl req -x509 -newkey rsa:2048 -keyout key.pem \
        -out cert.pem -days 2 -nodes -subj "/CN=localhost" > cert.log 2>&1; then
    HAVE_TLS=1
else
    echo "  SKIP: the TLS modes -- no openssl CLI to make a certificate"
fi

cat > srv.tk <<TOKE
m=srv;
i=http:std.http;
i=log:std.log;
i=args:std.args;
f=main():i64{
  http.getstatic("/ok"; "ok");
  let lr=log.openaccess(mt args.get(2){\$ok:v v;\$err:e ""};0;0;0);
  let mode=mt args.get(1){\$ok:v v;\$err:e ""};
  let r=mut.0;
  if(mode=="workers"){ r=http.serveworkerson("127.0.0.1";0;2) }el{
  if(mode=="single"){ r=http.serveworkerson("127.0.0.1";0;1) }el{
  if(mode=="tlsworkers"){ r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";2) }el{
  r=http.servetlsworkerson("127.0.0.1";0;"${WORK}/cert.pem";"${WORK}/key.pem";1) }}};
  <0
};
TOKE
if ! "${TKC}" --allow-all --out srv_bin srv.tk > build.log 2>&1; then
    echo "  FAIL: the server does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

listeners() {   # listeners PORT -> number of sockets still listening on PORT
    if command -v lsof >/dev/null 2>&1; then
        lsof -nP -iTCP:"$1" -sTCP:LISTEN 2>/dev/null | tail -n +2 | wc -l | tr -d ' '
    else
        ss -ltnH "sport = :$1" 2>/dev/null | wc -l | tr -d ' '
    fi
}

# rotate NAME MODE SCHEME LOCKLESS
rotate() {
    local name="$1" mode="$2" scheme="$3" lockless="$4" d port="" alive
    d="${WORK}/${name// /_}"; mkdir -p "${d}/logs"
    if [ "${lockless}" = 1 ]; then
        # no sidecar lock can be made in a directory the server cannot write
        : > "${d}/logs/access.log"; chmod 555 "${d}/logs"
    fi
    ./srv_bin "${mode}" "${d}/logs/access.log" > "${d}/out" 2> "${d}/err" &
    SRV=$!
    tk_wait_until "${SRV}" tk_listening "${d}/err"
    port="$(tk_listening_port "${d}/err")"
    local why=""
    if [ -z "${port}" ]; then
        why="server did not start: $(head -2 "${d}/err" | tr '\n' ' ')"
    else
        for i in 1 2; do curl -sk --max-time 5 -o /dev/null "${scheme}://127.0.0.1:${port}/ok?pre${i}"; done
        chmod 755 "${d}/logs"
        mv "${d}/logs/access.log" "${d}/logs/access.log.1"
        kill -HUP "${SRV}" 2>/dev/null
        sleep 0.5
        for i in 1 2; do curl -sk --max-time 5 -o /dev/null "${scheme}://127.0.0.1:${port}/ok?post${i}"; done
        sleep 0.3
        if kill -0 "${SRV}" 2>/dev/null; then alive=yes; else
            wait "${SRV}"; alive="no (exit $?)"; fi
        local new old
        new="$(grep -c 'post' "${d}/logs/access.log" 2>/dev/null)"; new="${new:-0}"
        old="$(grep -c 'post' "${d}/logs/access.log.1" 2>/dev/null)"; old="${old:-0}"
        [ "${alive}" = yes ] || why="${why} server gone after SIGHUP: ${alive};"
        [ "${new}" = 2 ] || why="${why} new log has ${new} of the 2 post-rotation lines;"
        [ "${old}" = 0 ] || why="${why} rotated log got ${old} post-rotation lines;"
    fi
    stop "${SRV}"; SRV=""
    if [ -n "${port}" ]; then
        local left; left="$(listeners "${port}")"
        if [ "${left}" != 0 ]; then
            why="${why} ${left} listener(s) left on ${port} after stop;"
            # orphans of OUR server (a supervisor killed by SIGHUP): kill
            # exactly those PIDs, and only if they run this test's binary
            for p in $(lsof -nP -t -iTCP:"${port}" -sTCP:LISTEN 2>/dev/null); do
                case "$(ps -o command= -p "${p}" 2>/dev/null)" in
                    "./srv_bin "*|"${WORK}/srv_bin "*) kill -9 "${p}" 2>/dev/null ;;
                esac
            done
        fi
    fi
    if [ -z "${why}" ]; then echo "  PASS: ${name}"; PASS=$((PASS + 1))
    else echo "  FAIL: ${name}:${why}"; FAIL=$((FAIL + 1)); fi
}

rotate "plain workers(2)"            workers    http  0
rotate "plain single(1)"             single     http  0
rotate "plain workers(2), lockless"  workers    http  1
rotate "plain single(1), lockless"   single     http  1
if [ "${HAVE_TLS}" = 1 ]; then
    rotate "tls workers(2)"          tlsworkers https 0
    rotate "tls single(1)"           tlssingle  https 0
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
