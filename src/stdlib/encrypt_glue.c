/*
 * encrypt_glue.c — i64-ABI wrappers for std.encrypt module.
 *
 * Story 114.17: the std.encrypt `_w` ABI wrappers were generated nowhere —
 * glue_gen.c skips encrypt (its functions take/return [byte], which the
 * auto-generator's is_simple_type rejects) and tk_web_glue.c references the
 * impls but defines no wrappers. As a result any `en.*` call linked with an
 * undefined `_tk_encrypt_*_w` symbol. This file provides hand-written wrappers
 * (modelled on crypto_glue.c) so std.encrypt links and runs standalone.
 *
 * ABI: every value crosses as int64_t. A toke [byte] is packed/unpacked via
 * bytes_rt.h. A toke str is a (const char *) reinterpreted as i64. A toke
 * record (Keypair, DecryptResult) is a heap i64-array, one slot per field in
 * declaration order, returned as a pointer to slot 0 (matches the codegen:
 * inttoptr -> bitcast i64* -> getelementptr field index).
 *
 * Wrapper names use the no-underscore (Profile-1) method spelling the compiler
 * emits — e.g. .tki `encrypt.aes256gcm_keygen` -> `tk_encrypt_aes256gcmkeygen_w`.
 */

#include "encrypt.h"
#include "bytes_rt.h"   /* tk_bytes_pack / tk_bytes_unpack */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Build a toke record from n i64 field slots (heap-allocated, pointer to
 * slot 0). Variadic args are the i64 field values in declaration order. */
static int64_t tk_rec2(int64_t f0, int64_t f1) {
    int64_t *r = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!r) return 0;
    r[0] = f0; r[1] = f1;
    return (int64_t)(intptr_t)r;
}

static int64_t dup_str_i64(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return 0;
    memcpy(out, s, n + 1);
    return (int64_t)(intptr_t)out;
}

/* encrypt.aes256gcm_encrypt(key,nonce,plaintext,aad):[byte]
 * .tki return is [byte]; on error return empty bytes. */
int64_t tk_encrypt_aes256gcmencrypt_w(int64_t key, int64_t nonce,
                                      int64_t pt, int64_t aad) {
    uint8_t *kb, *nb, *pb, *ab;
    uint64_t kn = tk_bytes_unpack(key, &kb), nn = tk_bytes_unpack(nonce, &nb),
             pn = tk_bytes_unpack(pt, &pb), an = tk_bytes_unpack(aad, &ab);
    ByteArray kba = { kb, kn }, nba = { nb, nn }, pba = { pb, pn }, aba = { ab, an };
    EncryptResult res = encrypt_aes256gcm_encrypt(kba, nba, pba, aba);
    int64_t out = res.is_err ? tk_bytes_pack((const uint8_t *)"", 0)
                             : tk_bytes_pack(res.ok, res.ok_len);
    if (res.ok) free(res.ok);
    free(kb); free(nb); free(pb); free(ab);
    return out;
}

/* encrypt.aes256gcm_decrypt(key,nonce,ciphertext,aad):DecryptResult
 * DecryptResult { ok:[byte], err:str }. */
int64_t tk_encrypt_aes256gcmdecrypt_w(int64_t key, int64_t nonce,
                                      int64_t ct, int64_t aad) {
    uint8_t *kb, *nb, *cb, *ab;
    uint64_t kn = tk_bytes_unpack(key, &kb), nn = tk_bytes_unpack(nonce, &nb),
             cn = tk_bytes_unpack(ct, &cb), an = tk_bytes_unpack(aad, &ab);
    ByteArray kba = { kb, kn }, nba = { nb, nn }, cba = { cb, cn }, aba = { ab, an };
    EncryptResult res = encrypt_aes256gcm_decrypt(kba, nba, cba, aba);
    int64_t ok_slot  = res.is_err ? tk_bytes_pack((const uint8_t *)"", 0)
                                  : tk_bytes_pack(res.ok, res.ok_len);
    int64_t err_slot = dup_str_i64(res.is_err ? (res.err_msg ? res.err_msg : "decrypt failed") : "");
    if (res.ok) free(res.ok);
    free(kb); free(nb); free(cb); free(ab);
    return tk_rec2(ok_slot, err_slot);
}

/* encrypt.aes256gcm_keygen():[byte] */
int64_t tk_encrypt_aes256gcmkeygen_w(void) {
    ByteArray k = encrypt_aes256gcm_keygen();
    int64_t out = tk_bytes_pack(k.data, k.len);
    if (k.data) free((void *)k.data);
    return out;
}

