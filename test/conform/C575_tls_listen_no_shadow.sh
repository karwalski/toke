#!/usr/bin/env bash
# C575_tls_listen_no_shadow.sh — tls.listen cannot start beside a live
# listener it would shadow, and still restarts over TIME_WAIT (135.34 (3)).
#
# THE DEFECT.  tls_listen() (src/stdlib/tls.c) set SO_REUSEADDR before its
# wildcard bind — the last bind site 135.32/135.33 left.  On Darwin that lets
# *:P bind beside a live 127.0.0.1:P listener, so tls.listen on a busy port
# "started" and sat in accept() while the other server kept loopback.  It now
# binds through tk_bind_exclusive() (tk_bind.h), like http/router/net.
#
# WHAT IS ASSERTED (a toke program: genselfsigned -> tlsconfig -> tls.listen
# on the port given as argv[1], then prints "returned" and exits 3 if listen
# gives up; each case uses a fresh port):
#   1. A = a plain listener on 127.0.0.1:P (python, SO_REUSEADDR set, as a
#      typical server does).  The program must return from tls.listen with
#      false within 5 s ("returned", exit 3); A still accepts on 127.0.0.1:P.
#   2. P holds a server-side TIME_WAIT socket (asserted from netstat/ss, so
#      the case cannot pass vacuously) and nothing listens: the program
#      binds and serves — still running after 2 s and accepting a TCP
#      connection on 127.0.0.1:P.
#
# Negative control: C575_STDLIB=<main's src/stdlib at cde89021, stdlib/
# beside it> — case 1 red on Darwin ("running": the wildcard bind succeeded
# beside A); case 2 green.  On Linux case 1 passes either way (Linux refuses
# an overlapping bind with or without SO_REUSEADDR): that is the point.
#
# Story: 135.34 (3)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C575_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 is needed for the peer listener"; exit 0; }
if command -v netstat >/dev/null 2>&1 && [ "$(uname -s)" = Darwin ]; then TW=netstat
elif command -v ss >/dev/null 2>&1; then TW=ss
else echo "CONFORM-SKIP: neither netstat (Darwin) nor ss to see TIME_WAIT sockets"; exit 0; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c575_XXXXXX")"
PIDS=()
stop() { [ -n "$1" ] || return 0; kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; }
cleanup() { for p in "${PIDS[@]+"${PIDS[@]}"}"; do stop "$p"; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; echo "        expected: $2"; echo "        got:      $3"; FAIL=$((FAIL + 1)); }
echo "C575: tls.listen cannot shadow a live listener; TIME_WAIT still rebinds"

# a free port
P="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"

cat > srv.tk <<EOF
m=srv;
i=io:std.io;
i=tls:std.tls;
i=args:std.args;
i=str:std.str;
f=onconn(conn:i64):i64{
  tls.close(conn);
  <0
};
f=main():i64{
  let kp=mt tls.genselfsigned("localhost";1) {\$ok:k k;\$err:e <2};
  let cfg=tls.tlsconfig(tls.certof(kp);tls.keyof(kp));
  let port=mt str.toint(args.get(1)) {\$ok:n n;\$err:e <4};
  let r=tls.listen(port as i32;cfg;&onconn);
  io.println("returned");
  <3
};
EOF
if ! "${TKC}" --allow-all --out srv srv.tk > build.log 2>&1; then
    echo "  FAIL: srv does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

# run_srv -> "returned <rc>" if it exits within $1 s, else "running"
run_srv() {
    ./srv "${P}" > srv.out 2> srv.err &
    local pid=$!; PIDS+=("${pid}")
    for _ in $(seq 1 $(( $1 * 10 ))); do kill -0 "${pid}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${pid}" 2>/dev/null; then
        SRVPID="${pid}"; echo "running"
    else
        local rc=0; wait "${pid}" || rc=$?; SRVPID=""
        echo "$(tr -d '\n' < srv.out) ${rc}"
    fi
}

# ── 1: a live listener on 127.0.0.1:P ────────────────────────────────────
python3 - "${P}" > peer.out 2>&1 <<'PY' &
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(16)
print("up", flush=True)
s.settimeout(30)
n = 0
while True:
    try: c, _ = s.accept()
    except Exception: break
    n += 1; c.close()
PY
PEER=$!; PIDS+=("${PEER}")
for _ in $(seq 1 50); do grep -q up peer.out && break; sleep 0.1; done
got="$(run_srv 5)"
[ -n "${SRVPID:-}" ] && stop "${SRVPID}"
reach="$(python3 -c "import socket,sys;s=socket.create_connection(('127.0.0.1',${P}),2);print('reached');s.close()" 2>/dev/null)"
if [ "${got}" = "returned 3" ] && [ "${reach}" = "reached" ]; then
    ok "1: tls.listen beside a live 127.0.0.1:${P} gives up; the listener keeps its port"
else
    bad "1: tls.listen beside a live listener" "returned 3 / reached" "${got} / ${reach:-unreachable}"
fi
stop "${PEER}"

# ── 2: TIME_WAIT on P, nothing listening (a fresh port: case 1's state on
# main — a shadowing server killed mid-accept — must not leak into it) ────
P="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
python3 - "${P}" <<'PY'
import socket, sys, time
p = int(sys.argv[1])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", p)); s.listen(4)
c = socket.create_connection(("127.0.0.1", p)); a, _ = s.accept()
a.close()           # the server side closes first -> TIME_WAIT on local port p
time.sleep(0.2); c.close(); s.close()
PY
tw() {
    if [ "${TW}" = netstat ]; then
        netstat -an -p tcp 2>/dev/null | awk -v p="$1" '$6=="TIME_WAIT" && $4 ~ ("\\." p "$") {n++} END{print n+0}'
    else
        ss -tanH state time-wait "sport = :$1" 2>/dev/null | wc -l | tr -d ' '
    fi
}
n_tw="$(tw "${P}")"
if [ "${n_tw}" -lt 1 ]; then
    bad "2: precondition — a TIME_WAIT socket on local port ${P}" ">= 1" "${n_tw}"
else
    got="$(run_srv 2)"
    reach=""
    [ "${got}" = "running" ] && reach="$(python3 -c "import socket;s=socket.create_connection(('127.0.0.1',${P}),2);print('reached');s.close()" 2>/dev/null)"
    [ -n "${SRVPID:-}" ] && stop "${SRVPID}"
    if [ "${got}" = "running" ] && [ "${reach}" = "reached" ]; then
        ok "2: tls.listen binds over TIME_WAIT (${n_tw} socket(s)) and accepts"
    else
        bad "2: tls.listen over TIME_WAIT" "running / reached" "${got} / ${reach:-unreachable}"
    fi
fi
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
