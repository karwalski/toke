/* SPDX-License-Identifier: MIT */
/*
 * tls_glue.c — i64-ABI wrappers for std.tls module.
 *
 * Story 136.44.  Until this story every wrapper here was `(void)arg; return 0;`
 * while src/stdlib/tls.c — 900-odd lines of working TLS 1.3, compiled and
 * linked against -lssl -lcrypto by stdlib_deps.c — sat in the same link unit
 * and was never called.  `tls.listen` returned before it bound a socket.  So
 * no TLS of any kind happened, and std.tls was a facade in front of a finished
 * implementation.  These wrappers call it.
 *
 * TWO NAMES WERE ALSO WRONG.  `tls.connect` mangles to tk_tls_connect_w and
 * `tls.listen` to tk_tls_listen_w; the glue spelled them connecttls/listentls,
 * so the two entry points of the module resolved to symbols that did not
 * exist.  The glue is renamed to match stdlib/tls.tki, which is the published
 * interface; these are internal C symbols with no callers outside this file.
 *
 * HANDLES CROSSING THE ABI
 *   TlsConfig   -> pointer to a heap TkTlsCfgRec (tls.h) owning its PEMs
 *   TlsKeypair  -> pointer to a heap TkTlsKeypairRec (tls.h) owning both PEMs
 *   TlsConn     -> pointer to a TlsConn {id}, the id as tls.c mints it
 *   (135.30: each is the .tki record, slot for slot; they were a struct
 *   with a C-typed tail, and the bare id string, before)
 *   str         -> char *, 0 for none
 *   bool        -> 1 / 0
 *
 * WHY THERE ARE CONSTRUCTORS RATHER THAN FIELDS.  stdlib/tls.tki declares
 * TlsConfig with the fields `peer_cert_pem` and `require_mutual`, and
 * TlsKeypair with `cert_pem`/`key_pem`.  Every one of those names contains an
 * underscore, which the 59-character default profile cannot express, so a
 * toke program cannot name them at all (136.46).  Mutual-auth configuration is
 * therefore surfaced as a constructor — tls.mutualconfig(cert; key; peercert)
 * — and the keypair through tls.certof/tls.keyof accessors, so that no caller
 * ever has to spell an unspellable field.  (113.2a has since renamed the
 * fields certpem/keypem/peercertpem/requiremutual, which a program can read;
 * 135.30 made them read what they say.)
 */

#include "tls.h"
#include "capabilities.h"   /* 124.4c: net capability gate */
#include "tk_strhdr.h"      /* 127.295: sealed strings, tk_strlen */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * 127.295 (runtime-abi §3.1, fifteenth tranche) -- every str this glue makes
 * for toke is sealed: a $TlsConn's .id (tls.connect, and the record each
 * tls.listen handler receives), tls.read, tls.peercert, tls.fingerprint,
 * tls.pairingcode, tls.group, tls.protocol, and tls.certof (a copy).  tls.c's
 * own producers (the registry's ids, tls_read, tls_peer_cert,
 * tls_fingerprint, tls_pairing_code, tls_group, tls_protocol) stay malloc'd:
 * their C callers -- test_tls.c, C206, C590, C1180 -- free what they return.
 * So the seal happens here, at the boundary: seal_take() makes the sealed
 * copy toke keeps and frees the malloc'd original, handing the original over
 * unsealed only if the copy cannot be made.  The copy is measured as a C
 * string, so plaintext tls.read returns ends at its first 0 byte exactly
 * where every reader already saw it end -- and it no longer keeps the whole
 * 64 KiB read buffer alive for a short record.  No C code calls a
 * tk_tls_*_w str producer.
 *
 * Left malloc'd, on purpose: a configuration's PEMs (dup_arg; the record's
 * storage, freed by tls.freeconfig) and a keypair's PEMs (freed by
 * tls.freekeypair, which first overwrites the private key -- a sealed string
 * is never written, so it could not be wiped).  tls.keyof therefore still
 * hands toke the keypair's own buffer: a sealed copy of a private key would
 * live, unwiped, for the life of the process.
 */
static char *seal_take(char *s)
{
    if (!s) return NULL;
    char *t = tks_strdup(s);
    if (!t) return s;
    free(s);
    return t;
}

/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

