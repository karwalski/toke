/*
 * tk_tls_groups.h — the one key-exchange group setup every stdlib TLS
 * context uses (stories 122.5, 122.15).
 *
 * tk_tls_set_groups(ctx) applies TK_TLS_GROUPS_HYBRID (tls.h): the
 * post-quantum hybrid X25519MLKEM768 first, then classical groups for peers
 * that do not offer it.  OpenSSL rejects a whole list when one name in it is
 * unknown (verified on 4.0.3) -- which is what an OpenSSL < 3.5 does with
 * X25519MLKEM768 -- so on rejection the context gets TK_TLS_GROUPS_CLASSICAL
 * instead.  If neither list is accepted it returns 0 and the caller fails
 * the context closed, exactly as std.tls always has (ADR-0013 (c)).
 *
 * Before 122.15 only std.tls (build_ssl_ctx in tls.c) did this.  The
 * std.http server contexts (http_tls_ctx_new, the SNI contexts,
 * http_tls_ctx_new_config), the std.http client and the std.llm client set
 * no list at all, so they were hybrid only because OpenSSL 3.5+'s built-in
 * default happens to prefer X25519MLKEM768: an openssl.cnf Groups line
 * changed it, and nothing could see a classical connection.
 *
 * OBSERVING THE NEGOTIATED GROUP.  The std.http and std.llm connections have
 * no toke-level handle to ask (std.tls has tls.group, 122.13), and adding a
 * public function needs a spec amendment.  So when the environment variable
 * TK_TLS_TRACE_GROUP is set (to anything non-empty) at the time a context is
 * built, tk_tls_set_groups also installs an info callback that writes one
 * line per completed handshake to stderr:
 *
 *     tk-tls-group: server X25519MLKEM768
 *     tk-tls-group: client x25519
 *
 * and one line saying which list the context received:
 *
 *     tk-tls-groups: hybrid        (or: classical)
 *
 * Unset (the production case) it installs nothing and writes nothing.  This
 * is a debugging and test hook (C1200-C1202), not a stdlib API.  The info
 * callback is the context's only one: no stdlib context sets another.
 *
 * Returns 1 when the hybrid list was applied, 2 when the classical fallback
 * was, 0 when neither was accepted.
 *
 * Header-only and static, like tk_ssl_host.h, so every stdlib translation
 * unit that includes it links without a new object in any link line.
 */
#ifndef TK_TLS_GROUPS_H
#define TK_TLS_GROUPS_H

#include <stdio.h>
#include <stdlib.h>
#include <openssl/opensslv.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include "tls.h"    /* TK_TLS_GROUPS_HYBRID / TK_TLS_GROUPS_CLASSICAL */

#define TK_TLS_GROUPS_NONE      0
#define TK_TLS_GROUPS_APPLIED_HYBRID    1
#define TK_TLS_GROUPS_APPLIED_CLASSICAL 2

/* The group a completed handshake settled on, or NULL (TLS 1.2 with a
 * non-EC key exchange, or no handshake yet).  Same lookup as tls.group. */
static inline const char *tk_tls_negotiated_group(SSL *ssl)
{
#if OPENSSL_VERSION_NUMBER >= 0x30200000L
    return SSL_get0_group_name(ssl);
#else
    int nid = SSL_get_negotiated_group(ssl);
    return nid ? SSL_group_to_name(ssl, nid) : NULL;
#endif
}

static inline void tk_tls_trace_group_cb(const SSL *ssl, int where, int ret)
{
    (void)ret;
    if (!(where & SSL_CB_HANDSHAKE_DONE)) return;
    const char *g = tk_tls_negotiated_group((SSL *)ssl);
    fprintf(stderr, "tk-tls-group: %s %s\n",
            SSL_is_server((SSL *)ssl) ? "server" : "client", g ? g : "(none)");
}

static inline int tk_tls_set_groups(SSL_CTX *ctx)
{
    int applied = TK_TLS_GROUPS_NONE;
    if (SSL_CTX_set1_groups_list(ctx, TK_TLS_GROUPS_HYBRID) == 1) {
        applied = TK_TLS_GROUPS_APPLIED_HYBRID;
    } else {
        /* Drop the rejection's error-queue entry so a later SSL_get_error
         * on this thread does not report it against the connection. */
        ERR_clear_error();
        if (SSL_CTX_set1_groups_list(ctx, TK_TLS_GROUPS_CLASSICAL) == 1)
            applied = TK_TLS_GROUPS_APPLIED_CLASSICAL;
    }

    const char *trace = getenv("TK_TLS_TRACE_GROUP");
    if (trace && *trace) {
        fprintf(stderr, "tk-tls-groups: %s\n",
                applied == TK_TLS_GROUPS_APPLIED_HYBRID    ? "hybrid"
              : applied == TK_TLS_GROUPS_APPLIED_CLASSICAL ? "classical"
                                                           : "none");
        if (applied) SSL_CTX_set_info_callback(ctx, tk_tls_trace_group_cb);
    }
    return applied;
}

#endif /* TK_TLS_GROUPS_H */
