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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

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

static AcmeChallenge g_challenges[ACME_MAX_CHALLENGES];
static int g_challenge_count = 0;

/* Forward declarations */
static int acme_write_file(const char *path, const uint8_t *data, size_t len);

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
    /* Header */
    char header[1024];
    if (ac->account_url) {
        snprintf(header, sizeof(header),
                 "{\"alg\":\"ES256\",\"kid\":\"%s\",\"nonce\":\"%s\",\"url\":\"%s\"}",
                 ac->account_url, nonce, url);
    } else {
        char *jwk = acme_jwk_json(ac->account_key);
        if (!jwk) return NULL;
        snprintf(header, sizeof(header),
                 "{\"alg\":\"ES256\",\"jwk\":%s,\"nonce\":\"%s\",\"url\":\"%s\"}",
                 jwk, nonce, url);
        free(jwk);
    }

    char *hdr_b64 = b64url((uint8_t *)header, strlen(header));
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
        acme_client_new(NULL); /* will return NULL, just cleanup */
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

    FILE *f = fopen(key_path, "r");
    if (f) {
        /* Load existing key */
        ac->account_key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
        fclose(f);
        return ac->account_key ? 0 : -1;
    }

    /* Generate new key */
    ac->account_key = acme_gen_key();
    if (!ac->account_key) return -1;

    /* Save with secure permissions (61.1.5) */
    int fd_key = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd_key < 0) return -1;
    f = fdopen(fd_key, "w");
    if (!f) { close(fd_key); return -1; }
    PEM_write_PrivateKey(f, ac->account_key, NULL, NULL, 0, NULL, NULL);
    fclose(f);
    return 0;
}

/* Register or find existing account */
int acme_register_account(AcmeClient *ac, const char *email)
{
    if (!ac || !ac->account_key) return -1;

    char *nonce = acme_get_nonce(ac);
    if (!nonce) return -1;

    char payload[512];
    if (email) {
        snprintf(payload, sizeof(payload),
                 "{\"termsOfServiceAgreed\":true,"
                 "\"contact\":[\"mailto:%s\"]}", email);
    } else {
        snprintf(payload, sizeof(payload),
                 "{\"termsOfServiceAgreed\":true}");
    }

    char *jws = acme_jws(ac, ac->new_account_url, payload, nonce);
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

/* Place a new order for a certificate */
int acme_order_certificate(AcmeClient *ac, const char **domains,
                            int ndomain, const char *cert_path,
                            const char *key_path)
{
    if (!ac || !ac->account_url || !domains || ndomain <= 0) return -1;

    /* Build identifiers JSON */
    char identifiers[4096];
    int off = 0;
    off += snprintf(identifiers + off, sizeof(identifiers) - (size_t)off,
                    "{\"identifiers\":[");
    for (int i = 0; i < ndomain; i++) {
        if (i > 0) off += snprintf(identifiers + off,
                                   sizeof(identifiers) - (size_t)off, ",");
        off += snprintf(identifiers + off, sizeof(identifiers) - (size_t)off,
                        "{\"type\":\"dns\",\"value\":\"%s\"}", domains[i]);
    }
    off += snprintf(identifiers + off, sizeof(identifiers) - (size_t)off, "]}");

    char *nonce = acme_get_nonce(ac);
    if (!nonce) return -1;

    char *jws = acme_jws(ac, ac->new_order_url, identifiers, nonce);
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

    free(resp.body);
    free(resp.err_msg);

    /* Process each authorization — complete HTTP-01 challenges */
    for (int i = 0; i < nauth; i++) {
        nonce = acme_get_nonce(ac);
        if (!nonce) continue;

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
                if (g_challenge_count < ACME_MAX_CHALLENGES) {
                    g_challenges[g_challenge_count].token = strdup(token);
                    g_challenges[g_challenge_count].key_auth = strdup(key_auth);
                    g_challenge_count++;
                }

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

    /* Wait for challenge validation (poll with backoff) */
    for (int attempt = 0; attempt < 30; attempt++) {
        sleep(2);
        /* Re-fetch the order to check status */
        /* For now, proceed to finalize after waiting */
        break; /* TODO: proper polling */
    }

    /* Generate CSR */
    EVP_PKEY *cert_key = acme_gen_key();
    if (!cert_key) { free(finalize_url); return -1; }

    X509_REQ *csr = X509_REQ_new();
    X509_REQ_set_version(csr, 0);
    X509_REQ_set_pubkey(csr, cert_key);

    /* Add SAN extension */
    STACK_OF(X509_EXTENSION) *exts = sk_X509_EXTENSION_new_null();
    char san_buf[4096];
    int san_off = 0;
    for (int i = 0; i < ndomain; i++) {
        if (i > 0) san_off += snprintf(san_buf + san_off,
                                       sizeof(san_buf) - (size_t)san_off, ",");
        san_off += snprintf(san_buf + san_off,
                            sizeof(san_buf) - (size_t)san_off,
                            "DNS:%s", domains[i]);
    }
    X509_EXTENSION *san_ext = X509V3_EXT_nconf_nid(
        NULL, NULL, NID_subject_alt_name, san_buf);
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
        char fin_payload[8192];
        snprintf(fin_payload, sizeof(fin_payload),
                 "{\"csr\":\"%s\"}", csr_b64);

        nonce = acme_get_nonce(ac);
        if (nonce) {
            jws = acme_jws(ac, finalize_url, fin_payload, nonce);
            free(nonce);
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
    }

    free(csr_b64);
    free(finalize_url);

    /* Save private key (61.1.5) */
    if (key_path && cert_key) {
        int fd_key = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd_key >= 0) {
            FILE *f = fdopen(fd_key, "w");
            if (f) {
                PEM_write_PrivateKey(f, cert_key, NULL, NULL, 0, NULL, NULL);
                fclose(f);
            } else {
                close(fd_key);
            }
        }
    }

    EVP_PKEY_free(cert_key);

    /* Clear challenges */
    for (int i = 0; i < g_challenge_count; i++) {
        free(g_challenges[i].token);
        free(g_challenges[i].key_auth);
    }
    g_challenge_count = 0;

    return 0;
}

/* ── Certificate storage (61.1.5) ────────────────────────────────────── */

/* Write data to file atomically with secure permissions */
static int acme_write_file(const char *path, const uint8_t *data, size_t len)
{
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;

    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
        if (w <= 0) { close(fd); unlink(tmp); return -1; }
        written += (size_t)w;
    }
    close(fd);

    /* Atomic rename */
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
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

    for (int i = 0; i < g_challenge_count; i++) {
        if (strcmp(g_challenges[i].token, token) == 0) {
            res.status = 200;
            res.body = g_challenges[i].key_auth;
            return res;
        }
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

/* Check certificate expiry — returns days until expiry, -1 on error */
static int acme_cert_days_remaining(const char *cert_path)
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
    return day;
}

/* Renewal check — call periodically (e.g. daily) */
int acme_check_renewal(AcmeClient *ac, TkTlsCtx *tls,
                        const char *cert_path, const char *key_path,
                        const char **domains, int ndomain)
{
    int days = acme_cert_days_remaining(cert_path);
    if (days < 0 || days > 30) return 0; /* no renewal needed */

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
