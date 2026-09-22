#!/usr/bin/env bash
# C022_tls_is_not_a_facade.sh — std.tls actually speaks TLS (story 136.44).
#
# BEFORE THIS STORY NO TLS OF ANY KIND HAPPENED.  src/stdlib/tls.c is ~910
# lines of working TLS 1.3 — a TLS1_3_VERSION floor on both min and max, mutual
# TLS via SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, peer-certificate
# pinning, an X25519MLKEM768 hybrid key exchange — compiled and LINKED against
# -lssl -lcrypto by stdlib_deps.c.  None of it was reachable.  Every wrapper in
# tls_glue.c was `(void)arg; return 0;`, `tls.listen` returned before it bound a
# socket, and two of the module's entry points resolved to symbols that did not
# exist: `tls.connect` mangles to tk_tls_connect_w while the glue spelled it
# tk_tls_connecttls_w, same for listen.  peercert, fingerprint and pairingcode
# had no wrapper at all.  std.tls was a facade in front of a finished
# implementation — the second instance of that class after 136.16 (std.toon).
#
# WHY THIS TEST IS SHAPED THE WAY IT IS.  A test that asserted "tls.connect
# returns non-zero" would pass on a stub that returns 1, which is exactly the
# failure mode this story exists to close.  So the assertions are facts only a
# real handshake can produce:
#
#   * the protocol NEGOTIATED on the wire is TLSv1.3, read back with
#     SSL_get_version through tls.protocol — not a claim about the source;
#   * the server reads the client's certificate off the connection and its
#     SHA-256 fingerprint equals the fingerprint of the file the client was
#     given, which no stub can fabricate;
#   * an application payload survives the round trip, encrypted;
#   * and MUTUAL AUTH FAILS CLOSED: a client with no certificate never reaches
#     the server's handler at all.
#
# THE GATE ON THE LAST ONE.  "The anonymous client was rejected" proves nothing
# unless something proves the server accepts anybody.  Part 4 therefore runs an
# AUTHENTICATED client against the same server first; that one must get all the
# way through.  If both arms were rejected the harness would be blind and the
# fail-closed result worthless, so the pair is asserted together.
#
# NO SECRETS.  Every certificate and key in this test is generated at run time
# into a mktemp -d that is removed on exit; nothing is committed, and the test
# prints fingerprints and protocol names only — never key material.
#
# Story: 136.44

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"
GLUE="${REPO_ROOT}/src/stdlib/tls_glue.c"

PASS=0
FAIL=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c022_XXXXXX)"
chmod 700 "${WORK}"
SRV_PID=""
cleanup() {
    [ -n "${SRV_PID}" ] && kill "${SRV_PID}" 2>/dev/null
    rm -rf "${WORK}"
}
trap cleanup EXIT

echo "C022: std.tls actually speaks TLS 1.3, and mutual auth fails closed"
echo "--------------------------------------"

# ── Part 1: the glue names the published interface ───────────────────────────
#
# stdlib/tls.tki declares tls.connect and tls.listen.  A call to `tls.connect`
# is lowered to tk_tls_connect_w; if the glue defines tk_tls_connecttls_w
# instead, the call reaches codegen and dies as an E9003 naming a mangled
# symbol.  These are internal C symbols with no callers outside the glue, so
# the interface is what they are named after.

for sym in tk_tls_connect_w tk_tls_listen_w tk_tls_peercert_w \
           tk_tls_fingerprint_w tk_tls_pairingcode_w tk_tls_protocol_w \
           tk_tls_mutualconfig_w; do
    if grep -qE "^int64_t ${sym}\(" "${GLUE}"; then
        ok "${sym} is defined in tls_glue.c"
    else
        bad "${sym} is not defined in tls_glue.c"
    fi
done

# Definitions, not mentions: the header comment above names both old symbols
# so that a reader knows what was renamed and why.
for gone in tk_tls_connecttls_w tk_tls_listentls_w; do
    if grep -qE "^int64_t ${gone}\\(" "${GLUE}"; then
        bad "${gone} is still defined — the glue spelling, not the interface"
    else
        ok "${gone} is no longer defined"
    fi
done

# ── Part 2: no wrapper is a stub ─────────────────────────────────────────────
#
# The class this story names is "the glue returns 0 while the C core is
# complete".  Every wrapper must reach the core, so every wrapper body must
# mention a tls_* function from tls.h (or a helper in this file that does).

# NOTE ON THE MATCH, because the obvious spelling is wrong.  The body must be
# taken WITHOUT its signature line and the core symbol matched with a
# left-boundary: `tls_[a-z_]+\(` alone matches the substring `tls_read_w(`
# inside the declarator `int64_t tk_tls_read_w(`, so every stub in the world
# "reaches the core".  This check passed on the unfixed glue until that was
# fixed, which is precisely the false pass this story is about.
stubs=""
while IFS= read -r sym; do
    body="$(awk -v s="${sym}" '
        $0 ~ "^int64_t " s "\\(" {inb=1}
        inb {print}
        inb && /^}/ {exit}' "${GLUE}" | tail -n +2)"
    if ! echo "${body}" | grep -qE '(^|[^A-Za-z0-9_])(tls_[a-z_]+\(|make_cfg\(|cfg_of\(|conn_of\(|kp->|c->)'; then
        stubs="${stubs} ${sym}"
    fi
