/*
 * tls.c — Implementation of the std.tls standard library module.
 *
 * Provides standalone TLS 1.3 connections using OpenSSL.
 *
 * Features:
 *   - P-384 self-signed certificate generation (EVP_PKEY_EC / NID_secp384r1)
 *   - TLS 1.3 only (SSL_CTX_set_min_proto_version TLS1_3_VERSION)
 *   - Mutual TLS (SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT)
 *   - Certificate pinning (peer_cert_pem match on connect/accept)
 *   - Client verification by default (121.64): chain against the default
 *     trust store + host name, unless pinned or explicitly insecure
 *   - Per-connection thread dispatch in tls_listen
 *   - SHA-256 certificate fingerprinting (DER encoding)
 *   - 6-digit pairing codes (XOR-derived from local + peer fingerprints)
 *
 * malloc is permitted: stdlib boundary, not arena-managed compiler code.
 * Callers own all returned heap pointers.
 *
 * Thread safety: each connection uses its own SSL object; the connection
 * registry is protected by a pthread mutex.
 *
 * Story: 72.9
 */

#include "tls.h"
#include "capabilities.h"   /* 124.4c2: net capability gate */
#include "tk_bind.h"       /* 135.34 (3): bind without shadowing a live listener */

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/sha.h>
#include <openssl/objects.h>
#include <openssl/asn1.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Connection registry
 *
 * Maps an opaque string id to a live SSL* + fd pair.  Protected by a mutex.
 * Maximum concurrent connections: TLS_MAX_CONNS.
 * ========================================================================= */

#define TLS_MAX_CONNS 1024

/* 121.19 (TLS-03, TLS-06).  The registry used to hand callers a pointer to
 * an entry while holding ONE process-wide mutex, and every operation held it
 * across its OpenSSL call -- including tls_read's blocking SSL_read.  Two
 * defects followed, both measured on main (C590):
 *   - tls_close read ssl/fd, dropped the lock, SSL_free'd, and only then
 *     removed the entry, so a concurrent tls_read/tls_write on the same id
 *     could find the entry and use the freed SSL* (the audit's UAF window);
 *   - a tls_read waiting for data held the global mutex, so every other
 *     connection's read/write/close -- and tls_close of the connection
 *     being read -- blocked behind it until the peer spoke.
 * Now: the global mutex guards the table only.  Each entry has its own `io`
 * mutex serialising OpenSSL calls on that SSL* (an SSL object is not safe
 * for concurrent use), and a reference count.  A caller acquires the entry
 * (refs++), takes io, uses the SSL*, drops io, releases.  tls_close marks
 * the entry closing -- no new acquire can find it -- wakes a blocked reader
 * by shutting the socket down, and the LAST release frees the SSL* and the
 * slot.  Nothing frees an SSL* another thread holds.
 *
 * The id was "%d:%p" -- the live SSL* address, a heap pointer disclosed to
 * any caller that logs the id (TLS-06).  It is now the slot index and a
 * process-wide generation counter, which also stops a stale id from naming
 * a reused slot. */
typedef struct {
    char   id[64];
    SSL   *ssl;
    int    fd;
    int    in_use;
    int    closing;   /* tls_close has run; acquire refuses the entry */
    int    refs;      /* acquirers currently holding the entry */
    int    io_init;
    pthread_mutex_t io;   /* serialises OpenSSL calls on ssl */
    /* Local cert PEM for pairing_code (may be NULL) */
    char  *local_cert_pem;
} ConnEntry;

static ConnEntry  g_conns[TLS_MAX_CONNS];
static pthread_mutex_t g_conns_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long g_conn_gen = 0;

