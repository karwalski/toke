#!/usr/bin/env bash
# C739_http_head_matches_get.sh -- a HEAD response carries the framing
# fields the GET for the same resource would: the same Content-Length (the
# gzip length when the GET is gzipped), Transfer-Encoding: chunked when the
# GET is chunked, and the same Content-Encoding and Vary (story 121.100).
#
# THE DEFECT, measured on main (b442a5d7) with this script.  RFC 9110
# sec. 9.3.2: "the server SHOULD send the same header fields in response to
# a HEAD request as it would have sent if the request method had been GET",
# and sec. 8.6: a Content-Length in a HEAD response must be the length the
# GET would send.  std.http's HEAD writers (plain, TLS, HTTP/2) made their
# own decision instead of the GET's:
#   - a body the GET sends gzipped (a 2xx text body of 256 bytes or more,
#     to a client whose Accept-Encoding admits gzip) was described by HEAD
#     with the UNCOMPRESSED length and no Content-Encoding or Vary -- on
#     HTTP/1.1, TLS and HTTP/2 alike;
#   - a body the GET sends chunked (a handler that set no Content-Length,
#     on plain HTTP/1.1) was described by HEAD with a fixed Content-Length.
# A cache or a client that sizes a download with HEAD (a resumable
# downloader, a link checker) was told a length and a coding the GET did not
# use.
#
# THE FIX.  One function makes the framing decision -- no body, gzip and its
# length, chunked, or the body's length -- for GET and HEAD alike
# (res_frame_plan); HEAD writes the same head and leaves the body out.
# HTTP/2's dispatch gzips for HEAD as for GET and sends the gzip length.
#
# WHAT IS ASSERTED, from the raw bytes over a real socket (plain and TLS),
# for each case GET then HEAD on one keep-alive connection, comparing
# Content-Length, Transfer-Encoding, Content-Encoding and Vary:
#   1. /big (2400-byte text, handler Content-Length) with Accept-Encoding:
#      gzip -- the GET is gzipped; HEAD's fields are the GET's;
#   2. /big without Accept-Encoding -- both the plain length;
#   3. /bigq with "gzip;q=0" -- no gzip on either;
#   4. /chunk (no Content-Length, short body) -- chunked on plain (both),
#      fixed on TLS (both);
#   5. /chunkbig (no Content-Length, 2400 bytes, gzip) -- gzipped, fixed
#      length, on both;
#   6. /png (an image/png body, gzip offered) -- not gzipped on either;
#   7. /nc (204) -- neither carries a length;
#   and every HEAD has no body: the pipelined GET behind it is read from the
#   byte after HEAD's head.  Then over HTTP/2 (curl, when one here speaks
#   h2): HEAD /big with accept-encoding gzip carries the GET's
#   content-length and content-encoding.
#
# Negative control: C739_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against a git archive of main's src/stdlib
# (b442a5d7): 52 passed, 6 failed -- HEAD's fields differ from the GET's
# for the gzipped /big and /chunkbig over plain and TLS, for the chunked
# /chunk over plain, and over HTTP/2 (content-length 2400, no
# content-encoding, where the GET sent 52 bytes of gzip).  On this branch:
# 58 passed (56 without an h2-capable curl).
#
# Story: 121.100, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C739_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c739_XXXXXX")"
cd "${WORK}" || exit 1
echo "C739: a HEAD response carries the GET's framing fields (121.100)"
echo "----------------------------------------------------------------------"

cat > handlers.tk.part <<'TK'
f=big(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Type";"text/plain");@("Content-Length";"1"));body:str.repeat("hello world ";200)}
};
f=chunk(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Type";"text/plain"));body:"short chunked body"}
};
f=chunkbig(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Type";"text/css"));body:str.repeat("a{color:red} ";200)}
};
f=png(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Type";"image/png");@("Content-Length";"1"));body:str.repeat("not really a png ";100)}
};
f=nc(req:$req):$res{
  <$res{status:204;headers:@(@("Content-Length";"4"));body:"oops"}
};
TK

