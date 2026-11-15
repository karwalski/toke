#!/usr/bin/env bash
# C1861_http_client_max_body.sh — http.withmaxbody: a response body over the
# cap is $err and the read stops there; a body at or under it is $ok; a
# client without it reads as before (story 138.24, owner answer 2.1 (a);
# loke T-63 MK19.11).
#
# THE GAP.  client_do_request read every body whole into memory (realloc
# doubling, no ceiling): a 64 MiB body answered $ok in 52 ms (loke, measured
# on v3.2.0), so a client fetching an untrusted URL could be made to hold
# whatever the server sent.
#
# NOW: http.withmaxbody(c;bytes).  A Content-Length over the cap is refused
# before the body is read; a chunked body is refused on the size line that
# would take it over; a close-delimited body when it passes the cap.  The
# connection is then closed with the rest unread.  The $err msg begins
# "response body exceeds the cap (<bytes> bytes)".  Never a truncated body
# answered $ok.
#
# A loopback server (127.0.0.1, a port in 48110-48119) answers by path and
# logs how many body bytes it managed to send:
#   1. CL 100, cap 100: $ok 100            2. CL 100, cap 99: $err (Content-Length is 100)
#   3. CL 64 MiB, cap 1 MiB: $err at once, and the server could not send it all
#   4. no length, 64 MiB then close, cap 1 MiB: $err, server could not send it all
#   5. chunked 100 (10 x 10), cap 100: $ok 100
#   6. chunked 100, cap 99: $err (a chunked body of more than 99 bytes)
#   7. a chunk size line of 0x7fffffff, cap 1000: $err on the size line, at once
#   8. a 20000-byte chunk extension, cap 100: $err (framing)
#   9. no length, 100 bytes then close, cap 100: $ok 100; cap 99: $err
#  10. 204 with cap 0: $ok (no body); CL 0 with cap 0: $ok 0
#  11. no cap, CL 8 MiB: $ok 8388608 (the default reads the whole body)
#
# Negative control: C1861_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15: against v3.2.0's src/stdlib the client does not link
# (_tk_http_withmaxbody_w undefined).  With the cap never applied in
# client_do_request: 6 passed, 7 failed -- 2, 3, 4, 6 and 9b answer $ok with
# the whole body (3 and 4 read all 67108864 bytes), 7 and 8 wait 5 s for the
# server to close and answer "incomplete response".
#
# Story: 138.24
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1861_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the loopback server"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1861_XXXXXX")"
SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1861: http.withmaxbody stops an over-cap body (138.24)"
echo "---------------------------------------------------------"

cat > srv.py <<'PY'
import socket, sys, threading, time
s = None
for p in range(48110, 48120):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", p)); s.listen(16); break
    except OSError:
        s.close(); s = None
if s is None: raise SystemExit("no free port in 48110..48119")
open(sys.argv[1], "w").write(str(p))
LOG = sys.argv[2]
MIB = 1 << 20
def head(extra): return b"HTTP/1.1 200 OK\r\n" + extra + b"Connection: close\r\n\r\n"
def stream(c, total):
    sent, block = 0, b"x" * 65536
    try:
        while sent < total:
            c.sendall(block[: min(65536, total - sent)]); sent += min(65536, total - sent)
    except OSError:
        pass
    return sent
def serve(c):
    sent = -1
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d: return
            buf += d
        path = buf.split(b" ")[1].decode()
        if path == "/cl100":    c.sendall(head(b"Content-Length: 100\r\n") + b"a" * 100)
        elif path == "/cl0":    c.sendall(head(b"Content-Length: 0\r\n"))
        elif path == "/no204":  c.sendall(b"HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n")
        elif path == "/big":
            c.sendall(head(b"Content-Length: %d\r\n" % (64 * MIB))); sent = stream(c, 64 * MIB)
        elif path == "/bigclose":
            c.sendall(head(b"")); sent = stream(c, 64 * MIB)
        elif path == "/big8":
            c.sendall(head(b"Content-Length: %d\r\n" % (8 * MIB))); sent = stream(c, 8 * MIB)
        elif path == "/ch100":
            c.sendall(head(b"Transfer-Encoding: chunked\r\n"))
            for _ in range(10): c.sendall(b"a\r\n" + b"b" * 10 + b"\r\n")
            c.sendall(b"0\r\n\r\n")
        elif path == "/chhuge":
            c.sendall(head(b"Transfer-Encoding: chunked\r\n") + b"7fffffff\r\nxyz"); time.sleep(5)
        elif path == "/chext":
            c.sendall(head(b"Transfer-Encoding: chunked\r\n") + b"1;" + b"e" * 20000); time.sleep(5)
        elif path == "/close100": c.sendall(head(b"") + b"c" * 100)
        with open(LOG, "a") as f: f.write("%s %d\n" % (path, sent))
    except OSError:
        pass
    finally:
        try: c.close()
        except OSError: pass
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
python3 srv.py port srv.log 2> srv.err & SRV=$!
for _ in $(seq 1 50); do [ -s port ] && break; sleep 0.1; done
P="$(cat port 2>/dev/null)"
if [ -z "${P}" ]; then fail "the loopback server did not start: $(head -c 200 srv.err)"; echo "C1861: ${PASS} passed, ${FAIL} failed"; exit 1; fi

