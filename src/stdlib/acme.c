/*
 * acme.c — ACME v2 client (RFC 8555) for automatic certificate provisioning.
 *
 * Stories: 61.1.1 (core), 61.1.2 (HTTP-01), 61.1.3 (DNS-01 hooks),
 *          61.1.4 (auto-renewal), 61.1.5 (cert storage).
 *
 * Uses std.http client for ACME server communication, std.encoding for
 * base64url, and OpenSSL for JWS/ES256 signing and key generation.
 */

#include "http.h"
#include "encoding.h"
#include "strbuf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>

#ifdef TK_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/sha.h>
#include <openssl/ecdsa.h>

/* ── ACME types ──────────────────────────────────────────────────────── */

typedef struct {
    char *directory_url;    /* e.g. "https://acme-v02.api.letsencrypt.org/directory" */
    char *new_nonce_url;
    char *new_account_url;
    char *new_order_url;
    char *account_url;      /* set after account creation/lookup */
    EVP_PKEY *account_key;  /* ES256 account key */
    HttpClient *client;
} AcmeClient;

/* ACME challenge types */
#define ACME_CHALLENGE_HTTP01 1
#define ACME_CHALLENGE_DNS01  2

/* ── Static challenge storage for HTTP-01 (61.1.2) ──────────────────── */

#define ACME_MAX_CHALLENGES 8

typedef struct {
    char *token;
    char *key_auth;
} AcmeChallenge;

/* 121.19 (ACME-02): the table was two plain globals.  acme_order_certificate
 * filled it, then freed every token/key_auth and reset the count while
 * acme_challenge_handler -- on the web server's request threads -- read it
 * unlocked and returned key_auth itself as the response body: a request
 * racing the cleanup read, and could serve, freed memory.  Every access is
 * now under g_challenge_mu, and the handler serves a per-thread copy. */
static AcmeChallenge g_challenges[ACME_MAX_CHALLENGES];
static int g_challenge_count = 0;
static pthread_mutex_t g_challenge_mu = PTHREAD_MUTEX_INITIALIZER;

static void acme_challenges_clear(void)
{
    pthread_mutex_lock(&g_challenge_mu);
    for (int i = 0; i < g_challenge_count; i++) {
        free(g_challenges[i].token);
        free(g_challenges[i].key_auth);
        g_challenges[i].token = NULL;
        g_challenges[i].key_auth = NULL;
    }
    g_challenge_count = 0;
    pthread_mutex_unlock(&g_challenge_mu);
}

static void acme_challenge_add(const char *token, const char *key_auth)
{
    char *t = strdup(token), *k = strdup(key_auth);
    pthread_mutex_lock(&g_challenge_mu);
    if (t && k && g_challenge_count < ACME_MAX_CHALLENGES) {
        g_challenges[g_challenge_count].token = t;
        g_challenges[g_challenge_count].key_auth = k;
        g_challenge_count++;
        t = k = NULL;
    }
    pthread_mutex_unlock(&g_challenge_mu);
    free(t); free(k);
}

/* 121.19 (ACME-03): a domain is interpolated into the newOrder JSON and the
 * CSR's "DNS:a,DNS:b" SAN list.  A '"' or '\\' broke out of the JSON string;
 * a ',' added a SAN entry.  Only DNS-name bytes are accepted: letters,
 * digits, '-', '.', and '*' for a wildcard label. */
static int acme_domain_ok(const char *d)
{
    if (!d || !*d) return 0;
    size_t n = 0;
    for (const char *p = d; *p; p++, n++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '*'))
            return 0;
    }
    return n <= 253;
}

/* Forward declarations */
static int acme_write_file(const char *path, const uint8_t *data, size_t len);
static int acme_write_key(const char *path, EVP_PKEY *key);

/* ── JWS / ES256 helpers ─────────────────────────────────────────────── */

/* Base64url-encode raw bytes */
static char *b64url(const uint8_t *data, size_t len)
{
    ByteArray ba = { .data = (uint8_t *)data, .len = (uint64_t)len };
    return (char *)encoding_b64urlencode(ba);
}

/* Generate an ES256 (P-256) key pair */
static EVP_PKEY *acme_gen_key(void)
{
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, NULL);
    if (!pctx) return NULL;
    if (EVP_PKEY_keygen_init(pctx) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(pctx, NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(pctx, &pkey) <= 0) {
        EVP_PKEY_CTX_free(pctx);
        return NULL;
    }
    EVP_PKEY_CTX_free(pctx);
    return pkey;
}

