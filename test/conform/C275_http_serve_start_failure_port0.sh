#!/usr/bin/env bash
# C275_http_serve_start_failure_port0.sh — a plain HTTP server that does not
# start makes the process exit non-zero, and port 0 means "any free port, and
# say which" (story 135.24 items 1 and 2).
#
# THE DEFECTS.
#   (1) http.serve / http.serveworkers on a port that could not be bound
#       printed "error: could not bind to port N" and returned -1.  Every
#       caller in the tree discards that value, so the process exited 0: to a
#       supervisor a server that never started looked like a clean shutdown.
#       135.22 fixed exactly this for servetls/servetlsworkers only.
#   (2) any port <= 0 or > 65535 was silently replaced by 8080 (8443 for
#       TLS).  `serveworkers(0;...)` served on 8080, undocumented, and a
#       caller could not ask for a free port and learn which it got — which
#       is why C108 and the other suites below probed a port with bind(0),
#       closed it, and handed the number over (a window another process can
#       take it in; C108 lost that race under load, 127.231 item 4).
#
# THE FIX (src/stdlib/tk_web_glue.c, bind_listen() in src/stdlib/http.c).
#   (1) a plain start failure returns -1, prints
#       `DIAGNOSTIC: kind=server_start_failed fn=... port=... stage=...`, and
#       records tk_exitfail(), so the process exits 1 however main ends — the
#       135.22 contract, now for every server entry point.
#   (2) port 0 binds a kernel-chosen port and prints
#       `DIAGNOSTIC: kind=server_listening port=N` once it is listening; a
#       port outside 0..65535 is a start failure at stage "port".
#
# WHAT IS ASSERTED:
#   1. serveworkers on a held port: caller sees -1, stage bind/fork, exit 3
#      (main returns 3, and since 135.29 a recorded failure keeps main's own
#      non-zero status; it would be 1 had main returned 0 — tk_exitfail.h)
#   2. serve on a held port: the same, reported as http.serve
#   3. serveworkers(70000;1): stage "port", exit 3 (main's) — not a server on 8080
#   4. serveworkers(0;2): reports a port that is not 8080, answers GET /ok
#      on it, and on SIGTERM returns 0 to main, whose own status (3) is the
#      exit status, with no failure lines
#
# Negative control: C275_STDLIB=<dir> builds against another stdlib tree.
# Against main's src/stdlib (before 135.24) cases 1-2 read "3 r=-1 0 0" (main's
# own status: nothing marks the failure),
# case 3 starts a server on 8080 (or fails to bind it) and case 4 never
# reports a port.
#
# Story: 135.24 (items 1 and 2), 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C275_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c275_XXXXXX")"
SRV=""; HOLD=""
# stop PID -- kill a server and its workers.  Freeze the supervisor FIRST
# (121.54, as C391 de559637): serveworkers respawns a worker that dies by a
# signal, so killing the workers while the supervisor still runs can fork a
# fresh worker between the pkill and the kill of the parent -- an orphan that
# keeps listening.  A stopped process cannot fork; SIGKILL still reaches it.
stop() { [ -n "$1" ] || return 0; kill -STOP "$1" 2>/dev/null; pkill -9 -P "$1" 2>/dev/null; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { stop "${SRV}"; stop "${HOLD}"; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C275: plain HTTP start failures exit non-zero; port 0 is any free port (135.24)"
echo "-------------------------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"
         [ -n "${4:-}" ] && sed 's/^/        | /' "$4" | head -5
         FAIL=$((FAIL + 1)); fi
}

# run_bounded NAME -- run ./NAME_bin, which must fail to start and exit at
# once.  121.54: this was a plain foreground run, and the port holder slept
# only 120 s: on a loaded host the compile outlasted the holder, the port came
# free, the server STARTED and the script waited on it forever (measured:
# C275 hung 2 h 15 min under 6x `yes`).  A server still running after 20 s is
# stopped and reads RC=137, which fails the case instead of hanging it.
run_bounded() {
    ./"$1"_bin > "$1.out" 2> "$1.err" &
    SRV=$!
    for _ in $(seq 1 200); do kill -0 "${SRV}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${SRV}" 2>/dev/null; then stop "${SRV}"; RC=137; else wait "${SRV}"; RC=$?; fi
    SRV=""
}

build() {   # build NAME CALL
    cat > "$1.tk" <<TOKE
m=srv;
i=http:std.http;
i=io:std.io;
f=main():i64{
  http.getstatic("/ok"; "ok");
  let r=$2;
  io.println("r=\(r)");
  <3
};
TOKE
    if ! "${TKC}" --allow-all --out "$1_bin" "$1.tk" > "$1.log" 2>&1; then
        echo "  FAIL: $1 did not compile"; sed 's/^/        /' "$1.log" | head -4
        FAIL=$((FAIL + 1)); return 1
    fi
}
# result NAME FN PORT STAGE -> "rc stdout-r start-failed-lines exit_status-lines"
result() {
    printf '%s %s %s %s' "$RC" "$(grep -a '^r=' "$1.out")" \
        "$(grep -a -c "^DIAGNOSTIC: kind=server_start_failed fn=\"$2\" port=$3 stage=\"$4\"$" "$1.err")" \
        "$(grep -a -c '^DIAGNOSTIC: kind=exit_status status=3' "$1.err")"
}

