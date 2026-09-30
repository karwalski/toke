#!/usr/bin/env bash
# C181_byte_producers_return_bytes.sh — chart.tojson and http.streamnext
# return the `[byte]` they are declared to return (story 135.19 item 3).
#
# THE DEFECT.  Both are declared `[byte]` (stdlib/chart.tki,
# stdlib/http.tki: `[byte]!httperr`) and both glue wrappers returned a
# malloc'd C STRING.  A toke caller holding it as [byte] read `.len` from the
# eight bytes before the string (0 here), and `str.frombytes` of it was empty,
# so the JSON / the chunk never reached the program.  They are fixed before
# the [byte] layout changes (135.18) because an unconverted producer would
# otherwise become an over-read of a different shape.
#
# Section 2 serves one plain-text response from a local Python socket; the
# stream is read with --allow-net.
#
# Negative control: TKC built from main at 7c16729 — section 1 exits 1 with
# "RT003: index 0 out of bounds for length 0"; section 2 prints a length read
# from before the string and a mangled chunk ("2 hu" in one run, "0 " in
# another — it is whatever precedes the malloc).
#
# Story: 135.19

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# 1. chart.tojson: a real [byte] whose length is the JSON's length
cat > "${WORK}/ch.tk" <<'EOF'
m=t;i=chart:std.chart;i=io:std.io;i=str:std.str;
f=main():i64{
  let c=chart.bar(@("Mon";"Tue");@(10.0;20.0);"Hits");
  let j=chart.tojson(c);
  let s=str.frombytes(j);
  io.println("\(j.len==str.len(s)) \(j.len>50) \(j.get(0))");
  io.println(s);
  <0
};
EOF
if "${TKC}" "${WORK}/ch.tk" -o "${WORK}/ch" >/dev/null 2>&1; then
    out="$("${WORK}/ch" 2>&1)"; rc=$?
    expect "1a-tojson-exit"            "${rc}" "0"
    expect "1b-tojson-len-and-first"   "$(printf '%s\n' "${out}" | sed -n 1p)" "true true 123"
    expect "1c-tojson-text-reaches-toke" \
           "$(printf '%s\n' "${out}" | sed -n 2p | grep -c '"labels":\["Mon","Tue"\]')" "1"
else
    expect "1-tojson-compiles" "compile-fail" "compiles"
fi

# 2. http.streamnext: the chunk is a [byte] holding the body bytes
cat > "${WORK}/sn.tk" <<'EOF'
m=t;i=http:std.http;i=io:std.io;i=str:std.str;i=os:std.os;
f=main():i64{
  let s=mt http.stream(http.client(os.getenv("URL"))) {$ok:v v;$err:e 0};
  let chunk=mt http.streamnext(s) {$ok:v v;$err:e @()};
  io.println("\(chunk.len) \(str.frombytes(chunk))");
  <0
};
EOF
cat > "${WORK}/drive.py" <<'EOF'
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
    sys.stdout.write(r.stdout.decode("utf-8", "replace").strip() + " rc=%d" % r.returncode)
except subprocess.TimeoutExpired:
    sys.stdout.write("TIMEOUT")
EOF
if "${TKC}" "${WORK}/sn.tk" -o "${WORK}/sn" >/dev/null 2>&1; then
    expect "2-streamnext-chunk-is-bytes" \
           "$(python3 "${WORK}/drive.py" "${WORK}/sn" --allow-net)" "11 hello-chunk rc=0"
else
    expect "2-streamnext-compiles" "compile-fail" "compiles"
fi

echo "C181: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
