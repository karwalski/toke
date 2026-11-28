#!/usr/bin/env bash
# C304_http_client_err_payload.sh — the $err payload of the typed http client
# calls is readable, and the retired http.get(url) / http.post(url;body)
# spellings are refused at build instead of registering routes (story 138.3
# (B3), the llvm.c half; loke T-15, T-57, T-10 shapes on the new calls).
#
# THE DEFECTS.
#   (a) http.clientget/clientpost/clientput/clientdelete/geturl return
#       `T!$httperr` and leave an {msg; code} box in the error slot, but
#       llvm.c's is_stdlib_slot_wrapper did not list them, so `$err:e e.msg`
#       read a nil payload: RT005 at run time.
#   (b) llvm.c rewrote a 1-argument `http.get(url)` to the client call
#       tk_http_get_w and a 2-argument non-handler `http.post` to
#       tk_http_posturl_w.  Those spellings are route registrations in
#       stdlib/http.tki; the client calls are http.geturl / http.clientpost.
#       With the rewrite retired, `http.get(url)` called the 2-parameter
#       route registration with ONE argument (built, garbage handler) and
#       `http.post(url;"body")` registered a str as a handler — so both are
#       now build errors (E4026 / E4031), located, no fix.
#
# Negative control: TKC built from this branch at dfb4c17 (before this change:
# no slot rows, the rewrites in place) — cases 1, 2, 3 red ("RT005: nil
# dereference"; case 2's program also reads a 404 payload), 4 and 5 red
# (both BUILT).  2b (300 requests arrive) is green on both.
#
# Story: 138.3, 127.565

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; REC=""
cleanup() { [ -n "${REC}" ] && { kill -9 "${REC}" 2>/dev/null; wait "${REC}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
build_run() { rm -f b; "${TKC}" "$1" --out b > build.log 2>&1 || { echo "build-failed"; return; }
              ./b --allow-net 2>&1 | tr '\n' ' '; }

# 1. closed port: every client call's $err binds a readable {msg; code}
cat > p1.tk <<'EOF'
m=p1;
i=io:std.io;
i=http:std.http;
i=str:std.str;
f=main():i64{
  let c=http.client("http://127.0.0.1:1");
  io.println(mt http.clientget(c;"/h") { $ok:r "ok"; $err:e "get:\(e.code):\(str.len(e.msg)>0)" });
  io.println(mt http.clientpost(c;"/p";str.bytes("x");"text/plain") { $ok:r "ok"; $err:e "post:\(e.code):\(str.len(e.msg)>0)" });
  io.println(mt http.clientput(c;"/p";str.bytes("x");"text/plain") { $ok:r "ok"; $err:e "put:\(e.code):\(str.len(e.msg)>0)" });
  io.println(mt http.clientdelete(c;"/p") { $ok:r "ok"; $err:e "del:\(e.code):\(str.len(e.msg)>0)" });
  io.println(mt http.geturl("http://127.0.0.1:1/h") { $ok:s "ok"; $err:e "url:\(e.code):\(str.len(e.msg)>0)" });
  <0
};
EOF
expect "1-closed-port-err-payload" "$(build_run p1.tk)" "get:0:true post:0:true put:0:true del:0:true url:0:true "

# 2-3. live recorder (T-57 shape: 300 requests, all on the ok arm; geturl 404
#      is $err with code 404)
cat > rec.py <<'PY'
import http.server, sys
import socketserver as _ss
def _bind(self):  # 127.439 (Z205): HTTPServer.server_bind does a reverse-DNS getfqdn() that stalls for seconds on the macos-15-intel runner
    _ss.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
http.server.HTTPServer.server_bind = _bind
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        with open("rec.log", "a") as f: f.write(self.path + "\n")
        code = 404 if self.path.startswith("/missing") else 200
        b = b"hi"; self.send_response(code); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b)
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 rec.py > rec.port 2> rec.err & REC=$!
tk_wait_until "${REC}" tk_nonempty rec.port
P="$(head -1 rec.port)"
cat > p2.tk <<TOKE
m=p2;
i=io:std.io;
i=http:std.http;
f=main():i64{
  let c=http.client("http://127.0.0.1:${P}");
  let oks=mut.0;
  lp(let k=0;k<300;k=k+1){ let up=mt http.clientget(c;"/health") { \$ok:r true; \$err:e false }; if(up){ oks=oks+1 }el{} };
  io.println("ok_arm=\(oks)");
  io.println(mt http.geturl("http://127.0.0.1:${P}/missing") { \$ok:s "ok"; \$err:e "err \(e.code)" });
  <0
};
TOKE
expect "2-T57-shape-300-ok-arms" "$(build_run p2.tk | cut -d' ' -f1)" "ok_arm=300"
expect "2b-300-requests-arrived" "$(grep -c '^/health$' rec.log 2>/dev/null | tr -d ' ')" "300"
expect "3-geturl-404-code" "$(build_run p2.tk | cut -d' ' -f2-3)" "err 404"

# 4. retired: 1-argument http.get(url) is a build error, not a route call
printf 'm=p4;\ni=http:std.http;\ni=io:std.io;\nf=main():i64{\n  let b=http.get("http://127.0.0.1:1/");\n  io.println(b);\n  <0\n};\n' > p4.tk
rm -f b; "${TKC}" p4.tk --out b > b4.log 2>&1; rc=$?
expect "4-http-get-url-is-E4026" "$([ ${rc} -ne 0 ] && grep -o '"error_code":"E4026"' b4.log | head -1 || echo built)" '"error_code":"E4026"'

# 5. retired: 2-argument http.post(url;"body") (T-10's client2.tk) is a build error
printf 'm=p5;\ni=http:std.http;\nf=main():i64{\n  let r=http.post("http://127.0.0.1:1/p";"body");\n  <0\n};\n' > p5.tk
rm -f b; "${TKC}" p5.tk --out b > b5.log 2>&1; rc=$?
expect "5-http-post-str-is-E4031" "$([ ${rc} -ne 0 ] && grep -o '"error_code":"E4031"' b5.log | head -1 || echo built)" '"error_code":"E4031"'

echo "C304: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
