# std.encrypt

Symmetric encryption (AES-256-GCM), key exchange (X25519), digital signatures (Ed25519), key derivation (HKDF), and TLS certificate fingerprinting.

## Types

```
type DecryptResult { ok: [byte], err: str }
type Keypair       { pubkey: [byte], privkey: [byte] }
```

## Functions

```
f=aes256gcm_encrypt(key:[byte];nonce:[byte];plaintext:[byte];aad:[byte]):[byte]
f=aes256gcm_decrypt(key:[byte];nonce:[byte];ciphertext:[byte];aad:[byte]):DecryptResult
f=aes256gcm_keygen():[byte]
f=aes256gcm_noncegen():[byte]
f=x25519_keypair():Keypair
f=x25519_dh(privkey:[byte];pubkey:[byte]):[byte]
f=ed25519_keypair():Keypair
f=ed25519_sign(privkey:[byte];msg:[byte]):[byte]
f=ed25519_verify(pubkey:[byte];msg:[byte];sig:[byte]):bool
f=hkdf_sha256(ikm:[byte];salt:[byte];info:[byte];len:u64):[byte]
f=tls_cert_fingerprint(hostname:str):[byte]
```

## Semantics

- `aes256gcm_encrypt` returns ciphertext with appended 16-byte auth tag.
- `aes256gcm_decrypt` returns `DecryptResult` with either plaintext in `ok` or error message in `err`.
- `aes256gcm_keygen` returns 32 random bytes; `aes256gcm_noncegen` returns 12 random bytes.
- `x25519_dh` computes a Diffie-Hellman shared secret from a private and public key.
- `ed25519_sign` / `ed25519_verify` produce and verify 64-byte signatures.
- `hkdf_sha256` derives `len` bytes of keying material using HKDF (RFC 5869).
- `tls_cert_fingerprint` connects to `hostname:443` and returns the SHA-256 fingerprint of the leaf certificate.

## Dependencies

- `std.crypto` (hash primitives, random bytes)
