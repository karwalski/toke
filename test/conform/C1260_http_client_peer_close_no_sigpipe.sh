#!/usr/bin/env bash
# C1260_http_client_peer_close_no_sigpipe.sh — a server that closes while the
# client is still sending the request must give the client $err, never kill it
# with SIGPIPE (story 127.341).
#
# THE DEFECT.  127.144 made every SERVER write SIGPIPE-proof (SO_NOSIGPIPE on
# accepted sockets, MSG_NOSIGNAL on send), but the CLIENT side of
# src/stdlib/http.c was left out: client_tcp_connect never set SO_NOSIGPIPE
# and every client send() passed flags 0.  A server that reads the request
# head and closes (a 413 without reading the body, a crash, a proxy dropping
# the connection) made the next send() fail with EPIPE AND raise SIGPIPE, and
# the toke program died with exit status 141 instead of seeing $err.  The fix
# calls http_sock_nosigpipe() on every client socket and passes
# HTTP_SEND_FLAGS (MSG_NOSIGNAL where it exists) on every client send().
#
# WHY IT IS DETERMINISTIC.  Nothing here waits on a clock.  The loopback
# server reads the request head and closes; the client is still sending an
# 8 MiB body, which cannot fit in the loopback buffers, so its body send()
# always meets the closed peer.  Whether the kernel reports that as EPIPE
# (which raises SIGPIPE) or ECONNRESET (which does not) varies from run to
# run, so the case runs RUNS times: the fixed client survives every run; the
# unfixed one died in 9 of 20 runs measured on Darwin, so RUNS=12 misses it
# with probability about 0.55^12, under 0.1%.  It can only pass by mistake,
# never fail by mistake.
#
# Negative control: C1260_STDLIB=<dir holding 031d72fd's src/stdlib> — see
# the commit for the measured result.
#
# Story: 127.341, 127.565

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C1260_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
RUNS="${C1260_RUNS:-12}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: compiler not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1260_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1

cat > srv.py <<'PY'
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    buf = b""
    try:
        while b"\r\n\r\n" not in buf:     # the request head only, never the body
            d = c.recv(4096)
            if not d: break
            buf += d
    except OSError:
        pass
    c.close()                             # the body is still on its way
PY
python3 srv.py > srv.port 2> srv.err & SRV=$!
tk_wait_until "${SRV}" tk_nonempty srv.port
P="$(cat srv.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the loopback server did not start"; cat srv.err; echo "C1260: 0 passed, 1 failed"; exit 1; fi

python3 -c "open('body.bin','wb').write(b'x' * (8 << 20))"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=file:std.file;
i=args:std.args;
f=main():i64{
  let body=mt file.readbytes(mt args.get(2){$ok:v v;$err:e ""}) {$ok:b b; $err:e @()};
  let c=http.client(mt args.get(1){$ok:v v;$err:e ""});
  let r=mt http.clientpost(c;"/up";body;"") {$ok:x "ok \(x.status)"; $err:e "err \(e.code)"};
  io.println(r);
  <0
};
TOKE
if ! "${TKC}" p.tk --out b > build.log 2>&1; then
    echo "FAIL: the program does not build"; head -6 build.log; echo "C1260: ${PASS} passed, 1 failed"; exit 1
fi

# Run with SIGPIPE at its default disposition whatever the caller's shell has.
killed=0; errs=0; other=""
for _ in $(seq 1 "${RUNS}"); do
    out="$(trap - PIPE; ./b "http://127.0.0.1:${P}" body.bin --allow-net --allow-read 2>&1)"; rc=$?
    if [ "${rc}" -ge 128 ]; then killed=$((killed + 1))
    elif [ "${rc}" -eq 0 ] && [ "${out}" = "err 0" ]; then errs=$((errs + 1))
    else other="rc=${rc} out=${out}"; fi
done
expect "1-peer-close-mid-request-never-kills-the-client" "killed ${killed} of ${RUNS}" "killed 0 of ${RUNS}"
expect "2-peer-close-mid-request-is-err-code-0"          "${errs}${other:+ (${other})}" "$((RUNS - killed))"

echo "C1260: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
