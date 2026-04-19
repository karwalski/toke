#ifndef TK_STDLIB_TLS_H
#define TK_STDLIB_TLS_H

/*
 * tls.h — C interface for the std.tls standard library module.
 *
 * Provides standalone TLS 1.3 connections backed by OpenSSL.
 *
 * Type mappings:
 *   str           = const char *  (null-terminated UTF-8; caller owns)
 *   bool          = int           (0 = false, 1 = true)
 *   ?(T)          = T + is_none flag
 *   T!TlsErr      = T + is_err flag + err_msg
 *
 * malloc is permitted: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own all returned pointers.
 *
 * Story: 72.9
 */

#include <stdint.h>

/* -------------------------------------------------------------------------
 * TlsKeypair — result of tls.gen_self_signed
 * ------------------------------------------------------------------------- */

typedef struct {
    char       *cert_pem;   /* PEM-encoded self-signed X.509 cert; caller frees */
    char       *key_pem;    /* PEM-encoded EC private key (P-384); caller frees */
    int         is_err;     /* 1 on failure */
    const char *err_msg;    /* static string; valid when is_err == 1 */
} TlsKeypairResult;

/* -------------------------------------------------------------------------
 * TlsConfig — configuration for listen and connect
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *cert_pem;       /* local certificate PEM (may be NULL/empty) */
    const char *key_pem;        /* local private key PEM  (may be NULL/empty) */
    const char *peer_cert_pem;  /* pin remote cert PEM    (NULL/empty = no pin) */
    int         require_mutual; /* 1 = demand client cert on server side */
} TlsConfig;

/* -------------------------------------------------------------------------
 * TlsConn — opaque handle to an established TLS connection
 *
 * The id field is a heap-allocated string of the form "<fd>:<ptr>" that
 * uniquely identifies this connection within the process.  It is intended
 * for diagnostics only; callers must not parse it.
 *
 * The internal SSL* and socket fd are stored in a registry keyed by id.
 * ------------------------------------------------------------------------- */

typedef struct {
    char *id;   /* opaque identifier; caller owns; free with tls_conn_free_id */
} TlsConn;

/* Free only the id string inside a TlsConn.  Does NOT close the connection. */
void tls_conn_free_id(TlsConn conn);

/* -------------------------------------------------------------------------
 * TlsConnResult — optional TlsConn (connect returns one of these)
 * ------------------------------------------------------------------------- */

typedef struct {
    TlsConn conn;
    int     is_none; /* 1 when connection failed */
} TlsConnResult;

/* -------------------------------------------------------------------------
 * TlsStrResult — optional string (read / peer_cert return one of these)
 * ------------------------------------------------------------------------- */

typedef struct {
    char *ok;       /* heap-allocated string; caller frees; NULL when is_none */
    int   is_none;  /* 1 when no data / no cert */
} TlsStrResult;

/* -------------------------------------------------------------------------
 * Connection callback type used by tls_listen
 * ------------------------------------------------------------------------- */

typedef void (*TlsConnCallback)(TlsConn conn);

/* =========================================================================
 * API
 * ========================================================================= */

/*
 * tls.gen_self_signed(common_name, valid_days) -> TlsKeypair!TlsErr
 *
 * Generates a P-384 EC keypair and a self-signed X.509 v3 certificate.
 * On success: result.cert_pem and result.key_pem are heap-allocated PEM
 * strings; caller frees both.
 * On failure: result.is_err == 1 and result.err_msg points to a static
 * error description.
 */
TlsKeypairResult tls_gen_self_signed(const char *common_name, int32_t valid_days);

/*
 * tls.listen(port, cfg, cb) -> bool
 *
 * Binds a TCP socket on `port`, accepts TLS connections in a loop, and calls
 * `cb` in a new POSIX thread for each accepted connection.  Does not return
 * on success.  Returns 0 if the socket or TLS context cannot be created.
 */
int tls_listen(int32_t port, TlsConfig cfg, TlsConnCallback cb);

/*
 * tls.connect(host, port, cfg) -> ?(TlsConn)
 *
 * Opens a TCP connection to host:port and performs a TLS 1.3 handshake.
 * Returns result.is_none == 0 and a valid conn on success.
 * Returns result.is_none == 1 on any failure.
 */
TlsConnResult tls_connect(const char *host, int32_t port, TlsConfig cfg);

/*
 * tls.read(conn) -> ?(str)
 *
 * Reads available data from conn.  Returns a heap-allocated string (caller
 * frees) on success.  Returns is_none == 1 on EOF or error.
 */
TlsStrResult tls_read(TlsConn conn);

/*
 * tls.write(conn, data) -> bool
 *
 * Writes data to conn.  Returns 1 on success, 0 on failure.
 */
int tls_write(TlsConn conn, const char *data);

/*
 * tls.close(conn) -> bool
 *
 * Performs a clean TLS shutdown and closes the underlying socket.
 * Returns 1 on success, 0 if already closed or on error.
 * The conn handle must not be used after this call.
 */
int tls_close(TlsConn conn);

/*
 * tls.peer_cert(conn) -> ?(str)
 *
 * Returns the peer's PEM-encoded X.509 certificate as presented during the
 * handshake.  Returns is_none == 1 if no peer certificate was presented.
 * The returned string is heap-allocated; caller frees.
 */
TlsStrResult tls_peer_cert(TlsConn conn);

/*
 * tls.fingerprint(pem) -> str
 *
 * Parses `pem`, computes SHA-256 of the DER encoding, and returns a
 * lowercase hex string (64 chars + NUL).  Returns an empty heap-allocated
 * string on parse error.  Caller frees.
 */
char *tls_fingerprint(const char *pem);

/*
 * tls.pairing_code(conn) -> str
 *
 * Derives a 6-digit decimal code from the XOR of the local and peer
 * certificate fingerprints.  Returns "000000" if either cert is unavailable.
 * Returned string is heap-allocated; caller frees.
 */
char *tls_pairing_code(TlsConn conn);

#endif /* TK_STDLIB_TLS_H */
