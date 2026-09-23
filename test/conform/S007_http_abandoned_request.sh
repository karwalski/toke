#!/usr/bin/env bash
# S007_http_abandoned_request.sh — a client that abandons its request must not
# kill the server (story 127.144).
#
# THE DEFECT.  `SIGPIPE`, `SO_NOSIGPIPE` and `MSG_NOSIGNAL` appeared ZERO
# times anywhere in src/.  Every write to a client socket therefore carried
# the default SIGPIPE disposition, which terminates the process.  One client
# that sends a request and resets before reading the answer was enough.
#
# MEASURED ON THIS TREE BEFORE THE FIX, macOS/arm64, one abandoned request:
#
#   http.serveworkers(p;1)   SERVER EXIT=141    (128 + SIGPIPE)
#   http.servetls(p;...)     SERVER EXIT=141
#   http.serve(p)            parent survives, BOTH worker pids replaced —
#                            one worker killed per abandoned request, and the
#                            supervisor respawns into the next one
#
# The three paths behave differently, which is why all three are driven here.
#
# THIS IS NOT A CRAFTED ATTACK.  It is what a browser does on navigation: it
# has read part of a response, the user clicks away, the socket closes with
# data still unread, and the peer sees RST.  It also damages evidence — a
# worker killed and respawned mid-run carries its leaked memory away with it,
# which is what destroyed a 200,000-request leak measurement under 127.72.
#
# WHY THE ASSERTION IS LOAD-INDEPENDENT.  It is not a count over many
# operations, so a slow or contended machine cannot make it pass for the
# wrong reason: after a fixed, small number of abandoned requests the server
# must still be alive, must still answer correctly, and — on the http.serve
# path — must still be running the SAME worker pids it started with.  A
# respawn is a change of pid, and a contended machine does not change a pid.
#
# Story: 127.144

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_s007_XXXXXX)"
SRV=""
cleanup() {
    [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null
    [ -n "${SRV}" ] && pkill -P "${SRV}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "S007: an abandoned request does not kill the server"
echo "--------------------------------------"

free_port() {
    python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

wait_up() {   # $1 = port, $2 = "tls" | "plain"
    for _ in $(seq 1 80); do
        python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.3)
sys.exit(0 if s.connect_ex(('127.0.0.1',$1))==0 else 1)" && return 0
        sleep 0.25
    done
    return 1
}

# ── the clients ──────────────────────────────────────────────────────────
# Plain: send the request, then close with SO_LINGER {1,0} so the kernel
# sends RST rather than FIN.  The server's next write into that socket is
# the one that used to raise SIGPIPE.
cat > "${WORK}/reset_plain.py" <<'PY'
import socket, struct, sys
port = int(sys.argv[1])
s = socket.socket()
s.connect(("127.0.0.1", port))
s.sendall(b"GET /hi HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s.close()
PY

# TLS: same, but the RST has to land between two SSL_write()s, so the
# response is large enough to take more than one and the client waits a beat
# before resetting.  detach() drops the SSLSocket without a close_notify.
cat > "${WORK}/reset_tls.py" <<'PY'
import socket, ssl, struct, sys, time
port = int(sys.argv[1])
ctx = ssl.create_default_context()
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
raw = socket.socket()
raw.connect(("127.0.0.1", port))
raw.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
s = ctx.wrap_socket(raw, server_hostname="localhost")
s.sendall(b"GET /big HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n")
time.sleep(0.05)
s.detach()
raw.close()
PY

BODY="hello world this is a response body long enough to matter"

# ── Case 1: http.serveworkers(port;1) — the single-process path ──────────
PORT="$(free_port)"
cat > "${WORK}/one.tk" <<EOF
m=srv;
i=http:std.http;
f=hello(req:i64):i64{
  <http.resok("${BODY}")
};
f=main():i64{
  http.gethandler("/hi";&hello);
  http.serveworkers(${PORT};1);
  <0
};
EOF
if ! (cd "${WORK}" && "${TKC}" --allow-all -o one.bin one.tk >one.log 2>&1); then
    echo "  FAIL: serveworkers server did not build"
    sed 's/^/      /' "${WORK}/one.log" | head -6
    FAIL=$((FAIL + 1))
else
    "${WORK}/one.bin" >/dev/null 2>&1 &
    SRV=$!
    if ! wait_up "${PORT}"; then
        echo "  FAIL: serveworkers server never came up"
        FAIL=$((FAIL + 1))
    else
        for _ in 1 2 3; do python3 "${WORK}/reset_plain.py" "${PORT}"; done
        sleep 0.5
        if ! kill -0 "${SRV}" 2>/dev/null; then
            wait "${SRV}"; st=$?
            echo "  FAIL: serveworkers(port;1) died — SERVER EXIT=${st} (141 = SIGPIPE)"
            FAIL=$((FAIL + 1)); SRV=""
        else
            got="$(curl -s --max-time 10 "http://127.0.0.1:${PORT}/hi")"
            if [ "${got}" = "${BODY}" ]; then
                echo "  PASS: serveworkers(port;1) survived 3 abandoned requests and still answers"
                PASS=$((PASS + 1))
            else
                echo "  FAIL: serveworkers(port;1) is alive but answered [${got}]"
                FAIL=$((FAIL + 1))
            fi
        fi
    fi
    [ -n "${SRV}" ] && { kill "${SRV}" 2>/dev/null; pkill -P "${SRV}" 2>/dev/null; SRV=""; }
fi

# ── Case 2: http.serve — the pre-fork path; a worker must not be lost ────
PORT="$(free_port)"
cat > "${WORK}/many.tk" <<EOF
m=srv;
i=http:std.http;
f=hello(req:i64):i64{
  <http.resok("${BODY}")
};
f=main():i64{
  http.gethandler("/hi";&hello);
  http.serve(${PORT});
  <0
};
EOF
if ! (cd "${WORK}" && "${TKC}" --allow-all -o many.bin many.tk >many.log 2>&1); then
    echo "  FAIL: http.serve server did not build"
    sed 's/^/      /' "${WORK}/many.log" | head -6
    FAIL=$((FAIL + 1))
else
    "${WORK}/many.bin" >/dev/null 2>&1 &
    SRV=$!
    if ! wait_up "${PORT}"; then
        echo "  FAIL: http.serve server never came up"
        FAIL=$((FAIL + 1))
    else
        before="$(pgrep -P "${SRV}" | sort | tr '\n' ' ')"
        for _ in 1 2 3; do python3 "${WORK}/reset_plain.py" "${PORT}"; done
        sleep 0.7
        after="$(pgrep -P "${SRV}" | sort | tr '\n' ' ')"
        got="$(curl -s --max-time 10 "http://127.0.0.1:${PORT}/hi")"
        if [ -z "${before}" ]; then
            echo "  FAIL: http.serve reported no workers to watch"
            FAIL=$((FAIL + 1))
        elif [ "${before}" != "${after}" ]; then
            echo "  FAIL: http.serve lost a worker to an abandoned request"
            echo "        workers before: ${before}"
            echo "        workers after:  ${after}"
            FAIL=$((FAIL + 1))
        elif [ "${got}" != "${BODY}" ]; then
            echo "  FAIL: http.serve kept its workers but answered [${got}]"
            FAIL=$((FAIL + 1))
        else
            echo "  PASS: http.serve kept the same workers (${after}) across 3 abandoned requests"
            PASS=$((PASS + 1))
        fi
    fi
    [ -n "${SRV}" ] && { kill "${SRV}" 2>/dev/null; pkill -P "${SRV}" 2>/dev/null; SRV=""; }
fi

# ── Case 3: http.servetls — OpenSSL writes through write(2), not our send ──
if ! command -v openssl >/dev/null 2>&1; then
    echo "  NOT RUN: servetls case — no openssl(1) on this host to make a test cert"
else
    openssl req -x509 -newkey rsa:2048 -keyout "${WORK}/key.pem" \
        -out "${WORK}/cert.pem" -days 2 -nodes -subj "/CN=localhost" \
        >"${WORK}/cert.log" 2>&1
    if [ ! -s "${WORK}/cert.pem" ]; then
        echo "  NOT RUN: servetls case — openssl could not make a test cert"
        sed 's/^/      /' "${WORK}/cert.log" | head -4
    else
        PORT="$(free_port)"
        cat > "${WORK}/tls.tk" <<EOF
m=srv;
i=http:std.http;
i=str:std.str;
f=big(req:i64):i64{
  let b=str.repeat("0123456789";200000);
  <http.resok(b)
};
f=main():i64{
  http.gethandler("/big";&big);
  http.servetls(${PORT};"${WORK}/cert.pem";"${WORK}/key.pem");
  <0
};
EOF
        if ! (cd "${WORK}" && "${TKC}" --allow-all -o tls.bin tls.tk >tls.log 2>&1); then
            echo "  FAIL: servetls server did not build"
            sed 's/^/      /' "${WORK}/tls.log" | head -6
            FAIL=$((FAIL + 1))
        else
            TK_HTTP_WORKERS=1 "${WORK}/tls.bin" >/dev/null 2>&1 &
            SRV=$!
            if ! wait_up "${PORT}"; then
                echo "  FAIL: servetls server never came up"
                FAIL=$((FAIL + 1))
            else
                for _ in 1 2 3; do python3 "${WORK}/reset_tls.py" "${PORT}" 2>/dev/null; done
                sleep 0.5
                if ! kill -0 "${SRV}" 2>/dev/null; then
                    wait "${SRV}"; st=$?
                    echo "  FAIL: servetls died — SERVER EXIT=${st} (141 = SIGPIPE)"
                    FAIL=$((FAIL + 1)); SRV=""
                else
                    n="$(curl -sk --max-time 20 "https://127.0.0.1:${PORT}/big" | wc -c | tr -d ' ')"
                    if [ "${n}" = "2000000" ]; then
                        echo "  PASS: servetls survived 3 abandoned requests and still serves 2000000 bytes"
                        PASS=$((PASS + 1))
                    else
                        echo "  FAIL: servetls is alive but served ${n} bytes, expected 2000000"
                        FAIL=$((FAIL + 1))
                    fi
                fi
            fi
            [ -n "${SRV}" ] && { kill "${SRV}" 2>/dev/null; pkill -P "${SRV}" 2>/dev/null; SRV=""; }
        fi
    fi
fi

# ── Case 4: the source-level fact that started this ──────────────────────
# Not a substitute for the three behavioural cases above; it pins the funnel
# so a future write(fd,...) added to a response path is caught in review
# rather than by a dead server.
# The single legitimate raw write() left in the tree is http_write_all's own
# ENOTSOCK fallback, matched by text so a stray write() on the same line
# number is not waved through.
stray="$(awk '
  /n = write\(fd, p \+ off, len - off\);/ { next }          # http_write_all itself
  /[^_a-zA-Z]write\(fd,|write\(sse->fd,|write\(client_fd,|write\(backend_fd,|write\(w->fd,/ {
      print FILENAME":"FNR": "$0
  }' "${REPO_ROOT}/src/stdlib/http.c" "${REPO_ROOT}/src/stdlib/ws_server.c")"
if [ -n "${stray}" ]; then
    echo "  FAIL: a client-socket write bypasses http_write_all:"
    echo "${stray}" | sed 's/^/      /'
    FAIL=$((FAIL + 1))
else
    echo "  PASS: every client-socket write in http.c and ws_server.c goes through http_write_all"
    PASS=$((PASS + 1))
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
