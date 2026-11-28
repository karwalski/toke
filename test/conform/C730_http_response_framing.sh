#!/usr/bin/env bash
# C730_http_response_framing.sh -- std.http frames every response itself:
# one Content-Length (or Transfer-Encoding: chunked) that matches the body
# sent, one Connection, and no body or length on 1xx/204/304 (story 121.97).
#
# THE DEFECT, measured on main (837e5f81) with this script.  http.c's
# HTTP/1.1 writers (send_response's fixed-length and chunked paths, HEAD,
# gzip, and their TLS copies) wrote their own status line, Content-Length or
# Transfer-Encoding and Connection, then every header the handler's $res
# carried -- including the handler's own Content-Length, Transfer-Encoding
# and Connection:
#   - a handler's Content-Length that differed from the body gave the
#     response two lengths ("Content-Length: 11" then "Content-Length: 5"),
#     and RFC 9112 sec. 6.3 lets a recipient frame by either: a proxy and
#     the server behind it can disagree on where the response ends (the
#     request-smuggling class);
#   - a handler's Transfer-Encoding went out beside the writer's
#     Content-Length, or as a second Transfer-Encoding line;
#   - a handler's Connection went out beside the writer's, contradicting it;
#   - a 204 (the CORS preflight answer, or a handler's) carried
#     "Content-Length: 0", which RFC 9110 sec. 8.6 forbids, and a handler's
#     204 or 304 with a body sent the body -- bytes a client reads as the
#     start of the NEXT response on the connection.
#
# THE FIX.  router.c's rule since 121.90: framing is the writer's own.
# res_headers_emit skips a handler's Content-Length, Transfer-Encoding and
# Connection; every HTTP/1.1 writer (plain and TLS) starts from one
# function (res_head_fmt) that writes the computed length or chunking; and
# 1xx/204/304 go out with no body and neither field.  A handler's
# Content-Length still chooses fixed-length over chunked framing, as before;
# only its value is not sent.  HTTP/2 already dropped all three
# (h2_field_name) and computed content-length from the body.
#
# WHAT IS ASSERTED, over a real socket against a toke server (http.serveon),
# from the raw bytes:
#   1. GET /cl (Content-Length "5" on an 11-byte body): one Content-Length,
#      11, the body whole, and the next pipelined response starts right
#      after it (every case pipelines a GET /ok behind it, and the client
#      frames by the FIRST length it sees, as a recipient may);
#   2. GET /clhuge (Content-Length "999" on a 1-byte body): one, 1;
#   3. GET /te (Transfer-Encoding "gzip, chunked", no Content-Length): one
#      framing -- chunked alone (plain) or the body's length alone (TLS,
#      whose writer always sends a length) -- the handler's coding not
#      sent, the body whole;
#   4. GET /tecl (Transfer-Encoding "chunked" AND Content-Length "3"): one
#      Content-Length matching the body, no Transfer-Encoding;
#   5. GET /conn (Connection "keep-alive") with Connection: close: one
#      Connection line, "close";
#   6. GET /nc (204, Content-Length "4", body "oops") and GET /nm (304, body):
#      no Content-Length, no Transfer-Encoding, no body -- the pipelined
#      request behind it is answered next, from the first byte;
#   7. HEAD /cl: one Content-Length, 11, no body; HEAD /nc: none;
#   8. GET /gz (Accept-Encoding: gzip, Content-Length "5"): one
#      Content-Length, the gzip length, and the body inflates;
#   9. OPTIONS with a matching Origin (http.setcors): 204 with no
#      Content-Length;
#   all of it again over TLS (http.servetlson).
#
# Negative control: C730_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against a git archive of main's src/stdlib
# and stdlib (837e5f81): 25 passed, 37 failed -- every framing assertion
# fails over both plain and TLS (two Content-Lengths, "Transfer-Encoding:
# chunked" twice, both framings at once on TLS, two Connection lines, a 204
# with "Content-Length: 4" twice and its body sent, a 304's body sent, the
# preflight's "Content-Length: 0"); what passes is the status lines, the
# first length, the gzip body.  On this branch: 62 passed.
#
# Story: 121.97, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C730_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c730_XXXXXX")"
cd "${WORK}" || exit 1
echo "C730: std.http frames its own responses (121.97)"
echo "----------------------------------------------------------------------"

cat > handlers.tk.part <<'TK'
f=cl(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Length";"5");@("Content-Type";"text/plain"));body:"hello world"}
};
f=clhuge(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Length";"999"));body:"x"}
};
f=te(req:$req):$res{
  <$res{status:200;headers:@(@("Transfer-Encoding";"gzip, chunked"));body:"chunky body"}
};
f=tecl(req:$req):$res{
  <$res{status:200;headers:@(@("Transfer-Encoding";"chunked");@("Content-Length";"3"));body:"fixed body"}
};
f=conn(req:$req):$res{
  <$res{status:200;headers:@(@("Connection";"keep-alive");@("X-Good";"1"));body:"conn"}
};
f=nc(req:$req):$res{
  <$res{status:204;headers:@(@("Content-Length";"4"));body:"oops"}
};
f=nm(req:$req):$res{
  <$res{status:304;headers:@(@("ETag";"\"v1\""));body:"stale body"}
};
f=ok(req:$req):$res{
  <$res{status:200;headers:@(@("X-Next";"1"));body:"next"}
};
f=gz(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Type";"text/plain");@("Content-Length";"5"));body:str.repeat("hello world ";200)}
};
TK