/* Get the JWK thumbprint of an ES256 key (SHA-256 of canonical JWK) */
static char *acme_jwk_thumbprint(EVP_PKEY *key)
{
    /* Extract x, y coordinates */
    BIGNUM *x_bn = NULL, *y_bn = NULL;
    if (!EVP_PKEY_get_bn_param(key, "qx", &x_bn) ||
        !EVP_PKEY_get_bn_param(key, "qy", &y_bn)) {
        BN_free(x_bn); BN_free(y_bn);
        return NULL;
    }

    uint8_t x[32], y[32];
    BN_bn2binpad(x_bn, x, 32);
    BN_bn2binpad(y_bn, y, 32);
    BN_free(x_bn);
    BN_free(y_bn);

    char *x_b64 = b64url(x, 32);
    char *y_b64 = b64url(y, 32);

    /* Canonical JWK for thumbprint (RFC 7638) — lexicographic key order */
    char jwk[512];
    snprintf(jwk, sizeof(jwk),
             "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}",
             x_b64, y_b64);
    free(x_b64);
    free(y_b64);

    uint8_t digest[32];
    SHA256((uint8_t *)jwk, strlen(jwk), digest);
    return b64url(digest, 32);
}

/* Build a JWK object for the account key */
static char *acme_jwk_json(EVP_PKEY *key)
{
    BIGNUM *x_bn = NULL, *y_bn = NULL;
    if (!EVP_PKEY_get_bn_param(key, "qx", &x_bn) ||
        !EVP_PKEY_get_bn_param(key, "qy", &y_bn)) {
        BN_free(x_bn); BN_free(y_bn);
        return NULL;
    }

    uint8_t x[32], y[32];
    BN_bn2binpad(x_bn, x, 32);
    BN_bn2binpad(y_bn, y, 32);
    BN_free(x_bn);
    BN_free(y_bn);

    char *x_b64 = b64url(x, 32);
    char *y_b64 = b64url(y, 32);

    char *jwk = malloc(512);
    if (!jwk) { free(x_b64); free(y_b64); return NULL; }
    snprintf(jwk, 512,
             "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}",
             x_b64, y_b64);
    free(x_b64);
    free(y_b64);
    return jwk;
}

/* Sign payload with ES256 (ECDSA P-256 + SHA-256), return raw r||s */
static uint8_t *acme_es256_sign(EVP_PKEY *key, const uint8_t *data,
                                 size_t data_len, size_t *sig_len)
{
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    if (!mdctx) return NULL;

    if (EVP_DigestSignInit(mdctx, NULL, EVP_sha256(), NULL, key) != 1) {
        EVP_MD_CTX_free(mdctx);
        return NULL;
    }

    size_t der_len = 0;
    if (EVP_DigestSign(mdctx, NULL, &der_len, data, data_len) != 1) {
        EVP_MD_CTX_free(mdctx);
        return NULL;
    }

    uint8_t *der_sig = malloc(der_len);
    if (!der_sig || EVP_DigestSign(mdctx, der_sig, &der_len, data, data_len) != 1) {
        free(der_sig);
        EVP_MD_CTX_free(mdctx);
        return NULL;
    }
    EVP_MD_CTX_free(mdctx);

    /* Convert DER-encoded ECDSA signature to raw r||s (64 bytes) */
    const uint8_t *p = der_sig;
    ECDSA_SIG *ecsig = d2i_ECDSA_SIG(NULL, &p, (long)der_len);
    free(der_sig);
    if (!ecsig) return NULL;

    const BIGNUM *r, *s;
    ECDSA_SIG_get0(ecsig, &r, &s);

    uint8_t *raw = malloc(64);
    if (!raw) { ECDSA_SIG_free(ecsig); return NULL; }
    BN_bn2binpad(r, raw, 32);
    BN_bn2binpad(s, raw + 32, 32);
    ECDSA_SIG_free(ecsig);

    *sig_len = 64;
    return raw;
}

/* Build a JWS (Flattened JSON Serialization) for ACME requests */
static char *acme_jws(AcmeClient *ac, const char *url,
                       const char *payload, const char *nonce)
{
    /* Header.  122.2: was a char[1024]; the account URL, nonce and request
     * URL all come from the ACME server, and a long one truncated the
     * protected header into invalid JSON that was then signed and sent. */
    TkStrBuf hb = TK_STRBUF_INIT;
    if (ac->account_url) {
        strbuf_appendf(&hb,
                 "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}",
                 ac->account_url, nonce, url);
    } else {
        char *jwk = acme_jwk_json(ac->account_key);
        if (!jwk) return NULL;
        strbuf_appendf(&hb,
                 "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
                 jwk, nonce, url);
        free(jwk);
    }
    char *header = strbuf_take(&hb);
    if (!header) return NULL;

    char *hdr_b64 = b64url((uint8_t *)header, strlen(header));
    free(header);
    char *pay_b64 = b64url((uint8_t *)payload, strlen(payload));

    /* Signing input: header.payload */
    size_t input_len = strlen(hdr_b64) + 1 + strlen(pay_b64);
    char *input = malloc(input_len + 1);
    if (!input) { free(hdr_b64); free(pay_b64); return NULL; }
    snprintf(input, input_len + 1, "%s.%s", hdr_b64, pay_b64);

    size_t sig_len = 0;
    uint8_t *sig = acme_es256_sign(ac->account_key,
                                    (uint8_t *)input, input_len, &sig_len);
    free(input);
    if (!sig) { free(hdr_b64); free(pay_b64); return NULL; }

    char *sig_b64 = b64url(sig, sig_len);
    free(sig);

    /* Flattened JWS JSON */
    size_t jws_len = strlen(hdr_b64) + strlen(pay_b64) + strlen(sig_b64) + 128;
    char *jws = malloc(jws_len);
    if (!jws) { free(hdr_b64); free(pay_b64); free(sig_b64); return NULL; }
    snprintf(jws, jws_len,
             "{\"protected\":\"%s\",\"payload\":\"%s\",\"signature\":\"%s\"}",
             hdr_b64, pay_b64, sig_b64);

    free(hdr_b64);
    free(pay_b64);
    free(sig_b64);
    return jws;
}