done < <(grep -oE '^int64_t tk_tls_[a-z]+_w' "${GLUE}" | sed 's/^int64_t //')

if [ -z "${stubs}" ]; then
    n="$(grep -cE '^int64_t tk_tls_[a-z]+_w' "${GLUE}")"
    ok "all ${n} tk_tls_*_w wrappers reach the C core (none is a (void)arg; return 0 stub)"
else
    bad "these wrappers are still stubs:${stubs}"
fi

# The quarantine entries that recorded the missing glue must be gone, or
# check-tki reports them as stale.
if grep -q '^tls\.tki::' "${REPO_ROOT}/scripts/check_tki_skiplist.txt" 2>/dev/null; then
    bad "scripts/check_tki_skiplist.txt still quarantines tls.tki exports"
else
    ok "no tls.tki export is quarantined any more"
fi

# ── Part 3: a real TLS 1.3 handshake, end to end, from toke ──────────────────

cd "${WORK}"

PORT="$(python3 - <<'PY'
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0))
print(s.getsockname()[1]); s.close()
PY
)"

cat > gen.tk <<EOF
m=gen;
i=tls:std.tls;
i=file:std.file;

f=emit(cn:str;certp:str;keyp:str):i64{
  let kp=tls.genselfsigned(cn; 1);
  if(kp==0){ <1 };
  let a=mt file.write(certp; tls.certof(kp)){\$ok:v v;\$err:e false};
  let b=mt file.write(keyp; tls.keyof(kp)){\$ok:v v;\$err:e false};
  tls.freekeypair(kp);
  if(a==false){ <1 };
  if(b==false){ <1 };
  <0
};

f=main():i64{
  let a=emit("toke-c022-server"; "${WORK}/s.crt"; "${WORK}/s.key");
  let b=emit("toke-c022-client"; "${WORK}/c.crt"; "${WORK}/c.key");
  <a+b
};
EOF

cat > srv.tk <<EOF
m=srv;
i=tls:std.tls;
i=str:std.str;
i=file:std.file;

f=rd(p:str):str{
  <mt file.read(p){\$ok:v v;\$err:e ""}
};

(* the handler logs to a file, not stdout: the harness kills this process and
   a block-buffered stdout would lose the evidence *)
f=say(s:str):i64{
  let ok=mt file.append("${WORK}/srv.log"; str.concat(s; "\\n")){\$ok:v v;\$err:e false};
  <0
};

f=onconn(conn:i64):i64{
  say(str.concat("proto="; tls.protocol(conn)));
  let pc=tls.peercert(conn);
  if(pc==0){
    say("peercert=none")
  }el{
    say(str.concat("peerfp="; tls.fingerprint(pc)))
  };
  say(str.concat("pairing="; tls.pairingcode(conn)));
  let msg=tls.read(conn);
  if(msg==0){
    say("read=none")
  }el{
    say(str.concat("got="; msg));
    tls.write(conn; str.concat("echo:"; msg))
  };
  tls.close(conn);
  <0
};

(* mutualconfig, not three field assignments: TlsConfig's peer_cert_pem and
   require_mutual carry underscores the default profile cannot express, so a
   toke program cannot name them (136.46) *)
f=main():i64{
  let cfg=tls.mutualconfig(rd("${WORK}/s.crt"); rd("${WORK}/s.key"); rd("${WORK}/c.crt"));
  say("listening");
  tls.listen(${PORT}; cfg; &onconn);
  <0
};
EOF

cat > cli.tk <<EOF
m=cli;
i=io:std.io;
i=tls:std.tls;
i=str:std.str;
i=file:std.file;
i=args:std.args;

f=rd(p:str):str{
  <mt file.read(p){\$ok:v v;\$err:e ""}
};

f=main():i64{
  let mode=args.get(1);
  let scert=rd("${WORK}/s.crt");
  let cfg=mut.0;
  if(mode=="auth"){
    cfg=tls.pinconfig(rd("${WORK}/c.crt"); rd("${WORK}/c.key"); scert)
  }el{
    cfg=tls.pinconfig(""; ""; scert)
  };
  let conn=tls.connect("127.0.0.1"; ${PORT}; cfg);
  if(conn==0){ io.println("connect=fail"); <0 };
  io.println(str.concat("proto="; tls.protocol(conn)));
  tls.write(conn; "ping");
  let r=tls.read(conn);
  if(r==0){ io.println("reply=none") }el{ io.println(str.concat("reply="; r)) };
  tls.close(conn);
  <0
};
EOF

built=1
for m in gen srv cli; do
    if ! "${TKC}" --allow-all -o "${m}.bin" "${m}.tk" >"${m}.build" 2>&1; then
        bad "${m}.tk did not compile"
        sed 's/^/      /' "${m}.build" | head -5
        built=0
    fi
