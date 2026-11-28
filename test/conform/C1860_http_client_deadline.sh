#!/usr/bin/env bash
# C1860_http_client_deadline.sh — http.withdeadline bounds the WHOLE request
# (connect, TLS handshake, response headers and body), and a client without
# it behaves as before (story 138.24, owner answer 2.1 (a); loke T-63 MK19.11).
#
# THE GAP.  The client's only clock was timeoutms, SO_RCVTIMEO set per socket
# (src/stdlib/http.c client_tcp_connect), which restarts on every byte: a
# server sending one body byte every 5 s held http.geturl for 40,383 ms and
# the call answered $ok (loke, measured on v3.2.0).  Nothing bounded a
# request as a whole.
#
# NOW: http.withdeadline(c;ms) returns a client whose requests must finish
# within ms of the call; the socket is non-blocking and every wait is a
# poll() bounded by the time left, so a drip inside the per-read timeout is
# cut off, as is a TLS handshake that never completes.  The $err msg begins
# "request deadline exceeded (<ms> ms)" and says where.
#
# A loopback server (127.0.0.1, a port in 48100-48109) answers by path:
#   /drip       Content-Length 6, one body byte every 0.4 s (2.4 s in all)
#   /driphead   the status line, one byte every 0.4 s
#   /dripchunk  a chunked body, one 1-byte chunk every 0.4 s
#   /dripclose  no length: one byte every 0.4 s, then close
#   /ok         Content-Length 2, at once
# and a second listener accepts TCP and never says anything (a TLS server
# that never answers the ClientHello).
#   1. no deadline, /drip: $ok 6 bytes after >= 2000 ms (the default is unchanged)
#   2. deadline 800, /drip: $err "request deadline exceeded (800 ms) after
#      N of 6 body bytes (Content-Length)" with N <= 2, after >= 600 ms
#   3. deadline 10000, /drip: $ok 6 (a deadline that is met cuts nothing)
#   4. deadline 800, /driphead: $err "... before the header block ended"
#   5. deadline 800, /dripchunk: $err "... before the last chunk of a chunked body"
#   6. deadline 800, /dripclose: $err "... of a body with no Content-Length"
#   7. deadline 800, https to the silent listener: $err "... during the TLS
#      handshake", after >= 600 ms and well inside the 30 s per-read timeout
#   8. deadline 0: $err "request deadline exceeded (0 ms) during connect",
#      and no request reached the server
#   9. withdeadline returns a copy: the client it was made from, used
#      afterwards on /drip, still answers $ok 6
#
# Negative control: C1860_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15: against v3.2.0's src/stdlib (ea7ddf22) the client does
# not link (_tk_http_withdeadline_w undefined): 0 passed, 1 failed.  With
# http_withdeadline made a no-op (the setting dropped): 2 passed, 7 failed --
# 2, 5, 6 answer $ok after the whole drip, 4 after 15.5 s, 7 "SSL handshake
# failed" after the 30 s per-read timeout, 8 reaches the server, 9 the copy
# reads the whole drip (re-measured 2026-11-28, 127.559: the same 2 / 7).
# Measured 2026-11-28 (127.559): with every deadline check 600 ms late
# (client_past and client_wait compare against at + 600), 2 fails -- "after 3
# of 6 body bytes" in 1401 ms, which the old 1900 ms bound passed -- and 8
# fails; with client_wait polling for the per-read timeout only, 7 fails
# ("SSL handshake failed" after 30019 ms).
#
# Timing (127.559).  2 and 7 used to require the answer within 1900 ms, an
# upper bound on a feature that is itself a deadline: under host load the
# client is only ever LATER, so a correct deadline failed and the bound proved
# nothing a load-free fact does not.  What they assert now:
#   2. the message, which the client writes only when its own clock passed the
#      deadline (client_past / client_wait in http.c), and N <= 2: byte k
#      leaves the server no sooner than 400*k ms after the request reached it,
#      which is after the deadline clock started, and the deadline is checked
#      before every read -- so bytes 3..6 can be read only by a client that
#      ran past its deadline.  Load slows the server too, which only lowers
#      N.  A floor of 600 ms keeps it from firing early (load cannot shorten
#      an elapsed time).
#   7. nothing arrives on the silent listener, so only a timer can end the
#      handshake; the message names the deadline, not the per-read timeout,
#      and MS < SEPARATE_MS (15000, half of the 30000 ms per-read timeout)
#      tells the deadline's timer from a wait that ignored it and ran to the
#      per-read timeout.  That bound is a separation of 14 s, not a margin for
#      scheduling, so it does not scale with TK_CONFORM_RUN_SECS.
# Every program run is under tk_run's hang guard (TK_CONFORM_RUN_SECS) and the
# builds under tk_build's (TK_CONFORM_BUILD_SECS).
#
# Story: 138.24, 127.559, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
export TKC_STDLIB_DIR="${C1860_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 runs the loopback servers"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1860_XXXXXX")"
SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1
echo "C1860: http.withdeadline bounds the whole request (138.24)"
echo "-----------------------------------------------------------"