/* ── handle plumbing ─────────────────────────────────────────────────────── */

/* 135.30: the records this file issued (configs and keypairs), so that
 * tls.freeconfig / tls.freekeypair free only what the glue allocated -- a
 * $TlsConfig or $TlsKeypair literal is not the glue's to free.  An issued
 * handle leaves the set when it is freed, so a second free is refused. */
static void **g_tls_issued = NULL;
static size_t g_tls_nissued = 0, g_tls_capissued = 0;

static int tls_issue(void *p)
{
    if (g_tls_nissued == g_tls_capissued) {
        size_t nc = g_tls_capissued ? 2 * g_tls_capissued : 16;
        void **n = realloc(g_tls_issued, nc * sizeof *n);
        if (!n) return 0;
        g_tls_issued = n; g_tls_capissued = nc;
    }
    g_tls_issued[g_tls_nissued++] = p;
    return 1;
}

static int tls_unissue(const void *p)
{
    for (size_t i = 0; p && i < g_tls_nissued; i++)
        if (g_tls_issued[i] == p) {
            g_tls_issued[i] = g_tls_issued[--g_tls_nissued];
            return 1;
        }
    return 0;
}

static char *dup_arg(int64_t s)
{
    const char *p = (const char *)(intptr_t)s;
    return strdup(p ? p : "");
}

static const char *as_str(int64_t s)
{
    return (const char *)(intptr_t)s;
}

/* A $TlsConn handle (a TlsConn *, 135.30) as the core's TlsConn value. */
static TlsConn conn_of(int64_t h)
{
    TlsConn c;
    c.id = h ? ((const TlsConn *)(intptr_t)h)->id : NULL;
    return c;
}

/* box_conn — the $TlsConn record for a connection id; 0 on OOM. */
static int64_t box_conn(char *id)
{
    TlsConn *b = malloc(sizeof *b);
    if (!b) return 0;
    b->id = seal_take(id);   /* 127.295: .id is a str toke can read */
    return (int64_t)(intptr_t)b;
}

static const char *nonempty(const char *s)
{
    return (s && *s) ? s : NULL;
}

/* An absent config handle is a valid, empty configuration: no local
 * certificate, no pin, no mutual auth -- so a client verifies the server
 * against the default trust store and the host name (121.64).  That is what
 * tls.connect against a public server wants.  135.30: read from the
 * $TlsConfig record, whether a constructor below or a literal built it; ""
 * means absent, and a literal that omits `insecure` leaves it false. */
static TlsConfig cfg_of(int64_t h)
{
    TlsConfig c;
    const TkTlsCfgRec *r = (const TkTlsCfgRec *)(intptr_t)h;
    c.cert_pem       = r ? nonempty(r->certpem) : NULL;
    c.key_pem        = r ? nonempty(r->keypem) : NULL;
    c.peer_cert_pem  = r ? nonempty(r->peercertpem) : NULL;
    c.require_mutual = (r && r->requiremutual) ? 1 : 0;
    c.insecure       = (r && r->insecure) ? 1 : 0;
    return c;
}

static int64_t make_cfg(int64_t cert, int64_t key, int64_t peer, int mutual,
                        int insecure)
{
    TkTlsCfgRec *c = calloc(1, sizeof *c);
    char *ce = dup_arg(cert), *ke = dup_arg(key), *pe = dup_arg(peer);
    if (!c || !ce || !ke || !pe || !tls_issue(c)) {
        free(c); free(ce); free(ke); free(pe);
        return 0;
    }
    c->certpem       = ce;
    c->keypem        = ke;
    c->peercertpem   = pe;
    c->requiremutual = mutual ? 1 : 0;
    c->insecure      = insecure ? 1 : 0;
    return (int64_t)(intptr_t)c;
}

/* ── configuration ───────────────────────────────────────────────────────── */

/* tls.tlsconfig(certpem; keypem) — a one-way TLS configuration.
 *
 * The arguments are PEM *text*, matching TlsConfig.cert_pem / key_pem and what
 * tls.genselfsigned hands back.  They were named cert_path/key_path here while
 * the body discarded them, so nothing depended on the other reading. */
int64_t tk_tls_tlsconfig_w(int64_t cert_pem, int64_t key_pem)
{
    return make_cfg(cert_pem, key_pem, 0, 0, 0);
}

