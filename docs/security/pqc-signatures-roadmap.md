# Post-quantum signatures: migration roadmap

**Story:** 122.5 (Epic 122, PQC / crypto-agility track) · **Decision record:** ADR-0013 (d), (e)
**Checked against the source:** 2026-11-10, toke `main` at `ccc09753` plus the 122.5 branch
**Companion documents:** [`pqc-hndl-triage.md`](pqc-hndl-triage.md) (key establishment),
[`threat-model.md` Part III](threat-model.md) (posture, guidance, deprecation track)

ADR-0013 (d) commits toke to migrating its signature schemes from Ed25519 and
ECDSA to ML-DSA (FIPS 204) or SLH-DSA (FIPS 205). Signatures are not a
harvest-now-decrypt-later exposure: a signature can only be forged once a
cryptographically relevant quantum computer (CRQC) exists. What matters is
**how long a signature must keep verifying** and **who else must verify it**.
Short-lived signatures can wait; signatures that must verify for years, or
whose verifiers are slow to upgrade, need a plan now.

Library: ADR-0013 (e) — post-quantum primitives come from a vetted library
(OpenSSL 3.5+ providers, or liboqs), behind the OpenSSL build; the bundled
libc-only core (`src/stdlib/crypto.h:11`) never grows a hand-rolled ML-DSA. The
linked Homebrew OpenSSL 4.0.3 provides ML-DSA-44/65/87 and the SLH-DSA family
in its default provider (`openssl list -signature-algorithms`).

## Inventory

| # | Site | Scheme today | Lifetime of a signature | Verifiers | Toke controls both ends? |
|---|---|---|---|---|---|
| S1 | `encrypt.ed25519keypair` / `ed25519sign` / `ed25519verify` (`stdlib/encrypt.tki:28-30`; `src/stdlib/encrypt.c:1553`, `:1582`, `:1660`) | Ed25519, hand-rolled libc core | program-defined, can be years | program-defined | often |
| S2 | `tls.genselfsigned` / `tls.genselfsignedalg` (`src/stdlib/tls.c:402`, `:573`; `stdlib/tls.tki:31-32`) | ECDSA P-384 by default; **ML-DSA-65 opt-in shipped** (`tls.c:432`) | certificate validity (`valid_days`) | TLS peers (pinned self-signed certs) | usually: pinned pairs are toke on both sides |
| S3 | ACME account key and certificate key (`src/stdlib/acme.c:137-150`, generated at `:456` and `:803`) | ECDSA P-256 (ES256 JWS, P-256 CSR) | account: indefinite; certificate: CA-set (~90 days) | the ACME CA; every browser and client | **no**: the CA and the WebPKI decide |
| S4 | Release signing (`.github/workflows/release.yml:149-170`, `docs/security/release-signing.md`) | sigstore cosign keyless (Fulcio-issued certificate, Rekor log) | indefinite: old releases stay verifiable | anyone installing `tkc` | **no**: sigstore decides |
| S5 | `std.auth` JWT (`src/stdlib/auth.c:89`) | HS256 only (allowlisted, `alg:none` rejected) | token lifetime | the issuing service | yes |

S5 is symmetric and therefore already adequate; it is listed because ADR-0013 (b)
plans an algorithm registry that a post-quantum JOSE algorithm would join. No
action is planned for it before S1–S2.

## Phases

Phases have triggers, not dates. A phase never removes a classical scheme a
peer still needs; deprecations follow `docs/governance.md` §4.3 and the
deprecation track in threat-model Part III.

### Phase 0 — shipped

- ML-DSA-65 self-signed certificates: `tls.genselfsignedalg(cn; days; "ml-dsa-65")`
  (124.1, wired to toke by 136.44a). `test/stdlib/test_tls.c`
  (`test_gen_self_signed_mldsa`) checks the certificate's signature algorithm
  is ML-DSA-65 and that it verifies under its own key.
- Hybrid key exchange in `std.tls`, so the channel that carries signatures is
  itself not HNDL-exposed (see the triage, H1).

