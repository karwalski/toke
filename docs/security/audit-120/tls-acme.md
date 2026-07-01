## 120.8 — TLS / ACME / transport

Scope: static/manual review of the toke stdlib transport surface — `src/stdlib/tls.c` (+ `tls.h`, `tls_glue.c`), `src/stdlib/acme.c`, and `src/stdlib/net.c` (+ `net.h`, `net_glue.c`). Method: line-by-line source read focused on certificate validation (chain/hostname), protocol/cipher/version floor, ACME challenge handling, account/key storage, private-key file permissions, and error-path info leaks. No build or dynamic run was performed (parallel audits share the tree); items needing runtime proof are listed under "Dynamic-testing follow-up".

Overall the TLS module makes several good choices (TLS 1.3-only floor, P-384 self-signed generation, exact-cert pinning, 0600 key files in ACME). The findings below are the concrete defects I could ground in code.

---

### TLS-01 — Stack buffer overflow in `x509_fingerprint_hex` (fixed 8 KiB DER buffer)
- Severity: High
- Reachability: remote-unauth
- Location: `src/stdlib/tls.c:167-169` (helper); reached from `tls_fingerprint` (`tls.c:744-762`) and `tls_pairing_code` (`tls.c:786-790`, peer cert).

Description: `x509_fingerprint_hex` DER-encodes a certificate into a fixed stack buffer with no length check:
```c
unsigned char der[8192];
unsigned char *p = der;
int der_len = i2d_X509(cert, &p);   /* writes full DER into der[], advances p */
```
`i2d_X509(cert, &p)` with a non-NULL `*p` writes the entire DER encoding into the buffer pointed at by `p` and does not bound the write to 8192 bytes. `SHA256(der, der_len, ...)` then also reads `der_len` bytes. A certificate whose DER encoding exceeds 8192 bytes (readily achievable with a large RSA public key plus many `subjectAltName` entries or oversized extensions) overflows the stack frame.

Impact: The peer certificate is attacker-controlled. `tls_pairing_code` — an advertised API — unconditionally calls this helper on `SSL_get_peer_certificate()` (`tls.c:782-788`), and `tls_fingerprint` runs it on any caller-supplied PEM. A malicious TLS peer presenting an oversized cert triggers stack memory corruption (crash/DoS with canaries; potential RCE without). Remotely reachable during/after a handshake.

Recommended fix: query the length first and heap-allocate, e.g. `int len = i2d_X509(cert, NULL); if (len <= 0) return -1; unsigned char *der = malloc(len); unsigned char *p = der; i2d_X509(cert, &p); ... free(der);` — mirroring the correct pattern already used for the CSR in `acme.c:714-717`.

---

### TLS-02 — `tls_connect` performs no server authentication by default; no system-CA trust path
- Severity: Medium
- Reachability: remote-unauth (network MITM)
- Location: `src/stdlib/tls.c:250-254` (ctx), `tls.c:564-604` (connect); no `SSL_CTX_set_default_verify_paths` anywhere.

Description: `build_ssl_ctx` sets `SSL_VERIFY_NONE` on the client whenever no pin and no mutual-TLS are configured:
```c
} else if (!cfg.require_mutual) {
    /* No pinning, no mutual TLS: skip server cert verification on client. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
}
```
`tls_connect` then completes the handshake with no certificate check and no hostname (`X509_VERIFY_PARAM_set1_host`) verification. There is also no call anywhere to load the system trust store, so authenticated-against-a-CA connections are impossible with this module; the only authentication mode offered is exact-cert pinning (`peer_cert_pem`).

Impact: A caller doing a plain `tls.connect(host, port, {})` gets a fully MITM-able channel — any attacker on-path can present any certificate and be accepted. This is documented as intentional (self-signed/pairing model), but the default is unsafe-by-default for a general-purpose TLS client and there is no supported way to get CA+hostname verification. Confidence is confirmed for the code behavior; the risk rating assumes callers who reasonably expect `tls.connect` to authenticate the server.

Recommended fix: default to `SSL_VERIFY_PEER` with `SSL_CTX_set_default_verify_paths` and `SSL_set1_host(ssl, host)` for hostname checking; require callers to opt in explicitly (a distinct `insecure`/`pin`-only flag) to disable it. At minimum, document loudly that the default provides no server authentication.

---

### TLS-03 — Connection-registry race → potential use-after-free of `SSL*`
- Severity: Medium
- Reachability: local (multi-threaded app), remote-adjacent
- Location: `src/stdlib/tls.c:97-131` (find/unlock/remove), `tls.c:635-699` (read/write/close).