/* tls.mutualconfig(certpem; keypem; peercertpem) — mutual TLS.
 *
 * Demands a certificate from the peer (SSL_VERIFY_FAIL_IF_NO_PEER_CERT on the
 * server side) and pins the peer to `peercertpem`.  This constructor exists so
 * that require_mutual and peer_cert_pem never have to be named by a caller
 * (136.46). */
int64_t tk_tls_mutualconfig_w(int64_t cert_pem, int64_t key_pem,
                              int64_t peer_cert_pem)
{
    return make_cfg(cert_pem, key_pem, peer_cert_pem, 1, 0);
}

/* tls.pinconfig(certpem; keypem; peercertpem) — pin the peer certificate
 * without demanding one.  The client half of a pinned connection. */
int64_t tk_tls_pinconfig_w(int64_t cert_pem, int64_t key_pem,
                           int64_t peer_cert_pem)
{
    return make_cfg(cert_pem, key_pem, peer_cert_pem, 0, 0);
}

/* tls.insecureconfig(certpem; keypem) — 121.64: a client that does NOT
 * authenticate the server (no chain, no host name, no pin).  Until 121.64
 * this was what tls.tlsconfig silently did on a client; it is now a named,
 * greppable opt-out.  On a server it is the same as tls.tlsconfig. */
int64_t tk_tls_insecureconfig_w(int64_t cert_pem, int64_t key_pem)
{
    return make_cfg(cert_pem, key_pem, 0, 0, 1);
}

/* tls.freeconfig(cfg) — release a configuration handle. */
int64_t tk_tls_freeconfig_w(int64_t cfg)
{
    TkTlsCfgRec *c = (TkTlsCfgRec *)(intptr_t)cfg;
    if (!c || !tls_unissue(c)) return 0;   /* 135.30: not ours to free */
    free((void *)c->certpem);
    free((void *)c->keypem);
    free((void *)c->peercertpem);
    free(c);
    return 1;
}

/* ── self-signed material ────────────────────────────────────────────────── */

/*
 * 127.104: the two keygen calls are `TlsKeypair!str` (stdlib/tls.tki).
 *
 * The interface used to say `TlsKeypair!TlsErr` with four variants
 * (CertErr/ConnErr/PinErr/IoErr) while these wrappers returned a bare 0 and
 * threw away the message tls.c produces.  Measured: every failure the core can
 * report here is a certificate-generation failure with a static message
 * ("unsupported key_alg ...", an OpenSSL reason string); ConnErr/PinErr/IoErr
 * are returned by no function in the module, and the variant names were not
 * even writable in the default syntax.  So the error is the message, as for
 * std.image (runtime-abi.md §7.3): on failure the slot holds the message
 * pointer and the value is 0; on success the slot is 0.  Both wrappers set
 * the slot on every return, which is what lets llvm.c's
 * is_stdlib_slot_wrapper() list them.
 */
static int64_t tls_keypair_or_raise(TlsKeypairResult r)
{
    if (r.is_err) {
        tk_current_error = (int64_t)(intptr_t)(r.err_msg && r.err_msg[0]
                                               ? r.err_msg
                                               : "tls: key generation failed");
        return 0;
    }
    TkTlsKeypairRec *kp = calloc(1, sizeof *kp);
    if (!kp || !tls_issue(kp)) {
        free(kp); free(r.cert_pem); free(r.key_pem);
        tk_current_error = (int64_t)(intptr_t)"tls: out of memory";
        return 0;
    }
    kp->certpem = r.cert_pem ? r.cert_pem : strdup("");
    kp->keypem  = r.key_pem ? r.key_pem : strdup("");
    tk_current_error = 0;
    return (int64_t)(intptr_t)kp;
}

/* tls.genselfsigned(commonname; validdays) — P-384 self-signed keypair.
 *
 * Returns a keypair handle; on failure 0, with the reason in the error slot.
 * Arity 2 matches stdlib/tls.tki ("str", "i32") and tls_gen_self_signed. */
int64_t tk_tls_genselfsigned_w(int64_t common_name, int64_t valid_days)
{
    const char *cn = as_str(common_name);
    if (!cn || *cn == '\0') {
        tk_current_error = (int64_t)(intptr_t)"common_name must not be empty";
        return 0;
    }

    int32_t days = (int32_t)valid_days;
    if (days <= 0) days = 365;

    return tls_keypair_or_raise(tls_gen_self_signed(cn, days));
}