/* Register a new connection; returns a heap-allocated id string or NULL. */
static char *registry_add(SSL *ssl, int fd, const char *local_cert_pem)
{
    pthread_mutex_lock(&g_conns_mu);
    for (int i = 0; i < TLS_MAX_CONNS; i++) {
        if (!g_conns[i].in_use) {
            if (!g_conns[i].io_init) {
                if (pthread_mutex_init(&g_conns[i].io, NULL) != 0) break;
                g_conns[i].io_init = 1;
            }
            snprintf(g_conns[i].id, sizeof(g_conns[i].id),
                     "%d:%llu", i, ++g_conn_gen);
            char *out = strdup(g_conns[i].id);
            char *pem = local_cert_pem ? strdup(local_cert_pem) : NULL;
            if (!out || (local_cert_pem && !pem)) {
                free(out); free(pem);
                memset(g_conns[i].id, 0, sizeof(g_conns[i].id));
                break;
            }
            g_conns[i].ssl      = ssl;
            g_conns[i].fd       = fd;
            g_conns[i].in_use   = 1;
            g_conns[i].closing  = 0;
            g_conns[i].refs     = 0;
            g_conns[i].local_cert_pem = pem;
            pthread_mutex_unlock(&g_conns_mu);
            return out;
        }
    }
    pthread_mutex_unlock(&g_conns_mu);
    return NULL;
}

/* registry_acquire -- find a live (not closing) entry by id and take a
 * reference.  Returns NULL if there is none.  The caller must pair it with
 * registry_release(), and must hold e->io around any use of e->ssl. */
static ConnEntry *registry_acquire(const char *id)
{
    if (!id) return NULL;
    ConnEntry *found = NULL;
    pthread_mutex_lock(&g_conns_mu);
    for (int i = 0; i < TLS_MAX_CONNS; i++) {
        if (g_conns[i].in_use && !g_conns[i].closing &&
            strcmp(g_conns[i].id, id) == 0) {
            g_conns[i].refs++;
            found = &g_conns[i];
            break;
        }
    }
    pthread_mutex_unlock(&g_conns_mu);
    return found;
}

/* Free the connection behind a closing entry and clear the slot.  Called
 * with g_conns_mu held, by whoever drops the last reference. */
static void registry_finalize_locked(ConnEntry *e)
{
    if (e->ssl) SSL_free(e->ssl);
    if (e->fd >= 0) close(e->fd);
    free(e->local_cert_pem);
    e->local_cert_pem = NULL;
    memset(e->id, 0, sizeof(e->id));
    e->ssl     = NULL;
    e->fd      = -1;
    e->closing = 0;
    e->refs    = 0;
    e->in_use  = 0;
}

static void registry_release(ConnEntry *e)
{
    pthread_mutex_lock(&g_conns_mu);
    if (--e->refs == 0 && e->closing) registry_finalize_locked(e);
    pthread_mutex_unlock(&g_conns_mu);
}

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Return the last OpenSSL error string (static buffer, NOT thread-safe for
 * diagnostic use only). */
static const char *ssl_err_str(void)
{
    unsigned long e = ERR_get_error();
    if (e == 0) return "unknown OpenSSL error";
    return ERR_reason_error_string(e);
}

/*
 * pem_to_x509 — parse a PEM string into an X509 object.
 * Returns NULL on failure.  Caller owns the returned X509*.
 */
static X509 *pem_to_x509(const char *pem)
{
    if (!pem || *pem == '\0') return NULL;
    BIO *bio = BIO_new_mem_buf(pem, -1);
    if (!bio) return NULL;
    X509 *cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
    BIO_free(bio);
    return cert;
}

/*
 * x509_fingerprint_hex — compute SHA-256 of DER encoding of cert.
 * Output: 64-character lowercase hex string + NUL (65 bytes).
 * buf must be at least 65 bytes.  Returns 0 on success, -1 on error.
 */
static int x509_fingerprint_hex(X509 *cert, char buf[65])
{
    /* 121.10 (TLS-01): size the DER buffer to the actual encoding length
     * instead of a fixed 8 KiB stack buffer that a larger cert overflows.
     * i2d_X509(cert, NULL) returns the required length. */
    int der_len = i2d_X509(cert, NULL);
    if (der_len <= 0) return -1;
    unsigned char *der = (unsigned char *)malloc((size_t)der_len);
    if (!der) return -1;
    unsigned char *p = der;
    if (i2d_X509(cert, &p) != der_len) { free(der); return -1; }

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(der, (size_t)der_len, digest);
    free(der);

    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        snprintf(buf + i * 2, 3, "%02x", digest[i]);
    buf[64] = '\0';
    return 0;
}

/*
 * build_ssl_ctx — create and configure an SSL_CTX for TLS 1.3.
 *
 * is_server:  1 = server role, 0 = client role
 * cfg:        caller-provided TlsConfig
 *
 * Returns NULL on error (caller should inspect stderr / ERR queue).
 */