### Phase 1 — a post-quantum signature API in the stdlib (S1)

**Trigger:** the owner decision in appendix A. **Work:** ML-DSA-65 keypair, sign
and verify functions; spec amendment, `.tki`, docs, glue and conformance tests
in one change (AGENTS.md §10). **Exit:** deprecation stage 1 for
`encrypt.ed25519sign` *when used for long-lived signatures* (threat-model
Part III) — Ed25519 itself stays, as a classical component and for short-lived
use.

### Phase 2 — signing-algorithm agility where toke controls both ends (S2)

- Re-evaluate `tls.genselfsigned`'s default once phase 1 has shipped for one
  deprecation cycle: pinned self-signed pairs are toke on both sides, so an
  ML-DSA-65 default breaks only peers running an OpenSSL older than 3.5.
- Keep `"ecdsa-p384"` selectable indefinitely for interop.
- Composite (ECDSA + ML-DSA in one certificate) is not planned: the linked
  OpenSSL 4.0.3 lists no composite signature algorithm in its default provider,
  and the IETF drafts are not final. Revisit if that changes.

### Phase 3 — algorithm agility where an external party decides (S3, S4)

These cannot move before the other party does; the work is to be **ready** so
that toke is not the blocker.

- **S3 ACME.** Make `acme_gen_key` take an algorithm (today it hard-codes
  P-256), and the JWS `alg` follow the key, so a CA that accepts ML-DSA account
  keys or issues ML-DSA certificates can be used without a code change. Until
  public CAs issue post-quantum certificates, ACME-issued certificates stay
  classical; this is not an HNDL exposure because the TLS key exchange, not the
  certificate key, protects the session (triage, "Not HNDL exposures").
- **S4 release signing.** Follows sigstore: when Fulcio and cosign support a
  post-quantum or hybrid signing algorithm, switch the workflow and update
  `docs/security/release-signing.md`. An interim second signature (an ML-DSA-65
  signature over `checksums.txt` with a project-held key) is possible with
  OpenSSL today, but it introduces a long-lived private key that keyless signing
  was chosen to avoid; that trade-off is an owner decision and is not proposed
  here.

### Phase 4 — default change and optional removal

Per threat-model Part III stages 2 and 3: new code defaults to the
post-quantum APIs; classical-only signature functions may be removed in a major
release, no sooner than governance §4.3 allows after phase 1.

## Appendix A — `std.encrypt` PQC API proposal (owner decision)

122.5 part 3 asked for an ML-KEM / ML-DSA API "per ADR-0013 (e)". ADR-0013 (e)
settles **the library** (OpenSSL 3.5+ providers or liboqs, behind the OpenSSL
build; never hand-rolled) but **not the API**: it names no functions, types,
parameter sets, key encodings, module or hybrid construction. Adding stdlib
surface is a spec amendment, and these are product decisions, so the API is
proposed here and the code is not written (rule: never invent stdlib surface).

Feasibility was checked on the linked OpenSSL 4.0.3: ML-KEM-768 keygen,
encapsulate and decapsulate through `EVP_PKEY_encapsulate` /
`EVP_PKEY_decapsulate` agree on a 32-byte shared secret; raw key export gives a
1184-byte public key and a 2400-byte private key, with a 1088-byte ciphertext.
ML-DSA-65 gives a 1952-byte public key, a 4032-byte private key and a
3309-byte signature (FIPS 203 / FIPS 204 sizes).

### Decisions needed