/*
 * tls.genselfsignedalg(commonname; validdays; keyalg) — the same keypair with
 * the signing algorithm chosen: "ecdsa-p384" (the default, broad interop) or
 * "ml-dsa-65" (post-quantum, FIPS 204, OpenSSL 3.5+).
 *
 * WHY THIS EXISTS SEPARATELY FROM tls.genselfsigned.  tls_gen_self_signed_alg
 * has been implemented and linked in tls.c since 124.1 shipped ADR-0013's
 * crypto agility, and it had NO wrapper and NO .tki export, so ML-DSA-65
 * certificate generation was unreachable from toke (136.44a).
 *
 * The core FAILS CLOSED and this wrapper must not soften that: an unsupported
 * algorithm name is an error, and an ML-DSA keygen failure is an error.
 * Neither falls back to P-384.  A classical certificate silently returned in
 * place of a post-quantum one is the worst outcome available here, because the
 * caller asked for ML-DSA precisely because it matters to them.
 *
 * FAILURE CONVENTION (127.104): 0, with the core's message in the error slot
 * — see tls_keypair_or_raise.  The other std.tls wrappers are not fallible
 * in the interface (bool / optional returns) and keep their 0 sentinel.
 */
int64_t tk_tls_genselfsignedalg_w(int64_t common_name, int64_t valid_days,
                                  int64_t key_alg)
{
    const char *cn = as_str(common_name);
    if (!cn || *cn == '\0') {
        tk_current_error = (int64_t)(intptr_t)"common_name must not be empty";
        return 0;
    }

    int32_t days = (int32_t)valid_days;
    if (days <= 0) days = 365;

    /* A null or empty algorithm means "the default", which is what the core
     * documents for a NULL key_alg — not an error. */
    const char *alg = as_str(key_alg);
    if (alg && *alg == '\0') alg = NULL;

    return tls_keypair_or_raise(tls_gen_self_signed_alg(cn, days, alg));
}

/* tls.certof(keypair) — the certificate PEM (public material). */
int64_t tk_tls_certof_w(int64_t keypair)
{
    TkTlsKeypairRec *kp = (TkTlsKeypairRec *)(intptr_t)keypair;
    if (!kp || !kp->certpem) return (int64_t)(intptr_t)"";
    /* 127.295: a sealed copy -- public material, and the str no longer
     * dangles once tls.freekeypair frees the record's own PEM. */
    char *c = tks_strdup(kp->certpem);
    return (int64_t)(intptr_t)(c ? c : kp->certpem);
}

/* tls.keyof(keypair) — the private key PEM.  Secret: do not log it. */
int64_t tk_tls_keyof_w(int64_t keypair)
{
    TkTlsKeypairRec *kp = (TkTlsKeypairRec *)(intptr_t)keypair;
    if (!kp || !kp->keypem) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)kp->keypem;
}

/* tls.freekeypair(keypair) — wipe and release the keypair, private key
 * included. */
int64_t tk_tls_freekeypair_w(int64_t keypair)
{
    TkTlsKeypairRec *kp = (TkTlsKeypairRec *)(intptr_t)keypair;
    if (!kp || !tls_unissue(kp)) return 0;   /* 135.30: not ours to free */
    if (kp->keypem) {
        /* Overwrite through a volatile view so the store is not elided. */
        volatile char *w = (volatile char *)kp->keypem;
        size_t n = tk_strlen(kp->keypem);
        while (n--) *w++ = 0;
        free(kp->keypem);
    }
    free(kp->certpem);
    free(kp);
    return 1;
}

/* ── connections ─────────────────────────────────────────────────────────── */

/* tls.connect(host; port; cfg) — TLS 1.3 handshake to host:port.
 *
 * Returns the connection id string, or 0 when the connection, the handshake or
 * the certificate pin fails.  Renamed from tk_tls_connecttls_w: `tls.connect`
 * mangles to this name, and the old spelling meant the published entry point
 * resolved to nothing. */
