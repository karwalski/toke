#!/usr/bin/env bash
# C278_http_client_calls.sh — the HTTP client calls send what they say, and a
# failure takes the $err arm (story 138.3, loke B3 / T-57 / T-15 / R7).
#
# THE DEFECT.  The client calls shared their names with route registration,
# and route registration won:
#   - http.get(client;"/health") registered a GET route whose path was the
#     client handle: nothing was sent, and its result read $err until the
#     256-entry route table filled, then $ok (T-57);
#   - tk_http_delete_w existed but no source form reached it (R7);
#   - http.get(url) returned "" for a closed port -- no error channel (T-15).
#
# THE FIX (src/stdlib/tk_web_glue.c, stdlib/http.tki).  The client calls have
# names of their own, typed in the interface:
#   http.clientget(c;path) / clientpost(c;path;body;ctype) /
#   clientput(c;path;body;ctype) / clientdelete(c;path) -> httpresp!httperr
#   http.geturl(url) -> str!httperr
# $err is a transport failure; an HTTP status is a response (r.status),
# except for geturl, which has nowhere to put one and makes a non-2xx $err.
#
# WHAT IS ASSERTED, against a recorder on 127.0.0.1 that bound port 0 itself:
#   1. clientget: GET /health arrives; $ok with status 200, the body, headers
#   2. clientpost: POST /p arrives with body x=1 and ONE Content-Type
#   3. clientput: PUT /q arrives with its body
#   4. clientdelete: DELETE /item/7 arrives (R7)
#   5. 300 clientget calls: 300 requests arrive, 300 take $ok (T-57's shape)
#   6. clientget of a 404: $ok, status 404
#   7. clientget against a closed port: $err (T-15's shape)
#   8. geturl: 2xx -> $ok body; closed port -> $err; 404 -> $err
#
# Negative control: C278_STDLIB=<dir> builds against another stdlib tree.
# Main's src/stdlib does not link (no tk_http_client*_w, tk_http_geturl_w).
# With client_result() ignoring is_err (measured), cases 7 and 8b take $ok.
#
# Story: 138.3
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C278_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c278_XXXXXX")"
REC=""
cleanup() { [ -n "${REC}" ] && kill -9 "${REC}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C278: HTTP client calls send, and fail through \$err (138.3)"
echo "-------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

cat > rec.py <<'PY'
import http.server, sys
import socketserver as _ss
def _bind(self):  # 127.439 (Z205): HTTPServer.server_bind does a reverse-DNS getfqdn() that stalls for seconds on the macos-15-intel runner
    _ss.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
http.server.HTTPServer.server_bind = _bind
LOG = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _do(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(n).decode() if n else ""
        ct = self.headers.get_all("Content-Type") or []
        with open(LOG, "a") as f:
            f.write("%s %s [%s] ct=%s\n" % (self.command, self.path, body, "|".join(ct)))
        code = 404 if self.path.startswith("/missing") else 200
        b = ("got %s %s" % (self.command, self.path)).encode()
        self.send_response(code)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("X-Rec", "yes")
        self.end_headers()
        self.wfile.write(b)
    do_GET = do_POST = do_PUT = do_DELETE = _do
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 rec.py rec.log > rec.port 2> rec.err &
REC=$!
for _ in $(seq 1 50); do [ -s rec.port ] && break; sleep 0.1; done
P="$(head -1 rec.port)"
if [ -z "${P}" ]; then echo "  FAIL: the recorder did not start"; echo "Results: 0 passed, 1 failed"; exit 1; fi

cat > cl.tk <<TOKE
m=cl;
i=http:std.http;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let c=http.client("http://127.0.0.1:${P}");
  io.println(mt http.clientget(c;"/health") {\$ok:r "1 ok \(r.status) [\(mt str.frombytes(r.body){\$ok:v v;\$err:e ""})] hdrs=\(r.headers.len)"; \$err:e "1 err"});
  io.println(mt http.clientpost(c;"/p";str.bytes("x=1");"text/plain") {\$ok:r "2 ok \(r.status)"; \$err:e "2 err"});
  io.println(mt http.clientput(c;"/q";str.bytes("y=2");"text/plain") {\$ok:r "3 ok \(r.status)"; \$err:e "3 err"});
  io.println(mt http.clientdelete(c;"/item/7") {\$ok:r "4 ok \(r.status)"; \$err:e "4 err"});
  let oks=mut.0;
  lp(let k=0;k<300;k=k+1){
    let up=mt http.clientget(c;"/many") {\$ok:r true; \$err:e false};
    if(up){ oks=oks+1 }el{}
  };
  io.println("5 ok_arm=\(oks)");
  io.println(mt http.clientget(c;"/missing") {\$ok:r "6 ok \(r.status)"; \$err:e "6 err"});
  let dead=http.client("http://127.0.0.1:1");
  io.println(mt http.clientget(dead;"/health") {\$ok:r "7 ok"; \$err:e "7 err"});
  io.println(mt http.geturl("http://127.0.0.1:${P}/g") {\$ok:s "8a ok [\(s)]"; \$err:e "8a err"});
  io.println(mt http.geturl("http://127.0.0.1:1/g") {\$ok:s "8b ok"; \$err:e "8b err"});
  io.println(mt http.geturl("http://127.0.0.1:${P}/missing") {\$ok:s "8c ok"; \$err:e "8c err"});
  <0
};
TOKE
if ! "${TKC}" --allow-all --out cl_bin cl.tk > build.log 2>&1; then
    echo "  FAIL: the client program does not build"; sed 's/^/      /' build.log | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi
./cl_bin > out.txt 2> err.txt; RC=$?
line() { awk -v k="$1" '$1==k{sub(/^[^ ]+ /,""); print; exit}' out.txt; }
rec() { grep -a -c "^$1 $2 " rec.log; }

expect "1 clientget: GET /health arrives; \$ok 200, body, headers" \
    "$(rec GET /health) $(line 1 | sed 's/hdrs=[1-9][0-9]*$/hdrs>0/')" "1 ok 200 [got GET /health] hdrs>0"
expect "2 clientpost: POST /p arrives with its body and one Content-Type" \
    "$(grep -a -c '^POST /p \[x=1\] ct=text/plain$' rec.log) $(line 2)" "1 ok 200"
expect "3 clientput: PUT /q arrives with its body" \
    "$(grep -a -c '^PUT /q \[y=2\]' rec.log) $(line 3)" "1 ok 200"
expect "4 clientdelete: DELETE /item/7 arrives" \
    "$(rec DELETE /item/7) $(line 4)" "1 ok 200"
expect "5 300 clientget calls: 300 arrive, 300 take \$ok" \
    "$(rec GET /many) $(line 5)" "300 ok_arm=300"
expect "6 clientget of a 404: \$ok with status 404" "$(line 6)" "ok 404"
expect "7 clientget against a closed port: \$err" "$(line 7)" "err"
expect "8 geturl: 2xx body / closed port \$err / 404 \$err" \
    "$(line 8a) | $(line 8b) | $(line 8c) rc=${RC}" "ok [got GET /g] | err | err rc=0"

echo "-------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