static SSL_CTX *build_ssl_ctx(int is_server, TlsConfig cfg)
{
    const SSL_METHOD *method = is_server ? TLS_server_method()
                                         : TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) return NULL;

    /* TLS 1.3 only */
    if (SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION) != 1) {
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* Post-quantum hybrid key exchange (ADR-0013 crypto agility).
     * Prefer X25519MLKEM768 (NIST ML-KEM-768 + X25519) so the TLS 1.3
     * handshake secret is quantum-resistant against "harvest-now,
     * decrypt-later" attacks, while classical groups are retained as
     * fallback for interop with peers that don't offer the hybrid group.
     * If the linked OpenSSL is too old to know the hybrid group
     * (< 3.5), degrade gracefully to classical groups rather than
     * failing the context. */
    if (SSL_CTX_set1_groups_list(ctx,
            "X25519MLKEM768:X25519:secp256r1:x448:secp384r1") != 1) {
        if (SSL_CTX_set1_groups_list(ctx,
                "X25519:secp256r1:x448:secp384r1") != 1) {
            SSL_CTX_free(ctx);
            return NULL;
        }
    }

    /* Load local certificate + key (if provided) */
    if (cfg.cert_pem && *cfg.cert_pem != '\0') {
        BIO *bio = BIO_new_mem_buf(cfg.cert_pem, -1);
        X509 *cert = bio ? PEM_read_bio_X509(bio, NULL, NULL, NULL) : NULL;
        BIO_free(bio);
        if (!cert || SSL_CTX_use_certificate(ctx, cert) != 1) {
            X509_free(cert);
            SSL_CTX_free(ctx);
            return NULL;
        }
        X509_free(cert);
    }

    if (cfg.key_pem && *cfg.key_pem != '\0') {
        BIO *bio = BIO_new_mem_buf(cfg.key_pem, -1);
        EVP_PKEY *pkey = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL)
                              : NULL;
        BIO_free(bio);
        if (!pkey || SSL_CTX_use_PrivateKey(ctx, pkey) != 1) {
            EVP_PKEY_free(pkey);
            SSL_CTX_free(ctx);
            return NULL;
        }
        EVP_PKEY_free(pkey);
    }

    /* Mutual TLS */
    if (cfg.require_mutual) {
        SSL_CTX_set_verify(ctx,
            SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    }

    /* Load peer cert as trusted CA for cert pinning / mTLS */
    if (cfg.peer_cert_pem && *cfg.peer_cert_pem != '\0') {
        X509 *peer = pem_to_x509(cfg.peer_cert_pem);
        if (!peer) { SSL_CTX_free(ctx); return NULL; }

        X509_STORE *store = SSL_CTX_get_cert_store(ctx);
        if (!store || X509_STORE_add_cert(store, peer) != 1) {
            X509_free(peer);
            SSL_CTX_free(ctx);
            return NULL;
        }
        X509_free(peer);

        /* Verify peer cert.
         *
         * 136.44: this used to be an unconditional
         *     SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
         * which CLEARED the SSL_VERIFY_FAIL_IF_NO_PEER_CERT bit set a few
         * lines above whenever a peer certificate was also pinned -- that is,
         * in the ordinary mutual-TLS configuration.  On a server,
         * SSL_VERIFY_PEER alone does not require the client to send a
         * certificate at all.
         *
         * MEASURED, NOT ASSUMED: that did NOT open the door, because
         * accept_thread runs verify_pin() after the handshake and a pinned
         * peer with no certificate fails it -- so an anonymous client was
         * still rejected.  What it cost was WHERE the rejection happened: the
         * server completed a full TLS handshake with an unauthenticated peer
         * and only then dropped it, instead of refusing during the handshake.
         * Combining the two settings restores the intent and moves the
         * rejection back into the handshake, where it belongs. */
        int verify_mode = SSL_VERIFY_PEER;
        if (cfg.require_mutual) verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
        SSL_CTX_set_verify(ctx, verify_mode, NULL);
    } else if (!is_server && !cfg.insecure) {
        /* 121.64 (TLS-02): a client with no pin verifies the server by
         * default -- chain against the default trust store (SSL_CERT_FILE /
         * SSL_CERT_DIR override it, as in llm.c since 121.23), and the host
         * name, which tls_connect sets on the SSL once it knows it.  This
         * branch used to be SSL_VERIFY_NONE for every unpinned client, so
         * tls.connect authenticated nothing unless the caller pinned. */
        if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
            SSL_CTX_free(ctx);
            return NULL;
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    } else if (!cfg.require_mutual) {
        /* A server that neither pins nor demands a client certificate, or a
         * client that opted out with tls.insecureconfig: verify nothing. */
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    }

    return ctx;
}

