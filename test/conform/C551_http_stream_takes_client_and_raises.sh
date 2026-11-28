#!/usr/bin/env bash
# C551_http_stream_takes_client_and_raises.sh — http.stream takes the
# $httpclient its .tki publishes, and http.stream / http.streamnext raise an
# $httperr on failure (story 121.53 item 2).
#
# THE DEFECT, measured on main (cde89021):
#   stdlib/http.tki publishes http.stream(httpclient), but tk_http_stream_w
#   (tk_web_glue.c) read its one argument as a URL string: the published call
#   `http.stream(http.client(url))` is rejected at compile time ("argument 1
#   of 'std.http.stream' is read by the runtime as a C string").  Both
#   wrappers returned a bare 0 on failure and sat on the bare-glue list in
#   stdlib_tables.c, so the $err arm bound nil and `e.msg` was RT005.
#
# NOW: the glue takes the client (borrowed), GETs its base URL, and raises an
# httperr; both left the bare-glue list (C375 keeps that list exact).
#
# Negative control: TKC=<tkc> picks another compiler; main's fails 3 of 3
# (the build is rejected).  With this change's glue but the two entries put
# back on stdlib_tables.c's bare-glue list, 1 of 3 passes: both $err cases
# print RT005, because the arm binds nil instead of the raised httperr.
#
# Story: 121.53, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c551_XXXXXX")"
cleanup() { if [ -n "${SRV}" ]; then kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; fi; rm -rf "${WORK}"; }
trap cleanup EXIT; cd "${WORK}"
echo "C551: http.stream takes its published client and raises httperr"
echo "-----------------------------------------------------------------"

cat > a.tk <<'EOF'
m=a;
i=http:std.http;
i=io:std.io;
i=str:std.str;
i=env:std.env;
f=main():i64{
  let c=http.client(env.getor("URL";""));
  let r=mt http.stream(c) {
    $ok:s mt http.streamnext(s) { $ok:b "ok:\(mt str.frombytes(b){$ok:v v;$err:e ""})"; $err:e "nexterr:\(e.msg)" };
    $err:e "err:\(e.msg)"
  };
  io.println(r);
  <0
};
EOF
printf 'hello-stream' > data.txt
# A server on an ephemeral port that closes after its one response, and a
# port nothing listens on (bound, then released).
cat > srv.py <<'EOF'
import http.server, socketserver, sys
class H(http.server.SimpleHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def log_message(self, *a): pass
with socketserver.TCPServer(("127.0.0.1", 0), H) as s:
    print(s.server_address[1], flush=True)
    s.serve_forever()
EOF
python3 srv.py > srv.port 2> srv.err & SRV=$!
tk_wait_until "${SRV}" tk_nonempty srv.port
PORT="$(head -1 srv.port)"
DEAD="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"

if "${TKC}" a.tk -o a > build.out 2>&1; then
    got="$(URL="http://127.0.0.1:${PORT}/data.txt" ./a --allow-net 2>&1 | head -1)"
    if [ "${got}" = "ok:hello-stream" ]; then PASS=$((PASS + 1)); echo "  PASS: http.stream(client) streams the base URL: ${got}"
    else FAIL=$((FAIL + 1)); echo "  FAIL: stream of a live server: '${got}' (want ok:hello-stream)"; fi
    got="$(URL="http://127.0.0.1:${DEAD}/x" ./a --allow-net 2>&1 | head -1)"
    if [ "${got}" = "err:TCP connect failed" ]; then PASS=$((PASS + 1)); echo "  PASS: a refused connection is \$err with e.msg: ${got}"
    else FAIL=$((FAIL + 1)); echo "  FAIL: refused connection: '${got}' (want err:TCP connect failed)"; fi
    got="$(URL="https://127.0.0.1:${DEAD}/x" ./a --allow-net 2>&1 | head -1)"
    if printf '%s' "${got}" | /usr/bin/grep -q '^err:.'; then PASS=$((PASS + 1)); echo "  PASS: an https base URL is \$err with e.msg: ${got}"
    else FAIL=$((FAIL + 1)); echo "  FAIL: https base URL: '${got}' (want err:<msg>)"; fi
else
    FAIL=$((FAIL + 3)); echo "  FAIL: build: $(/usr/bin/grep -a -o '"message":"[^"]*' build.out | head -1)"
fi
echo ""
echo "C551: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
