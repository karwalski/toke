#!/usr/bin/env bash
# C1865_http_client_record_literal.sh — a $httpclient built as a record
# literal is an ordinary client with no proxy and no controls, every time
# (story 138.24; review of Batch Z214, 2026-11-15).
#
# THE GAP.  $httpclient is a declared record of three fields (baseurl;
# poolsize; timeoutms -- stdlib/http.tki), so a program may write
# $httpclient{baseurl:..;poolsize:..;timeoutms:..} and --check accepts it.
# The literal is those three 8-byte slots.  The C client (HttpClient,
# http.h) keeps private settings after them -- proxy_url since 42.1.3, and
# 138.24's deadline, body cap and connect address -- and read them from any
# client it was handed, so through a literal it read whatever lay past the
# record's end.  Measured by the review at the first round of 138.24:
# clientget on a literal answered "err response body exceeds the cap (2
# bytes)" 5 of 5 times against a live server (v3.2.0: ok 200), took a
# spurious withconnectaddr or deadline error 20 of 20 times against a
# closed port, and http.withproxy(literal;...) segfaulted 4 runs in 20.
#
# NOW: http.c records every client it allocates (http.client and the with*
# copies) and reads the private settings only of those; any other client
# is a literal, with no proxy and every control off.  Nothing past a
# literal's third slot is read.
#
# Loopback only: an origin and a proxy recorder on 127.0.0.1 (ports in
# 48140-48149); the closed port is 127.0.0.1:1.  Each case runs RUNS times,
# because the old read past the end was not deterministic, and every run
# must give the same answer:
#   1. clientget on a literal: ok 200, the whole 783-byte body
#   2. the same against a closed port: "TCP connect failed"
#   3. http.withproxy(literal; the recorder): ok 200 from the proxy, which
#      read the absolute-form request line -- the copy carries the proxy
#   4. http.withdeadline(literal; 5000): ok 200, 783 bytes (no cap came in)
#   5. http.withmaxbody(literal; 10): $err "response body exceeds the cap
#      (10 bytes)" -- the copy carries the cap it was given, and only it
#   6. http.withconnectaddr(literal naming svc.invalid; 127.0.0.1): ok 200,
#      Host svc.invalid:P (svc.invalid is never looked up)
#   7. http.stream(literal): $ok -- not refused as carrying a control
#   8. http.downloadfile(literal; ...): 0, and the file holds the body
#
# Negative control: C1865_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15 against the first round's src/stdlib (2217524d), 8
# runs a case: 0 passed, 8 failed.  1 was "request deadline exceeded (0
# ms)" every run; 2-6 mixed spurious deadline, withconnectaddr and cap
# errors, and 3-6 segfaulted in 3 to 5 runs of 8 (rc 139); 7 and 8 were
# refused as carrying a control (-1 for the download).
#
# Story: 138.24, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1865_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
RUNS="${C1865_RUNS:-8}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the loopback servers"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1865_XXXXXX")"
S1=""; S2=""
cleanup() { for p in ${S1} ${S2}; do kill -9 "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1865: a record-literal \$httpclient has no proxy and no controls (138.24)"
echo "-------------------------------------------------------------------------"

# srv.py LOG BODYLEN BASEPORT: logs "GET <target> host=<Host>" per request
# and answers 200 with BODYLEN bytes of 'x'.
cat > srv.py <<'PY'
import http.server, socketserver, sys
LOG = sys.argv[1]; N = int(sys.argv[2]); base = int(sys.argv[3])
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        with open(LOG, "a") as f: f.write("GET %s host=%s\n" % (self.path, self.headers.get("Host")))
        b = b"x" * N
        self.send_response(200); self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(b)
class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True
    def server_bind(self):   # no reverse-DNS getfqdn() (127.439)
        socketserver.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
srv = None
for p in range(base, base + 5):
    try:
        srv = S(("127.0.0.1", p), H); break
    except OSError:
        pass
if srv is None: raise SystemExit("no free port in %d..%d" % (base, base + 4))
print(p, flush=True)
srv.serve_forever()
PY
python3 srv.py origin.log 783 48140 > origin.port 2> origin.err & S1=$!
python3 srv.py proxy.log 5 48145 > proxy.port 2> proxy.err & S2=$!
tk_wait_until "${S1}" tk_nonempty origin.port; tk_wait_until "${S2}" tk_nonempty proxy.port
OP="$(head -1 origin.port)"; PP="$(head -1 proxy.port)"
if [ -z "${OP}" ] || [ -z "${PP}" ]; then
    echo "  FAIL: the servers did not start: $(head -c 200 origin.err proxy.err)"; echo "C1865: 0 passed, 1 failed"; exit 1
fi

cat > lit.tk <<'TOKE'
m=lit;
i=http:std.http;
i=io:std.io;
i=args:std.args;
f=get(c:$httpclient):$str{
  <mt http.clientget(c;"/x") {$ok:r "ok \(r.status) len=\(r.body.len)"; $err:e "err \(e.msg)"}
};
f=main():i64{
  let mode=(mt args.get(2){$ok:v v;$err:e ""});
  let lit=$httpclient{baseurl:(mt args.get(1){$ok:v v;$err:e ""});poolsize:4;timeoutms:5000};
  let out=if(mode=="get"){get(lit)}
    el{if(mode=="proxy"){get(http.withproxy(lit;(mt args.get(3){$ok:v v;$err:e ""})))}
    el{if(mode=="deadline"){get(http.withdeadline(lit;5000))}
    el{if(mode=="cap"){get(http.withmaxbody(lit;10))}
    el{if(mode=="addr"){get(http.withconnectaddr(lit;"127.0.0.1"))}
    el{if(mode=="stream"){mt http.stream(lit) {$ok:s "ok stream"; $err:e "err \(e.msg)"}}
    el{"download \(http.downloadfile(lit;"/x";"dl.out"))"}}}}}};
  io.println(out);
  <0
};
TOKE
if ! "${TKC}" --allow-all --out lit_bin lit.tk > build.log 2>&1; then
    echo "  FAIL: the program does not build"; sed 's/^/      /' build.log | head -6
    echo "C1865: 0 passed, 1 failed"; exit 1
fi

# runs DESC -- ARGS...: run lit_bin RUNS times; one line per distinct
# answer, "<count>x <answer>" (a crash is "rc=<n>").
runs() {
    local i out rc
    for i in $(seq 1 "${RUNS}"); do
        out="$(./lit_bin "$@" 2>/dev/null)"; rc=$?
        if [ "${rc}" -ne 0 ]; then echo "rc=${rc}"; else echo "${out}"; fi
    done | LC_ALL=C sort | uniq -c | awk '{c=$1; $1=""; sub(/^ /,""); printf "%sx %s|", c, $0}'
}
O="http://127.0.0.1:${OP}"

expect "1 clientget on a literal: the whole body, every run" \
    "$(runs "${O}" get)" "${RUNS}x ok 200 len=783|"
expect "2 a literal against a closed port: TCP connect failed, every run" \
    "$(runs "http://127.0.0.1:1" get)" "${RUNS}x err TCP connect failed|"
: > proxy.log
expect "3 http.withproxy(literal): through the proxy, every run" \
    "$(runs "${O}" proxy "http://127.0.0.1:${PP}") $(grep -a -c "^GET http://127.0.0.1:${OP}/x " proxy.log)" \
    "${RUNS}x ok 200 len=5| ${RUNS}"
expect "4 http.withdeadline(literal;5000): the whole body (no cap came with it)" \
    "$(runs "${O}" deadline)" "${RUNS}x ok 200 len=783|"
expect "5 http.withmaxbody(literal;10): the cap it was given" \
    "$(runs "${O}" cap)" "${RUNS}x err response body exceeds the cap (10 bytes): Content-Length is 783|"
: > origin.log
expect "6 http.withconnectaddr(literal;127.0.0.1): Host is the literal's svc.invalid" \
    "$(runs "http://svc.invalid:${OP}" addr) $(grep -a -c "^GET /x host=svc.invalid:${OP}$" origin.log)" \
    "${RUNS}x ok 200 len=783| ${RUNS}"
expect "7 http.stream(literal): not refused as carrying a control" \
    "$(runs "${O}" stream)" "${RUNS}x ok stream|"
rm -f dl.out
expect "8 http.downloadfile(literal): 0, and the file holds the body" \
    "$(runs "${O}" download) $(wc -c 2>/dev/null < dl.out | tr -d ' ')" "${RUNS}x download 0| 783"

echo ""
echo "C1865: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