/*
 * verify_pin — after handshake, check the peer cert against the pinned PEM.
 * Returns 1 if pinning passes (or no pin configured), 0 on mismatch.
 */
static int verify_pin(SSL *ssl, const char *peer_cert_pem)
{
    if (!peer_cert_pem || *peer_cert_pem == '\0') return 1;

    X509 *peer_actual = SSL_get_peer_certificate(ssl);
    if (!peer_actual) return 0;

    X509 *peer_expected = pem_to_x509(peer_cert_pem);
    if (!peer_expected) { X509_free(peer_actual); return 0; }

    int match = (X509_cmp(peer_actual, peer_expected) == 0);
    X509_free(peer_actual);
    X509_free(peer_expected);
    return match;
}

/* =========================================================================
 * tls_conn_free_id
 * ========================================================================= */

void tls_conn_free_id(TlsConn conn)
{
    free(conn.id);
}

/* =========================================================================
 * tls_gen_self_signed
 * ========================================================================= */

/* key_alg selects the certificate's signature key:
 *   NULL or "ecdsa-p384" -> classical P-384 ECDSA (default, broad interop)
 *   "ml-dsa-65"          -> post-quantum ML-DSA-65 (FIPS 204, level 3;
 *                           OpenSSL 3.5+; won't verify with classical-only peers)
 * ADR-0013(d): additive/opt-in — the authentication counterpart to the hybrid
 * X25519MLKEM768 KEX (crypto agility, no default breakage). */
