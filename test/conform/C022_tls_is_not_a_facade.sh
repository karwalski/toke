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
# THE FAIL-CLOSED RESULT IS WITNESSED POSITIVELY, NOT INFERRED FROM SILENCE.
# A test that passes because the client crashed before it could report is
# indistinguishable from one that passes because mutual auth refused the
# connection — see the note above Part 5, where the first version of this file
# had exactly that fault.  So the refusal is observed by an INDEPENDENT TLS 1.3
# implementation (Python's ssl module, not the code under test) which probes for
# the refusal before writing anything and NAMES what happened; an empty
# observation fails.  That peer also proves interoperability, which no amount of
# toke-talking-to-toke can.
#
# AND IT IS A CONTROLLED EXPERIMENT.  The same peer runs twice against the same
# running server, differing in exactly one thing: whether a client certificate
# is presented.  The authenticated arm must reach the echo and the server's
# handler must run for it.  If both arms were refused the harness would be blind
# and the fail-closed result worthless, so the pair is asserted together.
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

(* mutualconfig, not three field assignments. Until 136.46 these fields were
   spelled peer_cert_pem and require_mutual, which the default profile cannot
   express, so a toke program could not name them at all; they are peercertpem
   and requiremutual now. The constructor remains the documented route, and
   this suite keeps using it: it is the one shape that was always writeable *)
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

# ── Part 3b: tls.genselfsignedalg, and that it does NOT fall back ────────────
#
# 136.44a. tls_gen_self_signed_alg has been implemented and linked in tls.c
# since 124.1 shipped ADR-0013's crypto agility, and it had no wrapper and no
# .tki export, so ML-DSA-65 certificate generation was unreachable from toke.
# 136.44's sweep could not have found it: that sweep compared declared .tki
# exports against the glue, and this one was never declared.
#
# The assertion that matters is NOT "it produced a certificate". It is that a
# caller who asked for ML-DSA-65 got ML-DSA-65 -- a classical certificate
# silently returned in its place is the worst outcome available here, because
# the caller asked for post-quantum precisely because it matters to them.

# Premise first: does the OpenSSL this machine links actually offer ML-DSA-65?
# Everything below is asserted against the answer, so neither branch can pass
# vacuously.
OSSL=""
for cand in /opt/homebrew/opt/openssl@3/bin/openssl \
            /opt/homebrew/opt/openssl/bin/openssl \
            /usr/local/opt/openssl@3/bin/openssl openssl; do
    if command -v "${cand}" >/dev/null 2>&1; then OSSL="${cand}"; break; fi
done
MLDSA_AVAILABLE=0
if [ -n "${OSSL}" ] && "${OSSL}" list -signature-algorithms 2>/dev/null \
        | grep -qi 'ML-DSA-65'; then
    MLDSA_AVAILABLE=1
    ok "premise: this OpenSSL offers ML-DSA-65, so toke is the only thing under test"
else
    ok "premise: this OpenSSL does NOT offer ML-DSA-65 — the refusal path is what is asserted"
fi

cat > alg.tk <<EOF
m=alg;
i=tls:std.tls;
i=file:std.file;
i=io:std.io;

f=emit(cn:str;alg:str;certp:str):i64{
  let kp=tls.genselfsignedalg(cn; 1; alg);
  if(kp==0){ <1 };
  let a=mt file.write(certp; tls.certof(kp)){\$ok:v v;\$err:e false};
  tls.freekeypair(kp);
  if(a==false){ <1 };
  <0
};

