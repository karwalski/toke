#!/usr/bin/env bash
# C387_tls_ws_record_fields.sh — std.tls's TlsConfig/TlsKeypair/TlsConn and
# std.ws's $wsconn/$wsmsg read what their names say (story 135.30).
#
# THE DEFECT.  tkc reads `x.f` as 8-byte slot <index of f in the .tki> of
# the block a record handle points at.
#   - tls.connect handed out the connection's id STRING as the $TlsConn, so
#     `conn.id` read the string's bytes as a pointer (SIGSEGV).  TlsConfig
#     kept `requiremutual` as a 4-byte int in its slot, and tls.freeconfig
#     free()d whatever it was given -- a $tlsconfig literal: abort.
#   - ws.connect handed out the C WsConn {uint32 magic; int fd; int mask;
#     int closed}: `c.id` read magic|fd<<32, `c.ready` mask|closed<<32.
#
# THE FIX (tls.h, tls_glue.c, ws.h, ws_glue.c).  TlsConn {id} is boxed;
# TkTlsCfgRec / TkTlsKeypairRec are the .tki records ("" for an absent
# PEM, the bool a whole slot) and the glue reads configs through them, so
# a literal config works; only configs and keypairs the glue issued are
# freed.  ws.connect hands out a TkWsConnRec {id; ready}, resolved through
# a table of issued records by address (a literal is still refused,
# 127.112), and ws.close clears ready; $wsmsg is a TkWsMsgRec.  All
# marked; make check-tki-layout checks them (C389).
#
# WHAT IS ASSERTED (loopback only; the TLS certificate is made here):
#   1. tls.genselfsigned: .certpem / .keypem are the PEMs
#   2. tls.mutualconfig / tls.tlsconfig: every field, requiremutual 1 / 0
#   3. tls.freeconfig frees an issued config, refuses a literal (no abort)
#   4. a $tlsconfig literal pinning a local server: tls.connect succeeds,
#      conn.id is the id, tls.protocol and tls.read work through it
#   5. ws.connect twice: ids 1 and 2, ready; ws.recv's fields; ws.close
#      clears ready
#   6. NEGATIVE CONTROL: a $wsconn literal is refused by ws.send ($err)
#
# C387_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: 6/12 -- the tls program aborts (SIGABRT) freeing the literal
# config, the connect program dies with SIGSEGV on conn.id, and the ws
# program prints magic|fd for .id and never clears .ready.
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C387_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c387_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# run NAME: build NAME.tk and run it; prints its stdout lines joined by '|',
# then " rc=<exit status>" (a crash shows as rc=139, a build failure as
# "compile failed: <first error>").
run() {
    if ! "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1; then
        echo "compile failed: $(grep -m1 '^error' "$1.log")"; return
    fi
    local out rc
    out=$(./"$1" 2>"$1.err"); rc=$?
    echo "$(printf '%s' "${out}" | tr '\n' '|') rc=${rc}"
}
SRV=""; trap 'rm -rf "${WORK}"; [ -n "${SRV}" ] && kill ${SRV} 2>/dev/null' EXIT
echo "C387: std.tls and std.ws record fields (135.30)"
echo "------------------------------------------------------------"
cat > t.tk <<'TK'
m=t;
i=io:std.io;
i=str:std.str;
i=tls:std.tls;
f=main():i64{
  let kp=mt tls.genselfsigned("localhost";30) {$ok:k k;$err:e $tlskeypair{certpem:"";keypem:""}};
  io.println(str.concat(str.fromint(str.indexof(kp.certpem;"-----BEGIN CERTIFICATE") as i64); str.concat(" "; str.fromint(str.indexof(kp.keypem;"PRIVATE KEY-----") as i64))));
  let c=tls.mutualconfig("a";"b";"c");
  io.println(str.concat(c.certpem; str.concat(c.keypem; str.concat(c.peercertpem; str.concat(" "; str.fromint(c.requiremutual as i64))))));
  let d=tls.tlsconfig("x";"");
  io.println(str.concat(d.certpem; str.concat("["; str.concat(d.keypem; str.concat("]"; str.fromint(d.requiremutual as i64))))));
  io.println(str.concat(str.fromint(tls.freeconfig(d) as i64); str.concat(" "; str.fromint(tls.freeconfig($tlsconfig{certpem:"";keypem:"";peercertpem:"";requiremutual:false;insecure:false}) as i64))));
  <0
};
TK
got="$(run t)"
f() { printf '%s' "${got% rc=*}" | cut -d'|' -f"$1"; }
expect "1  tls.genselfsigned: .certpem and .keypem are PEMs" "$(f 1)" "0 $(f 1 | cut -d' ' -f2)"
expect "1b ... the key PEM is not empty" "$([ "$(f 1 | cut -d' ' -f2)" -gt 0 ] 2>/dev/null && echo yes)" "yes"
expect "2  mutualconfig / tlsconfig: every field" "$(f 2)|$(f 3)" "abc 1|x[]0"
expect "3  freeconfig: an issued config 1, a literal 0 (not freed)" "$(f 4)" "1 0"
expect "   exit status (tls)" "${got##* }" "rc=0"

