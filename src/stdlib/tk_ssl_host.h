/*
 * tk_ssl_host.h — name the DNS host a TLS client verifies, on OpenSSL 3 and 4
 * (story 121.104).
 *
 * OpenSSL 4.0 deprecates SSL_set1_host in favour of SSL_set1_dnsname, which
 * does not exist in 3.x.  tls.c, http.c and llm.c all pass a DNS name here
 * (a literal address goes to X509_VERIFY_PARAM_set1_ip_asc instead), so the
 * DNS-only call is the exact replacement.  Returns 1 on success, as both do.
 *
 * Header-only and static, like tk_bind.h, so every stdlib translation unit
 * that includes it links without a new object in any link line.
 */
#ifndef TK_SSL_HOST_H
#define TK_SSL_HOST_H

#include <openssl/opensslv.h>
#include <openssl/ssl.h>

static inline int tk_ssl_set1_dnsname(SSL *ssl, const char *host)
{
#if OPENSSL_VERSION_MAJOR >= 4
    return SSL_set1_dnsname(ssl, host);
#else
    return SSL_set1_host(ssl, host);
#endif
}

#endif /* TK_SSL_HOST_H */