f=main():i64{
  (* "" means the core's default, which is P-384, not an error *)
  let d=emit("toke-c022-default"; ""; "${WORK}/d.crt");
  let e=emit("toke-c022-p384"; "ecdsa-p384"; "${WORK}/e.crt");
  let m=emit("toke-c022-mldsa"; "ml-dsa-65"; "${WORK}/m.crt");
  (* an algorithm the core does not know must be refused, not defaulted *)
  let b=emit("toke-c022-bogus"; "rsa-1024"; "${WORK}/b.crt");
  io.println("d=\(d) e=\(e) m=\(m) b=\(b)");
  <0
};
EOF
if "${TKC}" --allow-all -o alg.bin alg.tk >alg.build 2>&1; then
    ok "a toke program can call tls.genselfsignedalg"
    ALG_OUT="$(./alg.bin 2>/dev/null)"

    case "${ALG_OUT}" in
        *"d=0"*) ok "genselfsignedalg with an empty algorithm produced a certificate (the default)" ;;
        *)       bad "genselfsignedalg with an empty algorithm produced nothing: ${ALG_OUT}" ;;
    esac
    case "${ALG_OUT}" in
        *"e=0"*) ok "genselfsignedalg('ecdsa-p384') produced a certificate" ;;
        *)       bad "genselfsignedalg('ecdsa-p384') produced nothing: ${ALG_OUT}" ;;
    esac
    case "${ALG_OUT}" in
        *"b=1"*) ok "genselfsignedalg('rsa-1024') was REFUSED — no silent default" ;;
        *)       bad "genselfsignedalg('rsa-1024') did not fail; an unknown algorithm must not default" ;;
    esac
    if [ -s "${WORK}/b.crt" ]; then
        bad "a refused algorithm still wrote ${WORK}/b.crt"
    else
        ok "a refused algorithm wrote no certificate"
    fi

    if [ "${MLDSA_AVAILABLE}" -eq 1 ]; then
        case "${ALG_OUT}" in
            *"m=0"*) ok "genselfsignedalg('ml-dsa-65') produced a certificate" ;;
            *)       bad "genselfsignedalg('ml-dsa-65') produced nothing on an OpenSSL that has it: ${ALG_OUT}" ;;
        esac
        # The whole point: the certificate must really be ML-DSA, and the
        # classical one must really be ECDSA. Reading the algorithm out of the
        # DER is what distinguishes "it worked" from "it fell back".
        if [ -s "${WORK}/m.crt" ] && \
           "${OSSL}" x509 -in "${WORK}/m.crt" -noout -text 2>/dev/null \
             | grep -qi 'ML-DSA-65'; then
            ok "the ml-dsa-65 certificate really is signed with ML-DSA-65"
        else
            bad "the ml-dsa-65 certificate is not ML-DSA — a silent fallback to classical"
        fi
        if [ -s "${WORK}/e.crt" ] && \
           "${OSSL}" x509 -in "${WORK}/e.crt" -noout -text 2>/dev/null \
             | grep -qi 'ecdsa'; then
            ok "the ecdsa-p384 certificate really is ECDSA — the two paths differ"
        else
            bad "the ecdsa-p384 certificate is not ECDSA"
        fi
    else
        case "${ALG_OUT}" in
            *"m=1"*) ok "genselfsignedalg('ml-dsa-65') was refused on an OpenSSL without it — no classical substitute" ;;
            *)       bad "genselfsignedalg('ml-dsa-65') claimed success on an OpenSSL that has no ML-DSA: ${ALG_OUT}" ;;
        esac
        if [ -s "${WORK}/m.crt" ]; then
            bad "ML-DSA is unavailable here yet ${WORK}/m.crt was written"
        else
            ok "ML-DSA unavailable: no certificate was written under that name"
        fi
    fi
else
    bad "alg.tk did not compile — tls.genselfsignedalg is not reachable from toke"
    sed 's/^/      /' alg.build | head -5
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

# ── Part 5: mutual auth fails closed, witnessed positively ───────────────────
#
# THE FIRST VERSION OF THIS SECTION WAS NOT GOOD ENOUGH, AND THE REASON IS WORTH
# KEEPING.  It ran the toke client with no certificate and asserted that it
# printed "reply=none".  That was flaky — it passed on a terminal and failed
# under `make conform-sh` — and chasing the flake exposed the deeper fault.  The
# toke client dies of SIGPIPE when it writes into a connection the server has
# already torn down, and under a pipe its block-buffered stdout is lost with it,
# so the observation was an EMPTY STRING.  Relaxing the assertion to "no reply
# came back" would have made it green and meaningless: a test that passes
# because the client crashed before it could report is indistinguishable from
# one that passes because mutual auth refused the connection.
#
# So the refusal is witnessed POSITIVELY, by a peer that survives to report it:
# an independent TLS 1.3 implementation (Python's ssl module, i.e. not the code
# under test) which probes for the refusal BEFORE it writes anything, so nothing
# can race a SIGPIPE into the result.  It names what happened.
#
# And it is a CONTROLLED experiment.  The same peer script runs twice against
# the same running server, differing in exactly one thing — whether a client
# certificate is presented.  The authenticated arm must get all the way to the
# echo.  If both arms were refused the harness would be blind and the refusal
# would prove nothing, so the pair is asserted together.

cat > peer.py <<'PYEOF'
import socket, ssl, sys, os

