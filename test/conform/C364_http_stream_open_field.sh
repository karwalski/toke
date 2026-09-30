#!/usr/bin/env bash
# C364_http_stream_open_field.sh — `s.open` on an $httpstream reads the
# stream's open flag (story 138.18, found by its layout audit).
#
# THE DEFECT.  http.tki declares httpstream {id:u64; open:bool}: toke reads
# s.open as the whole 8-byte slot 1.  The C HttpStream the glue returns had
# `int open; int fd;` sharing that slot, so s.open read open | fd << 32.  When
# the peer closes, http.c sets open = 0 and fd = -1, and s.open read TRUE: a
# program looping `while s.open` never saw the end of the stream.
#
# THE FIX (src/stdlib/http.h).  `open` is an int64_t, a whole slot; the
# struct is under make check-tki-layout (tki-record-prefix, C361 case 6b).
#
# WHAT IS ASSERTED (a local one-shot HTTP server on 127.0.0.1, python3):
#   1. before reading: open=true; after the server has closed and a read has
#      returned the end: open=false
#   2. NEGATIVE CONTROL -- the first read got the body (11 bytes), so the
#      stream was really open and really read, not refused at the start
#
# Negative control of the suite: C364_STDLIB=<dir> builds against another
# stdlib tree.  Against main 73f7466 case 1 fails ("after: open=true").
#
# Story: 138.18
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C364_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v python3 >/dev/null 2>&1; then echo "SKIP: python3 is needed for the local server"; echo "Results: 0 passed, 0 failed"; exit 0; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c364_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
echo "C364: \$httpstream.open reads the open flag (138.18)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

cat > sn.tk <<'TK'
m=sn;
i=http:std.http;
i=io:std.io;
i=os:std.os;
f=main():i64{
  let s=mt http.stream(http.client(os.getenv("URL"))) {$ok:v v;$err:e 0};
  io.println("before open=\(s.open)");
  let a=mt http.streamnext(s) {$ok:v v;$err:e @()};
  let b=mt http.streamnext(s) {$ok:v v;$err:e @()};
  io.println("read \(a.len) then \(b.len)");
  io.println("after open=\(s.open)");
  <0
};
TK
# one connection: read the request, answer, close
cat > serve.py <<'PY'
import os, socket, subprocess, sys, threading
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(1); s.settimeout(10)
def serve():
    try:
        c, _ = s.accept(); c.settimeout(2)
        try: c.recv(4096)
        except OSError: pass
        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\nhello-chunk"); c.close()
    except OSError: pass
threading.Thread(target=serve, daemon=True).start()
env = dict(os.environ, URL="http://127.0.0.1:%d" % s.getsockname()[1])
try:
    r = subprocess.run(sys.argv[1:], env=env, capture_output=True, timeout=10)
    sys.stdout.write(r.stdout.decode("utf-8", "replace").replace("\n", "|") + "rc=%d" % r.returncode)
except subprocess.TimeoutExpired:
    sys.stdout.write("TIMEOUT")
PY
if ! "${TKC}" --diag-text --allow-all --out sn sn.tk > sn.log 2>&1; then
    expect "build" "$(grep -m1 '^error' sn.log)" ""
else
    got="$(python3 serve.py ./sn)"
    expect "1  open before, closed after the server closed" \
        "$(printf '%s' "${got}" | cut -d'|' -f1,3,4)" "before open=true|after open=false|rc=0"
    expect "2  NEGATIVE CONTROL: the stream was really read" \
        "$(printf '%s' "${got}" | cut -d'|' -f2)" "read 11 then 0"
fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
