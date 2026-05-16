/*
 * tls_glue.c — i64-ABI wrappers for std.tls module.
 *
 * TLS operations require linking against a TLS library (OpenSSL/LibreSSL/
 * SecureTransport). These stubs allow linking without the TLS backend.
 */

#include <stdint.h>
#include <stdlib.h>

/* tls.tlsconfig(cert, key) — create a TLS configuration, return handle */
int64_t tk_tls_tlsconfig_w(int64_t cert_path, int64_t key_path) {
    (void)cert_path;
    (void)key_path;
    return 0;
}

/* tls.connecttls(host, port) — establish TLS connection */
int64_t tk_tls_connecttls_w(int64_t host, int64_t port) {
    (void)host;
    (void)port;
    return 0;
}

/* tls.listentls(port, config) — listen for TLS connections */
int64_t tk_tls_listentls_w(int64_t port, int64_t config) {
    (void)port;
    (void)config;
    return 0;
}

/* tls.read(conn) — read from TLS connection */
int64_t tk_tls_read_w(int64_t conn) {
    (void)conn;
    return 0;
}

/* tls.write(conn, data) — write to TLS connection */
int64_t tk_tls_write_w(int64_t conn, int64_t data) {
    (void)conn;
    (void)data;
    return 0;
}

/* tls.close(conn) — close TLS connection */
int64_t tk_tls_close_w(int64_t conn) {
    (void)conn;
    return 0;
}

/* tls.genselfsigned(cn) — generate self-signed cert/key pair, return paths */
int64_t tk_tls_genselfsigned_w(int64_t common_name) {
    (void)common_name;
    return 0;
}
