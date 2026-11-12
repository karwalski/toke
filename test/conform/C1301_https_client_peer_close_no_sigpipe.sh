#!/usr/bin/env bash
# C1301_https_client_peer_close_no_sigpipe.sh — TLS records leave through
# send(MSG_NOSIGNAL), so an https client whose server has closed gets $err,
# never SIGPIPE, on every platform (story 127.346).
#
# THE DEFECT.  127.341 guarded the plain-HTTP client (SO_NOSIGPIPE on the
# socket, MSG_NOSIGNAL on each send).  The https client gave its socket to
# OpenSSL with SSL_set_fd, and OpenSSL's socket BIO writes with write(2): on
# Darwin SO_NOSIGPIPE still covers that, on Linux nothing does, so a server
# closing mid-request killed the program with status 141.  The fix attaches
# the socket with tk_ssl_set_fd_nosigpipe() (src/stdlib/tk_ssl_nosigpipe.h):
# OpenSSL's socket BIO for reading, a write BIO that calls
# send(fd, ..., MSG_NOSIGNAL).
#
# PART A — the mechanism, on any platform.  A C probe takes a socketpair,
# closes the peer, and runs SSL_connect on the survivor with SO_NOSIGPIPE NOT
# set, which is the Linux situation.  The ClientHello write always meets the
# closed peer (AF_UNIX: EPIPE, every time — no race, no clock).
#   bio   tk_ssl_set_fd_nosigpipe: the handshake fails and the probe lives.
#   plain SSL_set_fd (the old code): the probe is killed by SIGPIPE.  This
#         is the negative control, run in-test, so a probe that cannot
#         observe SIGPIPE at all cannot pass.
#
# PART B — the toke https client end to end, as C1260 does for http: a TLS
# server reads the request head and closes while an 8 MiB body is still being
# sent; RUNS runs, none killed, each $err code 0; and a small POST through
# the same client still gets 200, so the write BIO carries real traffic.  On
# macOS this part also passed before the fix (SO_NOSIGPIPE); it is the check
# that macOS behaviour is unchanged.  The Linux failure it would catch needs
# a Linux runner (127.346 row).
#
# Story: 127.346

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
RUNS="${C1301_RUNS:-12}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: compiler not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v openssl >/dev/null 2>&1; then
    echo "CONFORM-SKIP: no openssl(1) on this host to make test certificates"; exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1301_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1

# ── Part A ───────────────────────────────────────────────────────────────
OSSL_PREFIX="${TK_OPENSSL_PREFIX:-}"
if [ -z "${OSSL_PREFIX}" ]; then
    for d in /opt/homebrew /opt/homebrew/opt/openssl@3 /usr/local/opt/openssl@3 /usr/local /usr; do
        [ -f "${d}/include/openssl/ssl.h" ] && { OSSL_PREFIX="${d}"; break; }
    done
fi
cat > probe.c <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <openssl/ssl.h>
#include "tk_ssl_nosigpipe.h"
int main(int argc, char **argv)
{
    int sv[2];
    int bio = argc > 1 && strcmp(argv[1], "bio") == 0;
    signal(SIGPIPE, SIG_DFL);
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
    close(sv[1]);                        /* the peer is gone before any write */
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx ? SSL_new(ctx) : NULL;
    if (!ssl) return 3;
    if ((bio ? tk_ssl_set_fd_nosigpipe(ssl, sv[0]) : SSL_set_fd(ssl, sv[0])) != 1) return 4;
    int r = SSL_connect(ssl);            /* writes the ClientHello */
    printf("%s\n", r == 1 ? "connected" : "err");
    SSL_free(ssl); SSL_CTX_free(ctx); close(sv[0]);
    return 0;
}
C
if [ -z "${OSSL_PREFIX}" ] || ! cc -std=c99 -D_GNU_SOURCE -Wall -Wextra -Werror -Wno-deprecated-declarations \
        -I"${OSSL_PREFIX}/include" -I"${REPO_ROOT}/src/stdlib" probe.c -o probe \
        -L"${OSSL_PREFIX}/lib" -lssl -lcrypto -lpthread > probe.log 2>&1; then
    echo "FAIL: the probe does not build against OpenSSL (prefix '${OSSL_PREFIX}'; set TK_OPENSSL_PREFIX)"
    head -8 probe.log; FAIL=$((FAIL + 1))
