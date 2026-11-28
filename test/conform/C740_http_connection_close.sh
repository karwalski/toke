#!/usr/bin/env bash
# C740_http_connection_close.sh -- a handler's "Connection: close" closes
# the connection after its response, and a client's "close" is found in a
# Connection list, on HTTP/1.1 and TLS (story 121.101).
#
# THE DEFECT, measured on main (b442a5d7) with this script.  Since 121.97
# the writer owns Connection (res_framing_field): a handler's Connection
# field is not sent, and the writer's keep-alive decision never looked at
# it.  So a handler could not end a connection -- after a logout, a
# protocol error it detected, or to shed a client -- and a request
# pipelined behind its response was served on the connection it meant to
# close.  (Before 121.97 the field was sent, beside the writer's own
# "Connection: keep-alive", and ignored all the same.)  Separately, the
# request side read only the FIRST token of the first Connection field, so
# "Connection: upgrade, close" or "Connection: keep-alive, close" from a
# client kept the connection open (RFC 9110 sec. 7.6.1: Connection is a
# list; RFC 9112 sec. 9.6: "close" in it means the sender will close).
#
# THE FIX.  After the handler returns, a Connection field on its response
# whose list holds "close" (any case) turns keep-alive off: the response
# goes out with the writer's one "Connection: close", the loop ends, and the
# socket is half-closed and drained (http_linger_close) so bytes the client
# has already pipelined do not turn the close into a TCP reset that could
# discard the response.  The request's Connection fields are read as lists,
# all of them.  HTTP/2 has no Connection field (RFC 9113 sec. 8.2.2); a
# handler's is dropped there as before.
#
# WHAT IS ASSERTED, from the raw bytes over a real socket, plain and TLS:
#   1. GET /bye (handler: Connection: close) with GET /ok pipelined behind
#      it: one response, "Connection: close", and nothing after it -- /ok is
#      not served -- and the server closes (EOF, not a reset);
#   2. GET /byelist (handler: "Keep-Alive, CLOSE") likewise;
#   3. GET /bye with a 64 KiB POST pipelined behind it: the response arrives
#      whole (the close lingers instead of resetting);
#   4. GET /stay (handler: Connection: keep-alive): "keep-alive", and the
#      pipelined /ok IS served -- a handler's keep-alive changes nothing;
#   5. a client's "Connection: upgrade, close" and "Connection: keep-alive,
#      close": "Connection: close", the pipelined request not served;
#   6. a client's "Connection: close" (the control): the same.
#
# Negative control: C740_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against a git archive of main's src/stdlib
# and stdlib (b442a5d7): 32 passed, 20 failed -- every handler close and
# client list case, plain and TLS, answers "keep-alive" and serves the
# pipelined request; the client "close" control passes.  With the close but
# without the linger (plain close()), the 64 KiB case fails 3 runs in 3: the
# client reads a reset and no body.  On this branch: 52 passed.
#
# Story: 121.101, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C740_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c740_XXXXXX")"
cd "${WORK}" || exit 1
echo "C740: Connection: close from a handler or a client list ends the connection (121.101)"
echo "----------------------------------------------------------------------"

cat > handlers.tk.part <<'TK'
f=bye(req:$req):$res{
  <$res{status:200;headers:@(@("Connection";"close");@("X-Bye";"1"));body:"goodbye"}
};
f=byelist(req:$req):$res{
  <$res{status:200;headers:@(@("Connection";"Keep-Alive, CLOSE"));body:"goodbye"}
};
f=stay(req:$req):$res{
  <$res{status:200;headers:@(@("Connection";"keep-alive"));body:"stay"}
};
f=ok(req:$req):$res{
  <$res{status:200;headers:@(@("X-Next";"1"));body:"next"}
};
TK

build() {
    { printf 'm=%s;\ni=http:std.http;\n' "$1"; cat handlers.tk.part
      printf 'f=main():i64{\n  http.gethandler("/bye"; &bye);\n  http.gethandler("/byelist"; &byelist);\n  http.gethandler("/stay"; &stay);\n  http.gethandler("/ok"; &ok);\n  http.posthandler("/ok"; &ok);\n  %s;\n  <0\n};\n' "$2"; } > "$1.tk"
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
import socket, ssl, sys
port = int(sys.argv[1])
tls = len(sys.argv) > 2 and sys.argv[2] == "tls"
def raw(req):
    """send req, read to EOF; returns (bytes, how) with how one of
    'eof', 'timeout', 'reset'"""
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname="localhost")
    out, how = b"", "eof"
    try:
        s.sendall(req)
    except (ConnectionResetError, BrokenPipeError, ssl.SSLError):
        pass   # the server may close before taking all of a large pipeline
    while True:
        try:
            b = s.recv(65536)
        except socket.timeout:
            how = "timeout"; break
        except (ConnectionResetError, ssl.SSLError) as e:
            how = "reset"; break
        if not b:
            break
        out += b
    s.close()
    return out, how
def fields(head):
    d = {}
    for l in head.split(b"\r\n")[1:]:
        if b":" in l:
            k, _, v = l.partition(b":")
            d.setdefault(k.strip().lower(), []).append(v.strip())
    return d
def report(ok, msg):
    print(("OK " if ok else "BAD ") + msg)
def get(path, extra=b"", method=b"GET"):
    return b"%s %s HTTP/1.1\r\nHost: t\r\n%s\r\n" % (method, path, extra)
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
OK = get(b"/ok")

def closes(tag, req, body):
    out, how = raw(req)
    h, f, b, rest = frame(out)
    report(h.startswith(b"HTTP/1.1 200") and b == body, "%s: the response whole (%r %r)" % (tag, h[:15], b))
    report(f.get(b"connection") == [b"close"], "%s: one Connection, close (sent: %r)" % (tag, f.get(b"connection")))
    report(rest == b"", "%s: nothing after it -- the pipelined request is not served (%r)" % (tag, rest[:40]))
    report(how == "eof", "%s: the server closes the connection (%s)" % (tag, how))

# 1, 2. a handler's close, as given and in a list
closes("handler close", get(b"/bye") + OK + OK, b"goodbye")
closes("handler list", get(b"/byelist") + OK, b"goodbye")
# 3. a large request pipelined behind: the close lingers, the response survives
big = b"x" * 65536
closes("handler close, 64 KiB behind",
       get(b"/bye") + get(b"/ok", b"Content-Length: %d\r\nContent-Type: text/plain\r\n" % len(big), b"POST") + big,
       b"goodbye")
# 4. a handler's keep-alive changes nothing
out, how = raw(get(b"/stay") + get(b"/ok", b"Connection: close\r\n"))
h, f, b, rest = frame(out)
report(f.get(b"connection") == [b"keep-alive"] and b == b"stay", "handler keep-alive: kept (sent: %r)" % f.get(b"connection"))
h2, f2, b2, rest2 = frame(rest)
report(h2.startswith(b"HTTP/1.1 200") and f2.get(b"x-next") == [b"1"] and b2 == b"next",
       "handler keep-alive: the pipelined request is served (%r)" % rest[:30])
# 5, 6. a client's close, in a list and alone
closes("client upgrade, close", get(b"/ok", b"Connection: upgrade, close\r\n") + OK, b"next")
closes("client keep-alive, close", get(b"/ok", b"Connection: keep-alive, close\r\n") + OK, b"next")
closes("client close", get(b"/ok", b"Connection: Close\r\n") + OK, b"next")
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
