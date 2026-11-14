#!/usr/bin/env bash
# C341_https_via_proxy.sh — an https request through an http proxy opens the
# CONNECT tunnel first and runs TLS inside it (story 135.26 item 1).
#
# THE DEFECT.  http_request_headers (http.c) connected to the proxy and then
# called SSL_connect on that socket straight away -- a TLS ClientHello sent
# to the proxy, which speaks plain HTTP -- and only afterwards wrote
# "CONNECT host:port".  Every https call through http.withproxy failed.
# It also read the proxy's CONNECT reply with a single recv(), so a reply
# that arrived in two pieces left its tail in the socket for TLS to trip on.
#
# THE FIX (src/stdlib/http.c).  The CONNECT exchange runs on the bare socket
# before any TLS state is made, and the reply is read up to its blank line.
# TLS -- certificate verification and the name check included (138.8) -- is
# then negotiated with the origin through the tunnel.
#
# WHAT IS ASSERTED, all on 127.0.0.1 (a python CONNECT proxy and a python
# TLS origin, each on a port it bound itself; nothing leaves the host):
#   1. https via the proxy, origin trusted: $ok 200; the proxy saw
#      CONNECT 127.0.0.1:<origin port>; the origin served GET /v
#   2. the same through a proxy that sends its 200 reply in two writes
#   3. https via the proxy, origin NOT trusted: $err, the origin served
#      nothing (verification still happens inside the tunnel)
#   4. a proxy that refuses the CONNECT (403): $err
#   5. plain http via the proxy (absolute-URI request line): $ok 200
#
# Negative control: C341_STDLIB=<dir> builds against another stdlib tree.
# Against main (18b9f73) cases 1 and 2 fail (err; the proxy logged a TLS
# ClientHello where a CONNECT should be).  With the tunnel moved but the
# reply still read by one recv() (measured), case 2 fails.
#
# Story: 135.26
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C341_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "CONFORM-SKIP: no openssl(1) on this host to make test certificates"; exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c341_XXXXXX")"
PIDS=""
cleanup() { for p in ${PIDS}; do kill -9 "${p}" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C341: https through a proxy tunnels first (135.26)"
echo "--------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

openssl req -x509 -newkey rsa:2048 -keyout good.key -out good.pem -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" > cert.log 2>&1
if [ ! -s good.pem ]; then
    echo "  FAIL: openssl could not make the test certificate"; sed 's/^/      /' cert.log | head -4
    echo "Results: 0 passed, 1 failed"; exit 1
fi

# origin: TLS (argv[2]=tls) or plain (argv[2]=plain); logs each request line
cat > origin.py <<'PY'
import http.server, ssl, sys
import socketserver as _ss
def _bind(self):  # 127.439 (Z205): HTTPServer.server_bind does a reverse-DNS getfqdn() that stalls for seconds on the macos-15-intel runner
    _ss.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
http.server.HTTPServer.server_bind = _bind
LOG = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        with open(LOG, "a") as f: f.write("GET %s\n" % self.path)
        self.send_response(200); self.send_header("Content-Length", "2")
        self.end_headers(); self.wfile.write(b"ok")
s = http.server.HTTPServer(("127.0.0.1", 0), H)
if sys.argv[2] == "tls":
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain("good.pem", "good.key")
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
print(s.server_address[1], flush=True)
s.serve_forever()
PY

# proxy: mode ok | split | refuse.  Logs the first line of each connection
# (printable bytes only, so a stray ClientHello shows as such).
cat > proxy.py <<'PY'
import socket, sys, threading, time, select
LOG, MODE = sys.argv[1], sys.argv[2]
def log(s):
    with open(LOG, "a") as f: f.write(s + "\n")
def relay(a, b):
    socks = [a, b]
    try:
        while True:
            r, _, _ = select.select(socks, [], [], 10)
            if not r: return
            for s in r:
                d = s.recv(65536)
                if not d: return
                (b if s is a else a).sendall(d)
    finally:
        a.close(); b.close()
def handle(c):
    c.settimeout(5)          # a ClientHello never ends in a blank line
    buf = read_head(c)
    c.settimeout(None)
    try:
        dispatch(c, buf)
    except Exception as e:   # garbage in (a stray ClientHello): hang up
        log("DROP %s" % type(e).__name__)
        c.close()
def read_head(c):
    buf = b""
    while b"\r\n\r\n" not in buf:
        try:
            d = c.recv(4096)
        except OSError:
            break
        if not d: break
        buf += d
        if len(buf) > 65536: break
    return buf
def dispatch(c, buf):
    first = buf.split(b"\r\n", 1)[0]
    shown = "".join(ch if 32 <= ord(ch) < 127 else "?" for ch in first[:60].decode("latin-1"))
    log("LINE " + shown)
    parts = first.split(b" ")
    if len(parts) < 2: c.close(); return
    if parts[0] == b"CONNECT":
        if MODE == "refuse":
            c.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"); c.close(); return
        host, port = parts[1].decode().rsplit(":", 1)
        up = socket.create_connection((host, int(port)))
        if MODE == "split":
            c.sendall(b"HTTP/1.1 200 Connection established\r\n"); time.sleep(0.3)
            c.sendall(b"Proxy-Agent: c341\r\n\r\n")
        else:
            c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
        relay(c, up)
    else:
        # absolute-URI request: http://host:port/path -> forward as origin-form
        url = parts[1].decode()
        hp, _, path = url[len("http://"):].partition("/")
        host, port = hp.rsplit(":", 1)
        up = socket.create_connection((host, int(port)))
        rest = buf.split(b"\r\n", 1)[1]
        up.sendall(parts[0] + b" /" + path.encode() + b" HTTP/1.1\r\n" + rest)
        relay(c, up)
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
PY

# start NAME CMD... -> sets PORT from NAME.port
start() {
    local name=$1; shift
    rm -f "${name}.port"
    "$@" > "${name}.port" 2> "${name}.err" &
    PIDS="${PIDS} $!"
    for _ in $(seq 1 50); do [ -s "${name}.port" ] && break; sleep 0.1; done
    PORT="$(head -1 "${name}.port")"
}

cat > cl.tk <<'TOKE'
m=cl;
i=http:std.http;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let c=http.withproxy(http.client(args.get(1)); args.get(2));
  io.println(mt http.clientget(c;"/v") {$ok:r "ok \(r.status)"; $err:e "err"});
  <0
};
TOKE
if ! "${TKC}" --allow-all --out cl_bin cl.tk > build.log 2>&1; then
    echo "  FAIL: the client does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

