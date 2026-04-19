# std.tls — Standalone TLS 1.3 Connections

## Overview

The `std.tls` module provides a standalone TLS 1.3 interface for building
encrypted peer-to-peer and client-server connections. It covers key and
certificate generation, server-side listening, client-side connections, I/O,
certificate pinning, and human-friendly pairing codes for out-of-band
verification.

All connections enforce **TLS 1.3 only**. No fallback to TLS 1.2 or earlier
is permitted. This module is independent of `std.http`; if you need HTTPS, use
`http.serve_tls` from `std.http` instead.

Certificates are represented as PEM strings throughout the API, making them
easy to persist, log, and inspect. Private keys are also PEM strings — treat
them as secrets.

The underlying implementation uses OpenSSL (already a dependency of the toke
runtime).

## Types

### TlsConfig

Configuration record for both server and client endpoints.

| Field           | Type | Meaning |
|-----------------|------|---------|
| cert_pem        | str  | PEM-encoded certificate for this endpoint (may be empty for anonymous clients) |
| key_pem         | str  | PEM-encoded private key matching `cert_pem` (required when `cert_pem` is set) |
| peer_cert_pem   | str  | PEM-encoded certificate to pin the remote peer against (empty = no pinning) |
| require_mutual  | bool | When `true`, enforce mutual TLS: the remote must present a certificate |

```toke
let cfg = $TlsConfig{
    cert_pem      = my_cert;
    key_pem       = my_key;
    peer_cert_pem = "";
    require_mutual = false;
};
```

### TlsConn

An opaque handle to an established TLS connection. Pass this to `tls.read`,
`tls.write`, `tls.close`, `tls.peer_cert`, and `tls.pairing_code`.

| Field | Type | Meaning |
|-------|------|---------|
| id    | str  | Opaque connection identifier (implementation-defined; do not parse) |

### TlsKeypair

A freshly generated certificate and private key, both as PEM strings.

| Field    | Type | Meaning |
|----------|------|---------|
| cert_pem | str  | PEM-encoded self-signed X.509 certificate |
| key_pem  | str  | PEM-encoded EC private key (P-384) |

### TlsErr

Sum type for errors returned by fallible TLS operations.

| Variant | Meaning |
|---------|---------|
| CertErr | Certificate generation, parsing, or verification error |
| ConnErr | Connection-level failure (handshake failure, peer rejection) |
| PinErr  | Certificate pinning mismatch: peer presented a different certificate |
| IoErr   | Underlying socket or I/O error |

## Functions

### tls.gen_self_signed(common_name: str; valid_days: i32): TlsKeypair!TlsErr

Generates a fresh P-384 EC keypair and a self-signed X.509 certificate with
the given `common_name` as both the subject CN and the issuer CN. The
certificate is valid for `valid_days` days from the moment of generation.

Returns a `TlsKeypair` containing both the certificate and private key as PEM
strings. Returns `TlsErr.CertErr` if certificate generation fails.

**Example:**
```toke
let kp = tls.gen_self_signed("my-device"; 365);
(* kp.cert_pem  -- "-----BEGIN CERTIFICATE-----\n..." *)
(* kp.key_pem   -- "-----BEGIN EC PRIVATE KEY-----\n..." *)
```

### tls.listen(port: i32; cfg: TlsConfig; cb: fn(TlsConn): void): bool

Binds a TCP listening socket on `port` and accepts TLS connections in a loop.
Each accepted connection is wrapped with TLS using `cfg`, and `cb` is invoked
in a new thread with the resulting `TlsConn`.

When `cfg.require_mutual` is `true`, the server demands a client certificate.
When `cfg.peer_cert_pem` is non-empty, only clients presenting exactly that
certificate are accepted.

Returns `false` immediately if the socket cannot be bound or if the TLS
context cannot be created. Does not return once successfully listening (runs
until the process exits).