else
    out="$(trap - PIPE; DYLD_LIBRARY_PATH="${OSSL_PREFIX}/lib" ./probe plain 2>&1)"; rc=$?
    expect "A1-control-SSL_set_fd-on-a-closed-peer-is-killed-by-SIGPIPE" "rc=${rc}" "rc=141"
    out="$(trap - PIPE; DYLD_LIBRARY_PATH="${OSSL_PREFIX}/lib" ./probe bio 2>&1)"; rc=$?
    expect "A2-tk_ssl_set_fd_nosigpipe-on-a-closed-peer-is-an-error" "rc=${rc} ${out}" "rc=0 err"
fi

# ── Part B ───────────────────────────────────────────────────────────────
openssl req -x509 -newkey rsa:2048 -keyout k.pem -out c.pem -days 2 -nodes \
    -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1,DNS:localhost" > cert.log 2>&1
if [ ! -s c.pem ]; then echo "FAIL: openssl could not make the test certificate"; head -4 cert.log
    echo "C1301: ${PASS} passed, $((FAIL + 1)) failed"; exit 1; fi

cat > srv.py <<'PY'
import socket, ssl, sys
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(sys.argv[1], sys.argv[2])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
while True:
    raw, _ = s.accept()
    try:
        c = ctx.wrap_socket(raw, server_side=True)
    except (OSError, ssl.SSLError):
        raw.close(); continue
    try:
        buf = b""
        while b"\r\n\r\n" not in buf:
            d = c.recv(4096)
            if not d: break
            buf += d
        head = buf.split(b"\r\n\r\n", 1)[0].decode("latin1")
        if head.startswith("POST /ok "):
            n = 0
            for line in head.split("\r\n"):
                if line.lower().startswith("content-length:"): n = int(line.split(":")[1])
            body = buf.split(b"\r\n\r\n", 1)[1]
            while len(body) < n:
                d = c.recv(4096)
                if not d: break
                body += d
            c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
    except (OSError, ssl.SSLError):
        pass
    try: c.close()                       # /up: the body is still on its way
    except OSError: pass                 # (wrap_socket took over raw's fd)
PY
python3 srv.py c.pem k.pem > srv.port 2> srv.err & SRV=$!
for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
P="$(cat srv.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the TLS server did not start"; cat srv.err; echo "C1301: ${PASS} passed, $((FAIL + 1)) failed"; exit 1; fi
python3 -c "open('body.bin','wb').write(b'x' * (8 << 20))"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=file:std.file;
i=args:std.args;
f=main():i64{
  let body=mt file.readbytes(args.get(3)) {$ok:b b; $err:e @()};
  let c=http.client(args.get(1));
  let r=mt http.clientpost(c;args.get(2);body;"") {$ok:x "ok \(x.status)"; $err:e "err \(e.code)"};
  io.println(r);
  <0
};
TOKE
if ! "${TKC}" p.tk --out b > build.log 2>&1; then
    echo "FAIL: the program does not build"; head -6 build.log; echo "C1301: ${PASS} passed, $((FAIL + 1)) failed"; exit 1
fi
printf 'hello' > small.bin
expect "B1-a-small-https-POST-through-the-write-BIO-gets-200" \
    "$(SSL_CERT_FILE="${WORK}/c.pem" ./b "https://127.0.0.1:${P}" /ok small.bin --allow-net --allow-read 2>&1)" "ok 200"

killed=0; errs=0; other=""
for _ in $(seq 1 "${RUNS}"); do
    out="$(trap - PIPE; SSL_CERT_FILE="${WORK}/c.pem" ./b "https://127.0.0.1:${P}" /up body.bin --allow-net --allow-read 2>&1)"; rc=$?
    if [ "${rc}" -ge 128 ]; then killed=$((killed + 1))
    elif [ "${rc}" -eq 0 ] && [ "${out}" = "err 0" ]; then errs=$((errs + 1))
    else other="rc=${rc} out=${out}"; fi
done
expect "B2-tls-peer-close-mid-request-never-kills-the-client" "killed ${killed} of ${RUNS}" "killed 0 of ${RUNS}"
expect "B3-tls-peer-close-mid-request-is-err-code-0"          "${errs}${other:+ (${other})}" "$((RUNS - killed))"

echo "C1301: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