TlsKeypairResult tls_gen_self_signed_alg(const char *common_name,
                                          int32_t     valid_days,
                                          const char *key_alg)
{
    TlsKeypairResult result = {NULL, NULL, 0, NULL};

    if (!common_name || *common_name == '\0') {
        result.is_err  = 1;
        result.err_msg = "common_name must not be empty";
        return result;
    }
    if (valid_days <= 0) {
        result.is_err  = 1;
        result.err_msg = "valid_days must be positive";
        return result;
    }

    int use_mldsa = 0;
    if (key_alg && *key_alg) {
        if (strcmp(key_alg, "ml-dsa-65") == 0)      use_mldsa = 1;
        else if (strcmp(key_alg, "ecdsa-p384") != 0) {
            result.is_err  = 1;
            result.err_msg = "unsupported key_alg (want ecdsa-p384 or ml-dsa-65)";
            return result;
        }
    }

    /* Generate the signing key: P-384 EC (default) or ML-DSA-65 (opt-in). */
    EVP_PKEY *pkey = NULL;
    if (use_mldsa) {
        EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_from_name(NULL, "ML-DSA-65", NULL);
        if (!kctx) {
            result.is_err  = 1;
            result.err_msg = ssl_err_str();
            return result;
        }
        if (EVP_PKEY_keygen_init(kctx) != 1 ||
            EVP_PKEY_keygen(kctx, &pkey) != 1) {
            EVP_PKEY_CTX_free(kctx);
            result.is_err  = 1;
            result.err_msg = ssl_err_str();
            return result;
        }
        EVP_PKEY_CTX_free(kctx);
    } else {
        EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
        if (!kctx) {
            result.is_err  = 1;
            result.err_msg = ssl_err_str();
            return result;
        }
        if (EVP_PKEY_keygen_init(kctx) != 1 ||
            EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx, NID_secp384r1) != 1 ||
            EVP_PKEY_keygen(kctx, &pkey) != 1) {
            EVP_PKEY_CTX_free(kctx);
            result.is_err  = 1;
            result.err_msg = ssl_err_str();
            return result;
        }
        EVP_PKEY_CTX_free(kctx);
    }

    /* Build self-signed X.509 certificate */
    X509 *cert = X509_new();
    if (!cert) {
        EVP_PKEY_free(pkey);
        result.is_err  = 1;
        result.err_msg = "X509_new failed";
        return result;
    }

    /* Version 3 */
    X509_set_version(cert, 2);

    /* Serial number = 1 */
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);

    /* Validity window */
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), (long)valid_days * 86400L);

    /* Subject / issuer */
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (const unsigned char *)common_name, -1, -1, 0);
    X509_set_issuer_name(cert, name); /* self-signed */

    /* Public key */
    X509_set_pubkey(cert, pkey);

    /* Sign with the private key. ML-DSA is not hash-then-sign (NULL digest);
     * P-384 ECDSA pairs with SHA-384. */
    if (X509_sign(cert, pkey, use_mldsa ? NULL : EVP_sha384()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        result.is_err  = 1;
        result.err_msg = ssl_err_str();
        return result;
    }

    /* Serialise certificate to PEM */
    {
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio || PEM_write_bio_X509(bio, cert) != 1) {
            BIO_free(bio);
            X509_free(cert);
            EVP_PKEY_free(pkey);
            result.is_err  = 1;
            result.err_msg = "PEM_write_bio_X509 failed";
            return result;
        }
        BUF_MEM *bptr = NULL;
        BIO_get_mem_ptr(bio, &bptr);
        result.cert_pem = malloc(bptr->length + 1);
        if (result.cert_pem) {
            memcpy(result.cert_pem, bptr->data, bptr->length);
            result.cert_pem[bptr->length] = '\0';
        }
        BIO_free(bio);
    }

    /* Serialise private key to PEM */
    {
        BIO *bio = BIO_new(BIO_s_mem());
        if (!bio || PEM_write_bio_PrivateKey(bio, pkey, NULL,
                                              NULL, 0, NULL, NULL) != 1) {
            BIO_free(bio);
            X509_free(cert);
            EVP_PKEY_free(pkey);
            free(result.cert_pem);
            result.cert_pem = NULL;
            result.is_err   = 1;
            result.err_msg  = "PEM_write_bio_PrivateKey failed";
            return result;
        }
        BUF_MEM *bptr = NULL;
        BIO_get_mem_ptr(bio, &bptr);
        result.key_pem = malloc(bptr->length + 1);
        if (result.key_pem) {
            memcpy(result.key_pem, bptr->data, bptr->length);
            result.key_pem[bptr->length] = '\0';
        }
        BIO_free(bio);
    }

    X509_free(cert);
    EVP_PKEY_free(pkey);

    if (!result.cert_pem || !result.key_pem) {
        free(result.cert_pem);
        free(result.key_pem);
        result.cert_pem = NULL;
        result.key_pem  = NULL;
        result.is_err   = 1;
        result.err_msg  = "malloc failed";
    }
    return result;
}

/* Back-compat wrapper: classical P-384 ECDSA self-signed cert (default). */
TlsKeypairResult tls_gen_self_signed(const char *common_name,
                                      int32_t     valid_days)
{
    return tls_gen_self_signed_alg(common_name, valid_days, NULL);
}

/* =========================================================================
 * tls_listen — per-connection thread arg
 * ========================================================================= */

typedef struct {
    SSL            *ssl;
    int             fd;
    TlsConnCallback cb;
    char           *local_cert_pem; /* may be NULL */
    char           *peer_cert_pem;  /* for pin check; may be NULL */
} AcceptThreadArg;

static void *accept_thread(void *arg)
{
    AcceptThreadArg *a = (AcceptThreadArg *)arg;
    SSL *ssl            = a->ssl;
    int  fd             = a->fd;
    TlsConnCallback cb  = a->cb;
    char *local_pem     = a->local_cert_pem;
    char *peer_pem      = a->peer_cert_pem;
    free(a);

    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        close(fd);
        free(local_pem);
        free(peer_pem);
        return NULL;
    }

    /* Certificate pinning check on server side */
    if (peer_pem && *peer_pem != '\0' && !verify_pin(ssl, peer_pem)) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        free(local_pem);
        free(peer_pem);
        return NULL;
    }
    free(peer_pem);

    char *id = registry_add(ssl, fd, local_pem);
    free(local_pem);

    if (!id) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        return NULL;
    }

    TlsConn conn;
    conn.id = id;
    cb(conn);
    /* cb is responsible for calling tls_close */
    return NULL;
}