# build NAME MAIN-BODY: a server with the handlers above
build() {
    { printf 'm=%s;\ni=http:std.http;\ni=str:std.str;\n' "$1"; cat handlers.tk.part
      printf 'f=main():i64{\n  http.setcors("*");\n  http.gethandler("/cl"; &cl);\n  http.gethandler("/clhuge"; &clhuge);\n  http.gethandler("/te"; &te);\n  http.gethandler("/tecl"; &tecl);\n  http.gethandler("/conn"; &conn);\n  http.gethandler("/nc"; &nc);\n  http.gethandler("/nm"; &nm);\n  http.gethandler("/ok"; &ok);\n  http.gethandler("/gz"; &gz);\n  %s;\n  <0\n};\n' "$2"; } > "$1.tk"
    "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1
}
# start NAME: run it, wait for its port; sets SRV and PORT
start() {
    ./"$1" >/dev/null 2>"$1.err" & SRV=$!
    PORT=""
    tk_wait_until "${SRV}" tk_listening "$1.err"
    PORT="$(tk_listening_port "$1.err")"
}
trap 'stop; rm -rf "${WORK}"' EXIT
stop() { [ -n "${SRV}" ] && { pkill -P "${SRV}" 2>/dev/null; kill "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; SRV=""; }
# collect TAG: the client's OK/BAD lines as PASS/FAIL, prefixed with TAG
collect() {
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "$1${line#OK }" ;; BAD\ *) fail "$1${line#BAD }" ;; esac
    done < <(grep -a -E '^(OK|BAD) ' cli.out)
    grep -a -q -E '^(OK|BAD) ' cli.out || fail "$1the client produced nothing: $(head -c 300 cli.out)"
    grep -a -q -E '^Traceback' cli.out && fail "$1the client failed: $(tail -2 cli.out | head -c 300)"
}

# The client reads raw bytes and frames responses itself: curl would refuse
# or normalise the very responses this test has to see.
cat > cli.py <<'PY'
import socket, ssl, sys, zlib
port = int(sys.argv[1])
tls = len(sys.argv) > 2 and sys.argv[2] == "tls"
def raw(req):
    s = socket.create_connection(("127.0.0.1", port), timeout=10)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname="localhost")
    s.sendall(req)
    out = b""
    while True:
        try:
            b = s.recv(65536)
        except (socket.timeout, ssl.SSLError):
            break
        if not b:
            break
        out += b
    s.close()
    return out
def fields(head):
    d = {}
    for l in head.split(b"\r\n")[1:]:
        if b":" in l:
            k, _, v = l.partition(b":")
            d.setdefault(k.strip().lower(), []).append(v.strip())
    return d
def report(ok, msg):
    print(("OK " if ok else "BAD ") + msg)
def get(path, method=b"GET", extra=b""):
    return b"%s %s HTTP/1.1\r\nHost: t\r\n%s\r\n" % (method, path, extra)
def dechunk(b):
    out = b""
    while b:
        n, _, b = b.partition(b"\r\n")
        try:
            n = int(n, 16)
        except ValueError:        # not a chunk: the framing is broken
            return None, b""
        if n == 0:
            return out, b[2:] if b.startswith(b"\r\n") else b
        out += b[:n]
        b = b[n + 2:]
    return None, b""
def frame(out, nobody=False):
    """one response off the front of out, framed as a client would by its
    FIRST Content-Length or its chunking: (head, fields, body, rest)"""
    h, _, rest = out.partition(b"\r\n\r\n")
    f = fields(h)
    if nobody:
        return h, f, b"", rest
    if f.get(b"transfer-encoding") == [b"chunked"]:
        body, rest = dechunk(rest)
        return h, f, body, rest
    if f.get(b"content-length"):
        n = int(f[b"content-length"][0])
        return h, f, rest[:n], rest[n:]
    return h, f, rest, b""
CLOSE = b"Connection: close\r\n"
NEXT = get(b"/ok", extra=CLOSE)
def next_ok(tag, rest):
    """rest must be exactly the /ok response, from its first byte"""
    h, f, b, _ = frame(rest) if rest.startswith(b"HTTP/1.1 ") else (b"", {}, None, b"")
    report(h.startswith(b"HTTP/1.1 200") and f.get(b"x-next") == [b"1"] and b == b"next",
           "%s: the pipelined response behind it starts at its first byte (%r)" % (tag, rest[:40]))