# cl URL PATH CAP|- : one GET, prints the result and the elapsed ms.
cat > cl.tk <<'TOKE'
m=cl;
i=http:std.http;
i=io:std.io;
i=args:std.args;
i=str:std.str;
i=time:std.time;
f=num(s:$str):u64{
  <mt str.toint(s) {$ok:v v as u64; $err:e 0 as u64}
};
f=main():i64{
  let mb=args.get(3);
  let c0=http.client(args.get(1));
  let c1=if(mb=="-"){c0}el{http.withmaxbody(c0;num(mb))};
  let t0=time.nowms();
  let out=mt http.clientget(c1;args.get(2)) {$ok:r "ok \(r.status) \(r.body.len)"; $err:e "err \(e.msg)"};
  io.println(out);
  io.println("\(time.nowms()-t0)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out cl_bin cl.tk > cl.build 2>&1; then
    fail "cl.tk does not build: $(grep -v W1001 cl.build | head -c 400)"
    echo ""; echo "C1861: ${PASS} passed, ${FAIL} failed"; exit 1
fi
U="http://127.0.0.1:${P}"
run() { ./cl_bin "$@" > out 2>&1; R="$(sed -n 1p out)"; MS="$(sed -n 2p out)"; }
expect() { if [ "${R}" = "$2" ]; then pass "$1: '${R}'"; else fail "$1: got '${R}', want '$2'"; fi; }
sent() { for _ in $(seq 1 50); do grep -q "^$1 " srv.log 2>/dev/null && break; sleep 0.1; done
         grep "^$1 " srv.log | tail -1 | cut -d' ' -f2; }

run "${U}" /cl100 100; expect "1 CL 100, cap 100" "ok 200 100"
run "${U}" /cl100 99;  expect "2 CL 100, cap 99" "err response body exceeds the cap (99 bytes): Content-Length is 100"

run "${U}" /big 1048576
S="$(sent /big)"
if [ "${R}" = "err response body exceeds the cap (1048576 bytes): Content-Length is 67108864" ] \
   && [ -n "${S}" ] && [ "${S}" -lt 67108864 ] && [ "${MS:-9999}" -lt 3000 ]; then
    pass "3 CL 64 MiB, cap 1 MiB: '${R}' in ${MS} ms; the server sent ${S} of 67108864 bytes"
else fail "3 /big: '${R}' (${MS} ms), server sent '${S}'"; fi

run "${U}" /bigclose 1048576
S="$(sent /bigclose)"
if [ "${R}" = "err response body exceeds the cap (1048576 bytes): more than 1048576 bytes before the connection closed" ] \
   && [ -n "${S}" ] && [ "${S}" -lt 67108864 ]; then
    pass "4 64 MiB to close, cap 1 MiB: \$err; the server sent ${S} of 67108864 bytes"
else fail "4 /bigclose: '${R}', server sent '${S}'"; fi

run "${U}" /ch100 100; expect "5 chunked 100, cap 100" "ok 200 100"
run "${U}" /ch100 99;  expect "6 chunked 100, cap 99" "err response body exceeds the cap (99 bytes): a chunked body of more than 99 bytes"

run "${U}" /chhuge 1000
if [ "${R}" = "err response body exceeds the cap (1000 bytes): a chunked body of more than 1000 bytes" ] && [ "${MS:-9999}" -lt 3000 ]; then
    pass "7 a 0x7fffffff chunk, cap 1000: refused on its size line (${MS} ms)"
else fail "7 /chhuge: '${R}' (${MS} ms)"; fi

run "${U}" /chext 100
if [ "${R}" = "err response body exceeds the cap (100 bytes): a chunk size line or trailer of more than 8192 bytes" ] && [ "${MS:-9999}" -lt 3000 ]; then
    pass "8 a 20000-byte chunk extension, cap 100: \$err (${MS} ms)"
else fail "8 /chext: '${R}' (${MS} ms)"; fi

run "${U}" /close100 100; expect "9a no length, 100 bytes, cap 100" "ok 200 100"
run "${U}" /close100 99;  expect "9b no length, 100 bytes, cap 99" "err response body exceeds the cap (99 bytes): more than 99 bytes before the connection closed"

run "${U}" /no204 0; expect "10a 204, cap 0" "ok 204 0"
run "${U}" /cl0 0;   expect "10b CL 0, cap 0" "ok 200 0"

run "${U}" /big8 -;  expect "11 no cap, CL 8 MiB: read whole, as before" "ok 200 8388608"

echo ""
echo "C1861: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
