#!/usr/bin/env bash
# C744_http10_no_chunked.sh -- a response to an HTTP/1.0 request is never
# sent with Transfer-Encoding: chunked; it carries a Content-Length, on
# plain and TLS (story 121.103).
#
# THE DEFECT, measured on main (0936dc02 + fix/z67-http10) with this script.
# res_frame_plan chunked every body the handler gave no Content-Length,
# whatever the request's version, and send_response / send_error (the
# server's own answers: 405, the framing errors) did the same.  RFC 9112
# sec. 6.1: a server MUST NOT send a response containing Transfer-Encoding
# unless the request indicates HTTP/1.1 (or later).  An HTTP/1.0 client
# does not know the chunked coding, so it reads the chunk-size lines and
# the terminating "0" as part of the body.
#
# THE FIX.  The body is already complete in the response struct (Res.body),
# so nothing has to be close-delimited: the plain loop passes may_chunk 0 to
# res_frame_plan / send_response for an HTTP/1.0 request (req_is_http10, the
# version on the request line), and the response goes with its length.
# send_error answers requests whose framing failed, whose version may not
# be known, and closes after them -- it now always sends a length, which is
# valid on every version.  The TLS writer always sent a length.  An
# HTTP/1.1 response the handler gave no Content-Length is still chunked on
# plain (Story 27.1.4, unchanged).
#
# WHAT IS ASSERTED, from the raw bytes over a real socket, plain and TLS:
#   1. a 1.0 GET of a body with no Content-Length: no Transfer-Encoding, one
#      Content-Length equal to the body, the body whole, then EOF;
#   2. a 1.0 GET with Connection: keep-alive and a 1.0 GET pipelined behind
#      it: both responses carry a length and no Transfer-Encoding, and frame
#      back to back;
#   3. a 1.0 HEAD: no Transfer-Encoding, the GET's Content-Length, no body;
#   4. a 1.0 request with an unknown method (the server's own 405): no
#      Transfer-Encoding, a length matching its body;
#   5. a 1.0 request whose framing is bad (Content-Length "x", the server's
#      own 400 via send_error): no Transfer-Encoding, a length matching it;
#   6. the control: a 1.1 GET of the same body is chunked on plain (27.1.4)
#      and sent with its length on TLS -- either way, one framing.
#
# Negative control: C744_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against a git archive of the base's
# src/stdlib and stdlib (main 0936dc02 + fix/z67-http10): 37 passed, 17
# failed -- every plain 1.0 response (GET, keep-alive pair, HEAD, 405, 400)
# is chunked with no Content-Length; TLS and the 1.1 control pass.  On this
# branch: 54 passed.  Under an ASan/UBSan TKC_CC: 54 passed, no report.
#
# Story: 121.103, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C744_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c744_XXXXXX")"
cd "${WORK}" || exit 1
echo "C744: no Transfer-Encoding: chunked to an HTTP/1.0 client (121.103)"
echo "----------------------------------------------------------------------"

