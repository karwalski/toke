---
title: std.encrypt
slug: encrypt
section: reference/stdlib
order: 13
---

**Status: Implemented** -- C runtime backing.

The `std.encrypt` module provides authenticated encryption (AES-256-GCM), Diffie-Hellman key agreement (X25519), digital signatures (Ed25519), key derivation (HKDF-SHA-256), and TLS certificate fingerprinting. All primitives are implemented in C with no external dependencies beyond libc.

## Types

### $decryptresult

Returned by `encrypt.aes256gcm_decrypt` to report success or authentication failure. This is a struct type, not a sum type — check the `err` field to detect failure.

| Field | Type | Meaning |
|-------|------|---------|
| ok | @(byte) | Decrypted plaintext bytes on success; empty on failure |
| err | $str | Empty string on success; error message when authentication or decryption fails |

### $keypair

Holds the public and private key bytes for asymmetric key algorithms (X25519 and Ed25519).

| Field | Type | Meaning |
|-------|------|---------|
| pubkey | @(byte) | Public key bytes (32 bytes for both X25519 and Ed25519) |
| privkey | @(byte) | Private key bytes (32 bytes for X25519; 64 bytes for Ed25519) |

## Functions

### encrypt.aes256gcm_keygen(): @(byte)

Generates a fresh 32-byte (256-bit) AES key using a cryptographically secure random source. Always use this function rather than deriving keys by hand.

```toke
m=example;
i=enc:std.encrypt;

f=genkey():@(u8){
  let key=enc.aes256gcmkeygen();
< key
};
```

### encrypt.aes256gcm_noncegen(): @(byte)

Generates a fresh 12-byte (96-bit) nonce using a cryptographically secure random source. A nonce must never be reused with the same key.

```toke
m=example;
i=enc:std.encrypt;

f=gennonce():@(u8){
  let nonce=enc.aes256gcmnoncegen();
< nonce
};
```

### encrypt.aes256gcm_encrypt(key: @(byte); nonce: @(byte); plaintext: @(byte); aad: @(byte)): @(byte)

Encrypts `plaintext` using AES-256-GCM with the given `key` (32 bytes) and `nonce` (12 bytes), appending a 16-byte authentication tag to the returned ciphertext. The `aad` (additional authenticated data) parameter is included in the tag computation but not encrypted.

```toke
m=example;
i=enc:std.encrypt;
i=str:std.str;

f=encryptdemo():@(u8){
  let key=enc.aes256gcmkeygen();
  let nonce=enc.aes256gcmnoncegen();
  let ct=enc.aes256gcmencrypt(key;nonce;str.tobytes("hello, world");@());
< ct
};
```

### encrypt.aes256gcm_decrypt(key: @(byte); nonce: @(byte); ciphertext: @(byte); aad: @(byte)): $decryptresult

Decrypts `ciphertext` using AES-256-GCM, verifying the embedded authentication tag in constant time before returning the plaintext. Returns a `$decryptresult` struct — check the `err` field (empty string means success) before using the plaintext in `ok`.

```toke
m=example;
i=enc:std.encrypt;
i=str:std.str;

f=decryptdemo(key:@(u8);nonce:@(u8);ct:@(u8)):$str{
  let result=enc.aes256gcmdecrypt(key;nonce;ct;@());
  if(result.err=""){
    str.frombytes(result.ok)
  }el{
    ""
  }
};
```

### encrypt.x25519_keypair(): $keypair

Generates a fresh X25519 key pair. Both `pubkey` and `privkey` are 32 bytes, and the public key is safe to transmit to the other party.

```toke
m=example;
i=enc:std.encrypt;
t=$keypair{pubkey:@(u8);privkey:@(u8)};

f=genx25519():$keypair{
  let kp=enc.x25519keypair();
  < kp
};
```

### encrypt.x25519_dh(privkey: @(byte); peerpub: @(byte)): @(byte)

Performs X25519 Diffie-Hellman scalar multiplication of `privkey` against `peerpub`, returning a 32-byte shared secret. Pass the result through `encrypt.hkdf_sha256` before use as an encryption key.

```toke
m=example;
i=enc:std.encrypt;
i=str:std.str;

f=dhdemo(myprivkey:@(u8);peerpubkey:@(u8)):@(u8){
  let shared=enc.x25519dh(myprivkey;peerpubkey);
  let enckey=enc.hkdfsha256(shared;@();str.tobytes("enc");32);
  < enckey
};
```

### encrypt.ed25519_keypair(): $keypair

