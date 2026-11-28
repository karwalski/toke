#!/usr/bin/env bash
# C743_http10_keepalive.sh -- an HTTP/1.0 request closes the connection
# after its response unless it sent "Connection: keep-alive", on plain and
# TLS (story 121.102).
#
# THE DEFECT, measured on main (0936dc02) with this script.  req_wants_close
# looked only for "close" in the request's Connection fields and otherwise
# kept the connection, whatever the request's version.  RFC 9112 sec. 9.3:
# HTTP/1.1 defaults to persistence, but an HTTP/1.0 connection persists only
# when the client sent "Connection: keep-alive".  So a 1.0 request without
# it got "Connection: keep-alive", a request pipelined behind it was
# served, and a 1.0 client that reads the response to EOF (the 1.0 way)
# waited for the idle timeout (2 s) before it saw the end.
#
# THE FIX.  req_wants_close reads the version on the request line
# (req_is_http10, case-sensitive per RFC 9112 sec. 2.3): "close" in any
# Connection list closes on every version; otherwise HTTP/1.1 keeps the
# connection and HTTP/1.0 keeps it only when a Connection list holds
# "keep-alive" (any case).  A kept 1.0 connection is answered with the
# writer's "Connection: keep-alive", as the RFC's 1.0 keep-alive mechanism
# expects; a closed one with "Connection: close".
#
# WHAT IS ASSERTED, from the raw bytes over a real socket, plain and TLS:
#   1. a 1.0 GET with no Connection field, another pipelined behind it: one
#      response, "Connection: close", nothing after it, and EOF well inside
#      the 2 s idle timeout;
#   2. a 1.0 GET with "Connection: keep-alive": "Connection: keep-alive",
#      and the 1.0 GET pipelined behind it (no keep-alive) IS served, with
#      "Connection: close", then EOF at once;
#   3. a 1.0 GET listing "Connection: x-foo, Keep-Alive": kept likewise;
#   4. a 1.0 GET with "Connection: keep-alive, close": closed (close wins);
#   5. the control: a 1.1 GET with no Connection field is kept, and the
#      pipelined request served (121.101 behaviour unchanged).
#
# Negative control: C743_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against a git archive of main's src/stdlib
# and stdlib (0936dc02): 32 passed, 14 failed -- every 1.0 request without
# keep-alive answers "keep-alive", serves the pipelined request and reaches
# EOF only at the 2 s idle timeout; close-wins and the 1.1 control pass.
# On this branch: 46 passed.
#
# Story: 121.102, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C743_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c743_XXXXXX")"
cd "${WORK}" || exit 1
echo "C743: an HTTP/1.0 request without keep-alive closes the connection (121.102)"
echo "----------------------------------------------------------------------"

cat > handlers.tk.part <<'TK'
f=ok(req:$req):$res{
  <$res{status:200;headers:@(@("X-Next";"1"));body:"next"}
};
TK

build() {
    { printf 'm=%s;\ni=http:std.http;\n' "$1"; cat handlers.tk.part
      printf 'f=main():i64{\n  http.gethandler("/ok"; &ok);\n  %s;\n  <0\n};\n' "$2"; } > "$1.tk"
    "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1
}
start() {
    ./"$1" >/dev/null 2>"$1.err" & SRV=$!
    PORT=""
    tk_wait_until "${SRV}" tk_listening "$1.err"
    PORT="$(tk_listening_port "$1.err")"
}
trap 'stop; rm -rf "${WORK}"' EXIT
stop() { [ -n "${SRV}" ] && { pkill -P "${SRV}" 2>/dev/null; kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; SRV=""; }
collect() {
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "$1${line#OK }" ;; BAD\ *) fail "$1${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' cli.out)
    grep -a -q -E '^(OK|BAD) ' cli.out || fail "$1the client produced nothing: $(head -c 300 cli.out)"
    grep -a -q -E '^Traceback' cli.out && fail "$1the client failed: $(tail -2 cli.out | head -c 300)"
}

cat > cli.py <<'PY'
import socket, ssl, sys, time
port = int(sys.argv[1])
tls = len(sys.argv) > 2 and sys.argv[2] == "tls"
def raw(req):
    """send req, read to EOF; returns (bytes, how, seconds) with how one of
    'eof', 'timeout', 'reset'"""
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname="localhost")
    out, how = b"", "eof"
    t0 = time.monotonic()
    s.sendall(req)
    while True:
        try:
            b = s.recv(65536)
        except socket.timeout:
            how = "timeout"; break
        except (ConnectionResetError, ssl.SSLError):
            how = "reset"; break
        if not b:
            break
        out += b
    dt = time.monotonic() - t0
    s.close()
    return out, how, dt