/* =========================================================================
 * tls_listen
 * ========================================================================= */

int tls_listen(int32_t port, TlsConfig cfg, TlsConnCallback cb)
{
    TK_REQUIRE(TK_CAP_NET);
    SSL_CTX *ctx = build_ssl_ctx(1, cfg);
    if (!ctx) return 0;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    /* 135.34 (3): tk_bind_exclusive, not an unconditional SO_REUSEADDR --
     * on Darwin that let *:P bind beside a live 127.0.0.1:P listener and
     * take its traffic (tk_bind.h; http.c, router.c and net.c since
     * 135.32/135.33).  A port in TIME_WAIT still binds. */
    int listenfd = tk_bind_exclusive(&addr);
    if (listenfd < 0) { SSL_CTX_free(ctx); return 0; }
    if (listen(listenfd, SOMAXCONN) != 0) {
        close(listenfd);
        SSL_CTX_free(ctx);
        return 0;
    }

    /* Accept loop */
    for (;;) {
        struct sockaddr_in peer_addr;
        socklen_t peer_len = sizeof(peer_addr);
        int connfd = accept(listenfd, (struct sockaddr *)&peer_addr, &peer_len);
        if (connfd < 0) {
            if (errno == EINTR) continue;
            break;
        }

        SSL *ssl = SSL_new(ctx);
        if (!ssl) { close(connfd); continue; }
        SSL_set_fd(ssl, connfd);

        AcceptThreadArg *arg = malloc(sizeof(AcceptThreadArg));
        if (!arg) { SSL_free(ssl); close(connfd); continue; }

        arg->ssl            = ssl;
        arg->fd             = connfd;
        arg->cb             = cb;
        arg->local_cert_pem = (cfg.cert_pem && *cfg.cert_pem)
                                ? strdup(cfg.cert_pem) : NULL;
        arg->peer_cert_pem  = (cfg.peer_cert_pem && *cfg.peer_cert_pem)
                                ? strdup(cfg.peer_cert_pem) : NULL;

        pthread_t tid;
        if (pthread_create(&tid, NULL, accept_thread, arg) != 0) {
            free(arg->local_cert_pem);
            free(arg->peer_cert_pem);
            free(arg);
            SSL_free(ssl);
            close(connfd);
        } else {
            pthread_detach(tid);
        }
    }

    close(listenfd);
    SSL_CTX_free(ctx);
    return 0; /* only reached on accept error */
}

/* =========================================================================
 * tls_connect
 * ========================================================================= */

TlsConnResult tls_connect(const char *host, int32_t port, TlsConfig cfg)
{
    TK_REQUIRE(TK_CAP_NET);
    TlsConnResult result;
    result.conn.id = NULL;
    result.is_none = 1;

    if (!host || *host == '\0') return result;

    SSL_CTX *ctx = build_ssl_ctx(0, cfg);
    if (!ctx) return result;

    /* Resolve host */
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", (int)port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        SSL_CTX_free(ctx);
        return result;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) { SSL_CTX_free(ctx); return result; }

    SSL *ssl = SSL_new(ctx);
    if (!ssl) { close(fd); SSL_CTX_free(ctx); return result; }

    SSL_set_fd(ssl, fd);

    /* 121.64: the name the default (verifying) mode checks the certificate
     * against.  A literal address is matched as an IP SAN and sent no SNI
     * (RFC 6066 forbids an address there); a name is both. */
    unsigned char ipbuf[16];
    int is_ip = inet_pton(AF_INET, host, ipbuf) == 1 ||
                inet_pton(AF_INET6, host, ipbuf) == 1;
    int verifying = !(cfg.peer_cert_pem && *cfg.peer_cert_pem) && !cfg.insecure;
    int named = 1;
    if (!is_ip)
        named = SSL_set_tlsext_host_name(ssl, host) == 1; /* SNI */
    if (named && verifying)
        named = is_ip
            ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host) == 1
            : SSL_set1_host(ssl, host) == 1;
    if (!named) {
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return result;
    }

    if (SSL_connect(ssl) != 1 ||
        (verifying && SSL_get_verify_result(ssl) != X509_V_OK)) {
        ERR_clear_error();
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return result;
    }

    /* Certificate pinning */
    if (!verify_pin(ssl, cfg.peer_cert_pem)) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        SSL_CTX_free(ctx);
        return result;
    }

    SSL_CTX_free(ctx);

    char *id = registry_add(ssl, fd,
                            (cfg.cert_pem && *cfg.cert_pem) ? cfg.cert_pem : NULL);
    if (!id) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(fd);
        return result;
    }

    result.conn.id = id;
    result.is_none = 0;
    return result;
}