cat > srv.py <<'PY'
import socket, sys, threading, time
STEP = 0.4
def bind(base):
    for p in range(base, base + 10):
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("127.0.0.1", p)); s.listen(16); return s, p
        except OSError:
            s.close()
    raise SystemExit("no free port in %d..%d" % (base, base + 9))
http_s, hp = bind(48100)
mute_s, mp = bind(hp + 1)
open(sys.argv[1], "w").write("%d %d" % (hp, mp))
LOG = sys.argv[2]
def drip(c, head, body):
    c.sendall(head)
    for b in body:
        time.sleep(STEP); c.sendall(bytes([b]))
def serve(c):
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d: return
            buf += d
        path = buf.split(b" ")[1].decode()
        with open(LOG, "a") as f: f.write(path + "\n")
        if path == "/drip":
            drip(c, b"HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n", b"abcdef")
        elif path == "/driphead":
            drip(c, b"", b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")
        elif path == "/dripchunk":
            c.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
            for b in b"abcdef":
                time.sleep(STEP); c.sendall(b"1\r\n" + bytes([b]) + b"\r\n")
            c.sendall(b"0\r\n\r\n")
        elif path == "/dripclose":
            drip(c, b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n", b"abcdef")
        else:
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
    except OSError:
        pass
    finally:
        try: c.close()
        except OSError: pass
def mute():
    held = []
    while True:
        c, _ = mute_s.accept(); held.append(c)      # accept, say nothing
threading.Thread(target=mute, daemon=True).start()
while True:
    c, _ = http_s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
python3 srv.py ports srv.log 2> srv.err & SRV=$!
tk_wait_until "${SRV}" tk_nonempty ports
read -r HP MP < ports 2>/dev/null || true
if [ -z "${HP:-}" ]; then fail "the loopback server did not start: $(head -c 200 srv.err)"; echo "C1860: ${PASS} passed, ${FAIL} failed"; exit 1; fi

# cl URL PATH DEADLINE|- : one GET, prints the result and the elapsed ms.
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
  let dl=(mt args.get(3){$ok:v v;$err:e ""});
  let c0=http.client((mt args.get(1){$ok:v v;$err:e ""}));
  let c1=if(dl=="-"){c0}el{http.withdeadline(c0;num(dl))};
  let t0=time.nowms();
  let out=mt http.clientget(c1;(mt args.get(2){$ok:v v;$err:e ""})) {$ok:r "ok \(r.status) \(r.body.len)"; $err:e "err \(e.msg)"};
  io.println(out);
  io.println("\(time.nowms()-t0)");
  <0
};
TOKE
# copy URL PATH : withdeadline(c;500) is made, then the ORIGINAL client is used.
cat > cp.tk <<'TOKE'
m=cp;
i=http:std.http;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let c0=http.client((mt args.get(1){$ok:v v;$err:e ""}));
  let c1=http.withdeadline(c0;500);
  io.println(mt http.clientget(c0;(mt args.get(2){$ok:v v;$err:e ""})) {$ok:r "ok \(r.status) \(r.body.len)"; $err:e "err \(e.msg)"});
  io.println(mt http.clientget(c1;(mt args.get(2){$ok:v v;$err:e ""})) {$ok:r "ok \(r.status) \(r.body.len)"; $err:e "err \(e.msg)"});
  <0
};
TOKE
for p in cl cp; do
    if ! tk_build "${TKC}" --allow-all --out "${p}_bin" "${p}.tk" > "${p}.build" 2>&1; then
        fail "${p}.tk does not build: $(grep -v W1001 "${p}.build" | head -c 400)"
        echo ""; echo "C1860: ${PASS} passed, ${FAIL} failed"; exit 1
    fi
