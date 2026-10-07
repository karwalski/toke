#!/usr/bin/env bash
# C726_http_res_header_injection.sh -- std.http writes a handler's response
# headers whole and never lets one split the response (story 121.92).
#
# THE DEFECT, measured on main (7ca0e318) with this script.  http.c's
# write_res_headers (and the three copies of its loop on the TLS path)
# wrote every @(name; value) a handler's $res carried as
# snprintf("%s: %s\r\n") into a 512-byte stack buffer:
#   - no check on either string.  A $res is a plain toke record, so a
#     handler that puts request data in a header -- the obvious redirect,
#     `@("Location"; req.body)` -- let the client write CR LF into the
#     response: "Location: x\r\nSet-Cookie: pwn=1" arrived as two headers.
#     A name with CR LF, a space or a colon, or a value with another control
#     byte, went out the same way.
#   - a line longer than 511 bytes was cut at 511 with no CR LF, so the
#     header after it was glued onto it ("X-Long: zzz...zX-After: end"):
#     a long Content-Security-Policy or cookie value lost its tail AND took
#     the next header with it.
#
# THE FIX.  One rule, router.c's since 121.90 (tk_http_field.h, shared): a
# header whose name is not an RFC 9110 token (sec. 5.1) or whose value holds
# a control byte other than HTAB (sec. 5.5) is DROPPED whole -- never
# repaired, since stripping bytes would still send a value nobody wrote.
# No structured error: a handler's $res is not a fallible call, and the
# writer runs after the handler has returned.  Every line is written at its
# own length.  The HTTP/1.1 writer (fixed-length, chunked, HEAD and gzip
# paths) and the TLS writer go through the same function; HTTP/2 already
# dropped such fields (h2_field_name / h2_field_value_ok).
#
# WHAT IS ASSERTED, over a real socket against a toke server (http.serveon):
#   1. POST /loc with a body holding CR LF + Set-Cookie, put in Location by
#      the handler: no Set-Cookie line, Location dropped, 302 still sent,
#      the well-formed headers on both sides of it sent;
#   2. a 3000-byte body put in Location arrives whole, and the header after
#      it is its own line (the truncation);
#   3. GET /lit, a $res literal with CR LF in a name, CR LF / LF / 0x01 /
#      0x7f in values, a space or a colon in a name, an empty name: none is
#      sent, nothing is forged; HTAB in a value is kept; a 3000-byte value
#      is sent whole and X-After follows it on its own line; an
#      X-Frame-Options the writer drops does not suppress the default DENY;
#   4. HEAD /lit: the same (the HEAD writer);
#   5. GET /big with Accept-Encoding: gzip: the gzip writer drops the
#      CR LF header too, and the body inflates;
#   6. (story 121.94) gzip;q=0, *;q=0 and gzip;q=0,* get the plain body;
#      * and GZIP;Q=0.5 get gzip (the server read strstr(ae, "gzip"));
#   all of it again over TLS (http.servetlson), whose writer had three copies
#   of the old loop of its own.
#
# Negative control: C726_STDLIB=<dir> builds the server against another
# stdlib tree (TKC_STDLIB_DIR).  Against main's src/stdlib (7ca0e318):
# 20 passed, 42 failed -- 21 of 31 on each of plain and TLS.  What passes
# there is what never involved a bad or long header: the status lines and
# bodies, the well-formed headers, HTAB kept, the gzip body.  On this branch:
# 62 passed.
#
# Story: 121.92
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C726_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c726_XXXXXX")"
cd "${WORK}" || exit 1
echo "C726: std.http sends a handler's headers whole and unsplit (121.92)"
echo "----------------------------------------------------------------------"

