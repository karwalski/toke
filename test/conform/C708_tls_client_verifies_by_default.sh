#!/usr/bin/env bash
# C708 — a std.tls client verifies the server by default (story 121.64).
#
# THE DEFECT (audit-120 TLS-02, 121.19 remainder).  `tls.connect` with an
# unpinned configuration -- `tls.tlsconfig`, a `$tlsconfig` literal with no
# pin, or no configuration at all -- set SSL_VERIFY_NONE on the client and
# never loaded a trust store, so it accepted any certificate from anyone: an
# on-path attacker presenting a self-signed or expired certificate, or one
# issued for a different host, was handed the connection.  The owner decided
# (docs/owner-decisions-2026-10-01.md §3.1) that clients verify by default and
# that pinning and insecure mode are explicit.
#
# WHAT IS ASSERTED.  A local CA issues the certificates; SSL_CERT_FILE points
# OpenSSL's default trust store at it (the same override llm.c honours).
# Python's ssl module serves each certificate over TLS 1.3, and one toke
# client connects in each mode and prints ok / refused:
#   1. default, CA-issued for localhost, connect to "localhost"     -> ok
#   2. default, same certificate, connect to "127.0.0.1" (IP SAN)   -> ok
#   3. default, CA-issued for another host name                     -> refused
#   4. default, self-signed for localhost                           -> refused
#   5. default, CA-issued for localhost but expired                 -> refused
#   6. tls.insecureconfig against the self-signed server            -> ok
#   7. tls.pinconfig pinned to the self-signed certificate          -> ok
#   8. a $tlsconfig literal with no pin and no `insecure` field     -> refused
#      (the self-signed server: an omitted field is false, so literals verify)
#   9. NEGATIVE CONTROL: case 1 with SSL_CERT_FILE pointing at a file that
#      holds no CA -> refused.  The ok in case 1 comes from the trust store,
#      not from a check that passes everything.
#
# Against main (c7f376a7) the client does not compile (tls.insecureconfig is
# absent: 0/9).  With C708_NO_INSECURE=1 main scores 4/9 -- cases 3, 4, 5, 8
# and 9 all connect (ok): main verifies nothing.
#
# Story: 121.64
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c708_XXXXXX")"
PIDS=""
cleanup() { for p in ${PIDS}; do kill "${p}" 2>/dev/null; done; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"

echo "C708: std.tls clients verify the server by default (121.64)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

if ! command -v openssl >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1 ||
   ! openssl x509 -help 2>&1 | grep -q -- '-not_after'; then
    echo "  SKIP: needs python3 and an OpenSSL 3.4+ CLI (x509 -not_after)"
    echo "C708: 0 passed, 0 failed (skipped)"
    exit 0
fi

# ── certificates ───────────────────────────────────────────────────────────
q() { "$@" >/dev/null 2>&1 || { echo "FAIL: setup: $*"; exit 1; }; }
EC="-newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes"
q openssl req -x509 ${EC} -keyout ca.key -out ca.crt -days 2 -subj /CN=C708-test-CA \
    -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign
# leaf NAME SAN [extra x509 args...] -- a CA-issued certificate
leaf() {
    local n="$1" san="$2"; shift 2
    q openssl req -new ${EC} -keyout "$n.key" -out "$n.csr" -subj "/CN=$n"
    printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\n' "${san}" > "$n.ext"
    q openssl x509 -req -in "$n.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
        -extfile "$n.ext" -out "$n.crt" "$@"
}
leaf good   "DNS:localhost,IP:127.0.0.1" -days 2
leaf other  "DNS:not-this-host.invalid"  -days 2
leaf old    "DNS:localhost,IP:127.0.0.1" -not_before 20200101000000Z -not_after 20200201000000Z
q openssl req -x509 ${EC} -keyout self.key -out self.crt -days 2 -subj /CN=localhost \
    -addext subjectAltName=DNS:localhost,IP:127.0.0.1
printf 'not a certificate\n' > empty.pem

# ── servers: one TLS 1.3 listener per certificate, serving until killed ───
cat > srv.py <<'PY'
import socket, ssl, sys
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
ctx.load_cert_chain(sys.argv[1], sys.argv[2])
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(8)
print(s.getsockname()[1], flush=True)
while True:
    c, _ = s.accept()
    try:
        t = ctx.wrap_socket(c, server_side=True)
        t.sendall(b"hello")
        t.close()
    except Exception:
        c.close()
PY
start() {   # start NAME -> sets PORT_<NAME>
    # a subshell keeps the server out of this shell's job table, so killing
    # it at exit prints no "Terminated" line
    PIDS="${PIDS} $( (python3 srv.py "$1.crt" "$1.key" > "$1.port" 2>/dev/null & echo $!) )"
    for _ in $(seq 1 50); do [ -s "$1.port" ] && break; sleep 0.1; done
    eval "PORT_$1=$(head -1 "$1.port")"
}
start good; start other; start old; start self

# ── the client ─────────────────────────────────────────────────────────────
cat > c.tk <<'TK'
m=c;
i=io:std.io;
i=str:std.str;
i=tls:std.tls;
i=file:std.file;
i=env:std.env;
f=pick(mode:str;pin:str):$tlsconfig{
  if(mode=="insecure"){ <tls.insecureconfig("";"") };
  if(mode=="pin"){ <tls.pinconfig("";"";pin) };
  if(mode=="literal"){ <$tlsconfig{certpem:"";keypem:"";peercertpem:"";requiremutual:false} };
  <tls.tlsconfig("";"")
};
f=main():i64{
  let host=mt env.get("HOST") {$ok:v v;$err:e "localhost"};
  let port=mt env.get("PORT") {$ok:v v;$err:e "0"};
  let mode=mt env.get("MODE") {$ok:v v;$err:e "default"};
  let pn=mt str.toint(port) {$ok:n n;$err:e 0};
  let pin=mt file.read("self.crt") {$ok:s s;$err:e ""};
  let cfg=pick(mode;pin);
  let conn=tls.connect(host;pn as i32;cfg);
  if(conn==0){ io.println("refused"); <0 };
  let r=tls.read(conn);
  if(r==0){ io.println("ok-noread"); <0 };
  io.println(str.concat("ok:";r));
  tls.close(conn);
  <0
};
TK
# C708_NO_INSECURE=1: build the client without tls.insecureconfig (it calls
# tls.tlsconfig instead), so a compiler without 121.64 still builds it and
# the other cases show what that tree does.
if [ "${C708_NO_INSECURE:-0}" = 1 ]; then
    sed 's/tls\.insecureconfig/tls.tlsconfig/' c.tk > c2.tk && mv c2.tk c.tk
fi
if ! "${TKC}" --diag-text --allow-all --out c c.tk > c.log 2>&1; then
    echo "  (client does not compile: $(grep -m1 'error' c.log))"
    CLIENT=""
else
    CLIENT=./c
fi
# probe CERTFILE HOST PORT MODE -> the client's one line
probe() {
    [ -n "${CLIENT}" ] || { echo "compile failed"; return; }
    SSL_CERT_FILE="$1" SSL_CERT_DIR=/nonexistent HOST="$2" PORT="$3" MODE="$4" \
        "${CLIENT}" 2>/dev/null | head -1
}

expect "1 default: CA-issued for localhost -> ok" \
    "$(probe ca.crt localhost "${PORT_good}" default)" "ok:hello"
expect "2 default: same certificate by IP address -> ok" \
    "$(probe ca.crt 127.0.0.1 "${PORT_good}" default)" "ok:hello"
expect "3 default: CA-issued for another host -> refused" \
    "$(probe ca.crt localhost "${PORT_other}" default)" "refused"
expect "4 default: self-signed -> refused" \
    "$(probe ca.crt localhost "${PORT_self}" default)" "refused"
expect "5 default: expired -> refused" \
    "$(probe ca.crt localhost "${PORT_old}" default)" "refused"
expect "6 tls.insecureconfig: self-signed -> ok" \
    "$(probe ca.crt localhost "${PORT_self}" insecure)" "ok:hello"
expect "7 tls.pinconfig pinned to the self-signed cert -> ok" \
    "$(probe ca.crt localhost "${PORT_self}" pin)" "ok:hello"
expect "8 \$tlsconfig literal without insecure: self-signed -> refused" \
    "$(probe ca.crt localhost "${PORT_self}" literal)" "refused"
expect "9 NEGATIVE CONTROL: trust store without the CA -> refused" \
    "$(probe empty.pem localhost "${PORT_good}" default)" "refused"

echo "------------------------------------------------------------"
echo "C708: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
