#!/usr/bin/env bash
# C1030_http_client_close_delimited_eof.sh — a client response body with
# neither Content-Length nor chunked encoding ends only at a CLOSE; a reset or
# a timeout before it is $err, not $ok (story 138.31).
#
# THE DEFECT.  src/stdlib/http.c client_do_request read a close-delimited body
# (RFC 9112 6.3) with `if (n <= 0) break;` and answered success with whatever
# had arrived, so a connection reset (or the per-read timeout) part-way
# through the body was answered $ok with a cut body.  The fix classifies why
# a read returned nothing (client_recv): recv()==0 / TLS close_notify / TLS
# close without close_notify end the body; a reset, timeout, socket error or
# TLS error is an $httperr (code 0) naming the cause.
#
# TLS WITHOUT close_notify.  OpenSSL 3.0+ (4.0.x included) reports a TCP close
# with no close_notify as SSL_ERROR_SSL / SSL_R_UNEXPECTED_EOF_WHILE_READING.
# It is deliberately treated as a close (many servers never send the alert;
# Python's ssl does the same by default); RFC 9112 9.8 would make it an error
# -- see docs/stdlib/http.md.  Cases 8 and 9 prove the client saw the two TLS
# closes differently: a short Content-Length body names which one it was.
#
# The timeout case is not here: toke has no way to shorten the 30 s client
# timeout, so it is covered in C by test/stdlib/test_http.c (make
# test-stdlib-http), which sets HttpClient.timeout_ms directly.
#
# A loopback server (ephemeral ports, plain and TLS with a throwaway
# self-signed certificate trusted through SSL_CERT_FILE, as C279 does).
# Every case is decided by what the server sends and how it closes; the
# reset cases wait 0.3 s first so the bytes before them are read.
#
# Negative control: C1030_STDLIB=<dir with a37cd26e's src/stdlib> -- see the
# commit for the measured result.
#
# Story: 138.31

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C1030_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: compiler not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1030_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1

HAVE_TLS=1
if ! command -v openssl >/dev/null 2>&1 || \
   ! openssl req -x509 -newkey rsa:2048 -keyout k.pem -out c.pem -days 2 -nodes \
       -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" > cert.log 2>&1 || \
   [ ! -s c.pem ]; then
    HAVE_TLS=0; echo "NOTE: no openssl(1) to make a test certificate; TLS cases skipped"
fi

cat > srv.py <<'PY'
import os, socket, ssl, struct, sys, threading, time
def read_req(c):
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = c.recv(4096)
        if not d: return None
        buf += d
    return buf.split(b" ")[1].decode()
NOLEN = b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
CL8 = b"HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n"
# path -> (parts, how): how = fin | reset | notify | nonotify
R = {
  "/close":    ([NOLEN + b"abcdefgh"], "fin"),
  "/pieces":   ([NOLEN + b"abc", b"def", b"gh"], "fin"),
  "/reset":    ([NOLEN + b"abc"], "reset"),
  "/notify":   ([NOLEN + b"abcdefgh"], "notify"),
  "/nonotify": ([NOLEN + b"abcdefgh"], "nonotify"),
  "/tlsreset": ([NOLEN + b"abc"], "reset"),
  "/shortnotify":   ([CL8 + b"abc"], "notify"),
  "/shortnonotify": ([CL8 + b"abc"], "nonotify"),
}
def raw_of(c):
    return socket.socket(fileno=os.dup(c.fileno()))
def serve(c, tls):
    try:
        if tls:
            c.settimeout(5)
            c = tls.wrap_socket(c, server_side=True)
        path = read_req(c)
        if path is None: return
        parts, how = R.get(path, ([], "fin"))
        for i, p in enumerate(parts):
            if i: time.sleep(0.05)
            c.sendall(p)
        if how == "reset":
            time.sleep(0.3)
            r = raw_of(c)
            r.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            r.close(); c.close()     # last fd: RST, no FIN, no alert
            return
        if how == "notify":
            try: c.unwrap()          # sends close_notify
            except (OSError, ValueError): pass
        if how in ("nonotify", "notify"):
            r = raw_of(c)            # TCP FIN with no TLS alert after it
            try: r.shutdown(socket.SHUT_RDWR)
            except OSError: pass
            r.close()
        try: c.shutdown(socket.SHUT_RDWR)
        except (OSError, ValueError): pass
        c.close()
    except Exception as e:
        sys.stderr.write("srv: %r\n" % (e,))