cat > handlers.tk.part <<'TK'
f=loc(req:$req):$res{
  <$res{status:302;headers:@(@("X-Good";"1");@("Location";req.body);@("X-Tail";"2"));body:"moved"}
};
f=lit(req:$req):$res{
  <$res{status:200;headers:@(
    @("X-Good";"1");
    @("X-Bad\r\nSet-Cookie: n=1";"v");
    @("X-Crlf";"a\r\nSet-Cookie: c=1");
    @("X-Lf";"a\nInjected: 1");
    @("X-Ctl";"a\x01b");
    @("X-Del";"a\x7fb");
    @("X Space";"v");
    @("X-Colon:";"v");
    @("";"empty-name");
    @("X-Tab";"a\tb");
    @("X-Long";str.repeat("z";3000));
    @("X-After";"end");
    @("X-Frame-Options";"SAMEORIGIN\r\nSet-Cookie: f=1"));body:"lit"}
};
f=big(req:$req):$res{
  <$res{status:200;headers:@(@("Content-Type";"text/plain");@("X-Good";"1");@("X-Crlf";"a\r\nSet-Cookie: g=1"));body:str.repeat("hello world ";200)}
};
TK

# build NAME MAIN-BODY: a server with the handlers above
build() {
    { printf 'm=%s;\ni=http:std.http;\ni=str:std.str;\n' "$1"; cat handlers.tk.part
      printf 'f=main():i64{\n  http.posthandler("/loc"; &loc);\n  http.gethandler("/lit"; &lit);\n  http.gethandler("/big"; &big);\n  %s;\n  <0\n};\n' "$2"; } > "$1.tk"
    "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1
}
# start NAME: run it, wait for its port; sets SRV and PORT
start() {
    ./"$1" >/dev/null 2>"$1.err" & SRV=$!
    PORT=""
    for _ in $(seq 1 100); do
        PORT="$(sed -n 's/^DIAGNOSTIC: kind=server_listening port=\([0-9][0-9]*\)$/\1/p' "$1.err" | head -1)"
        [ -n "${PORT}" ] && break
        kill -0 "${SRV}" 2>/dev/null || break
        sleep 0.1
    done
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

# The client reads raw bytes: curl would reject or normalise the very
# responses this test has to see.  It prints OK/BAD lines.
cat > cli.py <<'PY'
import socket, ssl, sys, zlib
port = int(sys.argv[1])
tls = len(sys.argv) > 2 and sys.argv[2] == "tls"
def rt(req):
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
    head, _, body = out.partition(b"\r\n\r\n")
    if b"\r\ntransfer-encoding: chunked" in head.lower():
        raw, body = body, b""
        while raw:
            n, _, raw = raw.partition(b"\r\n")
            n = int(n, 16)
            if n == 0:
                break
            body += raw[:n]
            raw = raw[n + 2:]
    return head, body
def lines(head):
    return head.split(b"\r\n")
def fields(head):
    """name (lower) -> list of values, for lines that are name: value"""
    d = {}
    for l in lines(head)[1:]:
        if b":" in l:
            k, _, v = l.partition(b":")
            d.setdefault(k.strip().lower(), []).append(v.strip())
    return d
def report(ok, msg):
    print(("OK " if ok else "BAD ") + msg)

# 1. client CR LF through req.body into Location
evil = b"https://x.example/\r\nSet-Cookie: pwn=1"
h, b = rt(b"POST /loc HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: %d\r\n\r\n" % len(evil) + evil)
f = fields(h)
report(h.startswith(b"HTTP/1.1 302"), "loc: status 302 [%r]" % lines(h)[0])
report(b"set-cookie" not in f and b"Set-Cookie" not in h, "loc: CR LF in a value from the request forges no Set-Cookie line")
report(b"location" not in f, "loc: the Location holding CR LF is dropped whole (%r)" % f.get(b"location"))
report(f.get(b"x-good") == [b"1"] and f.get(b"x-tail") == [b"2"], "loc: the well-formed headers on both sides are sent (%r %r)" % (f.get(b"x-good"), f.get(b"x-tail")))

# 2. a 3000-byte value through req.body
longv = b"https://x.example/" + b"q" * 2982
h, b = rt(b"POST /loc HTTP/1.1\r\nHost: t\r\nConnection: close\r\nContent-Length: %d\r\n\r\n" % len(longv) + longv)
f = fields(h)
report(f.get(b"location") == [longv], "loc: a 3000-byte Location arrives whole (%d bytes)" % (len(f.get(b"location", [b""])[0])))
report(f.get(b"x-tail") == [b"2"], "loc: the header after it is a line of its own (%r)" % f.get(b"x-tail"))

def lit_checks(tag, h):
    f = fields(h)
    report(f.get(b"x-good") == [b"1"], "%s: the well-formed header is sent" % tag)
    report(b"Set-Cookie" not in h and b"set-cookie" not in f, "%s: CR LF in a name or value forges no Set-Cookie line" % tag)
    report(b"Injected" not in h, "%s: a bare LF in a value forges no line" % tag)
    bad = [k for k in (b"x-bad", b"x-crlf", b"x-lf", b"x-ctl", b"x-del", b"x space", b"x-colon") if k in f or (b"\r\n" + k) in h.lower()]
    report(not bad, "%s: every header with a bad name or a control byte is dropped (sent: %r)" % (tag, bad))
    report(b"empty-name" not in h, "%s: a header with an empty name is dropped" % tag)
    report(f.get(b"x-tab") == [b"a\tb"], "%s: HTAB in a value is kept (%r)" % (tag, f.get(b"x-tab")))
    report(f.get(b"x-long") == [b"z" * 3000], "%s: a 3000-byte value is sent whole (%d)" % (tag, len(f.get(b"x-long", [b""])[0])))
    report(f.get(b"x-after") == [b"end"], "%s: the header after it is a line of its own (%r)" % (tag, f.get(b"x-after")))
    report(all((not l) or (b":" in l) for l in lines(h)[1:]), "%s: every header line is name: value" % tag)
    report(f.get(b"x-frame-options") == [b"DENY"], "%s: a dropped X-Frame-Options leaves the hardened default (%r)" % (tag, f.get(b"x-frame-options")))

# 3. GET /lit
h, b = rt(b"GET /lit HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
report(h.startswith(b"HTTP/1.1 200") and b == b"lit", "lit: 200 with its body [%r %r]" % (lines(h)[0], b[:20]))
lit_checks("lit", h)
# 4. HEAD /lit
h, b = rt(b"HEAD /lit HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
report(h.startswith(b"HTTP/1.1 200") and b == b"", "head: 200, no body [%r]" % lines(h)[0])
lit_checks("head", h)
# 5. gzip path
h, b = rt(b"GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\nAccept-Encoding: gzip\r\n\r\n")
f = fields(h)
report(f.get(b"content-encoding") == [b"gzip"], "gzip: Content-Encoding %r" % f.get(b"content-encoding"))
try:
    ok = zlib.decompress(b, 16 + zlib.MAX_WBITS) == b"hello world " * 200
except Exception:
    ok = False
report(ok, "gzip: the body inflates to what the handler sent")
report(b"Set-Cookie" not in h and b"x-crlf" not in f and f.get(b"x-good") == [b"1"], "gzip: the CR LF header is dropped, the good one sent")
# 6. (121.94) Accept-Encoding is read with its weights
for ae, want in ((b"gzip;q=0", False), (b"deflate, *;q=0", False), (b"gzip;q=0, *", False),
                 (b"*", True), (b"GZIP;Q=0.5", True)):
    h, b = rt(b"GET /big HTTP/1.1\r\nHost: t\r\nConnection: close\r\nAccept-Encoding: " + ae + b"\r\n\r\n")
    f = fields(h)
    gz = f.get(b"content-encoding") == [b"gzip"]
    plain = b"content-encoding" not in f and b == b"hello world " * 200
    report(gz if want else plain, "gzip: Accept-Encoding [%s] -> %s (want %s) -- 121.94" %
           (ae.decode(), "gzip" if gz else "plain" if plain else "neither", "gzip" if want else "plain"))
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

# The TLS writer (handle_tls_connection) had its own three copies of the
# loop; it goes through the same function now.
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