done
U="http://127.0.0.1:${HP}"
run() { tk_run ./cl_bin "$@" > out 2>&1; R="$(sed -n 1p out)"; MS="$(sed -n 2p out)"; }
SEPARATE_MS=15000   # see "Timing": half the client's 30000 ms per-read timeout

run "${U}" /drip -
if [ "${R}" = "ok 200 6" ] && [ "${MS:-0}" -ge 2000 ]; then pass "1 no deadline: \$ok after the whole drip (${MS} ms)"
else fail "1 no deadline: '${R}' in ${MS} ms (want ok 200 6, >= 2000 ms)"; fi

run "${U}" /drip 800
case "${R}" in "err request deadline exceeded (800 ms) after "*" of 6 body bytes (Content-Length)")
    N="${R#err request deadline exceeded (800 ms) after }"; N="${N%% *}"
    if [ "${MS:-0}" -ge 600 ] && [ "${N}" -le 2 ] 2>/dev/null; then pass "2 deadline 800 cuts the drip: '${R}' (${MS} ms)"
    else fail "2 deadline 800: right message, ${N} body bytes read (want <= 2), ${MS} ms (want >= 600)"; fi ;;
*) fail "2 deadline 800, /drip: '${R}' (${MS} ms)" ;; esac

run "${U}" /drip 10000
if [ "${R}" = "ok 200 6" ]; then pass "3 a deadline that is met: \$ok 6 (${MS} ms)"; else fail "3 deadline 10000: '${R}'"; fi

run "${U}" /driphead 800
case "${R}" in "err request deadline exceeded (800 ms) before the header block ended ("*)
    pass "4 headers dripped: '${R}' (${MS} ms)" ;;
*) fail "4 /driphead: '${R}' (${MS} ms)" ;; esac

run "${U}" /dripchunk 800
case "${R}" in "err request deadline exceeded (800 ms) before the last chunk of a chunked body ("*)
    pass "5 chunked drip: '${R}' (${MS} ms)" ;;
*) fail "5 /dripchunk: '${R}' (${MS} ms)" ;; esac

run "${U}" /dripclose 800
case "${R}" in "err request deadline exceeded (800 ms) after "*" body bytes of a body with no Content-Length"*)
    pass "6 close-delimited drip: '${R}' (${MS} ms)" ;;
*) fail "6 /dripclose: '${R}' (${MS} ms)" ;; esac

run "https://127.0.0.1:${MP}" / 800
if [ "${R}" = "err request deadline exceeded (800 ms) during the TLS handshake" ] \
   && [ "${MS:-0}" -ge 600 ] && [ "${MS:-99999}" -lt "${SEPARATE_MS}" ]; then
    pass "7 a TLS handshake that never completes: '${R}' (${MS} ms)"
else fail "7 silent TLS peer: '${R}' (${MS} ms; want >= 600 and < ${SEPARATE_MS})"; fi

: > srv.log
run "${U}" /ok 0
if [ "${R}" = "err request deadline exceeded (0 ms) during connect" ] && [ ! -s srv.log ]; then
    pass "8 deadline 0: '${R}', nothing reached the server"
else fail "8 deadline 0: '${R}', server saw: $(tr '\n' ' ' < srv.log)"; fi

tk_run ./cp_bin "${U}" /drip > cpout 2>&1
if [ "$(sed -n 1p cpout)" = "ok 200 6" ]; then
    case "$(sed -n 2p cpout)" in "err request deadline exceeded (500 ms)"*)
        pass "9 withdeadline returns a copy: the original still reads the whole drip" ;;
    *) fail "9 the copy did not keep its deadline: $(sed -n 2p cpout)" ;; esac
else fail "9 the original client was changed: $(tr '\n' '|' < cpout)"; fi

echo ""
echo "C1860: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