def fields(head):
    d = {}
    for l in head.split(b"\r\n")[1:]:
        if b":" in l:
            k, _, v = l.partition(b":")
            d.setdefault(k.strip().lower(), []).append(v.strip())
    return d
def report(ok, msg):
    print(("OK " if ok else "BAD ") + msg)
def get(ver, extra=b""):
    return b"GET /ok %s\r\nHost: t\r\n%s\r\n" % (ver, extra)
def dechunk(b):
    out = b""
    while b:
        n, _, b = b.partition(b"\r\n")
        try:
            n = int(n, 16)
        except ValueError:
            return None, b""
        if n == 0:
            return out, b[2:] if b.startswith(b"\r\n") else b
        out += b[:n]
        b = b[n + 2:]
    return None, b""
def frame(out):
    h, _, rest = out.partition(b"\r\n\r\n")
    f = fields(h)
    if f.get(b"transfer-encoding") == [b"chunked"]:
        body, rest = dechunk(rest)
        return h, f, body, rest
    n = int(f[b"content-length"][0]) if f.get(b"content-length") else 0
    return h, f, rest[:n], rest[n:]
H10, H11 = b"HTTP/1.0", b"HTTP/1.1"

def closes(tag, req):
    out, how, dt = raw(req)
    h, f, b, rest = frame(out)
    report(h.startswith(b"HTTP/1.1 200") and b == b"next", "%s: the response whole (%r %r)" % (tag, h[:15], b))
    report(f.get(b"connection") == [b"close"], "%s: Connection: close (sent: %r)" % (tag, f.get(b"connection")))
    report(rest == b"", "%s: nothing after it -- the pipelined request is not served (%r)" % (tag, rest[:40]))
    report(how == "eof" and dt < 1.0, "%s: the server closes at once (%s after %.2f s)" % (tag, how, dt))

def kept(tag, req):
    out, how, dt = raw(req)
    h, f, b, rest = frame(out)
    report(h.startswith(b"HTTP/1.1 200") and b == b"next", "%s: the first response whole (%r %r)" % (tag, h[:15], b))
    report(f.get(b"connection") == [b"keep-alive"], "%s: Connection: keep-alive (sent: %r)" % (tag, f.get(b"connection")))
    h2, f2, b2, rest2 = frame(rest)
    report(h2.startswith(b"HTTP/1.1 200") and b2 == b"next", "%s: the pipelined request is served (%r)" % (tag, rest[:30]))
    report(f2.get(b"connection") == [b"close"] and rest2 == b"", "%s: the second (no keep-alive) closes (sent: %r)" % (tag, f2.get(b"connection")))
    report(how == "eof" and dt < 1.0, "%s: then EOF at once (%s after %.2f s)" % (tag, how, dt))

# 1. HTTP/1.0 without keep-alive closes
closes("1.0 no Connection", get(H10) + get(H10))
# 2, 3. HTTP/1.0 with keep-alive, alone and in a list, is kept
kept("1.0 keep-alive", get(H10, b"Connection: keep-alive\r\n") + get(H10))
kept("1.0 x-foo, Keep-Alive", get(H10, b"Connection: x-foo, Keep-Alive\r\n") + get(H10))
# 4. close wins over keep-alive
closes("1.0 keep-alive, close", get(H10, b"Connection: keep-alive, close\r\n") + get(H10))
# 5. control: HTTP/1.1 defaults to keep-alive
kept("1.1 no Connection (control)", get(H11) + get(H11, b"Connection: close\r\n"))
PY

if ! build srv 'http.serveon("127.0.0.1"; 0)'; then
    fail "the server does not build: $(grep -m1 '^error' srv.log)"
else
    start srv
    if [ -z "${PORT}" ]; then fail "the server did not start: $(head -3 srv.err)"
    else python3 cli.py "${PORT}" > cli.out 2>&1; collect ""; fi
    stop
fi

if ! command -v openssl >/dev/null 2>&1; then
    fail "tls: no openssl(1) on this host to make a test cert"
elif ! openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 2 \
        -nodes -subj "/CN=localhost" > cert.log 2>&1 || [ ! -s cert.pem ]; then
    fail "tls: openssl could not make a test cert"
elif ! build srvtls "http.servetlson(\"127.0.0.1\"; 0; \"${WORK}/cert.pem\"; \"${WORK}/key.pem\")"; then
    fail "tls: the server does not build: $(grep -m1 '^error' srvtls.log)"
else
    start srvtls
    if [ -z "${PORT}" ]; then fail "tls: the server did not start: $(head -3 srvtls.err)"
    else python3 cli.py "${PORT}" tls > cli.out 2>&1; collect "tls: "; fi
    stop
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
