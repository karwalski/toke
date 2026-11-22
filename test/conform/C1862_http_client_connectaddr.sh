#!/usr/bin/env bash
# C1862_http_client_connectaddr.sh — http.withconnectaddr connects to a
# caller-checked address while SNI, certificate verification and Host use
# the URL's host name (story 138.25, owner answer 2.2; loke T-63 controls 1-2,
# the curl --resolve equivalent).
#
# THE GAP.  The client resolved the URL's host inside connect
# (client_tcp_connect: getaddrinfo, first address that connects) and took no
# address, so a caller that resolved a name, checked every address against
# its SSRF rules and then fetched the URL let the name be resolved a SECOND
# time, to whatever it answered then (DNS rebinding).  loke shells out to
# curl --resolve meanwhile.
#
# NOW: http.withconnectaddr(c;addr) returns a client that connects to addr,
# an IPv4 or IPv6 literal, with AI_NUMERICHOST (no lookup of anything), and
# still sends Host, SNI and verifies the certificate against the URL's
# host.  A non-literal addr, or one combined with withproxy, is $err at
# request time; http.stream refuses a client carrying it.
#
# Loopback only: the servers bind 127.0.0.1 or ::1 (a port in 48120-48129);
# the URL names other.example / svc.invalid, which are never looked up.
#   1. http, URL svc.invalid, addr 127.0.0.1: $ok 200, Host svc.invalid:P
#   2. https, URL other.example, addr 127.0.0.1, the server's certificate
#      names other.example (trusted via SSL_CERT_FILE): $ok 200, SNI
#      other.example, Host other.example:P
#   3. https, the same URL and addr, the server's certificate names
#      127.0.0.1/localhost only: $err (certificate verification failed) --
#      the address connected to is not the name verified -- and nothing
#      reached the handler
#   4. addr "localhost": $err (not an address literal), nothing sent
#   5. addr "::1", the server listening on [::1] only: $ok 200, Host
#      svc.invalid:P -- the IPv6 literal is where the connection went (no
#      lookup of svc.invalid could have reached a socket bound to ::1 alone;
#      SKIP when this host has no IPv6 loopback)
#   6. addr with withproxy: $err
#   7. http.stream with a client carrying it: $err (not silently ignored)
#   8. http.downloadfile with a client carrying it: -1, nothing sent, no
#      file -- and, as a control, 0 and the body for the same URL on a
#      client without it
#   9. a CR/LF in the URL's host (review of Z214): $err "URL refused",
#      nothing sent.  With the address given, the host is never looked up,
#      so before the URL check it went into the Host line as it was, and a
#      second header ("X-Inj") reached the server; without the address the
#      lookup of that host failed.  Case 1 is the positive control.
#
# Negative control: C1862_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15: against v3.2.0's src/stdlib the client does not link
# (_tk_http_withconnectaddr_w undefined).  With the certificate verified
# against the connect address instead of the URL's host (and no SNI): 5
# passed, 2 failed -- 2 is $err "IP address mismatch" with no SNI sent, and
# 3 answers $ok 200 to a certificate that does not name other.example.
# (A control that ignores the address would look the URL's name up in DNS,
# which this test must not do, so it is not run.)  With client_parse_url's
# URL check removed: 9 fails -- $ok 200, and the server read Host
# svc.example followed by an X-Inj header line.
#
# Story: 138.25
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1862_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the loopback servers"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "CONFORM-SKIP: no openssl(1) on this host to make test certificates"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1862_XXXXXX")"
SRV=""
stop() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; SRV=""; }
cleanup() { stop; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1862: http.withconnectaddr connects to the address, verifies the name (138.25)"
echo "--------------------------------------------------------------------------------"

# other.pem names other.example only; local.pem names 127.0.0.1 and localhost.
openssl req -x509 -newkey rsa:2048 -keyout other.key -out other.pem -days 2 -nodes \
    -subj "/CN=other.example" -addext "subjectAltName=DNS:other.example" > cert.log 2>&1
openssl req -x509 -newkey rsa:2048 -keyout local.key -out local.pem -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" >> cert.log 2>&1
if [ ! -s other.pem ] || [ ! -s local.pem ]; then
    fail "openssl could not make the test certificates: $(head -c 200 cert.log)"
    echo "C1862: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# srv.py LOG [CERT KEY]: an HTTP(S) server on 127.0.0.1 that logs the SNI
# name and the Host header of each request.
cat > srv.py <<'PY'
import http.server, socketserver, ssl, sys
LOG = sys.argv[1]
def log(s):
    with open(LOG, "a") as f: f.write(s + "\n")
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        log("GET %s host=%s" % (self.path, self.headers.get("Host")))
        if self.headers.get("X-Inj") is not None: log("xinj=%s" % self.headers.get("X-Inj"))
        self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
import os, socket
V6 = os.environ.get("SRV6") == "1"   # bind [::1] only
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    address_family = socket.AF_INET6 if V6 else socket.AF_INET
    def server_bind(self):   # no reverse-DNS getfqdn() (127.439)
        if V6: self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        socketserver.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
srv = None
for p in range(48120, 48130):
    try:
        srv = S(("::1" if V6 else "127.0.0.1", p), H); break
    except OSError:
        pass
if srv is None: raise SystemExit("no free port in 48120..48129")
if len(sys.argv) > 3:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    ctx.sni_callback = lambda so, name, c: log("sni=%s" % name)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(p, flush=True)
srv.serve_forever()
PY
start() {   # start [CERTNAME] -> PORT
    stop; rm -f srv.port; : > srv.log
    if [ $# -gt 0 ]; then python3 srv.py srv.log "$1.pem" "$1.key" > srv.port 2> srv.err &
    else python3 srv.py srv.log > srv.port 2> srv.err & fi
    SRV=$!
    for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
    PORT="$(head -1 srv.port)"
    [ -n "${PORT}" ] || { fail "the server did not start: $(head -c 200 srv.err)"; echo "C1862: ${PASS} passed, ${FAIL} failed"; exit 1; }
}
start6() {   # a plain server on [::1] only -> PORT, or PORT empty
    stop; rm -f srv.port; : > srv.log
    SRV6=1 python3 srv.py srv.log > srv.port 2> srv.err &
    SRV=$!
    for _ in $(seq 1 50); do [ -s srv.port ] && break; kill -0 "${SRV}" 2>/dev/null || break; sleep 0.1; done
    PORT="$(head -1 srv.port 2>/dev/null)"
}

# cl URL ADDR MODE: MODE get = clientget; proxy = also withproxy; stream =
# http.stream; download = http.downloadfile to dl.out; plaindl = the same on
# http.client(URL), no address.
cat > cl.tk <<'TOKE'
m=cl;
i=http:std.http;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let mode=(mt args.get(3){$ok:v v;$err:e ""});
  let c=http.withconnectaddr(http.client((mt args.get(1){$ok:v v;$err:e ""}));(mt args.get(2){$ok:v v;$err:e ""}));
  let cp=if(mode=="proxy"){http.withproxy(c;"http://127.0.0.1:9")}el{c};
  let out=if(mode=="stream"){
    mt http.stream(c) {$ok:s "ok stream"; $err:e "err \(e.msg)"}
  }el{if(mode=="download"){
    "download \(http.downloadfile(c;"/v";"dl.out"))"
  }el{if(mode=="plaindl"){
    "download \(http.downloadfile(http.client((mt args.get(1){$ok:v v;$err:e ""}));"/v";"dl.out"))"
  }el{
    mt http.clientget(cp;"/v") {$ok:r "ok \(r.status)"; $err:e "err \(e.msg)"}
  }}};
  io.println(out);
  <0
};
TOKE
if ! "${TKC}" --allow-all --out cl_bin cl.tk > cl.build 2>&1; then
    fail "cl.tk does not build: $(head -c 400 cl.build)"
    echo ""; echo "C1862: ${PASS} passed, ${FAIL} failed"; exit 1
fi
handled() { grep -a -c '^GET /v' srv.log; }

start
R="$(./cl_bin "http://svc.invalid:${PORT}" 127.0.0.1 get)"
if [ "${R}" = "ok 200" ] && grep -q "^GET /v host=svc.invalid:${PORT}$" srv.log; then
    pass "1 http: connected to 127.0.0.1, Host svc.invalid:${PORT}"
else fail "1 http: '${R}', server log: $(tr '\n' '|' < srv.log)"; fi

start other
R="$(SSL_CERT_FILE="${WORK}/other.pem" ./cl_bin "https://other.example:${PORT}" 127.0.0.1 get)"
if [ "${R}" = "ok 200" ] && grep -q "^sni=other.example$" srv.log \
   && grep -q "^GET /v host=other.example:${PORT}$" srv.log; then
    pass "2 https: connected to 127.0.0.1; SNI other.example, certificate for other.example verified, Host other.example:${PORT}"
else fail "2 https: '${R}', server log: $(tr '\n' '|' < srv.log)"; fi

start local
R="$(SSL_CERT_FILE="${WORK}/local.pem" ./cl_bin "https://other.example:${PORT}" 127.0.0.1 get)"
case "${R}" in "err TLS certificate verification failed"*)
    if [ "$(handled)" = 0 ]; then pass "3 a certificate for 127.0.0.1 does not pass for the URL's name: '${R}'"
    else fail "3 the request reached the handler"; fi ;;