build() {
    { printf 'm=%s;\ni=http:std.http;\ni=str:std.str;\n' "$1"; cat handlers.tk.part
      printf 'f=main():i64{\n  http.gethandler("/big"; &big);\n  http.gethandler("/bigq"; &big);\n  http.gethandler("/chunk"; &chunk);\n  http.gethandler("/chunkbig"; &chunkbig);\n  http.gethandler("/png"; &png);\n  http.gethandler("/nc"; &nc);\n  %s;\n  <0\n};\n' "$2"; } > "$1.tk"
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
def req(method, path, ae, close=False):
    h = b"%s %s HTTP/1.1\r\nHost: t\r\n" % (method, path)
    if ae is not None:
        h += b"Accept-Encoding: " + ae + b"\r\n"
    if close:
        h += b"Connection: close\r\n"
    return h + b"\r\n"
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
def frame(out, nobody=False):
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
KEYS = (b"content-length", b"transfer-encoding", b"content-encoding", b"vary")
def pick(f):
    return {k: f.get(k) for k in KEYS}
# (tag, path, accept-encoding, what the GET must look like)
CASES = [
    ("big gzip",      b"/big",      b"gzip",     "gzip"),
    ("big identity",  b"/big",      None,        "len"),
    ("big gzip;q=0",  b"/bigq",     b"gzip;q=0", "len"),
    ("chunk",         b"/chunk",    None,        "len" if tls else "chunked"),
    ("chunkbig gzip", b"/chunkbig", b"gzip",     "gzip"),
    ("png gzip",      b"/png",      b"gzip",     "len"),
    ("204",           b"/nc",       b"gzip",     "none"),
]
for tag, path, ae, want in CASES:
    # GET, then HEAD, then a GET behind it: HEAD must end at its blank line
    out = raw(req(b"GET", path, ae) + req(b"HEAD", path, ae) + req(b"GET", path, ae, close=True))
    gh, gf, gb, rest = frame(out, nobody=(want == "none"))
    hh, hf, _, rest = frame(rest, nobody=True)
    th, tf, tb, rest = frame(rest, nobody=(want == "none"))
    g, h = pick(gf), pick(hf)
    shape = {"gzip": gf.get(b"content-encoding") == [b"gzip"] and gf.get(b"content-length") is not None,
             "len": gf.get(b"content-encoding") is None and gf.get(b"transfer-encoding") is None
                    and gf.get(b"content-length") is not None,
             "chunked": gf.get(b"transfer-encoding") == [b"chunked"] and gf.get(b"content-length") is None,
             "none": gf.get(b"content-length") is None and gf.get(b"transfer-encoding") is None}[want]
    report(gh.startswith(b"HTTP/1.1 ") and shape, "%s: the GET is %s (%r)" % (tag, want, g))
    report(hh.startswith(gh.split(b"\r\n")[0]), "%s: HEAD has the GET's status (%r)" % (tag, hh[:20]))
    report(g == h, "%s: HEAD's framing fields are the GET's (GET %r, HEAD %r)" % (tag, g, h))
    report(th.startswith(gh.split(b"\r\n")[0]) and pick(tf) == g and tb == gb,
           "%s: HEAD sent no body -- the GET behind it starts at its first byte (%r)" % (tag, th[:20]))
PY

if ! build srv 'http.serveon("127.0.0.1"; 0)'; then
    fail "the server does not build: $(grep -m1 '^error' srv.log)"
else
    start srv
    if [ -z "${PORT}" ]; then fail "the server did not start: $(head -3 srv.err)"
    else python3 cli.py "${PORT}" > cli.out 2>&1; collect ""; fi
    stop
fi

H2CURL=""
for c in curl /opt/homebrew/opt/curl/bin/curl /usr/local/opt/curl/bin/curl; do
    command -v "${c}" >/dev/null 2>&1 || continue
    v="$("${c}" -V 2>/dev/null)"
    case "${v}" in *SecureTransport*) continue ;; esac
    case "${v}" in *OpenSSL*|*LibreSSL*|*BoringSSL*|*quictls*) ;; *) continue ;; esac
    if printf '%s\n' "${v}" | grep -q '^Features:.*HTTP2'; then H2CURL="${c}"; break; fi
done
hdr() { tr -d '\r' < "$1" | grep -i "^$2:" | head -1 | sed 's/^[^:]*: *//'; }

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
    else
        python3 cli.py "${PORT}" tls > cli.out 2>&1; collect "tls: "
        if [ -n "${H2CURL}" ]; then
            U="https://127.0.0.1:${PORT}/big"
            "${H2CURL}" -sk --http2 -H 'Accept-Encoding: gzip' -D g.h -o g.b "${U}" > /dev/null 2>&1
            "${H2CURL}" -sk --http2 -H 'Accept-Encoding: gzip' -I -o h.h "${U}" > /dev/null 2>&1
            gv="$(head -1 g.h | tr -d '\r')"; hv="$(head -1 h.h | tr -d '\r')"
            gl="$(hdr g.h content-length)"; hl="$(hdr h.h content-length)"
            ge="$(hdr g.h content-encoding)"; he="$(hdr h.h content-encoding)"
            if [ "${gv%% *}" = "HTTP/2" ] && [ "${hv%% *}" = "HTTP/2" ] && [ "${ge}" = "gzip" ] &&
               [ -n "${gl}" ] && [ "${gl}" = "$(wc -c < g.b | tr -d ' ')" ]; then
                pass "h2: GET /big is gzipped, content-length ${gl}"
            else fail "h2: GET /big is not gzipped over HTTP/2 (${gv}; ${ge}; ${gl})"; fi
            if [ "${hl}" = "${gl}" ] && [ "${he}" = "${ge}" ]; then
                pass "h2: HEAD /big carries the GET's content-length and content-encoding (${hl} ${he})"
            else fail "h2: HEAD /big says content-length '${hl}' content-encoding '${he}', the GET '${gl}' '${ge}'"; fi
        else
            echo "  SKIP: HTTP/2 cases -- no curl here can negotiate h2"
        fi
    fi
    stop
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