/* =========================================================================
 * tls_read
 * ========================================================================= */

TlsStrResult tls_read(TlsConn conn)
{
    TlsStrResult result = {NULL, 1};
    if (!conn.id) return result;

    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return result;

    /* Read up to 64 KiB */
    char *buf = malloc(65536);
    if (!buf) { registry_release(e); return result; }

    pthread_mutex_lock(&e->io);
    int n = SSL_read(e->ssl, buf, 65535);
    pthread_mutex_unlock(&e->io);
    registry_release(e);

    if (n <= 0) {
        free(buf);
        return result;
    }
    buf[n]        = '\0';
    result.ok     = buf;
    result.is_none = 0;
    return result;
}

/* =========================================================================
 * tls_write
 * ========================================================================= */

int tls_write(TlsConn conn, const char *data)
{
    if (!conn.id || !data) return 0;

    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return 0;

    size_t len = strlen(data);
    pthread_mutex_lock(&e->io);
    int n = SSL_write(e->ssl, data, (int)len);
    pthread_mutex_unlock(&e->io);
    registry_release(e);

    return (n > 0) ? 1 : 0;
}

/* =========================================================================
 * tls_close
 * ========================================================================= */

int tls_close(TlsConn conn)
{
    if (!conn.id) return 0;

    /* 121.19 (TLS-03): mark the entry closing under the table lock, so no
     * new acquire finds it, and keep a reference of our own. */
    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return 0;
    pthread_mutex_lock(&g_conns_mu);
    if (e->closing) {                 /* a concurrent tls_close won */
        pthread_mutex_unlock(&g_conns_mu);
        registry_release(e);
        return 0;
    }
    e->closing = 1;
    pthread_mutex_unlock(&g_conns_mu);

    /* If nobody is inside an OpenSSL call on this connection, send
     * close_notify.  If someone is -- a reader blocked waiting for the
     * peer -- wake it by shutting the socket down; it returns, drops io
     * and releases, and the last release frees the SSL*. */
    if (pthread_mutex_trylock(&e->io) == 0) {
        SSL_shutdown(e->ssl);
        pthread_mutex_unlock(&e->io);
    } else {
        shutdown(e->fd, SHUT_RDWR);
    }
    registry_release(e);
    return 1;
}

/* =========================================================================
 * tls_peer_cert
 * ========================================================================= */

TlsStrResult tls_peer_cert(TlsConn conn)
{
    TlsStrResult result = {NULL, 1};
    if (!conn.id) return result;

    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return result;

    pthread_mutex_lock(&e->io);
    X509 *cert = SSL_get_peer_certificate(e->ssl);
    pthread_mutex_unlock(&e->io);
    registry_release(e);

    if (!cert) return result;

    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) { X509_free(cert); return result; }

    if (PEM_write_bio_X509(bio, cert) != 1) {
        BIO_free(bio);
        X509_free(cert);
        return result;
    }

    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(bio, &bptr);
    result.ok = malloc(bptr->length + 1);
    if (result.ok) {
        memcpy(result.ok, bptr->data, bptr->length);
        result.ok[bptr->length] = '\0';
        result.is_none = 0;
    }
    BIO_free(bio);
    X509_free(cert);
    return result;
}

/* =========================================================================
 * tls_fingerprint
 * ========================================================================= */

char *tls_fingerprint(const char *pem)
{
    /* Return empty string on any error */
    char *empty = strdup("");

    if (!pem || *pem == '\0') return empty;

    X509 *cert = pem_to_x509(pem);
    if (!cert) return empty;

    char hex[65];
    if (x509_fingerprint_hex(cert, hex) != 0) {
        X509_free(cert);
        return empty;
    }
    X509_free(cert);
    free(empty);
    return strdup(hex);
}