/* ── Nonce management ────────────────────────────────────────────────── */

static char *acme_get_nonce(AcmeClient *ac)
{
    HttpClientResp resp = http_get(ac->client, ac->new_nonce_url);
    if (resp.is_err) {
        free(resp.err_msg);
        return NULL;
    }

    /* Extract Replay-Nonce header */
    char *nonce = NULL;
    for (uint64_t i = 0; i < resp.headers.len; i++) {
        if (strcasecmp(resp.headers.data[i].key, "Replay-Nonce") == 0) {
            nonce = strdup(resp.headers.data[i].val);
            break;
        }
    }
    free(resp.body);
    return nonce;
}

/* ── ACME client lifecycle ───────────────────────────────────────────── */

void acme_client_free(AcmeClient *ac);   /* below; acme_client_new cleans up with it */

AcmeClient *acme_client_new(const char *directory_url)
{
    if (!directory_url) return NULL;

    AcmeClient *ac = calloc(1, sizeof(AcmeClient));
    if (!ac) return NULL;
    ac->directory_url = strdup(directory_url);

    /* Create HTTP client */
    ac->client = http_client(directory_url);
    if (!ac->client) { free(ac->directory_url); free(ac); return NULL; }

    /* Fetch directory */
    HttpClientResp resp = http_get(ac->client, directory_url);
    if (resp.is_err || resp.status != 200) {
        fprintf(stderr, "acme: failed to fetch directory: %s\n",
                resp.err_msg ? resp.err_msg : "unknown");
        free(resp.err_msg);
        free(resp.body);
        http_client_free(ac->client);
        free(ac->directory_url);
        free(ac);
        return NULL;
    }

    /* Parse directory JSON (minimal extraction) */
    char *body = (char *)resp.body;
    char *p;

    p = strstr(body, "\"newNonce\"");
    if (p) {
        p = strchr(p + 10, '"');
        if (p) {
            p++;
            char *end = strchr(p, '"');
            if (end) ac->new_nonce_url = strndup(p, (size_t)(end - p));
        }
    }

    p = strstr(body, "\"newAccount\"");
    if (p) {
        p = strchr(p + 13, '"');
        if (p) {
            p++;
            char *end = strchr(p, '"');
            if (end) ac->new_account_url = strndup(p, (size_t)(end - p));
        }
    }

    p = strstr(body, "\"newOrder\"");
    if (p) {
        p = strchr(p + 10, '"');
        if (p) {
            p++;
            char *end = strchr(p, '"');
            if (end) ac->new_order_url = strndup(p, (size_t)(end - p));
        }
    }

    free(resp.body);
    free(resp.err_msg);

    if (!ac->new_nonce_url || !ac->new_account_url || !ac->new_order_url) {
        fprintf(stderr, "acme: incomplete directory response\n");
        /* 135.3j (6): was `acme_client_new(NULL)`, which returns NULL at
         * once and frees nothing -- the client, its HTTP client and every
         * URL already parsed leaked on each incomplete directory. */
        acme_client_free(ac);
        return NULL;
    }

    return ac;
}

void acme_client_free(AcmeClient *ac)
{
    if (!ac) return;
    free(ac->directory_url);
    free(ac->new_nonce_url);
    free(ac->new_account_url);
    free(ac->new_order_url);
    free(ac->account_url);
    if (ac->account_key) EVP_PKEY_free(ac->account_key);
    if (ac->client) http_client_free(ac->client);
    free(ac);
}

/* ── Account management (61.1.1) ─────────────────────────────────────── */

