## 120.9 — Crypto & secrets

**Scope:** `src/stdlib/crypto.c`, `encrypt.c`, `auth.c`, `keychain.c`, `securemem.c`/`secure_mem.c` of the toke stdlib (all pure-C primitives). **Method:** manual static review of every in-scope file, focused on primitive/mode choice, IV/nonce handling, KDF parameters, constant-time comparison of MAC/token/password checks, RNG source, key lifecycle/zeroization, and session/auth-token entropy. No build or dynamic testing was run (parallel audits share the tree). Every finding cites code that was actually read.

The core primitives are, on the whole, competently implemented: SHA-256/512 and HMAC follow FIPS 180-4 / RFC 2104; AES-256-GCM and ChaCha20-Poly1305 verify tags in constant time; bcrypt, API-key, and JWT-signature comparisons are constant-time; `securemem` zeroizes and `mlock`s buffers. The findings below are the deviations worth acting on.

---

### CRY-01 — RNG fallback fails *open* (silent zero / uninitialised keys) on non-Apple platforms
- **Severity:** Medium
- **Reachability:** local (failure-conditioned)
- **Location:** `src/stdlib/encrypt.c:31-46` (`random_bytes`); related `src/stdlib/crypto.c:25-32` (`arc4random_buf` fallback)

**Description.** `encrypt.c`'s `random_bytes()` only uses `arc4random_buf` on `__APPLE__ / __FreeBSD__ / __OpenBSD__`. On **every other platform (including all Linux deployment targets — Ubuntu/Amazon Linux Lightsail/EC2), regardless of glibc version**, it always reads `/dev/urandom`, and on any failure to open or fully read it, executes the "last-resort" branch:

```c
/* last-resort: zero fill (insecure, should not happen) */
memset(buf, 0, n);
```

`random_bytes()` is the entropy source for `encrypt_aes256gcm_keygen`, `_noncegen`, `chacha20poly1305_keygen`/`_noncegen`, `x25519_keypair`, `ed25519_keypair` (seed), RSA prime generation (`bi_random_prime`, `bi_miller_rabin`), OAEP seed (`oaep_encode`), and PSS salt (`pss_encode`). Separately, `crypto.c`'s old-glibc `arc4random_buf` shim (`crypto.c:28-31`) ignores the `fread` return value entirely, leaving the caller's buffer (e.g. the bcrypt salt `crypto.c:1221`, `crypto_randombytes` output) partially or wholly uninitialised on short/failed reads.

**Impact.** If `/dev/urandom` is unavailable — restricted container / chroot / seccomp / early boot / fd exhaustion — key, nonce, private-key, and RSA-prime generation silently proceed with an **all-zero (or uninitialised heap) buffer** instead of aborting. An all-zero AES/ChaCha key or X25519/Ed25519 private key is catastrophic and completely undetectable at the call site. This is a fail-open design; the correct behaviour is to fail closed (return an error / abort), exactly as `auth.c`'s API-key and TOTP paths already do (`auth.c:458-463`, `auth.c:854-859` check `n != sizeof` and return NULL/error).

**Fix.** Make `random_bytes` return a status and have every caller propagate failure (never zero-fill). Prefer `getrandom(2)` (blocking, no fd) on Linux and use `arc4random_buf` on all glibc ≥2.36; on the `/dev/urandom` path treat a short read or open failure as a hard error. Apply the same `fread`-return check to the `crypto.c` shim.

---