/* =========================================================================
 * tls_pairing_code
 * ========================================================================= */

char *tls_pairing_code(TlsConn conn)
{
    const char *fallback = "000000";

    if (!conn.id) return strdup(fallback);

    /* Get local cert PEM from registry */
    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return strdup(fallback);

    char *local_pem = e->local_cert_pem ? strdup(e->local_cert_pem) : NULL;

    /* Get peer cert while holding the connection's io lock */
    pthread_mutex_lock(&e->io);
    X509 *peer_cert = SSL_get_peer_certificate(e->ssl);
    pthread_mutex_unlock(&e->io);
    registry_release(e);

    /* Compute peer fingerprint */
    char peer_hex[65] = {0};
    if (peer_cert) {
        x509_fingerprint_hex(peer_cert, peer_hex);
        X509_free(peer_cert);
    }

    /* Compute local fingerprint */
    char local_hex[65] = {0};
    if (local_pem) {
        X509 *local_cert = pem_to_x509(local_pem);
        if (local_cert) {
            x509_fingerprint_hex(local_cert, local_hex);
            X509_free(local_cert);
        }
        free(local_pem);
    }

    /* Need both fingerprints */
    if (peer_hex[0] == '\0' || local_hex[0] == '\0')
        return strdup(fallback);

    /*
     * XOR corresponding bytes of both hex strings, sum them up,
     * then take modulo 1000000 to produce a 6-digit code.
     *
     * Both strings are 64 hex chars representing 32 bytes each.
     * We treat each pair of hex digits as a byte, XOR them, and
     * accumulate into a 64-bit integer.
     */
    uint64_t acc = 0;
    for (int i = 0; i < 64; i += 2) {
        unsigned int lb = 0, pb = 0;
        /* parse hex byte from local */
        if (local_hex[i] >= '0' && local_hex[i] <= '9')
            lb = (unsigned int)(local_hex[i] - '0');
        else lb = (unsigned int)(local_hex[i] - 'a' + 10);
        lb <<= 4;
        if (local_hex[i+1] >= '0' && local_hex[i+1] <= '9')
            lb |= (unsigned int)(local_hex[i+1] - '0');
        else lb |= (unsigned int)(local_hex[i+1] - 'a' + 10);

        /* parse hex byte from peer */
        if (peer_hex[i] >= '0' && peer_hex[i] <= '9')
            pb = (unsigned int)(peer_hex[i] - '0');
        else pb = (unsigned int)(peer_hex[i] - 'a' + 10);
        pb <<= 4;
        if (peer_hex[i+1] >= '0' && peer_hex[i+1] <= '9')
            pb |= (unsigned int)(peer_hex[i+1] - '0');
        else pb |= (unsigned int)(peer_hex[i+1] - 'a' + 10);

        acc += (lb ^ pb);
    }

    uint32_t code = (uint32_t)(acc % 1000000ULL);
    char *out = malloc(7);
    if (!out) return strdup(fallback);
    snprintf(out, 7, "%06u", code);
    return out;
}

/* =========================================================================
 * tls_protocol (136.44)
 * ========================================================================= */

/* =========================================================================
 * tls_group (122.13)
 * ========================================================================= */

char *tls_group(TlsConn conn)
{
    if (!conn.id) return strdup("");

    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return strdup("");

    pthread_mutex_lock(&e->io);
    const char *g = NULL;
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
    g = SSL_get0_group_name(e->ssl);
#else
    {
        int nid = SSL_get_negotiated_group(e->ssl);
        g = nid ? SSL_group_to_name(e->ssl, nid) : NULL;
    }
#endif
    char *out = strdup(g ? g : "");
    pthread_mutex_unlock(&e->io);
    registry_release(e);
    return out ? out : strdup("");
}

char *tls_protocol(TlsConn conn)
{
    if (!conn.id) return strdup("");

    ConnEntry *e = registry_acquire(conn.id);
    if (!e) return strdup("");

    pthread_mutex_lock(&e->io);
    const char *v = SSL_get_version(e->ssl);
    char *out = strdup(v ? v : "");
    pthread_mutex_unlock(&e->io);
    registry_release(e);
    return out ? out : strdup("");
}