| # | Question | Options | Recommendation |
|---|---|---|---|
| A1 | Which module | (a) `std.encrypt`; (b) a new `std.pqc` module | **(b) `std.pqc`.** The link table gives each module one flag set (`src/stdlib_deps.c:114`: `encrypt` links no library; `:168`: `tls` links `-lssl -lcrypto`). Putting OpenSSL-backed functions in `std.encrypt` makes every `std.encrypt` program link libcrypto, which ADR-0013 (e) set out to avoid ("the zero-dependency default build stays classically secure"). A separate module keeps that invariant with no link-table change |
| A2 | Parameter sets | (a) fixed: ML-KEM-768 and ML-DSA-65 only; (b) a parameter string per call | **(a) fixed.** Matches the TLS choices already shipped (X25519MLKEM768, ML-DSA-65 certificates), avoids an algorithm-name string argument on every call, and leaves room for new functions later (`mlkem1024…`) without changing these |
| A3 | Key encoding | (a) raw FIPS 203/204 byte strings; (b) DER (SPKI / PKCS#8) | **(a) raw**, as the existing `x25519`/`ed25519` functions already use raw `[byte]` keys. Private keys as the expanded form OpenSSL exports (2400 / 4032 bytes) |
| A4 | Hybrid KEM | (a) ML-KEM-768 only; (b) also a hybrid X25519 + ML-KEM-768 KEM with a defined combiner (X-Wing draft, or HKDF-SHA-256 over both secrets bound to both ciphertexts and public keys) | **(a) now, (b) as a follow-up story.** A hybrid outside TLS needs a combiner, and the TLS concatenation is only safe inside the TLS key schedule. Programs that want a hybrid today can feed both secrets to `hkdfsha256` themselves; a library combiner should wait for a final standard |
| A5 | Behaviour without OpenSSL 3.5+ | (a) link error; (b) every call returns `$err` | **(b) `$err` with a message naming the missing algorithm**, matching `tls.genselfsignedalg` today |

### Proposed surface (assuming A1(b), A2(a), A3(a), A4(a), A5(b))

Error unions per owner decision 127.30 (`T!str`, the shape `tls.genselfsigned`
already uses). No-underscore names (113.2a). No diagnostic `fix` fields: these
are library functions; nothing new is emitted by the compiler.

Two record types, written as `.tki` entries would declare them:
`keypair` (`pubkey:[byte]`, `privkey:[byte]`) and `encapsulation`
(`ciphertext:[byte]`, `sharedsecret:[byte]`).

| Function | Params | Return | Algorithm |
|---|---|---|---|
| `pqc.mlkemkeypair` | — | `keypair!str` | ML-KEM-768 (FIPS 203) |
| `pqc.mlkemencap` | `pubkey:[byte]` | `encapsulation!str` | ML-KEM-768 |
| `pqc.mlkemdecap` | `privkey:[byte]`, `ciphertext:[byte]` | `[byte]!str` | ML-KEM-768 |
| `pqc.mldsakeypair` | — | `keypair!str` | ML-DSA-65 (FIPS 204) |
| `pqc.mldsasign` | `privkey:[byte]`, `msg:[byte]` | `[byte]!str` | ML-DSA-65 |
| `pqc.mldsaverify` | `pubkey:[byte]`, `msg:[byte]`, `sig:[byte]` | `bool!str` | ML-DSA-65 |

- `$err` cases: algorithm unavailable in the linked library; a key, ciphertext
  or signature of the wrong length; a key the library rejects. A signature that
  does not verify is `$ok:false`, not an error, so callers cannot confuse "bad
  signature" with "could not check".
- `mlkemdecap` never reports a decapsulation failure: ML-KEM uses implicit
  rejection, so a tampered ciphertext yields an unrelated secret. The docs must
  say so (authenticate the ciphertext, or confirm the key, at the protocol
  level).
- ML-DSA uses the pure (non-pre-hash) mode with an empty context string, as the
  ML-DSA-65 certificates already do.

### What landing it touches

`docs/spec/stdlib-signatures.md`, `stdlib/pqc.tki`, `docs/stdlib/pqc.md`,
`src/stdlib/pqc.c` + `pqc_glue.c`, a `src/stdlib_deps.c` row
(`-lcrypto`), C-series conformance tests (round trip, wrong-length key,
tampered signature, tampered ciphertext, unavailable-algorithm path), the
canonical stdlib module count that `verify_project_facts.py` derives, and the
threat-model / `encrypt.md` posture rows that today say "no ML-KEM or ML-DSA
API yet".