Description: `registry_find` returns a pointer to a `ConnEntry` and the mutex is released via `registry_unlock` before the caller finishes using `e->ssl`. `tls_close` reads `ssl`/`fd`, unlocks, then `SSL_free(ssl)` and only afterward calls `registry_remove` (`tls.c:686-698`). Between the unlock in `tls_close` and `registry_remove`, another thread calling `tls_read`/`tls_write`/`tls_pairing_code` on the same id can `registry_find` the still-`in_use` entry and dereference the freed `SSL*`.

Impact: Use-after-free / double-free of the OpenSSL `SSL` object across threads. `tls_listen` dispatches each connection to its own thread, and ids are heap strings that an app may share, so concurrent close+read on one connection is realistic. Memory corruption / crash.

Recommended fix: hold the registry lock for the full duration of operations on `e->ssl` (or refcount entries), and remove the entry from the registry under the lock before freeing the `SSL*`.

---

### ACME-01 — `snprintf` length-accumulation overflow in `identifiers[4096]` / `san_buf[4096]`
- Severity: Medium
- Reachability: local (remote-auth if domains derive from user input, e.g. multi-tenant issuance)
- Location: `src/stdlib/acme.c:443-453` (identifiers), `acme.c:695-703` (SAN).

Description: Both loops accumulate `snprintf`'s return value into an offset used as `sizeof(buf) - off`:
```c
off += snprintf(identifiers + off, sizeof(identifiers) - (size_t)off, ...);
```
`snprintf` returns the number of bytes it *would* have written, so once the JSON grows past 4096, `off` exceeds `sizeof(identifiers)` and `sizeof(identifiers) - (size_t)off` underflows to a huge `size_t`. The next iteration then calls `snprintf(identifiers + off /* past end */, HUGE, ...)`, writing past the fixed stack buffer.

Impact: Stack buffer overflow when the combined domain list exceeds ~4 KB (many SAN domains, or long/attacker-influenced domain strings). Memory corruption during order placement / CSR building.

Recommended fix: clamp after each write (`if (off >= (int)sizeof(buf)) { fail; }`), or build the JSON/SAN string with a growable heap buffer.

---

### ACME-02 — HTTP-01 challenge store is a global mutated without synchronization; freed while the handler may read it
- Severity: Medium
- Reachability: remote-unauth (challenge handler served on public port)
- Location: `src/stdlib/acme.c:58-59` (globals), `acme.c:590-594` (register), `acme.c:810-814` (clear), `acme.c:864-870` (handler).

Description: `g_challenges[]` / `g_challenge_count` are plain globals. `acme_order_certificate` populates them, then at the end frees every `token`/`key_auth` and resets the count (`acme.c:810-814`). Meanwhile `acme_challenge_handler` runs on the web-server's request threads and reads `g_challenges[i].token` / `.key_auth` with no lock. A request that races the cleanup (or a concurrent renewal) can read a freed `key_auth`/`token` pointer.

Impact: Use-after-free / data race — crash or serving freed heap memory in an HTTP-01 response, on a remotely reachable endpoint. The window is narrow but real, and renewals recur unattended (61.1.4).

Recommended fix: guard the challenge table with a mutex (or an RCU/atomic swap), and snapshot/copy `key_auth` under the lock in the handler before returning it.

---

### ACME-03 — JSON injection via unescaped `email` / domain values in ACME payloads
- Severity: Low
- Reachability: local (remote-auth if values are user-supplied)
- Location: `src/stdlib/acme.c:394-396` (email), `acme.c:451` (domain), `acme.c:702` (SAN).

