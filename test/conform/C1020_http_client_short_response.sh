#!/usr/bin/env bash
# C1020_http_client_short_response.sh — an HTTP client response that ends
# before its framing says it is complete is $err, not $ok (story 138.23,
# loke T-64 / MK19.11).
#
# THE DEFECT.  src/stdlib/http.c client_do_request read the response in three
# loops -- the header block, a Content-Length body, a chunked body -- and each
# did `if (n <= 0) break;` on a timeout or close and then answered success
# with what had arrived.  Measured: `http.geturl` returned $ok with 3 of the 8
# bytes a Content-Length promised.  The chunked loop also called the body
# complete as soon as the five bytes 0\r\n\r\n appeared anywhere in it, which
# chunk DATA can contain, so such a body was cut short and answered $ok.
#
# A loopback server (ephemeral port) answers by path; it closes the connection
# where a response is cut, so every case is deterministic (no timers decide
# anything).  The per-read timeout takes the same path as a close in the
# client, but costs 30 s, so it is not exercised here.
#
# Negative control: this compiler with main c26cbc1b's src/stdlib/http.c
# (TKC_STDLIB_DIR) -- 4 passed, 10 failed: cases 1-8 red (`ok 3 abc`, `ok 200`,
# `ok`, header cut answered `ok 0`), 10 red (the body was cut where its chunk
# data held 0\r\n\r\n: `ok 0`), 13 red (`ok 11 abcdefghXYZ`); 9, 11, 12, 14
# green on both (11 only because main read the missing status as 0).
#
# Story: 138.23

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: compiler not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1020_XXXXXX")"; SRV=""
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}" || exit 1

cat > srv.py <<'PY'
import socket, threading, time
def read_req(c):
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = c.recv(4096)
        if not d: return None
        buf += d
    head, rest = buf.split(b"\r\n\r\n", 1)
    n = 0
    for ln in head.split(b"\r\n")[1:]:
        k, _, v = ln.partition(b":")
        if k.strip().lower() == b"content-length": n = int(v)
    while len(rest) < n:
        d = c.recv(4096)
        if not d: break
        rest += d
    return head.split(b" ")[1].decode()
