#!/usr/bin/env bash
# C2061_http_client_header_cap.sh — under http.withmaxbody the response
# header block is capped too (story 138.49), so a hostile server cannot grow
# a capped client's memory with an endless header block.
#
# THE GAP.  client_do_request (src/stdlib/http.c, phase 1) read the header
# block until its blank line with realloc doubling and no ceiling, with or
# without http.withmaxbody: the 138.24 cap was on the body only.  A server
# that never sends the blank line held the client's memory growing at its
# send rate (only http.withdeadline bounded it, and only in time).
#
# NOW (the row's option (b), the 138.24 amendment's opt-in convention): a
# client carrying http.withmaxbody also refuses a header block (status line,
# headers and the blank line) longer than 65536 bytes, with $err "response
# header block exceeds the cap (65536 bytes): ..." and the connection
# closed.  A client without withmaxbody reads as before.
#
# A loopback server (127.0.0.1, a port in 48237-48239) answers by path:
#   1. an endless header block (64 MiB of header lines, no blank line),
#      cap 1000: $err at once; the server could not send it all
#   2. a 60000-byte header block + 10-byte body, cap 100: $ok 200 10
#   3. a header block of exactly 65536 bytes, cap 100: $ok 200 10
#   4. a header block of 65537 bytes, cap 100: $err (the header cap)
#   5. no cap, a 1 MiB header block: $ok 200 10 (unchanged)
#
# Negative control: TKC=<base ee27591c tree>/toke (measured 2026-11-23) --
# 1 reads all 67108881 bytes into memory and answers "incomplete response:
# connection closed before the header block ended" only when the server
# closes; 4 answers $ok 200 10.  2, 3 and 5 pass on both.
#
# Story: 138.49
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the loopback server"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2061_XXXXXX")"
SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C2061: http.withmaxbody caps the response header block (138.49)"
echo "----------------------------------------------------------------"

cat > srv.py <<'PY'
import socket, sys, threading
s = None
for p in range(48237, 48240):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind(("127.0.0.1", p)); s.listen(16); break
    except OSError:
        s.close(); s = None
if s is None: raise SystemExit("no free port in 48237..48239")
open(sys.argv[1], "w").write(str(p))
LOG = sys.argv[2]
def block(total):
    """a header block of exactly `total` bytes, blank line included"""
    a = b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\nConnection: close\r\n"
    pad = total - len(a) - len(b"X-Pad: \r\n\r\n")
    return a + b"X-Pad: " + b"p" * pad + b"\r\n\r\n"
def serve(c):
    sent = -1
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d: return
            buf += d
        path = buf.split(b" ")[1].decode()
        if path == "/endless":
            c.sendall(b"HTTP/1.1 200 OK\r\n"); sent = 17
            line = b"X-Junk: " + b"j" * 1014 + b"\r\n"
            try:
                while sent < (64 << 20):
                    c.sendall(line); sent += len(line)
            except OSError:
                pass
        elif path.startswith("/hdr"):
            c.sendall(block(int(path[4:])) + b"0123456789")
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
if [ -z "${P}" ]; then fail "the loopback server did not start: $(head -c 200 srv.err)"; echo "C2061: ${PASS} passed, ${FAIL} failed"; exit 1; fi

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
  let mb=(mt args.get(3){$ok:v v;$err:e ""});
  let c0=http.client((mt args.get(1){$ok:v v;$err:e ""}));
  let c1=if(mb=="-"){c0}el{http.withmaxbody(c0;num(mb))};
  let t0=time.nowms();
  let out=mt http.clientget(c1;(mt args.get(2){$ok:v v;$err:e ""})) {$ok:r "ok \(r.status) \(r.body.len)"; $err:e "err \(e.msg)"};
  io.println(out);
  io.println("\(time.nowms()-t0)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out cl_bin cl.tk > cl.build 2>&1; then
    fail "cl.tk does not build: $(grep -v W1001 cl.build | head -c 400)"
    echo ""; echo "C2061: ${PASS} passed, ${FAIL} failed"; exit 1
fi
U="http://127.0.0.1:${P}"
run() { perl -e 'alarm 30; exec @ARGV' ./cl_bin "$@" > out 2>&1; R="$(sed -n 1p out)"; MS="$(sed -n 2p out)"; }
expect() { if [ "${R}" = "$2" ]; then pass "$1: '${R}'"; else fail "$1: got '${R}', want '$2'"; fi; }
sent() { for _ in $(seq 1 50); do grep -q "^$1 " srv.log 2>/dev/null && break; sleep 0.1; done
         grep "^$1 " srv.log | tail -1 | cut -d' ' -f2; }
CAPMSG="err response header block exceeds the cap (65536 bytes)"

run "${U}" /endless 1000
S="$(sent /endless)"
if [ "${R}" = "${CAPMSG}: no blank line yet" ] && [ -n "${S}" ] && [ "${S}" -lt 67108864 ] && [ "${MS:-9999}" -lt 3000 ]; then
    pass "1 endless header block, cap 1000: \$err in ${MS} ms; the server sent ${S} of 67108864 bytes"
else fail "1 /endless: '${R}' (${MS} ms), server sent '${S}'"; fi

run "${U}" /hdr60000 100; expect "2 60000-byte header block, cap 100" "ok 200 10"
run "${U}" /hdr65536 100; expect "3 header block of exactly 65536 bytes, cap 100" "ok 200 10"
run "${U}" /hdr65537 100
case "${R}" in
    "${CAPMSG}: the blank line is past it"|"${CAPMSG}: no blank line yet") pass "4 header block of 65537 bytes, cap 100: '${R}'" ;;
    *) fail "4 /hdr65537: got '${R}', want the header cap \$err" ;;
esac
run "${U}" /hdr1048576 -; expect "5 no cap, a 1 MiB header block: read as before" "ok 200 10"

echo ""
echo "C2061: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