Generates a fresh Ed25519 key pair. `pubkey` is 32 bytes and `privkey` is 64 bytes (seed concatenated with public key).

```toke
m=example;
i=enc:std.encrypt;
t=$keypair{pubkey:@(u8);privkey:@(u8)};

f=gened25519():$keypair{
  let kp=enc.ed25519keypair();
  < kp
};
```

### encrypt.ed25519_sign(privkey: @(byte); msg: @(byte)): @(byte)

Signs `msg` with the 64-byte Ed25519 `privkey` and returns a 64-byte deterministic signature.

```toke
m=example;
i=enc:std.encrypt;
i=str:std.str;

f=signdemo(privkey:@(u8)):@(u8){
  let sig=enc.ed25519sign(privkey;str.tobytes("authenticate this"));
< sig
};
```

### encrypt.ed25519_verify(pubkey: @(byte); msg: @(byte); sig: @(byte)): bool

Verifies that `sig` is a valid Ed25519 signature over `msg` made by the holder of the private key corresponding to `pubkey`.

```toke
m=example;
i=enc:std.encrypt;
i=str:std.str;

f=verifydemo(pubkey:@(u8);sig:@(u8)):bool{
  let ok=enc.ed25519verify(pubkey;str.tobytes("authenticate this");sig);
  < ok
};
```

### encrypt.hkdf_sha256(ikm: @(byte); salt: @(byte); info: @(byte); outlen: u64): @(byte)

Derives `outlen` bytes of keying material from the input key material `ikm` using HKDF-SHA-256 (RFC 5869) with the given `salt` and context `info`. Maximum output length is 8160 bytes. Returns an empty byte array on invalid input.

```toke
m=example;
i=enc:std.encrypt;
i=str:std.str;

f=hkdfdemo(sharedsecret:@(u8);salt:@(u8)):@(u8){
  let enckey=enc.hkdfsha256(sharedsecret;salt;str.tobytes("enc");32);
  < enckey
};
```

### encrypt.tls_cert_fingerprint(pem_cert: $str): @(byte)

Parses a PEM-encoded TLS certificate, SHA-256 hashes the DER-encoded body, and returns the 32-byte fingerprint. Returns an empty byte array if the PEM cannot be parsed.

```toke
m=example;
i=enc:std.encrypt;
i=file:std.file;
i=crypto:std.crypto;

f=certfp():$str{
  let pem=file.read("/etc/ssl/certs/server.pem")|{
    $ok:s  s;
    $err:e ""
  };
  let fp=enc.tlscertfingerprint(pem);
  let hex=crypto.tohex(fp);
  < hex
};
```

## Usage Examples

### X25519 key exchange, HKDF key derivation, AES-256-GCM encrypt and decrypt

Two parties perform a Diffie-Hellman key exchange, derive a symmetric key with HKDF, then use AES-256-GCM for authenticated encryption:

```toke
m=encryptdemo;
i=enc:std.encrypt;
i=crypto:std.crypto;
i=str:std.str;

f=main():i64{
let alice=enc.x25519keypair();
  let bob=enc.x25519keypair();
let aliceshared=enc.x25519dh(alice.privkey;bob.pubkey);
  let bobshared=enc.x25519dh(bob.privkey;alice.pubkey);

let salt=crypto.randombytes(32);
  let enckey=enc.hkdfsha256(aliceshared;salt;str.tobytes("enc-v1");32);
let nonce=enc.aes256gcmnoncegen();
  let ct=enc.aes256gcmencrypt(enckey;nonce;str.tobytes("hello from alice");@());
let result=enc.aes256gcmdecrypt(enckey;nonce;ct;@());
  if(result.err=""){
    let plain=str.frombytes(result.ok);
< 0
  }el{
    < 1
  }
};
```

### Ed25519 digital signatures

A signing key pair is generated once; the private key is stored securely and the public key is distributed for verification:

```toke
m=sigdemo;
i=enc:std.encrypt;
i=str:std.str;

f=main():i64{
  let kp=enc.ed25519keypair();
  let msg=str.tobytes("invoice #1042: $500.00");
let sig=enc.ed25519sign(kp.privkey;msg);
let ok=enc.ed25519verify(kp.pubkey;msg;sig);

let tampered=str.tobytes("invoice #1042: $5000.00");
  let bad=enc.ed25519verify(kp.pubkey;tampered;sig);
if(ok){
    if(!(bad)){
      < 0
    }el{
      < 1
    }
  }el{
    < 1
  }
};
```

## See Also

- [std.crypto](/docs/stdlib/crypto) -- SHA-256, SHA-512, HMAC, random bytes, and hex encoding