*) fail "3 local.pem for URL other.example: '${R}'" ;; esac

start
R="$(./cl_bin "http://svc.invalid:${PORT}" localhost get)"
if [ "${R}" = "err withconnectaddr: not an IPv4 or IPv6 address literal" ] && [ "$(handled)" = 0 ]; then
    pass "4 a name is not an address: '${R}', nothing sent"
else fail "4 addr localhost: '${R}', handled $(handled)"; fi

start6
if [ -n "${PORT}" ]; then
    R="$(./cl_bin "http://svc.invalid:${PORT}" ::1 get)"
    if [ "${R}" = "ok 200" ] && grep -q "^GET /v host=svc.invalid:${PORT}$" srv.log; then
        pass "5 an IPv6 literal is the address: the request reached the server on [::1]:${PORT} only"
    else fail "5 addr ::1: '${R}', server log: $(tr '\n' '|' < srv.log)"; fi
else
    echo "  SKIP: 5 this host has no IPv6 loopback to listen on ($(head -c 120 srv.err))"
fi
start

R="$(./cl_bin "http://svc.invalid:${PORT}" 127.0.0.1 proxy)"
if [ "${R}" = "err withconnectaddr cannot be combined with withproxy" ]; then pass "6 with withproxy: '${R}'"
else fail "6 with withproxy: '${R}'"; fi