if command -v openssl >/dev/null 2>&1 &&
   openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
       -keyout srv.key -out srv.crt -days 2 -subj /CN=localhost >/dev/null 2>&1; then
cat > tlssrv.py <<'PY'
import socket, ssl
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain("srv.crt", "srv.key")
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(1)
print(s.getsockname()[1], flush=True)
c, _ = s.accept()
t = ctx.wrap_socket(c, server_side=True)
t.sendall(b"hello-from-server")
try: t.recv(100)
except Exception: pass
t.close()
PY
cat > c.tk <<'TK'
m=c;
i=io:std.io;
i=str:std.str;
i=tls:std.tls;
i=file:std.file;
i=env:std.env;
f=main():i64{
  let pem=mt file.read("srv.crt") {$ok:s s;$err:e ""};
  let port=mt env.get("PORT") {$ok:v v;$err:e "0"};
  let pn=mt str.toint(port) {$ok:n n;$err:e 0};
  let cfg=$tlsconfig{certpem:"";keypem:"";peercertpem:pem;requiremutual:false;insecure:false};
  let conn=tls.connect("127.0.0.1";pn as i32;cfg);
  if(conn==0){ io.println("connect=fail"); <1 };
  io.println(str.concat(str.fromint(str.len(conn.id) as i64); str.concat(" "; tls.protocol(conn))));
  let r=tls.read(conn);
  if(r==0){ io.println("read=none"); <1 };
  io.println(r);
  <0
};
TK
python3 tlssrv.py > tport.txt 2>/dev/null & SRV=$!
for _ in $(seq 1 50); do [ -s tport.txt ] && break; sleep 0.1; done
got="$(PORT="$(head -1 tport.txt)" run c)"
expect "4a a \$tlsconfig literal pins and connects; conn.id is the id" \
    "$(f 1 | sed 's/^[1-9][0-9]* /N /')" "N TLSv1.3"
expect "4b tls.read through the \$TlsConn" "$(f 2)" "hello-from-server"
kill ${SRV} 2>/dev/null; SRV=""
else
    echo "  SKIP: 4 (no openssl CLI able to make an EC certificate here)"
fi

cat > wsd.py <<'PY'
import socket, base64, hashlib, sys, threading
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
srv = socket.socket()
srv.bind(("127.0.0.1", 0)); srv.listen(5)
print(srv.getsockname()[1], flush=True)
def handle(c):
    data = b""
    while b"\r\n\r\n" not in data:
        b = c.recv(4096)
        if not b: c.close(); return
        data += b
    key = ""
    for line in data.decode("latin1").split("\r\n"):
        if line.lower().startswith("sec-websocket-key:"):
            key = line.split(":", 1)[1].strip()
    acc = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
    c.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    c.sendall(bytes([0x82, 3]) + b"\x00ab")          # binary, final, NUL first
    try:
        while c.recv(4096): pass
    except Exception: pass
    c.close()
while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
PY
python3 wsd.py > wport.txt 2>/dev/null & SRV=$!
for _ in $(seq 1 50); do [ -s wport.txt ] && break; sleep 0.1; done
cat > w.tk <<TK
m=w;
i=io:std.io;
i=str:std.str;
i=ws:std.ws;
f=rd(b:bool):str{ if(b){<"ready"}; <"closed" };
f=main():i64{
  let a=mt ws.connect("ws://127.0.0.1:$(head -1 wport.txt)/") {\$ok:c c;\$err:e \$wsconn{id:0;ready:false}};
  let b=mt ws.connect("ws://127.0.0.1:$(head -1 wport.txt)/") {\$ok:c c;\$err:e \$wsconn{id:0;ready:false}};
  io.println(str.concat(str.fromint(a.id as i64); str.concat(" "; str.concat(str.fromint(b.id as i64); str.concat(" "; rd(a.ready))))));
  let m=mt ws.recv(a) {\$ok:v v;\$err:e \$wsmsg{payload:@();fin:false;opcode:0}};
  io.println(str.concat(str.fromint(m.payload.len as i64); str.concat(" "; str.concat(str.fromint(m.opcode as i64); str.concat(" "; rd(m.fin))))));
  ws.close(a);
  io.println(str.concat(rd(a.ready); str.concat(" "; rd(b.ready))));
  let fake=\$wsconn{id:2;ready:true};
  io.println(mt ws.send(fake;"x") {\$ok:v "sent";\$err:e "refused"});
  <0
};
TK
got="$(run w)"
expect "5a ws.connect twice: ids 1 and 2, ready" "$(f 1)" "1 2 ready"
expect "5b ws.recv: payload.len, opcode, fin" "$(f 2)" "3 2 ready"
expect "5c ws.close clears .ready (the other stays ready)" "$(f 3)" "closed ready"
expect "6  NEGATIVE CONTROL: a \$wsconn literal naming a live id is refused" "$(f 4)" "refused"
expect "   exit status (ws)" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