/* Load or generate account key */
int acme_load_account_key(AcmeClient *ac, const char *key_path)
{
    if (!ac || !key_path) return -1;

    /* 121.19 (ACME-07): an existing key was loaded whatever its mode.  A
     * key readable by group or others is refused, as ssh refuses one: the
     * account key authorises certificate issuance for every domain on the
     * account.  O_NOFOLLOW: the key is not read through a symlink. */
    int fd_in = open(key_path, O_RDONLY | O_NOFOLLOW);
    if (fd_in >= 0) {
        struct stat st;
        if (fstat(fd_in, &st) != 0 || !S_ISREG(st.st_mode)) {
            close(fd_in);
            return -1;
        }
        if (st.st_mode & 077) {
            fprintf(stderr, "acme: refusing account key %s: mode %03o is "
                    "readable by group or others (chmod 600 it)\n",
                    key_path, (unsigned)(st.st_mode & 0777));
            close(fd_in);
            return -1;
        }
        FILE *f = fdopen(fd_in, "r");
        if (!f) { close(fd_in); return -1; }
        /* Load existing key */
        ac->account_key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
        fclose(f);
        return ac->account_key ? 0 : -1;
    }
    if (errno != ENOENT) return -1;   /* a symlink (ELOOP) or unreadable */

    /* Generate new key */
    ac->account_key = acme_gen_key();
    if (!ac->account_key) return -1;

    /* Save with secure permissions (61.1.5), atomically (121.19 ACME-05) */
    return acme_write_key(key_path, ac->account_key);
}

/* acme_account_payload -- the newAccount request body.  122.2: was a
 * char[512]; a long contact address was cut mid-JSON.  Returns malloc'd, or
 * NULL on OOM. */
static char *acme_account_payload(const char *email)
{
    TkStrBuf pb = TK_STRBUF_INIT;
    if (email) {
        /* 121.19 (ACME-03): JSON-escaped -- a '"' in the address closed the
         * string and could add fields to the newAccount request. */
        strbuf_append(&pb, "{\"termsOfServiceAgreed\":true,"
                           "\"contact\":[\"mailto:");
        strbuf_append_json(&pb, email);
        strbuf_append(&pb, "\"]}");
    } else {
        strbuf_append(&pb, "{\"termsOfServiceAgreed\":true}");
    }
    return strbuf_take(&pb);
}

/* Register or find existing account */
int acme_register_account(AcmeClient *ac, const char *email)
{
    if (!ac || !ac->account_key) return -1;

    char *nonce = acme_get_nonce(ac);
    if (!nonce) return -1;

    char *payload = acme_account_payload(email);
    if (!payload) { free(nonce); return -1; }

    char *jws = acme_jws(ac, ac->new_account_url, payload, nonce);
    free(payload);
    free(nonce);
    if (!jws) return -1;

    HttpClientResp resp = http_post(ac->client, ac->new_account_url,
                                     (uint8_t *)jws, (uint64_t)strlen(jws),
                                     "application/jose+json");
    free(jws);

    if (resp.is_err || (resp.status != 200 && resp.status != 201)) {
        fprintf(stderr, "acme: account registration failed: %llu\n",
                (unsigned long long)resp.status);
        free(resp.body);
        free(resp.err_msg);
        return -1;
    }

    /* Extract Location header = account URL */
    for (uint64_t i = 0; i < resp.headers.len; i++) {
        if (strcasecmp(resp.headers.data[i].key, "Location") == 0) {
            free(ac->account_url);
            ac->account_url = strdup(resp.headers.data[i].val);
            break;
        }
    }

    free(resp.body);
    free(resp.err_msg);
    return ac->account_url ? 0 : -1;
}

/* ── Order and certificate issuance (61.1.1) ─────────────────────────── */

/* acme_identifiers_json -- the newOrder payload for `domains`.  122.2: this
 * was `off += snprintf(identifiers+off, sizeof identifiers - off, ...)` into a
 * char[4096]; past ~90 domains `off` passed the end, the size underflowed and
 * snprintf wrote past the stack array (121.19 ACME-01).  Returns malloc'd, or
 * NULL on OOM. */
static char *acme_identifiers_json(const char **domains, int ndomain)
{
    for (int i = 0; i < ndomain; i++)
        if (!acme_domain_ok(domains[i])) return NULL;   /* 121.19 ACME-03 */
    TkStrBuf b = TK_STRBUF_INIT;
    strbuf_append(&b, "{\"identifiers\":[");
    for (int i = 0; i < ndomain; i++) {
        if (i > 0) strbuf_putc(&b, ',');
        strbuf_appendf(&b, "{\"type\":\"dns\",\"value\":\"%s\"}", domains[i]);
    }
    strbuf_append(&b, "]}");
    return strbuf_take(&b);
}

/* acme_san_list -- "DNS:a,DNS:b,..." for the CSR's subjectAltName.  Same
 * char[4096] + snprintf-offset overflow as acme_identifiers_json (122.2). */