### AUTH-01 — `auth_jwtverify` does not enforce `exp`; expired tokens verify as valid
- **Severity:** Medium
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/auth.c:311-407` (esp. 392-405)

**Description.** `auth_jwtverify` correctly checks the HMAC-SHA256 signature (constant-time), then parses `sub`/`iss`/`iat`/`exp` and returns the claims with `is_err = 0`. It **reads `exp` (`has_exp`, `exp_val`) but never compares it against the current time** — the token is reported valid regardless of expiry. Expiry is only checked by a *separate* function, `auth_jwtexpired` (`auth.c:413-444`), which the caller must remember to invoke.

**Impact.** Any caller that treats "`auth_jwtverify` succeeded" as "token is currently valid" — the natural reading of a `verify` API, and how most JWT libraries behave — will accept expired tokens indefinitely. A user whose access token/session should have expired (revocation-by-expiry, short-lived tokens) remains authenticated forever with a token they already hold. This turns short token lifetimes into a no-op security control.

**Fix.** Enforce `exp` (and, if present, `nbf`) inside `auth_jwtverify`: if `has_exp && exp_val < time(NULL)` return `is_err = 1` with an "expired" message. Optionally add a small leeway parameter. At minimum, document loudly that `jwtverify` does not check expiry and callers MUST call `jwtexpired`.

---

### AUTH-02 — TOTP code comparison is not constant-time
- **Severity:** Low
- **Reachability:** remote-unauth
- **Location:** `src/stdlib/auth.c:954-957`

**Description.** `auth_totp_verify` formats the expected 6-digit code and compares it to the user-supplied token with `strcmp`:

```c
snprintf(expected, sizeof(expected), "%06u", (unsigned int)code);
if (strcmp(expected, token) == 0) matched = 1;
```

`strcmp` is data-dependent (returns at the first differing byte), so this is a non-constant-time comparison of a one-time authentication credential — the same class of check the audit brief flags for MAC/token/password verification. Every other credential comparison in this codebase (`crypto_constanteq`, `ct_memeq`, `ct_memcmp`, bcrypt/API-key checks) is constant-time; this is the outlier.

**Impact.** Real-world exploitability is low: the code space is only 10^6, codes rotate every 30 s, and network jitter dwarfs a 6-byte `strcmp` timing delta. Still, it is a genuine, easily-fixed deviation from the constant-time discipline used elsewhere.

**Fix.** Compare with a constant-time routine over a fixed length (e.g. `crypto_constanteq` / `ct_memeq` on the 6-byte strings, guarding the length first), matching the rest of the module.

---

### ENC-02 — RSA-OAEP decode is not constant-time (Manger-style padding-oracle shape)
- **Severity:** Low
- **Reachability:** remote-auth
- **Location:** `src/stdlib/encrypt.c:2504-2532` (`oaep_decode`)

**Description.** `oaep_decode` branches and returns early on structural padding features: it bails immediately when the leading byte is non-zero (`em[0] != 0x00`, line 2509), and later distinguishes the `lHash` mismatch, the `0x00`-padding scan (`while (pos<db_len && rdb[pos]==0x00) pos++`, line 2527), and the `0x01` separator check (line 2528) with separate early returns whose ordering and work differ per failure mode. This is the classic input pattern Manger's attack exploits against RSA-OAEP — the most-significant-byte test in particular.

**Impact.** If an application surfaces decrypt-failure as a distinguishable error or exhibits measurable timing differences between failure modes, an adversary who can submit chosen ciphertexts to an RSA-OAEP decryption endpoint could mount a Manger-style recovery. OAEP is far more resistant than PKCS#1 v1.5, and exploitability depends entirely on the calling application exposing an oracle, so this is Low and somewhat speculative — but the primitive itself is not written to be constant-time / oracle-free.

**Fix.** Rewrite `oaep_decode` to run in constant time: compute all checks (leading byte, `lHash` equality, padding scan, separator) into a single accumulated `bad` mask without early returns, and return success/failure from that mask at the end, copying out the message unconditionally into a fixed buffer.

---

### HYG-01 — Sensitive key material left un-zeroized on the stack/heap
- **Severity:** Low
- **Reachability:** local
- **Location:** `src/stdlib/crypto.c:474-514` (HMAC `k_block`), `crypto.c:1203-1233` (bcrypt `salt`/`ctx`), `encrypt.c:2118-2181` (PBKDF2 `t[]`, `dk` on error), `encrypt.c` AES `AES256Ctx` round keys and X25519/Ed25519 private-key copies (e.g. `encrypt.c:745-748`)

**Description.** Numerous routines leave derived/secret material in stack buffers and heap allocations without wiping: the HMAC key block (`k_block`), bcrypt's `BlowfishCtx` and salt, PBKDF2's per-block accumulator `t[]` and the `dk` buffer freed on error paths, AES round keys, and copied X25519/Ed25519 private scalars. The codebase already ships a correct `secure_zero`/`explicit_bzero` primitive in `securemem.c` but does not use it here.

**Impact.** Defense-in-depth only. toke compiles to a native binary with full ambient authority and no language sandbox, so an attacker with process-memory access has already won; the concrete residual risk is secrets lingering in freed heap / swap / core dumps. Worth doing for KDF and long-lived-key paths.

**Fix.** `secure_zero` (or `explicit_bzero`) the HMAC key block, bcrypt context/salt, PBKDF2 intermediates, AES key schedule, and private-key copies before they go out of scope or are freed — especially on error paths.

---

### Dynamic-testing follow-up
- **CRY-01:** Under ASAN/MSAN, fault-inject `/dev/urandom` open/read failure (e.g. run in a namespace without `/dev/urandom`, or LD_PRELOAD a failing `fread`) and confirm keys are not silently zeroed; MSAN would also catch the uninitialised-buffer path in the `crypto.c` shim.
- **ENC-02:** Statistically time `oaep_decode` across many chosen-ciphertext failure classes (valid-leading-byte vs not, good vs bad `lHash`, varying `0x00` run length) to quantify the timing signal before deciding whether the constant-time rewrite is mandatory or defense-in-depth.
- **AUTH-02:** Micro-benchmark the `strcmp` timing delta over the loopback interface to confirm (as expected) it is dominated by network jitter.
- **GCM/ChaCha nonce reuse (see Positives):** Fuzz/property-test that `noncegen` output distribution is uniform and add a runtime guard-test that reusing a (key, nonce) pair is caught by application-level tests, since the library cannot enforce it.
- **RSA `bi_miller_rabin` (encrypt.c:2381):** Not reported as a finding — 5 MR rounds on *randomly generated* candidates (with small-prime trial division pre-filter) is adequate per Damgård–Landrock–Pomerance for these sizes. If the same routine is ever reused to validate externally-supplied primes, re-evaluate (adversarial composites need ~64 rounds).

### Positive observations (already correct)
- **AEAD tag verification is constant-time and before-decrypt:** AES-256-GCM (`encrypt.c:431`, `ct_memcmp`) and ChaCha20-Poly1305 (`encrypt.c:2072-2074`, `crypto_constanteq`) both verify the tag in constant time and reject before releasing plaintext.
- **No JWT `alg`-confusion / `alg:none` bypass:** `auth_jwtverify` always recomputes HMAC-SHA256 and requires the provided signature to match a 32-byte MAC (`auth.c:346-351`); an empty/`none` signature fails the length+content check, and there is no asymmetric verification path to confuse. Signature is checked *before* the payload is parsed.
- **Constant-time credential comparisons:** `crypto_constanteq` (`crypto.c:574-587`), `ct_memeq` (`auth.c:244-251`), bcrypt full-string compare (`crypto.c:1370-1374`), and API-key compare with length-leak mitigation (`auth.c:480-499`) are all constant-time.
- **Sound KDF/primitive choices:** bcrypt default cost 12 (`auth.c:980`), cost clamped 4–31 (`crypto.c:1217-1218`); PBKDF2 follows RFC 2898 with caller-supplied iterations/salt; HKDF-SHA256 handles the empty-salt case correctly (`encrypt.c:1591-1597`); password hashing uses bcrypt, not a bare hash.
- **Strong token/key entropy from a CSPRNG:** API keys are 32 random bytes / 256 bits (`auth.c:452`), TOTP secrets 20 bytes / 160 bits (`auth.c:849`), bcrypt salts 16 bytes (`crypto.c:1221`) — all from `arc4random_buf` on the primary (Apple) path and `/dev/urandom` (fail-closed in `auth.c`) elsewhere.
- **GCM nonce handling:** 96-bit nonce with the standard `J0 = nonce || 0x00000001` construction and correct 32-bit CTR (`encrypt.c:340-342`, `407-409`, `278-303`); `noncegen` produces random 96-bit nonces. (Caveat: the API cannot prevent a caller from reusing a (key, nonce) pair, and random 96-bit nonces have a birthday bound (~2^32 messages/key) — document the uniqueness requirement.)
- **`securemem`/`secure_mem`:** buffers are zeroed on alloc/write/free via a non-elidable `secure_zero` (explicit_bzero / memset_s / volatile-loop, `securemem.c:64-79`), `mlock`ed against swap, TTL-expiring, and mutex-protected.
- **Keychain backend:** uses OS-native stores (macOS Security.framework / Windows Credential Manager), never logs secrets, and fails gracefully.