cat > handlers.tk.part <<'TK'
f=ok(req:$req):$res{
  <$res{status:200;headers:@(@("X-Next";"1"));body:"a body for one-dot-oh"}
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
BODY = b"a body for one-dot-oh"
def raw(req):
    """send req, read to EOF (or a 3 s silence); returns (bytes, how)"""
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    if tls:
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        s = ctx.wrap_socket(s, server_hostname="localhost")
    out, how = b"", "eof"
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
def by_length(out, head_only=False):
    """frame one response by Content-Length alone, the HTTP/1.0 way"""
    h, _, rest = out.partition(b"\r\n\r\n")
    f = fields(h)
    cl = f.get(b"content-length")
    if head_only or cl is None or len(cl) != 1:
        return h, f, None if not head_only else b"", rest
    n = int(cl[0])
    return h, f, rest[:n], rest[n:]
H10 = b"HTTP/1.0"

def one_oh(tag, out, status, body, head_only=False, more=False):
    h, f, b, rest = by_length(out, head_only)
    report(h.startswith(b"HTTP/1.1 " + status), "%s: status %s (%r)" % (tag, status.decode(), h[:20]))
    report(f.get(b"transfer-encoding") is None,
           "%s: no Transfer-Encoding (sent: %r)" % (tag, f.get(b"transfer-encoding")))
    report(f.get(b"content-length") == [b"%d" % len(body)],
           "%s: one Content-Length of %d (sent: %r)" % (tag, len(body), f.get(b"content-length")))
    if not head_only:
        report(b == body, "%s: the body whole by its length (%r)" % (tag, (b or b"")[:40]))
    if not more:
        report(rest == b"", "%s: nothing after it (%r)" % (tag, rest[:40]))
    return rest

# 1. a 1.0 GET of a body with no Content-Length
out, how = raw(b"GET /ok HTTP/1.0\r\nHost: t\r\n\r\n")
one_oh("1.0 GET", out, b"200", BODY)
report(how == "eof", "1.0 GET: EOF after it (%s)" % how)

# 2. 1.0 keep-alive, then a pipelined 1.0 GET
out, how = raw(b"GET /ok HTTP/1.0\r\nHost: t\r\nConnection: keep-alive\r\n\r\n"
               b"GET /ok HTTP/1.0\r\nHost: t\r\n\r\n")
rest = one_oh("1.0 keep-alive first", out, b"200", BODY, more=True)
one_oh("1.0 keep-alive second", rest, b"200", BODY)

# 3. a 1.0 HEAD carries the GET's length and no body
out, how = raw(b"HEAD /ok HTTP/1.0\r\nHost: t\r\n\r\n")
h, _, rest = out.partition(b"\r\n\r\n")
one_oh("1.0 HEAD", out, b"200", BODY, head_only=True)
report(rest == b"", "1.0 HEAD: no body (%r)" % rest[:40])

# 4. the server's own 405
out, how = raw(b"BREW /ok HTTP/1.0\r\nHost: t\r\n\r\n")
h, f, b, rest = by_length(out)
report(h.startswith(b"HTTP/1.1 405"), "1.0 405: status 405 (%r)" % h[:20])
report(f.get(b"transfer-encoding") is None,
       "1.0 405: no Transfer-Encoding (sent: %r)" % f.get(b"transfer-encoding"))
report(b is not None and len(b) > 0 and rest == b"",
       "1.0 405: one Content-Length that frames the body (CL %r, rest %r)" % (f.get(b"content-length"), rest[:40]))

# 5. the server's own 400 for a request it could not frame
out, how = raw(b"POST /ok HTTP/1.0\r\nHost: t\r\nContent-Length: x\r\n\r\n")
h, f, b, rest = by_length(out)
report(h.startswith(b"HTTP/1.1 400"), "1.0 400: status 400 (%r)" % h[:20])
report(f.get(b"transfer-encoding") is None,
       "1.0 400: no Transfer-Encoding (sent: %r)" % f.get(b"transfer-encoding"))
report(b is not None and len(b) > 0 and rest == b"",
       "1.0 400: one Content-Length that frames the body (CL %r, rest %r)" % (f.get(b"content-length"), rest[:40]))

# 6. control: HTTP/1.1 -- chunked on plain, a length on TLS; one framing
out, how = raw(b"GET /ok HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
h, _, rest = out.partition(b"\r\n\r\n")
f = fields(h)
if tls:
    report(f.get(b"transfer-encoding") is None and f.get(b"content-length") == [b"%d" % len(BODY)]
           and rest == BODY, "1.1 control: a length on TLS (TE %r CL %r)"
           % (f.get(b"transfer-encoding"), f.get(b"content-length")))
else:
    body, tail = dechunk(rest)
    report(f.get(b"transfer-encoding") == [b"chunked"] and f.get(b"content-length") is None
           and body == BODY and tail == b"", "1.1 control: still chunked on plain (TE %r CL %r)"
           % (f.get(b"transfer-encoding"), f.get(b"content-length")))
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