static char *acme_san_list(const char **domains, int ndomain)
{
    for (int i = 0; i < ndomain; i++)
        if (!acme_domain_ok(domains[i])) return NULL;   /* 121.19 ACME-03 */
    TkStrBuf b = TK_STRBUF_INIT;
    for (int i = 0; i < ndomain; i++) {
        if (i > 0) strbuf_putc(&b, ',');
        strbuf_appendf(&b, "DNS:%s", domains[i]);
    }
    return strbuf_take(&b);
}

/* Place a new order for a certificate */
int acme_order_certificate(AcmeClient *ac, const char **domains,
                            int ndomain, const char *cert_path,
                            const char *key_path)
{
    if (!ac || !ac->account_url || !domains || ndomain <= 0) return -1;

    /* 121.19 (ACME-02): a previous order that failed part-way left its
     * challenges registered, and the table holds ACME_MAX_CHALLENGES; start
     * this order from an empty one. */
    acme_challenges_clear();

    /* Build identifiers JSON */
    char *identifiers = acme_identifiers_json(domains, ndomain);
    if (!identifiers) return -1;

    char *nonce = acme_get_nonce(ac);
    if (!nonce) { free(identifiers); return -1; }

    char *jws = acme_jws(ac, ac->new_order_url, identifiers, nonce);
    free(identifiers);
    free(nonce);
    if (!jws) return -1;

    HttpClientResp resp = http_post(ac->client, ac->new_order_url,
                                     (uint8_t *)jws, (uint64_t)strlen(jws),
                                     "application/jose+json");
    free(jws);

    if (resp.is_err || resp.status != 201) {
        fprintf(stderr, "acme: order failed: %llu\n",
                (unsigned long long)resp.status);
        free(resp.body);
        free(resp.err_msg);
        return -1;
    }

    /* Parse order response — extract authorization URLs and finalize URL.
     * This is a minimal JSON parser for the order object. */
    char *body = (char *)resp.body;

    /* Extract finalize URL */
    char *finalize_url = NULL;
    char *fp = strstr(body, "\"finalize\"");
    if (fp) {
        fp = strchr(fp + 10, '"');
        if (fp) {
            fp++;
            char *end = strchr(fp, '"');
            if (end) finalize_url = strndup(fp, (size_t)(end - fp));
        }
    }

    /* Extract authorization URLs */
    char *auth_urls[16];
    int nauth = 0;
    char *ap = strstr(body, "\"authorizations\"");
    if (ap) {
        ap = strchr(ap, '[');
        if (ap) {
            ap++;
            while (nauth < 16) {
                char *q = strchr(ap, '"');
                if (!q) break;
                q++;
                char *end = strchr(q, '"');
                if (!end) break;
                auth_urls[nauth] = strndup(q, (size_t)(end - q));
                nauth++;
                ap = end + 1;
                if (*ap == ']') break;
            }
        }
    }

    /* Extract Location header = order URL for polling */
    char *order_url = NULL;
    for (uint64_t i = 0; i < resp.headers.len; i++) {
        if (strcasecmp(resp.headers.data[i].key, "Location") == 0) {
            order_url = strdup(resp.headers.data[i].val);
            break;
        }
    }

    free(resp.body);
    free(resp.err_msg);

    /* Process each authorization — complete HTTP-01 challenges */
    for (int i = 0; i < nauth; i++) {
        nonce = acme_get_nonce(ac);
        if (!nonce) { free(auth_urls[i]); continue; }   /* 135.3j (6): leaked */

        jws = acme_jws(ac, auth_urls[i], "", nonce);
        free(nonce);
        if (!jws) { free(auth_urls[i]); continue; }

        resp = http_post(ac->client, auth_urls[i],
                          (uint8_t *)jws, (uint64_t)strlen(jws),
                          "application/jose+json");
        free(jws);
        free(auth_urls[i]);

        if (resp.is_err || resp.status != 200) {
            free(resp.body);
            free(resp.err_msg);
            continue;
        }

        /* Find http-01 challenge */
        char *cb = (char *)resp.body;
        char *http01 = strstr(cb, "\"http-01\"");
        if (!http01) { free(resp.body); free(resp.err_msg); continue; }

        /* Extract token */
        char *tp = strstr(http01, "\"token\"");
        if (!tp) { free(resp.body); free(resp.err_msg); continue; }
        tp = strchr(tp + 7, '"');
        if (!tp) { free(resp.body); free(resp.err_msg); continue; }
        tp++;
        char *tend = strchr(tp, '"');
        if (!tend) { free(resp.body); free(resp.err_msg); continue; }
        char *token = strndup(tp, (size_t)(tend - tp));

        /* Extract challenge URL */
        /* Look backwards from http-01 for "url" */
        char *challenge_url = NULL;
        char *up = strstr(cb, "\"url\"");
        /* Find the url closest to the http-01 type */
        char *last_url = NULL;
        while (up && up < http01) {
            char *uq = strchr(up + 5, '"');
            if (uq) {
                uq++;
                char *uend = strchr(uq, '"');
                if (uend) {
                    free(last_url);
                    last_url = strndup(uq, (size_t)(uend - uq));
                }
            }
            up = strstr(up + 1, "\"url\"");
        }
        challenge_url = last_url;

        /* Build key authorization: token.thumbprint */
        char *thumbprint = acme_jwk_thumbprint(ac->account_key);
        if (thumbprint && token) {
            size_t ka_len = strlen(token) + 1 + strlen(thumbprint) + 1;
            char *key_auth = malloc(ka_len);
            if (key_auth) {
                snprintf(key_auth, ka_len, "%s.%s", token, thumbprint);

                /* Register challenge for HTTP-01 serving (61.1.2) */
                acme_challenge_add(token, key_auth);

                /* Respond to challenge */
                if (challenge_url) {
                    nonce = acme_get_nonce(ac);
                    if (nonce) {
                        jws = acme_jws(ac, challenge_url, "{}", nonce);
                        free(nonce);
                        if (jws) {
                            HttpClientResp cr = http_post(
                                ac->client, challenge_url,
                                (uint8_t *)jws, (uint64_t)strlen(jws),
                                "application/jose+json");
                            free(jws);
                            free(cr.body);
                            free(cr.err_msg);
                        }
                    }
                }
                free(key_auth);
            }
        }
        free(thumbprint);
        free(token);
        free(challenge_url);
        free(resp.body);
        free(resp.err_msg);
    }

    /* Wait for challenge validation (poll order status) */
    int poll_ok = 0;
    for (int attempt = 0; attempt < 30; attempt++) {
        sleep(2);

        if (!order_url) break;

        /* POST-as-GET to fetch current order status */
        nonce = acme_get_nonce(ac);
        if (!nonce) continue;

        jws = acme_jws(ac, order_url, "", nonce);
        free(nonce);
        if (!jws) continue;

        resp = http_post(ac->client, order_url,
                          (uint8_t *)jws, (uint64_t)strlen(jws),
                          "application/jose+json");
        free(jws);

        if (resp.is_err || resp.status != 200) {
            free(resp.body);
            free(resp.err_msg);
            continue;
        }

        /* Check order status in response body */
        char *sb = (char *)resp.body;
        char *sp = strstr(sb, "\"status\"");
        if (sp) {
            sp = strchr(sp + 8, '"');
            if (sp) {
                sp++;
                if (strncmp(sp, "ready", 5) == 0 ||
                    strncmp(sp, "valid", 5) == 0) {
                    poll_ok = 1;
                    free(resp.body);
                    free(resp.err_msg);
                    break;
                } else if (strncmp(sp, "invalid", 7) == 0) {
                    fprintf(stderr,
                            "acme: challenge validation failed\n");
                    free(resp.body);
                    free(resp.err_msg);
                    free(order_url);
                    free(finalize_url);
                    return -1;
                }
                /* "pending" or "processing" — keep polling */
            }
        }
        free(resp.body);
        free(resp.err_msg);
    }
    free(order_url);

    if (!poll_ok) {
        fprintf(stderr, "acme: challenge validation timed out\n");
        free(finalize_url);
        return -1;
    }

    /* Generate CSR */
    EVP_PKEY *cert_key = acme_gen_key();
    if (!cert_key) { free(finalize_url); return -1; }

    X509_REQ *csr = X509_REQ_new();
    X509_REQ_set_version(csr, 0);
    X509_REQ_set_pubkey(csr, cert_key);

    /* Add SAN extension */
    STACK_OF(X509_EXTENSION) *exts = sk_X509_EXTENSION_new_null();
    char *san_buf = acme_san_list(domains, ndomain);
    X509_EXTENSION *san_ext = X509V3_EXT_nconf_nid(
        NULL, NULL, NID_subject_alt_name, san_buf ? san_buf : "");
    free(san_buf);
    if (san_ext) {
        sk_X509_EXTENSION_push(exts, san_ext);
        X509_REQ_add_extensions(csr, exts);
    }

    X509_REQ_sign(csr, cert_key, EVP_sha256());

    /* DER-encode CSR */
    int csr_der_len = i2d_X509_REQ(csr, NULL);
    uint8_t *csr_der = malloc((size_t)csr_der_len);
    uint8_t *csr_p = csr_der;
    i2d_X509_REQ(csr, &csr_p);
    X509_REQ_free(csr);
    sk_X509_EXTENSION_pop_free(exts, X509_EXTENSION_free);

    char *csr_b64 = b64url(csr_der, (size_t)csr_der_len);
    free(csr_der);

    /* Finalize the order */
    if (finalize_url && csr_b64) {
        /* 122.2: was a char[8192]; a CSR carrying many SANs outgrew it and
         * was sent truncated. */
        TkStrBuf fb = TK_STRBUF_INIT;
        strbuf_appendf(&fb, "{\"csr\":\"%s\"}", csr_b64);
        char *fin_payload = strbuf_take(&fb);

        nonce = fin_payload ? acme_get_nonce(ac) : NULL;
        if (nonce) {
            jws = acme_jws(ac, finalize_url, fin_payload, nonce);
            free(nonce);
            free(fin_payload);
            fin_payload = NULL;
            if (jws) {
                resp = http_post(ac->client, finalize_url,
                                  (uint8_t *)jws, (uint64_t)strlen(jws),
                                  "application/jose+json");
                free(jws);

                /* Extract certificate URL from response */
                if (!resp.is_err && resp.body) {
                    char *cert_url = NULL;
                    char *cp = strstr((char *)resp.body, "\"certificate\"");
                    if (cp) {
                        cp = strchr(cp + 13, '"');
                        if (cp) {
                            cp++;
                            char *cend = strchr(cp, '"');
                            if (cend) cert_url = strndup(cp, (size_t)(cend - cp));
                        }
                    }

                    free(resp.body);
                    free(resp.err_msg);

                    /* Download certificate */
                    if (cert_url) {
                        sleep(2); /* brief wait for issuance */
                        nonce = acme_get_nonce(ac);
                        if (nonce) {
                            jws = acme_jws(ac, cert_url, "", nonce);
                            free(nonce);
                            if (jws) {
                                resp = http_post(ac->client, cert_url,
                                                  (uint8_t *)jws,
                                                  (uint64_t)strlen(jws),
                                                  "application/jose+json");
                                free(jws);

                                if (!resp.is_err && resp.body &&
                                    resp.body_len > 0) {
                                    /* Write certificate (61.1.5) */
                                    acme_write_file(cert_path,
                                                    resp.body, resp.body_len);
                                }
                                free(resp.body);
                                free(resp.err_msg);
                            }
                        }
                        free(cert_url);
                    }
                } else {
                    free(resp.body);
                    free(resp.err_msg);
                }
            }
        }
        free(fin_payload);
    }

    free(csr_b64);
    free(finalize_url);

    /* Save private key (61.1.5) */
    if (key_path && cert_key) {
        /* 121.19 (ACME-05): temp + O_EXCL + rename, as the certificate is
         * written -- open(O_CREAT|O_TRUNC) followed a planted symlink. */
        (void)acme_write_key(key_path, cert_key);
    }

    EVP_PKEY_free(cert_key);

    /* Clear challenges */
    acme_challenges_clear();

    return 0;
}