# 1. a handler Content-Length that is wrong: one length, the body's
h, f, b, rest = frame(raw(get(b"/cl") + NEXT))
report(f.get(b"content-length") == [b"11"], "cl: one Content-Length, the body's 11 (sent: %r)" % f.get(b"content-length"))
report(b"transfer-encoding" not in f, "cl: no Transfer-Encoding beside it")
report(b == b"hello world", "cl: the 11-byte body whole (%r)" % b)
next_ok("cl", rest)
# 2. far too large
h, f, b, rest = frame(raw(get(b"/clhuge", extra=CLOSE)))
report(f.get(b"content-length") == [b"1"] and b == b"x" and rest == b"",
       "clhuge: one Content-Length, 1, body 'x', nothing after (sent: %r %r %r)" % (f.get(b"content-length"), b[:10], rest[:10]))
# 3. handler Transfer-Encoding: one framing, and the body arrives whole
h, f, b, rest = frame(raw(get(b"/te") + NEXT))
te, cl = f.get(b"transfer-encoding"), f.get(b"content-length")
report((te == [b"chunked"] and cl is None) or (te is None and cl == [b"11"]),
       "te: one framing -- chunked alone or the body's length alone (sent: TE %r CL %r)" % (te, cl))
report(b"gzip" not in h.lower(), "te: the handler's Transfer-Encoding is not sent")
report(b == b"chunky body", "te: the body arrives whole (%r)" % b)
next_ok("te", rest)
# 4. handler Transfer-Encoding AND Content-Length
h, f, b, rest = frame(raw(get(b"/tecl") + NEXT))
report(f.get(b"content-length") == [b"10"] and b"transfer-encoding" not in f,
       "tecl: one Content-Length, the body's 10, no Transfer-Encoding (sent: %r %r)" % (f.get(b"content-length"), f.get(b"transfer-encoding")))
report(b == b"fixed body", "tecl: the body whole")
next_ok("tecl", rest)
# 5. handler Connection
h, f, b, rest = frame(raw(get(b"/conn", extra=CLOSE)))
report(f.get(b"connection") == [b"close"], "conn: one Connection, the server's close (sent: %r)" % f.get(b"connection"))
report(f.get(b"x-good") == [b"1"] and b == b"conn", "conn: the rest of the response as given")
# 6. 204 and 304 with bodies
for path, st in ((b"/nc", b"204"), (b"/nm", b"304")):
    tag = path.decode()[1:]
    h, f, b, rest = frame(raw(get(path) + NEXT), nobody=True)
    report(h.startswith(b"HTTP/1.1 " + st), "%s: status %s [%r]" % (tag, st.decode(), h[:20]))
    report(b"content-length" not in f and b"transfer-encoding" not in f,
           "%s: no Content-Length and no Transfer-Encoding (sent: %r %r)" % (tag, f.get(b"content-length"), f.get(b"transfer-encoding")))
    report(not rest.startswith(b"oops") and not rest.startswith(b"stale"), "%s: the handler's body is not sent (%r)" % (tag, rest[:12]))
    next_ok(tag, rest)
# 7. HEAD
h, f, b, rest = frame(raw(get(b"/cl", b"HEAD") + NEXT), nobody=True)
report(f.get(b"content-length") == [b"11"], "head cl: one Content-Length, 11 (sent: %r)" % f.get(b"content-length"))
next_ok("head cl", rest)
h, f, b, rest = frame(raw(get(b"/nc", b"HEAD") + NEXT), nobody=True)
report(h.startswith(b"HTTP/1.1 204") and b"content-length" not in f, "head nc: 204 with no Content-Length (sent: %r)" % f.get(b"content-length"))
next_ok("head nc", rest)
# 8. gzip
h, f, b, rest = frame(raw(get(b"/gz", extra=b"Accept-Encoding: gzip\r\n") + NEXT))
cl = f.get(b"content-length") or []
report(f.get(b"content-encoding") == [b"gzip"] and len(cl) == 1, "gz: gzip with one Content-Length (sent: %r)" % cl)
try:
    ok = zlib.decompress(b, 16 + zlib.MAX_WBITS) == b"hello world " * 200
except Exception:
    ok = False
report(ok, "gz: that many bytes inflate to the handler's body")
next_ok("gz", rest)
# 9. CORS preflight
h, f, b, rest = frame(raw(b"OPTIONS /cl HTTP/1.1\r\nHost: t\r\nOrigin: http://o.example\r\n\r\n" + NEXT), nobody=True)
report(h.startswith(b"HTTP/1.1 204") and b"content-length" not in f,
       "preflight: 204 with no Content-Length [%r %r]" % (h[:20], f.get(b"content-length")))
next_ok("preflight", rest)
PY
if ! build srv 'http.serveon("127.0.0.1"; 0)'; then
    fail "the server does not build: $(grep -m1 '^error' srv.log)"
else
    start srv
    if [ -z "${PORT}" ]; then
        fail "the server did not start: $(head -3 srv.err)"
    else
        python3 cli.py "${PORT}" > cli.out 2>&1
        collect ""
    fi
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
    if [ -z "${PORT}" ]; then
        fail "tls: the server did not start: $(head -3 srvtls.err)"
    else
        python3 cli.py "${PORT}" tls > cli.out 2>&1
        collect "tls: "
    fi
    stop
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
