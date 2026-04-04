# std.encrypt — Symmetric and Asymmetric Encryption

## Overview

The `std.encrypt` module provides authenticated encryption (AES-256-GCM),
Diffie-Hellman key exchange (X25519), digital signatures (Ed25519), HKDF key
derivation, and TLS certificate fingerprinting. Together these primitives cover
data-at-rest encryption and end-to-end channel security.

The module depends on `std.crypto` for cryptographically secure random byte
generation (`crypto.randombytes`). The underlying algorithms are provided by
libsodium on all supported platforms; no other third-party cryptographic
library is required.

All key and nonce material is represented as raw `@byte` slices. Callers are
responsible for keeping private keys and symmetric keys out of logs and
serialised state.

## Types

### DecryptResult

```
{ok: @byte; err: $str}
```

Returned by `encrypt.aes256gcm_decrypt`. If decryption and tag verification
succeed, `ok` holds the plaintext and `err` is an empty string. If the
authentication tag fails, `ok` is an empty slice and `err` describes the
failure reason.

### Keypair

```
{pubkey: @byte; privkey: @byte}
```

Returned by `encrypt.x25519_keypair` and `encrypt.ed25519_keypair`. Both
fields are raw byte slices of the appropriate length for the algorithm (32
bytes for X25519; 32-byte seed and 64-byte expanded key for Ed25519).

## Functions

### encrypt.aes256gcm_encrypt(key: @byte; nonce: @byte; plaintext: @byte; aad: @byte): @byte

Encrypts `plaintext` using AES-256-GCM with the given 32-byte `key` and
12-byte `nonce`. `aad` is additional authenticated data — it is authenticated
but not encrypted; pass `@()` if not needed. Returns the ciphertext with the
16-byte authentication tag appended (total length = `plaintext.len + 16`).

The nonce must never be reused with the same key. Use `encrypt.aes256gcm_noncegen`
to produce a fresh random nonce for each message.

**Example:**
```toke
let key=encrypt.aes256gcm_keygen();
let nonce=encrypt.aes256gcm_noncegen();
let plain=str.bytes("hello world");
let ct=encrypt.aes256gcm_encrypt(key;nonce;plain;@());
(* ct.len == plain.len + 16 *)
```

### encrypt.aes256gcm_decrypt(key: @byte; nonce: @byte; ciphertext: @byte; aad: @byte): DecryptResult

Decrypts `ciphertext` (which must include the 16-byte tag at the end) using
AES-256-GCM. If the tag verifies, returns `{ok: <plaintext>; err: ""}`. If
authentication fails — due to a wrong key, corrupted ciphertext, or replayed
nonce — returns `{ok: @(); err: "aead: authentication failed"}`. The
comparison is constant-time.

**Example:**
```toke
let result=encrypt.aes256gcm_decrypt(key;nonce;ct;@());
if result.err==""{
  let plain=result.ok;
  (* use plain *)
}el{
  (* handle result.err *)
};
```

### encrypt.aes256gcm_keygen(): @byte

Generates a fresh 32-byte AES-256 key using the OS entropy pool via
`crypto.randombytes`. The returned key should be stored securely and never
logged.

**Example:**
```toke
let key=encrypt.aes256gcm_keygen();
(test.eq(key.len;32));
```

### encrypt.aes256gcm_noncegen(): @byte

Generates a fresh 12-byte random nonce suitable for AES-256-GCM. Generate a
new nonce for every encryption call; do not reuse nonces under the same key.

**Example:**
```toke
let nonce=encrypt.aes256gcm_noncegen();
(test.eq(nonce.len;12));
```

### encrypt.x25519_keypair(): Keypair

Generates a random X25519 key pair. Both `pubkey` and `privkey` are 32 bytes.
The public key may be freely shared; the private key must be kept secret.

**Example:**
```toke
let alice=encrypt.x25519_keypair();
let bob=encrypt.x25519_keypair();
(* share alice.pubkey with bob, share bob.pubkey with alice *)
```

### encrypt.x25519_dh(privkey: @byte; peerpub: @byte): @byte

Performs an X25519 Diffie-Hellman operation using the local 32-byte `privkey`
and the remote party's 32-byte `peerpub`. Returns a 32-byte shared secret.
Both parties compute the same secret when they swap public keys. The raw output
should be passed through `encrypt.hkdf_sha256` before use as a symmetric key.

**Example:**
```toke
let alice=encrypt.x25519_keypair();
let bob=encrypt.x25519_keypair();
let shared_a=encrypt.x25519_dh(alice.privkey;bob.pubkey);
let shared_b=encrypt.x25519_dh(bob.privkey;alice.pubkey);
(* shared_a == shared_b *)
```

### encrypt.ed25519_keypair(): Keypair

Generates a random Ed25519 signing key pair. `pubkey` is 32 bytes (the
verification key); `privkey` is 64 bytes (the expanded signing key including
the public key). The public key may be distributed freely.