Description: `email` and domain strings are interpolated directly into JSON with `snprintf("...\"%s\"...", email)` without escaping. A value containing `"` or `\` breaks out of the JSON string and can inject additional fields into the account/order request.

Impact: Malformed or attacker-shaped ACME requests (e.g. injecting extra JSON keys into `newAccount`/`newOrder`). Low in the common single-operator case; higher where these values originate from untrusted input.

Recommended fix: JSON-escape all interpolated string values, or use a real JSON builder.

---

### ACME-04 — Expired certificate is never renewed (`days < 0` conflates error and already-expired)
- Severity: Low
- Reachability: local
- Location: `src/stdlib/acme.c:891-908` (`acme_cert_days_remaining`), `acme.c:915-916` (`acme_check_renewal`).

Description: `acme_cert_days_remaining` returns the signed `day` from `ASN1_TIME_diff`, which is negative for an already-expired cert; it uses the same negative range to signal errors (`return -1`). `acme_check_renewal` then does `if (days < 0 || days > 30) return 0;` — so an already-expired certificate (negative days) is treated as "no renewal needed" and is never renewed.

Impact: Availability/security-hygiene: once a cert lapses, auto-renewal stops attempting, leaving the service on an expired certificate indefinitely. Not a memory-safety issue.

Recommended fix: distinguish parse errors from a valid negative delta; treat `days <= 30` (including negatives) as "renew".

---

### ACME-05 — Predictable temp filename and missing `O_EXCL` on key/cert writes (symlink/clobber)
- Severity: Low
- Reachability: local
- Location: `src/stdlib/acme.c:824-827` (temp name + open), `acme.c:375` and `acme.c:795` (key open `O_CREAT|O_TRUNC` without `O_EXCL`).

Description: `acme_write_file` builds a predictable temp path `"%s.tmp.<pid>"` and opens it with `O_CREAT|O_TRUNC` (no `O_EXCL`), following any pre-existing symlink at that path. The private-key writes at `acme.c:375`/`795` likewise use `O_CREAT|O_TRUNC` without `O_EXCL`. If the target directory is shared/writable, an attacker can pre-plant a symlink to redirect the write or clobber a victim file.

Impact: In a shared-directory scenario, cert/key writes can follow an attacker symlink (write-where primitive on public cert data, or unexpected clobber). Bounded because paths are normally app-owned and mode is 0600. Note the cert-key path here is only weakly protected against a pre-existing hostile file.

Recommended fix: create with `O_CREAT|O_EXCL` (unlinking the temp first) and/or verify the parent directory is not world-writable; keep the 0600 mode.

---

### TLS-06 — Connection id embeds a live heap/`SSL*` pointer (ASLR leak if exposed)
- Severity: Low / Info
- Reachability: local
- Location: `src/stdlib/tls.c:78-79`.

Description: `registry_add` builds the connection id as `"%d:%p"` including the raw `SSL*` pointer. `tls.h` calls the id "for diagnostics only", but if an app ever logs or returns it, it discloses a heap pointer (ASLR bypass aid).

Recommended fix: derive the id from the index plus a random/counter token, not a live pointer.

---

### ACME-07 — Account key loaded from disk without permission check
- Severity: Info
- Reachability: local
- Location: `src/stdlib/acme.c:362-368`.

Description: When an account key already exists, it is `fopen`ed and parsed with no check that the file is not group/world-readable. New keys are correctly created 0600, but a pre-existing loosely-permissioned key is used silently.

Recommended fix: `stat` the key and warn/refuse if mode is broader than 0600.

---

## Dynamic-testing follow-up
- TLS-01: build with ASAN and drive `tls_pairing_code`/`tls_fingerprint` against a peer presenting a >8 KiB DER certificate (RSA-4096 + many SANs) to confirm the stack overflow and characterize exploitability under the shipped hardening flags.
- TLS-03 / ACME-02: run a threaded stress harness (concurrent `tls_read`+`tls_close` on one id; concurrent HTTP-01 requests during a renewal cycle) under ASAN/TSan to confirm the use-after-free/data-race windows.
- ACME-01: fuzz `acme_order_certificate` with many long domains (SAN JSON > 4 KB) under ASAN to confirm the `snprintf` accumulation overflow.
- TLS-02: integration test that `tls.connect` to a MITM proxy with a bogus cert succeeds by default (proving the missing authentication), and validate any hardened default.

## Positive observations
- TLS 1.3 is pinned as both floor and ceiling (`tls.c:197-198`), eliminating downgrade/legacy-cipher exposure; TLS 1.3 default ciphersuites are all AEAD.
- Self-signed generation uses P-384 with SHA-384 signing (`tls.c:308-357`) — strong parameters.
- Certificate pinning uses a full `X509_cmp` of the presented vs expected cert (`tls.c:263-277`), not a spoofable field, and is enforced on both connect and accept paths.
- ACME account/cert private keys are written via `open(..., 0600)` (`acme.c:375`, `acme.c:795`) and certs are written atomically via temp+rename (`acme.c:822-844`).
- SNI is set on outbound connections (`tls.c:597`).
- The CSR DER encoding uses the correct length-then-allocate pattern (`acme.c:714-717`) — the safe counterpart to the TLS-01 bug.