def listen():
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", 0)); s.listen(16); return s
def loop(s, tls):
    while True:
        c, _ = s.accept()
        threading.Thread(target=serve, args=(c, tls), daemon=True).start()
plain = listen()
tls = None; ts = None
if len(sys.argv) > 2:
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); tls.load_cert_chain(sys.argv[1], sys.argv[2])
    ts = listen()
print(plain.getsockname()[1], ts.getsockname()[1] if ts else 0, flush=True)
if ts: threading.Thread(target=loop, args=(ts, tls), daemon=True).start()
loop(plain, None)
PY
if [ "${HAVE_TLS}" = 1 ]; then python3 srv.py c.pem k.pem > srv.port 2> srv.err & SRV=$!
else python3 srv.py > srv.port 2> srv.err & SRV=$!; fi
for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
P="$(awk '{print $1}' srv.port 2>/dev/null)"; TP="$(awk '{print $2}' srv.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the loopback server did not start"; cat srv.err; echo "C1030: 0 passed, 1 failed"; exit 1; fi

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=str:std.str;
i=args:std.args;
f=gu(u:str):str{
  <mt http.geturl(u) { $ok:b "ok \(str.len(b) as i64) \(b)"; $err:e "err \(e.code) \(e.msg)" }
};
f=main():i64{
  io.println(gu(mt args.get(1){$ok:v v;$err:e ""}));
  <0
};
TOKE
if ! "${TKC}" p.tk --out b > build.log 2>&1; then
    echo "FAIL: the program does not build"; head -6 build.log; echo "C1030: ${PASS} passed, 1 failed"; exit 1
fi
get() { ./b "$1" --allow-net 2>&1 | head -1; }
# "err 0 <msg>" -> "err 0" plus whether msg holds $2
errhas() { case "$1" in "err 0 "*"$2"*) echo "err 0 +";; "err 0 "*) echo "err 0 -: ${1#err 0 }";; *) echo "$1";; esac; }

H="http://127.0.0.1:${P}"
expect "1-plain-close-delimited-clean-close-is-ok" "$(get "${H}/close")"  "ok 8 abcdefgh"
expect "2-plain-close-delimited-in-pieces-is-ok"   "$(get "${H}/pieces")" "ok 8 abcdefgh"
R3="$(get "${H}/reset")"
expect "3-plain-reset-mid-body-is-err"             "$(errhas "${R3}" "connection reset by peer")" "err 0 +"
expect "4-reset-msg-names-the-framing"             "$(errhas "${R3}" "no Content-Length")"         "err 0 +"

if [ "${HAVE_TLS}" = 1 ]; then
    export SSL_CERT_FILE="${WORK}/c.pem"
    T="https://127.0.0.1:${TP}"
    expect "5-tls-close-notify-then-close-is-ok"       "$(get "${T}/notify")"   "ok 8 abcdefgh"
    expect "6-tls-close-without-close-notify-is-ok"    "$(get "${T}/nonotify")" "ok 8 abcdefgh"
    expect "7-tls-reset-mid-body-is-err"               "$(errhas "$(get "${T}/tlsreset")" "connection reset by peer")" "err 0 +"
    expect "8-tls-short-body-names-missing-close-notify" \
        "$(errhas "$(get "${T}/shortnonotify")" "connection closed (TLS, without close_notify)")" "err 0 +"
    R9="$(get "${T}/shortnotify")"
    case "${R9}" in *"without close_notify"*) R9="err 0 says-no-notify";; "err 0 "*"connection closed"*) R9="err 0 +";; esac
    expect "9-tls-short-body-after-close-notify-is-a-plain-close" "${R9}" "err 0 +"
fi

echo "C1030: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