arm, port, d = sys.argv[1], int(sys.argv[2]), sys.argv[3]

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.check_hostname = False
ctx.verify_mode = ssl.CERT_NONE
ctx.minimum_version = ssl.TLSVersion.TLSv1_3
if arm == "auth":
    ctx.load_cert_chain(os.path.join(d, "c.crt"), os.path.join(d, "c.key"))

raw = socket.create_connection(("127.0.0.1", port), 5)
raw.settimeout(5)
try:
    s = ctx.wrap_socket(raw, suppress_ragged_eofs=False)
except ssl.SSLError as e:
    print("refused-at-handshake=%s" % str(e).replace("\n", " "))
    raise SystemExit

print("version=%s" % s.version())
print("cipher=%s" % (s.cipher()[0],))

# Probe for a refusal BEFORE speaking.  In TLS 1.3 the client finishes its half
# of the handshake before the server has validated it, so a server that refuses
# does so as a post-handshake alert landing on the client's first READ.  Writing
# first races that alert: the write hits a socket the server has already torn
# down and ECONNRESET masks the reason.  This server speaks only after the
# client does, so a live connection simply times out here — which is itself the
# signal that it was NOT refused.
s.settimeout(1.5)
try:
    early = s.recv(4096)
    if early == b"":
        print("refused=server-closed-before-any-data")
        raise SystemExit
    print("unexpected-early-data=%r" % early)
except ssl.SSLError as e:
    print("refused-alert=%s" % str(e).replace("\n", " "))
    raise SystemExit
except socket.timeout:
    pass
except OSError as e:
    print("refused-reset=%s" % e)
    raise SystemExit

s.settimeout(5)
try:
    s.sendall(b"ping")
    data = s.recv(4096)
    if not data:
        print("refused=closed-after-request")
    else:
        print("reply=%s" % data.decode(errors="replace"))
except ssl.SSLError as e:
    print("refused-alert=%s" % str(e).replace("\n", " "))
except OSError as e:
    print("refused-reset=%s" % e)
PYEOF

if [ "${built}" -eq 1 ]; then
    # -- the control arm: the same peer, WITH a certificate, must get in --
    pbefore="$(wc -l < srv.log 2>/dev/null | tr -d ' ')"
    peerauth="$(python3 peer.py auth "${PORT}" "${WORK}" 2>&1)"
    sleep 0.5
    pafter="$(wc -l < srv.log 2>/dev/null | tr -d ' ')"

    if echo "${peerauth}" | grep -q '^version=TLSv1.3$'; then
        ok "an INDEPENDENT TLS 1.3 implementation interoperates: $(echo "${peerauth}" | grep '^cipher=')"
    else
        bad "the independent peer did not negotiate TLS 1.3: [${peerauth}]"
    fi

    if echo "${peerauth}" | grep -q '^reply=echo:ping$'; then
        ok "control arm: the authenticated peer completed the exchange"
    else
        bad "control arm: the authenticated peer was refused, so the next assertion would prove nothing: [${peerauth}]"
    fi

    if [ "${pbefore}" != "${pafter}" ]; then
        ok "control arm: the server handler ran for the authenticated peer"
    else
        bad "control arm: the server handler did not run — the srv.log signal is blind"
    fi

    # -- the arm under test: the same peer, WITHOUT a certificate --
    abefore="$(wc -l < srv.log 2>/dev/null | tr -d ' ')"
    peeranon="$(python3 peer.py anon "${PORT}" "${WORK}" 2>&1)"
    sleep 0.5
    aafter="$(wc -l < srv.log 2>/dev/null | tr -d ' ')"

    # POSITIVE: the peer ran to completion and NAMED the refusal.  An empty
    # observation is now a failure, not a pass.
    refusal="$(echo "${peeranon}" | grep -E '^refused' | head -1)"
    if [ -n "${refusal}" ]; then
        ok "mutual auth fails closed, and the peer reported it: [${refusal}]"
    else
        bad "the unauthenticated peer reported no refusal at all: [${peeranon:-<no output — the witness itself failed>}]"
    fi

    if echo "${peeranon}" | grep -q '^reply='; then
        bad "FAIL-OPEN: the unauthenticated peer got application data: [${peeranon}]"
    else
        ok "the unauthenticated peer received no application data"
    fi

    if [ "${abefore}" = "${aafter}" ]; then
        ok "and the server handler never ran for it (srv.log did not grow)"
    else
        bad "FAIL-OPEN: the unauthenticated peer reached the server handler"
        diff <(head -n "${abefore}" srv.log) srv.log | sed 's/^/      /' | head -8
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
