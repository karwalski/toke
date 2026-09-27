#!/usr/bin/env bash
# C280_http_postheaders_typed.sh — http.postheaders is typed, sends one
# Content-Type, and is measured over https against a local TLS server
# (story 138.8, loke B8 / R6 / T-48).  Never a real provider.
#
# THE DEFECT.  http.postheaders was `(i64, i64, i64, i64) -> i64 [abi]`: the
# glue read (url; body; headersjson) and ignored a fourth argument, parsed
# the headers out of a JSON string by hand, sent its own
# `Content-Type: application/json` AND the caller's (two lines), returned the
# body or 0 -- no status, no error channel -- and opened its own TLS
# connection with no certificate verification.  https was never measured.
#
# THE FIX.  http.postheaders(url:str; body:str; headers:[[str]]) ->
# httpresp!httperr, through the client every other call uses: a
# Content-Type among the headers replaces the default application/json, a
# header name or value with CR/LF is refused (header injection), and https
# verifies the server (C279).
#
# WHAT IS ASSERTED, against local recorders on ports they bound themselves:
#   1. http: body and x-api-key arrive, ONE Content-Type (the caller's
#      application/json -- it used to arrive twice)
#   2. http: a caller Content-Type text/plain replaces the default
#   3. http: no Content-Type header -> the default application/json, once
#   4. http: a 401 is a response: $ok, status 401, its body
#   5. http: a header value with CR/LF -> $err, nothing arrives
#   6. closed port -> $err
#   7. https, the server's certificate trusted via SSL_CERT_FILE: arrives,
#      $ok 200 with the body, x-api-key delivered
#   8. https, certificate not trusted -> $err, nothing arrives
#
# Negative control: C280_STDLIB=<dir> builds against another stdlib tree.
# Main's src/stdlib does not build the program (postheaders there is four
# untyped i64s).  With client_do_request() sending its default Content-Type
# even when the caller gave one (measured), cases 1, 2 and 7 fail: two
# Content-Type lines arrive -- 5 passed, 3 failed.
#
# Story: 138.8
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C280_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "CONFORM-SKIP: no openssl(1) on this host to make a test certificate"; exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c280_XXXXXX")"
R1=""; R2=""
cleanup() { for p in ${R1} ${R2}; do kill -9 "$p" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C280: http.postheaders typed, one Content-Type, https measured (138.8)"
echo "-----------------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

openssl req -x509 -newkey rsa:2048 -keyout srv.key -out srv.pem -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" > cert.log 2>&1
[ -s srv.pem ] || { echo "  FAIL: openssl could not make a certificate"; echo "Results: 0 passed, 1 failed"; exit 1; }

cat > rec.py <<'PY'
import http.server, ssl, sys
LOG = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n).decode() if n else ""
        ct = self.headers.get_all("Content-Type") or []
        with open(LOG, "a") as f:
            f.write("POST %s [%s] ct=%s key=%s\n" % (self.path, body, "|".join(ct), self.headers.get("x-api-key")))
        code = 401 if self.path == "/deny" else 200
        b = ('{"path":"%s"}' % self.path).encode()
        self.send_response(code); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
if len(sys.argv) > 2:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[2], sys.argv[3])
    s.socket = ctx.wrap_socket(s.socket, server_side=True)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 rec.py plain.log > plain.port 2>/dev/null & R1=$!
python3 rec.py tls.log srv.pem srv.key > tls.port 2>/dev/null & R2=$!
for _ in $(seq 1 50); do [ -s plain.port ] && [ -s tls.port ] && break; sleep 0.1; done
HP="$(head -1 plain.port)"; SP="$(head -1 tls.port)"
if [ -z "${HP}" ] || [ -z "${SP}" ]; then echo "  FAIL: the recorders did not start"; echo "Results: 0 passed, 1 failed"; exit 1; fi

cat > ph.tk <<TOKE
m=ph;
i=http:std.http;
i=io:std.io;
i=str:std.str;
i=args:std.args;
f=main():i64{
  let base=args.get(1);
  let h=@(@("x-api-key";"k1");@("content-type";"application/json"));
  io.println(mt http.postheaders(str.concat(base;"/one");"{\"n\":1}";h) {\$ok:r "1 ok \(r.status) \(str.frombytes(r.body))"; \$err:e "1 err"});
  io.println(mt http.postheaders(str.concat(base;"/two");"n=2";@(@("Content-Type";"text/plain"))) {\$ok:r "2 ok \(r.status) \(str.frombytes(r.body))"; \$err:e "2 err"});
  io.println(mt http.postheaders(str.concat(base;"/three");"{}";@(@("x-api-key";"k3"))) {\$ok:r "3 ok \(r.status) \(str.frombytes(r.body))"; \$err:e "3 err"});
  io.println(mt http.postheaders(str.concat(base;"/deny");"{}";h) {\$ok:r "4 ok \(r.status) \(str.frombytes(r.body))"; \$err:e "4 err"});
  io.println(mt http.postheaders(str.concat(base;"/inj");"{}";@(@("x-api-key";"a\r\nX-Evil: 1"))) {\$ok:r "5 ok \(r.status) \(str.frombytes(r.body))"; \$err:e "5 err"});
  io.println(mt http.postheaders("http://127.0.0.1:1/x";"{}";h) {\$ok:r "6 ok \(r.status) \(str.frombytes(r.body))"; \$err:e "6 err"});
  <0
};
TOKE
if ! "${TKC}" --allow-all --out ph_bin ph.tk > build.log 2>&1; then
    echo "  FAIL: the program does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi
./ph_bin "http://127.0.0.1:${HP}" > plain.out 2>&1
line() { awk -v k="$2" '$1==k{print; exit}' "$1"; }

expect "1 body and x-api-key arrive, ONE Content-Type" \
    "$(grep -a -c '^POST /one \[{"n":1}\] ct=application/json key=k1$' plain.log) $(line plain.out 1)" \
    '1 1 ok 200 {"path":"/one"}'
expect "2 a caller Content-Type replaces the default" \
    "$(grep -a -c '^POST /two \[n=2\] ct=text/plain key=None$' plain.log)" "1"
expect "3 no caller Content-Type: the default application/json, once" \
    "$(grep -a -c '^POST /three \[{}\] ct=application/json key=k3$' plain.log)" "1"
expect "4 a 401 is a response: \$ok 401 with its body" "$(line plain.out 4)" '4 ok 401 {"path":"/deny"}'
expect "5 a CR/LF in a header value: \$err, nothing arrives" \
    "$(line plain.out 5) $(grep -a -c '^POST /inj' plain.log)" "5 err 0"
expect "6 closed port: \$err" "$(line plain.out 6)" "6 err"

SSL_CERT_FILE="${WORK}/srv.pem" ./ph_bin "https://127.0.0.1:${SP}" > tls.out 2>&1
expect "7 https, certificate trusted: arrives, \$ok 200, key delivered" \
    "$(grep -a -c '^POST /one \[{"n":1}\] ct=application/json key=k1$' tls.log) $(line tls.out 1)" \
    '1 1 ok 200 {"path":"/one"}'
: > tls.log
env -u SSL_CERT_FILE -u SSL_CERT_DIR ./ph_bin "https://127.0.0.1:${SP}" > tls2.out 2>&1
expect "8 https, certificate not trusted: \$err, nothing arrives" \
    "$(line tls2.out 1) $(grep -a -c '^POST' tls.log)" "1 err 0"

echo "-----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
