#!/usr/bin/env bash
# C1866_http_client_url_refused.sh — every client call refuses a URL whose
# host, port or path would rewrite the request, before anything is sent
# (story 138.24; review of Batch Z214, 2026-11-15).
#
# THE GAP.  The client wrote the URL's host, port and path into the request
# line, the Host header and the proxy's request line as they were.  138.8
# refused CR/LF in the caller's headers, but a CR/LF in the URL ended the
# request line just the same and let the URL write headers -- or a whole
# Host -- of its own.  Measured by the review: http.getheaders with
# "http://127.0.0.1:P/x HTTP/1.1\r\nHost: evil.example\r\nX-Inj: 1\r\nY: z"
# answered $ok 200 and the server read Host: evil.example ahead of the
# client's (C1863 10); under http.withconnectaddr a CR/LF in the URL's host
# went straight into Host (C1862 9).  The same route was open in every call
# below, from before this story.
#
# NOW: client_parse_url, which every client call goes through, refuses
# (-2) a host that is not a DNS name or an IPv4 address, and a port or path
# holding a control byte (C0 or DEL) or a space.  The request calls answer
# $err "URL refused: ...", http.downloadfile -1; a refused proxy URL is
# "failed to parse proxy URL".  Nothing is connected or sent.
#
# One recorder on 127.0.0.1 (a port in 48150-48154) counts what arrives:
#   1. http.clientget, CR/LF + a header in the path: $err, nothing arrives
#   2. http.client with CR/LF in the base URL's host: $err, nothing arrives
#   3. http.geturl, CR/LF in the path: $err, nothing arrives
#   4. http.postheaders, request-line smuggling in the url: $err, nothing
#   5. http.clientpost, a bare LF in the path: $err, nothing arrives
#   6. http.clientget, a TAB in the path: $err, nothing arrives
#   7. http.clientget, a space in the path: $err, nothing arrives
#   8. http.stream, CR/LF in the base URL's path: $err, nothing arrives
#   9. http.downloadfile, CR/LF in the url: -1, nothing arrives, no file
#  10. http.withproxy, CR/LF in the proxy URL's host: $err "failed to parse
#      proxy URL", nothing arrives
#  11-13. controls, sent as before: a percent-encoded query string, a
#      UTF-8 path (bytes over 0x7f are not refused), and http.geturl of a
#      plain URL
#
# Negative control: C1866_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15 with client_parse_url's check removed: 3 passed (the
# controls), 11 failed -- 1, 3, 4, 5, 6, 7, 8 and 9 reached the recorder (1
# and 4 answered $ok 200 with the smuggled X-Inj header delivered; 5-7 were
# $ok 400), and 2 and 10 failed at the name lookup ("TCP connect failed")
# rather than being refused.
#
# Story: 138.24
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1866_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the recorder"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1866_XXXXXX")"
S1=""
cleanup() { [ -n "${S1}" ] && { kill -9 "${S1}" 2>/dev/null; wait "${S1}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1866: the client refuses a URL that would rewrite the request (138.24)"
echo "------------------------------------------------------------------------"

# rec.py LOG: one line per request, "<METHOD> <target> inj=<X-Inj>", 200 "ok".
cat > rec.py <<'PY'
import http.server, socketserver, sys
LOG = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def any(self):
        with open(LOG, "a") as f: f.write("%s %s inj=%s\n" % (self.command, self.path, self.headers.get("X-Inj")))
        n = int(self.headers.get("Content-Length") or 0)
        if n: self.rfile.read(n)
        self.send_response(200); self.send_header("Content-Length", "2"); self.send_header("Connection", "close")
        self.end_headers(); self.wfile.write(b"ok")
    do_GET = any; do_POST = any
    def send_error(self, code, message=None, explain=None):   # a malformed request is logged too
        with open(LOG, "a") as f: f.write("BAD %s %s\n" % (code, (self.raw_requestline or b"")[:60]))
        http.server.BaseHTTPRequestHandler.send_error(self, code, message, explain)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    def server_bind(self):   # no reverse-DNS getfqdn() (127.439)
        socketserver.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
srv = None
for p in range(48150, 48155):
    try:
        srv = S(("127.0.0.1", p), H); break
    except OSError:
        pass
if srv is None: raise SystemExit("no free port in 48150..48154")
print(p, flush=True)
srv.serve_forever()
PY
python3 rec.py rec.log > rec.port 2> rec.err & S1=$!
for _ in $(seq 1 50); do [ -s rec.port ] && break; sleep 0.1; done
P="$(head -1 rec.port)"
[ -n "${P}" ] || { echo "  FAIL: the recorder did not start: $(head -c 200 rec.err)"; echo "C1866: 0 passed, 1 failed"; exit 1; }

cat > u.tk <<'TOKE'
m=u;
i=http:std.http;
i=io:std.io;
i=str:std.str;
i=args:std.args;
f=g(k:$str;c:$httpclient;p:$str):$str{
  <mt http.clientget(c;p) {$ok:v "\(k) ok \(v.status)"; $err:e "\(k) err \(e.msg)"}
};
f=main():i64{
  let b=str.concat("http://127.0.0.1:";args.get(1));
  let c=http.client(b);
  io.println(g("1";c;"/one HTTP/1.1\r\nX-Inj: 1\r\nY: z"));
  io.println(g("2";http.client(str.concat("http://127.0.0.1\r\nX-Inj: 1:";args.get(1)));"/two"));
  io.println(mt http.geturl(str.concat(b;"/three\r\nX-Inj: 1")) {$ok:s "3 ok \(s)"; $err:e "3 err \(e.msg)"});
  io.println(mt http.postheaders(str.concat(b;"/four HTTP/1.1\r\nX-Inj: 1\r\nY: z");"{}";@()) {$ok:v "4 ok \(v.status)"; $err:e "4 err \(e.msg)"});
  io.println(mt http.clientpost(c;"/five\nX-Inj: 1";str.bytes("x");"text/plain") {$ok:v "5 ok \(v.status)"; $err:e "5 err \(e.msg)"});
  io.println(g("6";c;"/six\tx"));
  io.println(g("7";c;"/seven x"));
  io.println(mt http.stream(http.client(str.concat(b;"/eight\r\nX-Inj: 1"))) {$ok:s "8 ok stream"; $err:e "8 err \(e.msg)"});
  io.println("9 download \(http.downloadfile(c;"/nine\r\nX-Inj: 1";"dl.out"))");
  io.println(g("10";http.withproxy(c;str.concat("http://127.0.0.1\r\nX-Inj: 1:";args.get(1)));"/ten"));
  io.println(g("11";c;"/eleven?q=a%20b&x=1"));
  io.println(g("12";c;"/twelve/\xc3\xa9"));
  io.println(mt http.geturl(str.concat(b;"/thirteen")) {$ok:s "13 ok \(s)"; $err:e "13 err \(e.msg)"});
  <0
};
TOKE
if ! "${TKC}" --allow-all --out u_bin u.tk > build.log 2>&1; then
    echo "  FAIL: the program does not build"; sed 's/^/      /' build.log | head -6
    echo "C1866: 0 passed, 1 failed"; exit 1
fi
rm -f dl.out
./u_bin "${P}" > u.out 2>&1
line() { awk -v k="$1" '$1==k{print; exit}' u.out; }
arrived() { grep -a -c "$1" rec.log; }
REF="err URL refused: the host must be a DNS name or an IPv4 address, and the port and path may not hold a space or a control character"

expect "1 clientget: CR/LF and a header in the path refused, nothing arrives" "$(line 1) | $(arrived /one)" "1 ${REF} | 0"
expect "2 http.client: CR/LF in the host refused" "$(line 2) | $(arrived /two)" "2 ${REF} | 0"
expect "3 geturl: CR/LF in the path refused, nothing arrives" "$(line 3) | $(arrived /three)" "3 ${REF} | 0"
expect "4 postheaders: request-line smuggling in the url refused" "$(line 4) | $(arrived /four)" "4 ${REF} | 0"
expect "5 clientpost: a bare LF in the path refused" "$(line 5) | $(arrived /five)" "5 ${REF} | 0"
expect "6 clientget: a TAB in the path refused" "$(line 6) | $(arrived /six)" "6 ${REF} | 0"
expect "7 clientget: a space in the path refused" "$(line 7) | $(arrived /seven)" "7 ${REF} | 0"
expect "8 http.stream: CR/LF in the base URL's path refused" "$(line 8) | $(arrived /eight)" "8 ${REF} | 0"
expect "9 downloadfile: -1, nothing arrives, no file" \
    "$(line 9) | $(arrived /nine) $([ -e dl.out ] && echo file || echo nofile)" "9 download -1 | 0 nofile"
expect "10 withproxy: CR/LF in the proxy URL's host refused" "$(line 10) | $(arrived /ten)" "10 err failed to parse proxy URL | 0"
expect "11 control: a percent-encoded query is sent as it is" \
    "$(line 11) | $(arrived '^GET /eleven?q=a%20b&x=1 inj=None$')" "11 ok 200 | 1"
expect "12 control: a UTF-8 path is sent (bytes over 0x7f are not refused)" \
    "$(line 12) | $(arrived '^GET /twelve/')" "12 ok 200 | 1"
expect "13 control: geturl of a plain URL" "$(line 13) | $(arrived '^GET /thirteen inj=None$')" "13 ok ok | 1"
expect "nothing smuggled reached the recorder" "$(arrived 'inj=1') $(arrived '^BAD')" "0 0"

echo ""
echo "C1866: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