start tls python3 origin.py tls.log tls;       TLS_PORT=${PORT}
start plain python3 origin.py plain.log plain; PLAIN_PORT=${PORT}
start pok python3 proxy.py pok.log ok;         P_OK=${PORT}
start psplit python3 proxy.py psplit.log split; P_SPLIT=${PORT}
start pref python3 proxy.py pref.log refuse;   P_REF=${PORT}
: > tls.log; : > plain.log; : > pok.log; : > psplit.log; : > pref.log

TRUST="${WORK}/good.pem"
expect "1 https via proxy: \$ok 200, CONNECT seen, origin served GET /v" \
    "$(SSL_CERT_FILE="${TRUST}" ./cl_bin "https://127.0.0.1:${TLS_PORT}" "http://127.0.0.1:${P_OK}") | $(head -1 pok.log) | $(grep -a -c '^GET /v' tls.log)" \
    "ok 200 | LINE CONNECT 127.0.0.1:${TLS_PORT} HTTP/1.1 | 1"
expect "2 the proxy's 200 reply in two writes: \$ok 200" \
    "$(SSL_CERT_FILE="${TRUST}" ./cl_bin "https://127.0.0.1:${TLS_PORT}" "http://127.0.0.1:${P_SPLIT}")" "ok 200"
: > tls.log
expect "3 origin not trusted, via proxy: \$err, origin served nothing" \
    "$(env -u SSL_CERT_FILE -u SSL_CERT_DIR ./cl_bin "https://127.0.0.1:${TLS_PORT}" "http://127.0.0.1:${P_OK}") $(grep -a -c '^GET /v' tls.log)" \
    "err 0"
expect "4 proxy refuses the CONNECT: \$err" \
    "$(SSL_CERT_FILE="${TRUST}" ./cl_bin "https://127.0.0.1:${TLS_PORT}" "http://127.0.0.1:${P_REF}")" "err"
expect "5 plain http via proxy: \$ok 200, absolute-URI request line" \
    "$(./cl_bin "http://127.0.0.1:${PLAIN_PORT}" "http://127.0.0.1:${P_OK}") | $(grep -a -c "^LINE GET http://127.0.0.1:${PLAIN_PORT}/v " pok.log)" \
    "ok 200 | 1"

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
