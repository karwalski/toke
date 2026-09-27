#!/usr/bin/env bash
# C279_https_client_verifies.sh — std.http's client verifies the server's
# certificate and name over https (found measuring story 138.8).
#
# THE DEFECT.  client_do_request() in src/stdlib/http.c created its TLS
# context with OpenSSL's default, SSL_VERIFY_NONE, and never set a name to
# check: every https:// request from http.clientget/clientpost/... (and
# http.get(url), http.posturl) accepted ANY certificate for ANY host, so the
# request -- API keys in its headers included -- was readable by whoever
# answered the connection.  Measured 2026-09-27 on main c15dd11: a request
# to a local server presenting a self-signed certificate nothing trusts
# succeeded.
#
# THE FIX.  SSL_VERIFY_PEER against the default trust store (SSL_CERT_FILE /
# SSL_CERT_DIR override it, which is how this suite trusts its own CA), and
# the certificate must name the host (SSL_set1_host) or the IP literal
# (X509_VERIFY_PARAM_set1_ip_asc).
#
# WHAT IS ASSERTED, against a local TLS server on a port it bound itself:
#   1. a certificate nothing trusts: $err, and nothing reaches the handler
#   2. the same certificate trusted through SSL_CERT_FILE: $ok, status 200
#   3. a trusted certificate for another name (other.example): $err
#
# Negative control: C279_STDLIB=<dir> builds against another stdlib tree.
# Main's src/stdlib has no http.clientget; with SSL_CTX_set_verify(...)
# changed to SSL_VERIFY_NONE (measured), cases 1 and 3 take $ok.
#
# Story: 138.8 (the defect is filed separately; see the commit)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C279_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "CONFORM-SKIP: no openssl(1) on this host to make test certificates"; exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c279_XXXXXX")"
SRV=""
cleanup() { [ -n "${SRV}" ] && kill -9 "${SRV}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C279: the https client verifies the server (138.8)"
echo "----------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

# good.pem names 127.0.0.1 and localhost; other.pem names other.example only.
openssl req -x509 -newkey rsa:2048 -keyout good.key -out good.pem -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" > cert.log 2>&1
openssl req -x509 -newkey rsa:2048 -keyout other.key -out other.pem -days 2 -nodes \
    -subj "/CN=other.example" -addext "subjectAltName=DNS:other.example" >> cert.log 2>&1
if [ ! -s good.pem ] || [ ! -s other.pem ]; then
    echo "  FAIL: openssl could not make the test certificates"; sed 's/^/      /' cert.log | head -4
    echo "Results: 0 passed, 1 failed"; exit 1
fi

cat > tsrv.py <<'PY'
import http.server, ssl, sys
LOG = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        with open(LOG, "a") as f: f.write("GET %s\n" % self.path)
        b = b"ok"
        self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2], sys.argv[3])
s.socket = ctx.wrap_socket(s.socket, server_side=True)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
start() {   # start CERTNAME -> PORT
    [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }
    rm -f srv.port
    python3 tsrv.py srv.log "$1.pem" "$1.key" > srv.port 2> srv.err &
    SRV=$!
    for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
    PORT="$(head -1 srv.port)"
}

cat > cl.tk <<'TOKE'
m=cl;
i=http:std.http;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let c=http.client(args.get(1));
  io.println(mt http.clientget(c;"/v") {$ok:r "ok \(r.status)"; $err:e "err"});
  <0
};
TOKE
if ! "${TKC}" --allow-all --out cl_bin cl.tk > build.log 2>&1; then
    echo "  FAIL: the client does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

start good
: > srv.log
expect "1 untrusted self-signed certificate: \$err, nothing handled" \
    "$(env -u SSL_CERT_FILE -u SSL_CERT_DIR ./cl_bin "https://127.0.0.1:${PORT}") $(grep -a -c '^GET /v' srv.log)" "err 0"
expect "2 the same certificate, trusted via SSL_CERT_FILE: \$ok 200" \
    "$(SSL_CERT_FILE="${WORK}/good.pem" ./cl_bin "https://127.0.0.1:${PORT}")" "ok 200"
start other
expect "3 a trusted certificate for another name: \$err" \
    "$(SSL_CERT_FILE="${WORK}/other.pem" ./cl_bin "https://127.0.0.1:${PORT}")" "err"

echo "----------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