/* encrypt.aes256gcm_noncegen():[byte] */
int64_t tk_encrypt_aes256gcmnoncegen_w(void) {
    ByteArray n = encrypt_aes256gcm_noncegen();
    int64_t out = tk_bytes_pack(n.data, n.len);
    if (n.data) free((void *)n.data);
    return out;
}

/* encrypt.x25519_keypair():Keypair { pubkey:[byte], privkey:[byte] } */
int64_t tk_encrypt_x25519keypair_w(void) {
    X25519Keypair kp = encrypt_x25519_keypair();
    int64_t pub  = tk_bytes_pack(kp.pubkey, 32);
    int64_t priv = tk_bytes_pack(kp.privkey, 32);
    return tk_rec2(pub, priv);
}

/* encrypt.x25519_dh(privkey,peerpub):[byte] */
int64_t tk_encrypt_x25519dh_w(int64_t privkey, int64_t peerpub) {
    uint8_t *sb, *pb;
    uint64_t sn = tk_bytes_unpack(privkey, &sb), pn = tk_bytes_unpack(peerpub, &pb);
    ByteArray sba = { sb, sn }, pba = { pb, pn };
    ByteArray shared = encrypt_x25519_dh(sba, pba);
    int64_t out = tk_bytes_pack(shared.data, shared.len);
    if (shared.data) free((void *)shared.data);
    free(sb); free(pb);
    return out;
}

/* encrypt.ed25519_keypair():Keypair { pubkey:[byte](32), privkey:[byte](64) } */
int64_t tk_encrypt_ed25519keypair_w(void) {
    Ed25519Keypair kp = encrypt_ed25519_keypair();
    int64_t pub  = tk_bytes_pack(kp.pubkey, 32);
    int64_t priv = tk_bytes_pack(kp.privkey, 64);
    return tk_rec2(pub, priv);
}

/* encrypt.ed25519_sign(privkey,msg):[byte] */
int64_t tk_encrypt_ed25519sign_w(int64_t privkey, int64_t msg) {
    uint8_t *kb, *mb;
    uint64_t kn = tk_bytes_unpack(privkey, &kb), mn = tk_bytes_unpack(msg, &mb);
    ByteArray kba = { kb, kn }, mba = { mb, mn };
    ByteArray sig = encrypt_ed25519_sign(kba, mba);
    int64_t out = tk_bytes_pack(sig.data, sig.len);
    if (sig.data) free((void *)sig.data);
    free(kb); free(mb);
    return out;
}

/* encrypt.ed25519_verify(pubkey,msg,sig):bool */
int64_t tk_encrypt_ed25519verify_w(int64_t pubkey, int64_t msg, int64_t sig) {
    uint8_t *pb, *mb, *sb;
    uint64_t pn = tk_bytes_unpack(pubkey, &pb), mn = tk_bytes_unpack(msg, &mb),
             sn = tk_bytes_unpack(sig, &sb);
    ByteArray pba = { pb, pn }, mba = { mb, mn }, sba = { sb, sn };
    int64_t r = (int64_t)encrypt_ed25519_verify(pba, mba, sba);
    free(pb); free(mb); free(sb);
    return r;
}

/* encrypt.hkdf_sha256(ikm,salt,info,outlen:u64):[byte] */
int64_t tk_encrypt_hkdfsha256_w(int64_t ikm, int64_t salt, int64_t info, int64_t outlen) {
    uint8_t *ib, *sb, *nb;
    uint64_t in = tk_bytes_unpack(ikm, &ib), sn = tk_bytes_unpack(salt, &sb),
             nn = tk_bytes_unpack(info, &nb);
    ByteArray iba = { ib, in }, sba = { sb, sn }, nba = { nb, nn };
    ByteArray okm = encrypt_hkdf_sha256(iba, sba, nba, (uint64_t)outlen);
    int64_t out = tk_bytes_pack(okm.data, okm.len);
    if (okm.data) free((void *)okm.data);
    free(ib); free(sb); free(nb);
    return out;
}

/* encrypt.tls_cert_fingerprint(pem:str):[byte] */
int64_t tk_encrypt_tlscertfingerprint_w(int64_t pem) {
    const char *p = (const char *)(intptr_t)pem;
    ByteArray fp = encrypt_tls_cert_fingerprint(p ? p : "");
    int64_t out = tk_bytes_pack(fp.data, fp.len);
    if (fp.data) free((void *)fp.data);
    return out;
}
