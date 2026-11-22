#!/usr/bin/env bash
# C1863_http_getheaders.sh — http.getheaders(url;headers) is a GET that
# carries the caller's headers, typed like http.postheaders, with 138.8's
# header rules (story 138.27, owner answer 2.3; loke T-58 addendum).
#
# THE GAP.  std.http had no GET that takes headers: http.geturl(url) and
# http.clientget(c;path) only.  The GitHub API refuses a request without a
# User-Agent, so loke's update checks answered "no update" without sending
# (loke VM1.6g, F10.41).
#
# NOW: http.getheaders(url:$str;headers:@(@($str))):$httpresp!$httperr, on
# the client every other call uses (http_request_headers): the headers are
# sent as given, Host and Content-Length are the client's and may not be
# given, a name or value with CR or LF is refused before anything is sent,
# a status is in r.status ($err only for a transport failure), and https
# is verified.
#
# Loopback recorders (127.0.0.1, ports in 48130-48139), one plain, one TLS:
#   1. User-Agent and X-A arrive on a GET with no body, no Content-Length and
#      no Content-Type; Host is 127.0.0.1:P; $ok 200
#   2. a caller Host: $err, nothing arrives
#   3. a caller Content-Length: $err, nothing arrives
#   4. CR/LF in a value: $err, nothing arrives
#   5. LF in a name: $err, nothing arrives
#   6. a 404 is a response: $ok 404
#   7. a closed port: $err
#   8. https, certificate trusted: $ok 200, User-Agent delivered
#   9. https, certificate not trusted: $err, nothing arrives
#  10. the url carries the smuggling (review of Z214): a path of
#      "/inj HTTP/1.1\r\nHost: evil.example\r\nX-Inj: 1\r\nY: z" is $err
#      "URL refused", and nothing arrives -- no Host evil.example, no X-Inj.
#      The header rules above held for the headers argument, but the url
#      went into the request line as it was: $ok 200, and the server read
#      the url's Host ahead of the client's.
#  11. a CR/LF in the url's host: $err "URL refused", nothing arrives
#  12. a space in the url's path: $err "URL refused", nothing arrives
#  13. control: a query string (?a=1&b=two%20x) is sent as it is, $ok 200
#
# Negative control: C1863_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15: against v3.2.0's src/stdlib the program does not
# check (E4034: getheaders' result is not a known record there).  With
# tk_http_getheaders_w dropping the caller's headers: 3 passed, 6 failed --
# 1 and 8 arrive without User-Agent, and 2-5 are sent ($ok) because nothing
# is left to refuse.  With client_parse_url's URL check removed: 10 and 12
# fail ($ok 200 / $ok 400, and the server read Host evil.example and X-Inj
# for 10); 11 fails at the lookup instead ("TCP connect failed").
#
# Story: 138.27
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1863_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the recorders"; exit 0; }
command -v openssl >/dev/null 2>&1 || { echo "CONFORM-SKIP: no openssl(1) on this host to make a test certificate"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1863_XXXXXX")"
R1=""; R2=""
cleanup() { for p in ${R1} ${R2}; do kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1863: http.getheaders sends the caller's headers on a GET (138.27)"
echo "---------------------------------------------------------------------"

openssl req -x509 -newkey rsa:2048 -keyout srv.key -out srv.pem -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" > cert.log 2>&1
[ -s srv.pem ] || { echo "  FAIL: openssl could not make a certificate"; echo "C1863: 0 passed, 1 failed"; exit 1; }

cat > rec.py <<'PY'
import http.server, socketserver, ssl, sys
LOG = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        h = self.headers
        with open(LOG, "a") as f:
            f.write("GET %s ua=%s xa=%s host=%s cl=%s ct=%s\n" % (self.path, h.get("User-Agent"), h.get("X-A"),
                    h.get("Host"), h.get("Content-Length"), h.get("Content-Type")))
            if h.get("X-Inj") is not None: f.write("SMUGGLED X-Inj: %s\n" % h.get("X-Inj"))
            for v in h.get_all("Host") or []:
                if "evil" in v: f.write("SMUGGLED Host: %s\n" % v)
        code = 404 if self.path == "/missing" else 200
        self.send_response(code); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
class S(http.server.HTTPServer):
    allow_reuse_address = True
    def server_bind(self):   # no reverse-DNS getfqdn() (127.439)
        socketserver.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
base = int(sys.argv[2]); srv = None
for p in range(base, base + 5):
    try:
        srv = S(("127.0.0.1", p), H); break
    except OSError:
        pass
if srv is None: raise SystemExit("no free port in %d..%d" % (base, base + 4))
if len(sys.argv) > 3:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[3], sys.argv[4])
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print(p, flush=True)
srv.serve_forever()
PY
python3 rec.py plain.log 48130 > plain.port 2> plain.err & R1=$!
python3 rec.py tls.log 48135 srv.pem srv.key > tls.port 2> tls.err & R2=$!
for _ in $(seq 1 50); do [ -s plain.port ] && [ -s tls.port ] && break; sleep 0.1; done
HP="$(head -1 plain.port)"; SP="$(head -1 tls.port)"
if [ -z "${HP}" ] || [ -z "${SP}" ]; then echo "  FAIL: the recorders did not start: $(head -c 200 plain.err tls.err)"; echo "C1863: 0 passed, 1 failed"; exit 1; fi

cat > gh.tk <<'TOKE'
m=gh;
i=http:std.http;
i=io:std.io;
i=str:std.str;
i=args:std.args;
f=show(k:$str;u:$str;h:@(@($str))):$str{
  <mt http.getheaders(u;h) {$ok:r "\(k) ok \(r.status) \(mt str.frombytes(r.body){$ok:b b;$err:x ""})"; $err:e "\(k) err \(e.msg)"}
};
f=main():i64{
  let base=(mt args.get(1){$ok:v v;$err:e ""});
  io.println(show("1";str.concat(base;"/one");@(@("User-Agent";"loke/1.0");@("X-A";"b"))));
  io.println(show("2";str.concat(base;"/host");@(@("Host";"evil.example"))));
  io.println(show("3";str.concat(base;"/len");@(@("content-length";"5"))));
  io.println(show("4";str.concat(base;"/crlf");@(@("X-A";"b\r\nX-Injected: 1"))));
  io.println(show("5";str.concat(base;"/lfname");@(@("X-A\nX-B";"b"))));
  io.println(show("6";str.concat(base;"/missing");@(@("User-Agent";"loke/1.0"))));
  io.println(show("7";"http://127.0.0.1:1/x";@(@("User-Agent";"loke/1.0"))));
  io.println(show("10";str.concat(base;"/inj HTTP/1.1\r\nHost: evil.example\r\nX-Inj: 1\r\nY: z");@(@("User-Agent";"t"))));
  io.println(show("11";str.concat(str.concat("http://127.0.0.1\r\nX-Inj: 1:";(mt args.get(2){$ok:v v;$err:e ""}));"/h");@(@("User-Agent";"t"))));
  io.println(show("12";str.concat(base;"/sp ace");@(@("User-Agent";"t"))));
  io.println(show("13";str.concat(base;"/q?a=1&b=two%20x");@(@("User-Agent";"t"))));
  <0
};
TOKE
if ! "${TKC}" --allow-all --out gh_bin gh.tk > build.log 2>&1; then
    echo "  FAIL: the program does not build"; sed 's/^/      /' build.log | head -6
    echo "C1863: 0 passed, 1 failed"; exit 1
fi
./gh_bin "http://127.0.0.1:${HP}" "${HP}" > plain.out 2>&1
line() { awk -v k="$2" '$1==k{print; exit}' "$1"; }
sent() { grep -a -c "^GET $1 " plain.log; }
RES="err Content-Length and Host are set by the client, not the caller"
INV="err invalid header name or value (empty, or contains CR, LF or ':')"

expect "1 User-Agent and X-A arrive on a bodiless GET, Host is the client's" \
    "$(line plain.out 1) | $(grep -a "^GET /one " plain.log)" \
    "1 ok 200 ok | GET /one ua=loke/1.0 xa=b host=127.0.0.1:${HP} cl=None ct=None"
expect "2 a caller Host: \$err, nothing arrives" "$(line plain.out 2) $(sent /host)" "2 ${RES} 0"
expect "3 a caller Content-Length: \$err, nothing arrives" "$(line plain.out 3) $(sent /len)" "3 ${RES} 0"
expect "4 CR/LF in a value: \$err, nothing arrives" "$(line plain.out 4) $(sent /crlf)" "4 ${INV} 0"
expect "5 LF in a name: \$err, nothing arrives" "$(line plain.out 5) $(sent /lfname)" "5 ${INV} 0"
expect "6 a 404 is a response" "$(line plain.out 6)" "6 ok 404 ok"
expect "7 a closed port: \$err" "$(line plain.out 7)" "7 err TCP connect failed"
REF="err URL refused: the host must be a DNS name or an IPv4 address, and the port and path may not hold a space or a control character"
expect "10 Host and a header smuggled through the url: \$err, nothing arrives" \
    "$(line plain.out 10) | $(sent /inj) $(grep -a -c '^SMUGGLED' plain.log)" "10 ${REF} | 0 0"
expect "11 CR/LF in the url's host: \$err, nothing arrives" \
    "$(line plain.out 11) | $(sent /h)" "11 ${REF} | 0"
expect "12 a space in the url's path: \$err, nothing arrives" \
    "$(line plain.out 12) | $(sent /sp)" "12 ${REF} | 0"
expect "13 control: a query string is sent as it is" \
    "$(line plain.out 13) | $(grep -a -c '^GET /q?a=1&b=two%20x ua=t ' plain.log)" "13 ok 200 ok | 1"

SSL_CERT_FILE="${WORK}/srv.pem" ./gh_bin "https://127.0.0.1:${SP}" "${SP}" > tls.out 2>&1
expect "8 https, certificate trusted: \$ok 200, User-Agent delivered" \
    "$(line tls.out 1) | $(grep -a -c '^GET /one ua=loke/1.0 ' tls.log)" "1 ok 200 ok | 1"
: > tls.log
env -u SSL_CERT_FILE -u SSL_CERT_DIR ./gh_bin "https://127.0.0.1:${SP}" "${SP}" > tls2.out 2>&1
case "$(line tls2.out 1)" in
"1 err TLS certificate verification failed"*)
    expect "9 https, certificate not trusted: \$err, nothing arrives" "$(grep -a -c '^GET' tls.log)" "0" ;;
*) expect "9 https, certificate not trusted" "$(line tls2.out 1)" "1 err TLS certificate verification failed: ..." ;;
esac

echo ""
echo "C1863: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
