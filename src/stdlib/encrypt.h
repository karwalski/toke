#ifndef TK_STDLIB_ENCRYPT_H
#define TK_STDLIB_ENCRYPT_H

/*
 * encrypt.h — C interface for the std.encrypt standard library module.
 *
 * Type mappings:
 *   [Byte]  = ByteArray  (defined in str.h)
 *   Str     = const char *  (null-terminated UTF-8)
 *   bool    = int (0 = false, 1 = true)
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 14.1.2
 */

#include "str.h"

/* Result type for operations that can fail (decrypt, bad input). */
typedef struct {
    uint8_t      *ok;
    uint64_t      ok_len;
    int           is_err;
    const char   *err_msg;
} EncryptResult;

/* Key-pair types for asymmetric algorithms. */
typedef struct { uint8_t pubkey[32]; uint8_t privkey[32]; } X25519Keypair;
typedef struct { uint8_t pubkey[32]; uint8_t privkey[64]; } Ed25519Keypair;

/* -----------------------------------------------------------------------
 * AES-256-GCM
 * ----------------------------------------------------------------------- */

/* encrypt.aes256gcm_encrypt(key:[Byte]; nonce:[Byte]; plaintext:[Byte]; aad:[Byte]):[Byte]
 * key must be 32 bytes, nonce must be 12 bytes.
 * Returns ciphertext || 16-byte auth tag on success; is_err=1 on bad input.
 * Caller owns ok. */
EncryptResult encrypt_aes256gcm_encrypt(ByteArray key, ByteArray nonce,
                                         ByteArray plaintext, ByteArray aad);

/* encrypt.aes256gcm_decrypt(key:[Byte]; nonce:[Byte]; ciphertext:[Byte]; aad:[Byte]):DecryptResult
 * ciphertext must be at least 16 bytes (tag). Verifies tag first (constant-time).
 * Returns is_err=1 on auth failure or bad input.
 * Caller owns ok. */
EncryptResult encrypt_aes256gcm_decrypt(ByteArray key, ByteArray nonce,
                                         ByteArray ciphertext, ByteArray aad);

/* encrypt.aes256gcm_keygen():[Byte]
 * Returns 32 cryptographically random bytes. Caller owns .data. */
ByteArray encrypt_aes256gcm_keygen(void);

/* encrypt.aes256gcm_noncegen():[Byte]
 * Returns 12 cryptographically random bytes. Caller owns .data. */
ByteArray encrypt_aes256gcm_noncegen(void);

/* -----------------------------------------------------------------------
 * X25519 Diffie-Hellman (RFC 7748)
 * ----------------------------------------------------------------------- */

/* encrypt.x25519_keypair():Keypair
 * Generates a fresh X25519 keypair; privkey and pubkey are each 32 bytes. */
X25519Keypair encrypt_x25519_keypair(void);

/* encrypt.x25519_dh(privkey:[Byte]; peerpub:[Byte]):[Byte]
 * Performs scalar multiplication. Returns 32-byte shared secret.
 * Caller owns .data. */
ByteArray encrypt_x25519_dh(ByteArray privkey, ByteArray peerpub);

/* -----------------------------------------------------------------------
 * Ed25519 (RFC 8032)
 * ----------------------------------------------------------------------- */

/* encrypt.ed25519_keypair():Keypair
 * Generates a fresh Ed25519 keypair; pubkey=32 bytes, privkey=64 bytes
 * (seed || pubkey, matching the standard representation).
 */
Ed25519Keypair encrypt_ed25519_keypair(void);

/* encrypt.ed25519_sign(privkey:[Byte]; msg:[Byte]):[Byte]
 * Signs msg with privkey (64-byte extended key). Returns 64-byte signature.
 * Caller owns .data. */
ByteArray encrypt_ed25519_sign(ByteArray privkey, ByteArray msg);

/* encrypt.ed25519_verify(pubkey:[Byte]; msg:[Byte]; sig:[Byte]):bool
 * Returns 1 if signature is valid, 0 otherwise. */
int encrypt_ed25519_verify(ByteArray pubkey, ByteArray msg, ByteArray sig);

/* -----------------------------------------------------------------------
 * HKDF-SHA-256 (RFC 5869)
 * ----------------------------------------------------------------------- */

/* encrypt.hkdf_sha256(ikm:[Byte]; salt:[Byte]; info:[Byte]; outlen:u64):[Byte]
 * Derives outlen bytes (max 255*32). Returns empty ByteArray on error.
 * Caller owns .data. */
ByteArray encrypt_hkdf_sha256(ByteArray ikm, ByteArray salt,
                               ByteArray info, uint64_t outlen);

/* -----------------------------------------------------------------------
 * TLS certificate fingerprint
 * ----------------------------------------------------------------------- */

/* encrypt.tls_cert_fingerprint(pem_cert:Str):[Byte]
 * Parses a PEM-encoded certificate, SHA-256 hashes the DER body.
 * Returns 32-byte digest, or empty ByteArray on parse error.
 * Caller owns .data. */
ByteArray encrypt_tls_cert_fingerprint(const char *pem_cert);

#endif /* TK_STDLIB_ENCRYPT_H */
