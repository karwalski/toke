#!/usr/bin/env bash
# C1021_http_client_host_port.sh — the HTTP client's Host header carries the
# port when it is not the scheme's default (story 138.28, RFC 9110 7.2).
#
# THE DEFECT.  src/stdlib/http.c formatted `Host: %s` with the bare host in
# every request it built -- client_do_request (geturl, clientget/post/put/
# delete, postheaders), http.stream and http.downloadfile -- so a request to
# 127.0.0.1:47974 said `Host: 127.0.0.1`, and a virtual-hosted server on a
# non-default port could route it to the wrong site.
#
# A loopback recorder (ephemeral port) logs "<request-target> <Host>" for
# every request.  The default-port cases go through http.withproxy to the
# same recorder (the absolute request-target names the port either way),
# so the client names a host on port 80 without anything
# listening there and without a DNS lookup (the proxy is what it connects to).
#
# Negative control: this compiler with main c26cbc1b's src/stdlib/http.c
# (TKC_STDLIB_DIR) -- 3 passed, 7 failed: cases 1-5, 7 and 9 red (`127.0.0.1`
# / `example.test`, no port); 0, 6 and 8 green on both.
#
# Story: 138.28

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: compiler not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1021_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1

cat > rec.py <<'PY'
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def rec(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        if n: self.rfile.read(n)
        with open("host.log", "a") as f:
            f.write("%s %s\n" % (self.path, "|".join(self.headers.get_all("Host") or ["-"])))
        b = b"hi"; self.send_response(200); self.send_header("Content-Length", "2")
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b)
    do_GET = rec; do_POST = rec
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 rec.py > rec.port 2> rec.err & SRV=$!
for _ in $(seq 1 50); do [ -s rec.port ] && break; sleep 0.1; done
P="$(head -1 rec.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the recorder did not start"; echo "C1021: 0 passed, 1 failed"; exit 1; fi

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=str:std.str;
i=args:std.args;
f=main():i64{
  let base=args.get(1);
  let c=http.client(base);
  io.println(mt http.geturl(str.concat(base;"/1")) { $ok:b "1 ok"; $err:e "1 err" });
  io.println(mt http.clientget(c;"/2") { $ok:r "2 ok"; $err:e "2 err" });
  io.println(mt http.clientpost(c;"/3";str.bytes("x");"text/plain") { $ok:r "3 ok"; $err:e "3 err" });
  io.println(mt http.postheaders(str.concat(base;"/4");"{}";@(@("x-k";"1"))) { $ok:r "4 ok"; $err:e "4 err" });
  let sk=http.client(str.concat(base;"/5"));
  io.println(mt http.stream(sk) { $ok:s "5 ok"; $err:e "5 err" });
  let p80=http.withproxy(http.client("http://example.test");base);
  io.println(mt http.clientget(p80;"/6") { $ok:r "6 ok"; $err:e "6 err" });
  let p8080=http.withproxy(http.client("http://example.test:8080");base);
  io.println(mt http.clientget(p8080;"/7") { $ok:r "7 ok"; $err:e "7 err" });
  let px80=http.withproxy(http.client("http://example.test:80");base);
  io.println(mt http.clientget(px80;"/8") { $ok:r "8 ok"; $err:e "8 err" });
  io.println("9 \(http.downloadfile(c;"/9";args.get(2)))");
  <0
};
TOKE
if ! "${TKC}" p.tk --out b > build.log 2>&1; then
    echo "FAIL: the program does not build"; head -6 build.log; echo "C1021: ${PASS} passed, 1 failed"; exit 1
fi
./b "http://127.0.0.1:${P}" "${WORK}/dl.out" --allow-net --allow-fs-write > out.txt 2>&1
host_for() { awk -v k="$1" '$1==k{ print $2; exit }' host.log 2>/dev/null; }

expect "0-all-requests-answered" "$(grep -c ' ok$' out.txt | tr -d ' ')" "8"
expect "1-geturl-nondefault-port"       "$(host_for /1)" "127.0.0.1:${P}"
expect "2-clientget-nondefault-port"    "$(host_for /2)" "127.0.0.1:${P}"
expect "3-clientpost-nondefault-port"   "$(host_for /3)" "127.0.0.1:${P}"
expect "4-postheaders-nondefault-port"  "$(host_for /4)" "127.0.0.1:${P}"
expect "5-stream-nondefault-port"       "$(host_for /5)" "127.0.0.1:${P}"
expect "6-default-port-80-bare-host"    "$(host_for http://example.test:80/6)" "example.test"
expect "7-explicit-8080-kept"           "$(host_for http://example.test:8080/7)" "example.test:8080"
expect "8-explicit-80-is-default"       "$(host_for http://example.test:80/8)" "example.test"
expect "9-downloadfile-nondefault-port" "$(host_for /9)" "127.0.0.1:${P}"

echo "C1021: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