done
if [ "${built}" -eq 1 ]; then
    ok "a toke program can call tls.genselfsigned / connect / listen / peercert / fingerprint / pairingcode"
fi

if [ "${built}" -eq 1 ] && ./gen.bin >/dev/null 2>&1 \
   && [ -s s.crt ] && [ -s s.key ] && [ -s c.crt ] && [ -s c.key ]; then
    ok "tls.genselfsigned produced two real keypairs (P-384 self-signed)"
else
    bad "tls.genselfsigned produced nothing — a stub returns 0 and writes no PEM"
    built=0
fi

if [ "${built}" -eq 1 ]; then
    ./srv.bin >/dev/null 2>&1 &
    SRV_PID=$!
    disown 2>/dev/null || true
    up="$(python3 - "${PORT}" <<'PY'
import socket, sys, time
port = int(sys.argv[1])
for _ in range(200):
    try:
        socket.create_connection(("127.0.0.1", port), 0.2).close()
        print("up"); break
    except OSError:
        time.sleep(0.05)
else:
    print("down")
PY
)"
    if [ "${up}" = "up" ]; then
        ok "tls.listen bound a socket (the stub returned before binding one)"
    else
        bad "tls.listen never bound port ${PORT}"
        built=0
    fi
fi

# ── Part 4: the authenticated arm — the gate on the fail-closed result ───────

if [ "${built}" -eq 1 ]; then
    authout="$(./cli.bin auth 2>&1)"

    if echo "${authout}" | grep -q '^proto=TLSv1.3$'; then
        ok "client negotiated TLSv1.3 on the wire (SSL_get_version, not a source claim)"
    else
        bad "client did not negotiate TLS 1.3: [${authout}]"
    fi

    if echo "${authout}" | grep -q '^reply=echo:ping$'; then
        ok "an application payload round-tripped through the encrypted connection"
    else
        bad "no encrypted round trip: [${authout}]"
    fi

    sleep 0.5
    srvlog="$(cat srv.log 2>/dev/null)"

    if echo "${srvlog}" | grep -q '^proto=TLSv1.3$'; then
        ok "server side negotiated TLSv1.3 too"
    else
        bad "server did not report TLSv1.3: [${srvlog}]"
    fi

    # tls.peercert + tls.fingerprint had NO wrapper before this story.  The
    # fingerprint the server computed from the certificate it received over the
    # wire must equal the fingerprint of the file the client was handed.  No
    # stub can produce that agreement.
    wirefp="$(echo "${srvlog}" | sed -n 's/^peerfp=//p' | head -1)"
    filefp="$(openssl x509 -in c.crt -outform DER 2>/dev/null | openssl dgst -sha256 -r 2>/dev/null | cut -d' ' -f1)"
    if [ -n "${wirefp}" ] && [ "${wirefp}" = "${filefp}" ]; then
        ok "the certificate the server read off the wire is the client's (fingerprints agree)"
    else
        bad "peer-certificate fingerprint mismatch: wire=[${wirefp}] file=[${filefp}]"
    fi

    if echo "${srvlog}" | grep -qE '^pairing=[0-9]{6}$'; then
        ok "tls.pairingcode returned a 6-digit code derived from both fingerprints"
    else
        bad "tls.pairingcode did not return 6 digits: [${srvlog}]"
    fi

    if echo "${srvlog}" | grep -q '^got=ping$'; then
        ok "the server's handler ran and read the plaintext — an authenticated peer gets in"
    else
        bad "the server handler never received the payload: [${srvlog}]"
    fi
fi

# ── Part 5: mutual auth fails closed ─────────────────────────────────────────
#
# Same server, same port, same moment.  The only difference is that this client
# presents no certificate.  With mutual auth working the server's handler must
# never run for it — so srv.log must not grow.

if [ "${built}" -eq 1 ]; then
    before="$(wc -l < srv.log 2>/dev/null | tr -d ' ')"
    anonout="$(./cli.bin anon 2>&1)"
    sleep 0.5
    after="$(wc -l < srv.log 2>/dev/null | tr -d ' ')"

    if [ "${before}" = "${after}" ]; then
        ok "mutual auth fails closed: the anonymous client never reached the handler"
    else
        bad "FAIL-OPEN: the anonymous client reached the server handler"
        diff <(head -n "${before}" srv.log) srv.log | sed 's/^/      /' | head -8
    fi

    if echo "${anonout}" | grep -qE '^(connect=fail|reply=none)$'; then
        ok "and the anonymous client got no application data back: [$(echo "${anonout}" | tail -1)]"
    else
        bad "the anonymous client got a reply it should not have: [${anonout}]"
    fi
fi

# ── Part 6: no key material escaped ──────────────────────────────────────────

if grep -rq 'PRIVATE KEY' srv.log *.build 2>/dev/null; then
    bad "private key material appears in a log this test produced"
else
    ok "no private key material appears in any output"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