def hd(extra): return b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n" + extra + b"Connection: close\r\n\r\n"
CHUNKED = b"Transfer-Encoding: chunked\r\n"
R = {
  "/short":   [hd(b"Content-Length: 8\r\n") + b"abc"],
  "/chtrunc": [hd(CHUNKED) + b"3\r\nabc\r\n"],
  "/chmid":   [hd(CHUNKED) + b"8\r\nabc"],
  "/hdrcut":  [b"HTTP/1.1 200 OK\r\nContent-Le"],
  "/empty":   [],
  "/full":    [hd(b"Content-Length: 8\r\n") + b"abcdefgh"],
  "/extra":   [hd(b"Content-Length: 8\r\n") + b"abcdefghXYZ"],
  # chunk data that contains 0\r\n\r\n, a chunk extension, a trailer
  "/chdata":  [hd(CHUNKED) + b"8;ext=1\r\nx0\r\n\r\nyz\r\n3\r\nend\r\n0\r\nX-T: 1\r\n\r\n"],
  # the same body sent in pieces, split inside a size line, data and CRLF
  "/chsplit": [hd(CHUNKED) + b"4", b"\r\nab", b"cd\r", b"\n2\r\nef\r\n0\r", b"\n\r\n"],
  "/no204":   [b"HTTP/1.1 204 No Content\r\nContent-Length: 5\r\nConnection: close\r\n\r\n"],
}
def serve(c):
    try:
        path = read_req(c)
        if path is None: return
        for i, part in enumerate(R.get(path, [])):
            if i: time.sleep(0.05)
            c.sendall(part)
    finally:
        try: c.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        c.close()
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    threading.Thread(target=serve, args=(c,), daemon=True).start()
PY
python3 srv.py > srv.port 2> srv.err & SRV=$!
for _ in $(seq 1 50); do [ -s srv.port ] && break; sleep 0.1; done
P="$(head -1 srv.port 2>/dev/null)"
if [ -z "${P}" ]; then echo "FAIL: the loopback server did not start"; echo "C1020: 0 passed, 1 failed"; exit 1; fi

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=http:std.http;
i=str:std.str;
i=args:std.args;
f=gu(u:str):str{
  <mt http.geturl(u) { $ok:b "ok \(str.len(b) as i64) \(b)"; $err:e "err \(e.code)" }
};
f=main():i64{
  let base=mt args.get(1){$ok:v v;$err:e ""};
  let c=http.client(base);
  io.println(str.concat("1 ";gu(str.concat(base;"/short"))));
  let r2=mt http.clientget(c;"/short") { $ok:r "ok \(r.status)"; $err:e "err \(e.code)" };
  io.println(str.concat("2 ";r2));
  let r3=mt http.clientpost(c;"/short";str.bytes("x");"text/plain") { $ok:r "ok"; $err:e "err \(e.code)" };
  io.println(str.concat("3 ";r3));
  let r4=mt http.postheaders(str.concat(base;"/short");"{}";@(@("x-k";"1"))) { $ok:r "ok"; $err:e "err \(e.code)" };
  io.println(str.concat("4 ";r4));
  let r5=mt http.geturl(str.concat(base;"/short")) { $ok:b false; $err:e str.len(e.msg)>0 };
  io.println("5 \(r5)");
  io.println(str.concat("6 ";gu(str.concat(base;"/chtrunc"))));
  io.println(str.concat("7 ";gu(str.concat(base;"/chmid"))));
  io.println(str.concat("8 ";gu(str.concat(base;"/hdrcut"))));
  io.println(str.concat("9 ";gu(str.concat(base;"/full"))));
  let r10=mt http.geturl(str.concat(base;"/chdata")) { $ok:b "ok \(str.len(b) as i64) \(str.startswith(b;"x0")) \(str.endswith(b;"yzend"))"; $err:e "err \(e.code)" };
  io.println(str.concat("10 ";r10));
  io.println(str.concat("11 ";gu(str.concat(base;"/empty"))));
  io.println(str.concat("12 ";gu(str.concat(base;"/chsplit"))));
  io.println(str.concat("13 ";gu(str.concat(base;"/extra"))));
  let r14=mt http.clientget(c;"/no204") { $ok:r "ok \(r.status)"; $err:e "err \(e.code)" };
  io.println(str.concat("14 ";r14));
  <0
};
TOKE
if ! "${TKC}" p.tk --out b > build.log 2>&1; then
    echo "FAIL: the program does not build"; head -6 build.log; echo "C1020: ${PASS} passed, 1 failed"; exit 1
fi
./b "http://127.0.0.1:${P}" --allow-net > out.txt 2>&1
line() { awk -v k="$1" '$1==k{ $1=""; sub(/^ /,""); print; exit }' out.txt; }

expect "1-geturl-short-content-length-is-err"     "$(line 1)"  "err 0"
expect "2-clientget-short-content-length-is-err"  "$(line 2)"  "err 0"
expect "3-clientpost-short-content-length-is-err" "$(line 3)"  "err 0"
expect "4-postheaders-short-content-length-is-err" "$(line 4)" "err 0"
expect "5-short-body-err-has-a-message"           "$(line 5)"  "true"
expect "6-chunked-missing-last-chunk-is-err"      "$(line 6)"  "err 0"
expect "7-chunked-cut-inside-chunk-data-is-err"   "$(line 7)"  "err 0"
expect "8-incomplete-header-block-is-err"         "$(line 8)"  "err 0"
expect "9-full-content-length-body-is-ok"         "$(line 9)"  "ok 8 abcdefgh"
expect "10-chunk-data-holding-0crlfcrlf-is-whole" "$(line 10)" "ok 11 true true"
expect "11-no-response-at-all-is-err"             "$(line 11)" "err 0"
expect "12-chunked-in-split-pieces-is-ok"         "$(line 12)" "ok 6 abcdef"
expect "13-bytes-past-content-length-dropped"     "$(line 13)" "ok 8 abcdefgh"
expect "14-204-owes-no-body"                      "$(line 14)" "ok 204"

echo "C1020: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