int64_t tk_tls_connect_w(int64_t host, int64_t port, int64_t cfg)
{
    TK_REQUIRE(TK_CAP_NET);
    const char *h = as_str(host);
    if (!h || *h == '\0') return 0;

    TlsConnResult r = tls_connect(h, (int32_t)port, cfg_of(cfg));
    if (r.is_none) return 0;
    return box_conn(r.conn.id);   /* 135.30: a $TlsConn record */
}

/* call_tls_handler — invoke a toke connection handler with the conn id.
 * `&handler` lowers to a raw function pointer, not a [fn,env] closure pair,
 * so it is called directly (same convention as router_glue.c). */
typedef int64_t (*TlsConnFn1)(int64_t);

/* tls_listen's callback type carries no user data, and tls_listen never
 * returns while the listener is healthy, so one process-wide handler is the
 * whole of the state.  It is written once before the accept loop starts and
 * only read afterwards. */
static int64_t g_tls_handler = 0;

static void tls_handler_trampoline(TlsConn conn)
{
    int64_t fn = g_tls_handler;
    if (!fn) return;
    int64_t rec = box_conn(conn.id);   /* 135.30: the handler takes $TlsConn */
    if (rec) ((TlsConnFn1)(intptr_t)fn)(rec);
}

/* tls.listen(port; cfg; handler) — bind, accept, handshake, dispatch.
 *
 * Does not return while the listener is healthy; returns 0 if the socket or
 * the TLS context could not be created.  Renamed from tk_tls_listentls_w for
 * the same reason as connect, and — unlike the stub it replaces — it now
 * actually binds a socket. */
int64_t tk_tls_listen_w(int64_t port, int64_t cfg, int64_t handler)
{
    TK_REQUIRE(TK_CAP_NET);
    if (!handler) return 0;
    g_tls_handler = handler;
    return tls_listen((int32_t)port, cfg_of(cfg), tls_handler_trampoline);
}

/* tls.read(conn) — read available plaintext; 0 on EOF or error. */
int64_t tk_tls_read_w(int64_t conn)
{
    TlsStrResult r = tls_read(conn_of(conn));
    if (r.is_none) return 0;
    return (int64_t)(intptr_t)seal_take(r.ok);
}

/* tls.write(conn; data) — write plaintext; 1 on success. */
int64_t tk_tls_write_w(int64_t conn, int64_t data)
{
    const char *d = as_str(data);
    if (!d) return 0;
    return tls_write(conn_of(conn), d);
}

/* tls.close(conn) — clean TLS shutdown plus socket close. */
int64_t tk_tls_close_w(int64_t conn)
{
    return tls_close(conn_of(conn));
}

/* ── peer identity ───────────────────────────────────────────────────────── */

/* tls.peercert(conn) — the peer's certificate PEM, 0 if it presented none.
 * Declared in tls.tki since the module existed; it never had a wrapper. */
int64_t tk_tls_peercert_w(int64_t conn)
{
    TlsStrResult r = tls_peer_cert(conn_of(conn));
    if (r.is_none) return 0;
    return (int64_t)(intptr_t)seal_take(r.ok);
}

/* tls.fingerprint(pem) — lowercase hex SHA-256 of the certificate's DER.
 * Declared in tls.tki; it never had a wrapper. */
int64_t tk_tls_fingerprint_w(int64_t pem)
{
    const char *p = as_str(pem);
    if (!p) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)seal_take(tls_fingerprint(p));
}

/* tls.pairingcode(conn) — 6-digit code from the two fingerprints.
 * Declared in tls.tki; it never had a wrapper. */
int64_t tk_tls_pairingcode_w(int64_t conn)
{
    return (int64_t)(intptr_t)seal_take(tls_pairing_code(conn_of(conn)));
}

/* tls.group(conn) — the key-exchange group actually negotiated, e.g.
 * "X25519MLKEM768", or the classical group a fallback settled on (122.13). */
int64_t tk_tls_group_w(int64_t conn)
{
    return (int64_t)(intptr_t)seal_take(tls_group(conn_of(conn)));
}

/* tls.protocol(conn) — the protocol actually negotiated, e.g. "TLSv1.3".
 * Makes the version an observable fact rather than a claim about the source
 * (136.44). */
int64_t tk_tls_protocol_w(int64_t conn)
{
    return (int64_t)(intptr_t)seal_take(tls_protocol(conn_of(conn)));
}