# A port held by a listener that bound port 0 itself and printed it: there
# is no window in which another process can take it.
python3 -c '
import socket, time
s = socket.socket(); s.bind(("", 0)); s.listen(8)
print(s.getsockname()[1], flush=True)
time.sleep(3600)' > hold.port &   # killed by stop below / cleanup
HOLD=$!
tk_wait_until "${HOLD}" tk_nonempty hold.port
BUSY="$(head -1 hold.port)"

# 1-2. a held port
if [ -z "${BUSY}" ]; then
    echo "  FAIL: could not hold a port for cases 1-2"; FAIL=$((FAIL + 2))
else
    if build busyw "http.serveworkers(${BUSY};2)"; then
        run_bounded busyw
        expect "serveworkers on a held port: -1, stage bind/fork, exit 3 (main returned 3)" \
            "$(result busyw http.serveworkers "${BUSY}" bind/fork)" "3 r=-1 1 1" busyw.err
    fi
    if build busys "http.serve(${BUSY})"; then
        run_bounded busys
        expect "serve on a held port: -1, reported as http.serve, exit 3 (main's)" \
            "$(result busys http.serve "${BUSY}" bind/fork)" "3 r=-1 1 1" busys.err
    fi
fi
stop "${HOLD}"; HOLD=""

# 3. an out-of-range port is refused, not replaced by 8080.  If it were
#    replaced, the server would either start (killed after 3s: rc 137) or
#    fail to bind 8080 (stage bind/fork, port=8080) — both read differently.
if build range "http.serveworkers(70000;1)"; then
    ./range_bin > range.out 2> range.err &
    SRV=$!
    for _ in $(seq 1 30); do kill -0 "${SRV}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${SRV}" 2>/dev/null; then stop "${SRV}"; RC=137; else wait "${SRV}"; RC=$?; fi
    SRV=""
    expect "serveworkers(70000;1): stage port, exit 3 (main's), no server on 8080" \
        "$(result range http.serveworkers 70000 port)" "3 r=-1 1 1" range.err
fi

# 4. port 0: a free port, reported, served, and a clean shutdown exits 0
# answers_ok URL -- GET URL says "ok" (a tk_wait_until check, 127.565)
answers_ok() { [ "$(curl -s --max-time 2 "$1" 2>/dev/null)" = "ok" ]; }
if build any "http.serveworkers(0;2)"; then
    ./any_bin > any.out 2> any.err &
    SRV=$!
    PORT=""; up=1
    if tk_wait_until "${SRV}" tk_listening any.err; then
        PORT="$(tk_listening_port any.err)"
        tk_wait_until "${SRV}" answers_ok "http://127.0.0.1:${PORT}/ok" && up=0
    fi
    if [ "${up}" -ne 0 ]; then
        echo "  FAIL: serveworkers(0;2) never answered on a reported port (read back: '${PORT}')"
        sed 's/^/        | /' any.err | head -4; FAIL=$((FAIL + 1))
    else
        expect "serveworkers(0;2) reports one port, not 8080, and answers on it" \
            "$(grep -a -c '^DIAGNOSTIC: kind=server_listening port=' any.err) $([ "${PORT}" != 8080 ] && echo not-8080)" \
            "1 not-8080" any.err
        kill -TERM "${SRV}"; wait "${SRV}"; RC=$?; SRV=""
        expect "serveworkers(0;2) then SIGTERM: exit 3 from main, r=0, no failure lines" \
            "$(printf '%s %s %s %s' "${RC}" "$(grep -a '^r=' any.out)" \
                "$(grep -a -c 'server_start_failed' any.err)" "$(grep -a -c 'kind=exit_status' any.err)")" \
            "3 r=0 0 0" any.err
    fi
fi

echo "-------------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