/* ── Certificate storage (61.1.5) ────────────────────────────────────── */

/* Write data to file atomically with secure permissions */
static int acme_write_file(const char *path, const uint8_t *data, size_t len)
{
    /* 122.2: was a char[1024]; for a path of 1024+ bytes the temporary name
     * was truncated to a prefix of `path` itself -- no ".tmp.<pid>" suffix --
     * so O_TRUNC opened and clobbered whatever file had that name. */
    TkStrBuf tb = TK_STRBUF_INIT;
    strbuf_appendf(&tb, "%s.tmp.%d", path, (int)getpid());
    char *tmp = strbuf_take(&tb);
    if (!tmp) return -1;

    /* 121.19 (ACME-05): the temp name is predictable, and O_CREAT|O_TRUNC
     * followed whatever was already there -- a symlink planted at
     * "<path>.tmp.<pid>" redirected the write to its target.  Remove any
     * leftover, then create exclusively: if something reappears in between,
     * O_EXCL fails and nothing is written. */
    (void)unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) { free(tmp); return -1; }

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
        if (w <= 0) { close(fd); unlink(tmp); free(tmp); return -1; }
        written += (size_t)w;
    }
    close(fd);

    /* Atomic rename */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        free(tmp);
        return -1;
    }
    free(tmp);
    return 0;
}