**Example:**
```toke
let kp=encrypt.ed25519_keypair();
(test.eq(kp.pubkey.len;32));
(test.eq(kp.privkey.len;64));
```

### encrypt.ed25519_sign(privkey: @byte; msg: @byte): @byte

Signs `msg` with the 64-byte Ed25519 `privkey`. Returns a 64-byte signature.
The signature is deterministic: signing the same message with the same key
always produces the same bytes.

**Example:**
```toke
let kp=encrypt.ed25519_keypair();
let sig=encrypt.ed25519_sign(kp.privkey;str.bytes("hello"));
(test.eq(sig.len;64));
```

### encrypt.ed25519_verify(pubkey: @byte; msg: @byte; sig: @byte): bool

Verifies an Ed25519 `sig` over `msg` using the 32-byte `pubkey`. Returns
`true` if the signature is valid, `false` otherwise. The check is
constant-time with respect to the signature bytes.

**Example:**
```toke
let kp=encrypt.ed25519_keypair();
let msg=str.bytes("authenticate this");
let sig=encrypt.ed25519_sign(kp.privkey;msg);
let ok=encrypt.ed25519_verify(kp.pubkey;msg;sig);
(* ok == true *)
let bad=encrypt.ed25519_verify(kp.pubkey;str.bytes("tampered");sig);
(* bad == false *)
```

### encrypt.hkdf_sha256(ikm: @byte; salt: @byte; info: @byte; len: u64): @byte

Derives `len` bytes of keying material from input key material `ikm` using
HKDF-SHA-256 (RFC 5869). `salt` is optional — pass `@()` to use the zero
salt. `info` is a context label that binds the output to a specific use; pass
`@()` if not needed. Maximum output length is 8160 bytes (255 × 32).

Use this function to derive symmetric keys from a raw DH shared secret, from a
passphrase, or from any high-entropy source.

**Example:**
```toke
let raw=encrypt.x25519_dh(alice_priv;bob_pub);
let salt=encrypt.aes256gcm_noncegen();
let info=str.bytes("session-key-v1");
let session_key=encrypt.hkdf_sha256(raw;salt;info;32);
```

### encrypt.tls_cert_fingerprint(pem_cert: $str): @byte

Parses a PEM-encoded X.509 certificate and returns its SHA-256 fingerprint as
a 32-byte slice. The fingerprint is computed over the DER-encoded certificate
bytes. Use `crypto.to_hex` to produce the familiar colon-separated hex string
for display or pinning comparisons.

**Example:**
```toke
let pem="-----BEGIN CERTIFICATE-----\n...base64...\n-----END CERTIFICATE-----";
let fp=encrypt.tls_cert_fingerprint(pem);
let hex=crypto.to_hex(fp);
(* hex is a 64-character lowercase hex string, e.g. "3a:f1:..." when formatted *)
```

## Usage Pattern — Sealed Message (X25519 + AES-256-GCM)

A minimal end-to-end encryption pattern: the sender encrypts to the recipient's
public key without needing a pre-shared secret.

```toke
m=example;
i=enc:std.encrypt;
i=cry:std.crypto;

(* Sender side *)
f=seal(recipient_pub:@byte;plaintext:@byte):@byte{
  let eph=enc.x25519_keypair();
  let raw=enc.x25519_dh(eph.privkey;recipient_pub);
  let key=enc.hkdf_sha256(raw;@();str.bytes("seal-v1");32);
  let nonce=enc.aes256gcm_noncegen();
  let ct=enc.aes256gcm_encrypt(key;nonce;plaintext;@());
  (* prepend ephemeral pubkey (32) + nonce (12) + ciphertext *)
  <str.bytes_concat(@(eph.pubkey;nonce;ct))
};

(* Recipient side *)
f=unseal(recipient_priv:@byte;sealed:@byte):@byte{
  let ephem_pub=sealed.slice(0;32);
  let nonce=sealed.slice(32;44);
  let ct=sealed.slice(44;sealed.len);
  let raw=enc.x25519_dh(recipient_priv;ephem_pub);
  let key=enc.hkdf_sha256(raw;@();str.bytes("seal-v1");32);
  let result=enc.aes256gcm_decrypt(key;nonce;ct;@());
  <result.ok
};
```

## Notes

- `encrypt.aes256gcm_keygen` and `encrypt.aes256gcm_noncegen` delegate to
  `crypto.randombytes` from `std.crypto`.
- Never reuse a (key, nonce) pair with AES-256-GCM; doing so destroys
  confidentiality and authenticity.
- Ed25519 private keys returned by `encrypt.ed25519_keypair` are 64-byte
  expanded keys (seed || public), compatible with libsodium's
  `crypto_sign_keypair` layout.
- `encrypt.hkdf_sha256` will panic if `len` exceeds 8160 bytes.
- `encrypt.tls_cert_fingerprint` will panic if `pem_cert` is not a valid
  PEM-encoded X.509 certificate.