R="$(./cl_bin "http://svc.invalid:${PORT}/v" 127.0.0.1 stream)"
if [ "${R}" = "err http.stream does not apply withdeadline, withmaxbody or withconnectaddr" ] && [ "$(handled)" = 0 ]; then
    pass "7 http.stream refuses it: '${R}'"
else fail "7 http.stream: '${R}', handled $(handled)"; fi

rm -f dl.out
R="$(./cl_bin "http://svc.invalid:${PORT}" 127.0.0.1 download)"
if [ "${R}" = "download -1" ] && [ "$(handled)" = 0 ] && [ ! -e dl.out ]; then
    pass "8 http.downloadfile refuses it: -1, nothing sent, no file"
else fail "8 http.downloadfile: '${R}', handled $(handled), dl.out $([ -e dl.out ] && echo exists || echo absent)"; fi
R="$(./cl_bin "http://127.0.0.1:${PORT}" 127.0.0.1 plaindl)"
if [ "${R}" = "download 0" ] && [ "$(cat dl.out 2>/dev/null)" = "ok" ]; then
    pass "8b control: the same download on a client without it is 0, body 'ok'"
else fail "8b control download: '${R}', dl.out '$(head -c 40 dl.out 2>/dev/null)'"; fi

: > srv.log
R="$(./cl_bin $'http://svc.example\r\nX-Inj:'"${PORT}" 127.0.0.1 get)"
if [ "${R}" = "err URL refused: the host must be a DNS name or an IPv4 address, and the port and path may not hold a space or a control character" ] \
   && [ ! -s srv.log ]; then
    pass "9 a CR/LF in the URL's host: refused, nothing sent"
else fail "9 CR/LF host: '${R}', server log: $(tr '\n' '|' < srv.log)"; fi

echo ""
echo "C1862: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