/* acme_write_key -- PEM-encode `key` and write it through acme_write_file
 * (0600, temp + O_EXCL + rename). */
static int acme_write_key(const char *path, EVP_PKEY *key)
{
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio) return -1;
    if (PEM_write_bio_PrivateKey(bio, key, NULL, NULL, 0, NULL, NULL) != 1) {
        BIO_free(bio);
        return -1;
    }
    BUF_MEM *bptr = NULL;
    BIO_get_mem_ptr(bio, &bptr);
    int rc = (bptr && bptr->length > 0)
           ? acme_write_file(path, (const uint8_t *)bptr->data, bptr->length)
           : -1;
    BIO_free(bio);
    return rc;
}

/* ── HTTP-01 challenge handler (61.1.2) ──────────────────────────────── */

/* Route handler for /.well-known/acme-challenge/<token> */
Res acme_challenge_handler(Req req)
{
    Res res = {0};
    const char *path = req.path;

    /* Strip prefix */
    const char *prefix = "/.well-known/acme-challenge/";
    if (!path || strncmp(path, prefix, strlen(prefix)) != 0) {
        res.status = 404;
        res.body = "Not Found";
        return res;
    }

    const char *token = path + strlen(prefix);

    /* 121.19 (ACME-02): look up and copy under the lock.  Res.body is not
     * freed by the server, so the copy lives in a per-thread buffer that
     * stays valid until this thread's next challenge request.  A key
     * authorisation is token + "." + a 43-byte thumbprint. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
    static _Thread_local char served[1024];
#else
    static __thread char served[1024];
#endif
    int found = 0;
    pthread_mutex_lock(&g_challenge_mu);
    for (int i = 0; i < g_challenge_count; i++) {
        if (strcmp(g_challenges[i].token, token) == 0) {
            size_t n = strlen(g_challenges[i].key_auth);
            if (n < sizeof served) {
                memcpy(served, g_challenges[i].key_auth, n + 1);
                found = 1;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_challenge_mu);
    if (found) {
        res.status = 200;
        res.body = served;
        return res;
    }

    res.status = 404;
    res.body = "Not Found";
    return res;
}

/* ── Auto-renewal scheduler (61.1.4) ─────────────────────────────────── */

typedef struct {
    AcmeClient  *ac;
    TkTlsCtx    *tls;
    const char  *cert_path;
    const char  *key_path;
    const char  *account_key_path;
    const char **domains;
    int          ndomain;
    int          running;
} AcmeRenewalCtx;

/* Check certificate expiry.  121.19 (ACME-04): this returned the day count
 * and -1 for an error, and an expired certificate's count is negative too,
 * so acme_check_renewal's `days < 0` read "expired" as "error" and never
 * renewed a lapsed certificate.  Now: returns 0 and stores the (signed) day
 * count in *days_out, or returns -1 if the certificate cannot be read. */
static int acme_cert_days_remaining(const char *cert_path, int *days_out)
{
    FILE *f = fopen(cert_path, "r");
    if (!f) return -1;

    X509 *cert = PEM_read_X509(f, NULL, NULL, NULL);
    fclose(f);
    if (!cert) return -1;

    const ASN1_TIME *not_after = X509_get0_notAfter(cert);
    int day, sec;
    if (!ASN1_TIME_diff(&day, &sec, NULL, not_after)) {
        X509_free(cert);
        return -1;
    }
    X509_free(cert);
    /* ASN1_TIME_diff splits the delta into days and seconds of the same
     * sign; expired by less than a day is day == 0, sec < 0. */
    if (day == 0 && sec < 0) day = -1;
    *days_out = day;
    return 0;
}

/* Renewal check — call periodically (e.g. daily) */
int acme_check_renewal(AcmeClient *ac, TkTlsCtx *tls,
                        const char *cert_path, const char *key_path,
                        const char **domains, int ndomain)
{
    int days = 0;
    if (acme_cert_days_remaining(cert_path, &days) != 0) return 0; /* unreadable */
    if (days > 30) return 0; /* no renewal needed; expired (< 0) renews */

    fprintf(stderr, "acme: certificate expires in %d days, renewing...\n", days);

    int rc = acme_order_certificate(ac, domains, ndomain, cert_path, key_path);
    if (rc != 0) {
        fprintf(stderr, "acme: renewal failed\n");
        return -1;
    }

    /* Hot-swap certificate in running TLS context */
    if (tls) {
        if (http_tls_reload_cert(tls, cert_path, key_path) == 0) {
            fprintf(stderr, "acme: certificate renewed and hot-swapped\n");
        } else {
            fprintf(stderr, "acme: renewed but hot-swap failed\n");
            return -1;
        }
    }

    return 0;
}

/* ── DNS-01 challenge hook (61.1.3) ──────────────────────────────────── */

/* DNS-01 creates a TXT record at _acme-challenge.<domain>.
 * The value is SHA-256(key_authorization) base64url-encoded.
 * We provide a hook-based interface — the user supplies a callback. */

typedef int (*AcmeDns01Hook)(const char *domain, const char *txt_value,
                              int action); /* action: 1=create, 0=delete */

static AcmeDns01Hook g_dns01_hook = NULL;

void acme_set_dns01_hook(AcmeDns01Hook hook)
{
    g_dns01_hook = hook;
}

/* Compute the DNS-01 challenge value for a given key authorization */
char *acme_dns01_value(const char *key_auth)
{
    if (!key_auth) return NULL;
    uint8_t digest[32];
    SHA256((uint8_t *)key_auth, strlen(key_auth), digest);
    return b64url(digest, 32);
}

#else /* no OpenSSL */

/* Stub declarations when OpenSSL is not available */
typedef struct AcmeClient AcmeClient;

AcmeClient *acme_client_new(const char *directory_url)
{
    (void)directory_url;
    fprintf(stderr, "acme: requires OpenSSL\n");
    return NULL;
}

void acme_client_free(AcmeClient *ac) { (void)ac; }

#endif /* TK_HAVE_OPENSSL */
