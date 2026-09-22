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
 *   TlsConfig   -> pointer to a heap TkTlsCfg that owns copies of its PEMs
 *   TlsKeypair  -> pointer to a heap TkTlsKeypair that owns both PEMs
 *   TlsConn     -> the connection's id string (char *), as tls.c mints it
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
 * ever has to spell an unspellable field.
 */

#include "tls.h"
#include "capabilities.h"   /* 124.4c: net capability gate */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── handle plumbing ─────────────────────────────────────────────────────── */

typedef struct {
    TlsConfig cfg;      /* points into the three fields below */
    char     *cert;
    char     *key;
    char     *peer;
} TkTlsCfg;

typedef struct {
    char *cert;
    char *key;
} TkTlsKeypair;

static char *dup_arg(int64_t s)
{
    const char *p = (const char *)(intptr_t)s;
    if (!p || *p == '\0') return NULL;
    return strdup(p);
}

static const char *as_str(int64_t s)
{
    return (const char *)(intptr_t)s;
}

static TlsConn conn_of(int64_t h)
{
    TlsConn c;
    c.id = (char *)(intptr_t)h;
    return c;
}

/* An absent config handle is a valid, empty configuration: no local
 * certificate, no pin, no mutual auth.  That is what tls.connect against a
 * public server wants. */
static TlsConfig cfg_of(int64_t h)
{
    TlsConfig empty;
    empty.cert_pem       = NULL;
    empty.key_pem        = NULL;
    empty.peer_cert_pem  = NULL;
    empty.require_mutual = 0;
    if (!h) return empty;
    return ((TkTlsCfg *)(intptr_t)h)->cfg;
}

static int64_t make_cfg(int64_t cert, int64_t key, int64_t peer, int mutual)
{
    TkTlsCfg *c = calloc(1, sizeof *c);
    if (!c) return 0;
    c->cert = dup_arg(cert);
    c->key  = dup_arg(key);
    c->peer = dup_arg(peer);
    c->cfg.cert_pem       = c->cert;
    c->cfg.key_pem        = c->key;
    c->cfg.peer_cert_pem  = c->peer;
    c->cfg.require_mutual = mutual;
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
    return make_cfg(cert_pem, key_pem, 0, 0);
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
    return make_cfg(cert_pem, key_pem, peer_cert_pem, 1);
}

/* tls.pinconfig(certpem; keypem; peercertpem) — pin the peer certificate
 * without demanding one.  The client half of a pinned connection. */
int64_t tk_tls_pinconfig_w(int64_t cert_pem, int64_t key_pem,
                           int64_t peer_cert_pem)
{
    return make_cfg(cert_pem, key_pem, peer_cert_pem, 0);
}

/* tls.freeconfig(cfg) — release a configuration handle. */
int64_t tk_tls_freeconfig_w(int64_t cfg)
{
    TkTlsCfg *c = (TkTlsCfg *)(intptr_t)cfg;
    if (!c) return 0;
    free(c->cert);
    free(c->key);
    free(c->peer);
    free(c);
    return 1;
}

/* ── self-signed material ────────────────────────────────────────────────── */

/* tls.genselfsigned(commonname; validdays) — P-384 self-signed keypair.
 *
 * Returns a keypair handle, or 0 on failure.  Arity 2 matches stdlib/tls.tki
 * ("str", "i32") and tls_gen_self_signed; the old arity-1 stub discarded its
 * one argument, so there was no behaviour to preserve. */
int64_t tk_tls_genselfsigned_w(int64_t common_name, int64_t valid_days)
{
    const char *cn = as_str(common_name);
    if (!cn || *cn == '\0') return 0;

    int32_t days = (int32_t)valid_days;
    if (days <= 0) days = 365;

    TlsKeypairResult r = tls_gen_self_signed(cn, days);
    if (r.is_err) return 0;

    TkTlsKeypair *kp = calloc(1, sizeof *kp);
    if (!kp) { free(r.cert_pem); free(r.key_pem); return 0; }
    kp->cert = r.cert_pem;
    kp->key  = r.key_pem;
    return (int64_t)(intptr_t)kp;
}

/* tls.certof(keypair) — the certificate PEM (public material). */
int64_t tk_tls_certof_w(int64_t keypair)
{
    TkTlsKeypair *kp = (TkTlsKeypair *)(intptr_t)keypair;
    if (!kp || !kp->cert) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)kp->cert;
}

/* tls.keyof(keypair) — the private key PEM.  Secret: do not log it. */
int64_t tk_tls_keyof_w(int64_t keypair)
{
    TkTlsKeypair *kp = (TkTlsKeypair *)(intptr_t)keypair;
    if (!kp || !kp->key) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)kp->key;
}

/* tls.freekeypair(keypair) — wipe and release the keypair, private key
 * included. */
int64_t tk_tls_freekeypair_w(int64_t keypair)
{
    TkTlsKeypair *kp = (TkTlsKeypair *)(intptr_t)keypair;
    if (!kp) return 0;
    if (kp->key) {
        /* Overwrite through a volatile view so the store is not elided. */
        volatile char *w = (volatile char *)kp->key;
        size_t n = strlen(kp->key);
        while (n--) *w++ = 0;
        free(kp->key);
    }
    free(kp->cert);
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
    return (int64_t)(intptr_t)r.conn.id;
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
    ((TlsConnFn1)(intptr_t)fn)((int64_t)(intptr_t)conn.id);
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
    return (int64_t)(intptr_t)r.ok;
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
    return (int64_t)(intptr_t)r.ok;
}

/* tls.fingerprint(pem) — lowercase hex SHA-256 of the certificate's DER.
 * Declared in tls.tki; it never had a wrapper. */
int64_t tk_tls_fingerprint_w(int64_t pem)
{
    const char *p = as_str(pem);
    if (!p) return (int64_t)(intptr_t)"";
    return (int64_t)(intptr_t)tls_fingerprint(p);
}

/* tls.pairingcode(conn) — 6-digit code from the two fingerprints.
 * Declared in tls.tki; it never had a wrapper. */
int64_t tk_tls_pairingcode_w(int64_t conn)
{
    return (int64_t)(intptr_t)tls_pairing_code(conn_of(conn));
}

/* tls.protocol(conn) — the protocol actually negotiated, e.g. "TLSv1.3".
 * Makes the version an observable fact rather than a claim about the source
 * (136.44). */
int64_t tk_tls_protocol_w(int64_t conn)
{
    return (int64_t)(intptr_t)tls_protocol(conn_of(conn));
}