**Example:**
```toke
let kp = tls.gen_self_signed("server"; 365);
let cfg = $TlsConfig{
    cert_pem = kp.cert_pem; key_pem = kp.key_pem;
    peer_cert_pem = ""; require_mutual = false;
};
tls.listen(8443; cfg; fn(conn) {
    let msg = tls.read(conn);
    tls.write(conn; "hello back");
    tls.close(conn);
});
```

### tls.connect(host: str; port: i32; cfg: TlsConfig): ?(TlsConn)

Opens a TCP connection to `host:port` and performs a TLS 1.3 handshake.
Returns `?(TlsConn)` on success, or `none` if the connection or handshake
fails.

If `cfg.cert_pem` and `cfg.key_pem` are non-empty, the client presents them
as its certificate during the handshake (required for mutual TLS servers).

If `cfg.peer_cert_pem` is non-empty, the server's certificate is pinned
against it; the connection is rejected if the server presents a different
certificate.

**Example:**
```toke
let conn = tls.connect("127.0.0.1"; 8443; cfg);
if conn {
    tls.write(conn!; "hello");
    let reply = tls.read(conn!);
    tls.close(conn!);
}
```

### tls.read(conn: TlsConn): ?(str)

Reads available data from `conn` and returns it as a string. Returns `none`
if the connection has been closed by the peer or if an error occurs.

**Example:**
```toke
let data = tls.read(conn);
(* data = some("hello world") or none *)
```

### tls.write(conn: TlsConn; data: str): bool

Writes `data` to `conn`. Returns `true` on success, `false` if the write
fails (e.g. because the connection was closed).

**Example:**
```toke
let ok = tls.write(conn; "ping");
```

### tls.close(conn: TlsConn): bool

Performs a clean TLS shutdown on `conn` and closes the underlying socket.
Returns `true` on success, `false` if the connection was already closed or an
error occurred. After `tls.close`, the `TlsConn` handle must not be used.

**Example:**
```toke
tls.close(conn);
```

### tls.peer_cert(conn: TlsConn): ?(str)

Returns the peer's PEM-encoded X.509 certificate as presented during the TLS
handshake. Returns `none` if the peer did not present a certificate (e.g. when
mutual TLS was not required on the server side and the client chose not to
present one).

**Example:**
```toke
let pem = tls.peer_cert(conn);
if pem {
    let fp = tls.fingerprint(pem!);
    log.info("peer fingerprint: " + fp);
}
```

### tls.fingerprint(pem: str): str

Parses the PEM-encoded X.509 certificate in `pem`, converts it to DER, and
returns the SHA-256 hash of the DER encoding as a lowercase hex string (64
characters). Returns an empty string if `pem` cannot be parsed.

This is the same fingerprint shown by `openssl x509 -fingerprint -sha256`.

**Example:**
```toke
let fp = tls.fingerprint(kp.cert_pem);
(* fp = "3a4f...b2c1" (64 hex chars) *)
```

### tls.pairing_code(conn: TlsConn): str

Derives a 6-digit decimal pairing code from the XOR of the local and peer
certificate SHA-256 fingerprints. Both endpoints on a correctly established
connection will produce the same code, so it can be read aloud or displayed
for out-of-band confirmation that no man-in-the-middle is present.

Returns `"000000"` if either certificate is unavailable.

**Example:**
```toke
let code = tls.pairing_code(conn);
log.info("confirm pairing code: " + code);
(* user compares this with the remote display *)
```

## Security Notes

- **TLS 1.3 only.** No protocol downgrade is possible.
- **Certificate pinning** (`peer_cert_pem`) provides strong identity
  guarantees beyond the standard CA chain. Use it for device-to-device or
  service-to-service connections where you control both endpoints.
- **Mutual TLS** (`require_mutual = true`) ensures both sides are
  authenticated. Combine with pinning for the strongest guarantee.
- **Pairing codes** are a 6-digit approximation for human verification — they
  are not a substitute for full fingerprint comparison in high-security
  contexts.
- Generated keys use **P-384** (NIST secp384r1), which provides ~192-bit
  security. This is the recommended curve for new deployments as of 2024.
