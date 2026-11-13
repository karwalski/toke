---
title: Harvest-now-decrypt-later (HNDL) triage
slug: pqc-hndl-triage
section: security
order: 2
---

**Story:** 122.5 (Epic 122, PQC / crypto-agility track) · **Decision record:** ADR-0013
**Checked against the source:** 2026-11-10, toke `main` at `ccc09753` plus the 122.5 branch
**Companion documents:** [`threat-model.md` Part III](/docs/security/threat-model/) (posture and guidance, 122.6),
[`pqc-signatures-roadmap.md`](/docs/security/pqc-signatures-roadmap/) (signatures, 122.5)

This document lists every place in the toke tree where a quantum-vulnerable
**key-establishment** primitive (RSA-OAEP, X25519 Diffie-Hellman, classical
(EC)DHE in TLS) protects data that could be recorded now and decrypted once a
cryptographically relevant quantum computer (CRQC) exists. Signatures are not an
HNDL exposure (a signature can only be forged after a CRQC exists, it cannot be
"harvested"), so they are triaged separately in the signatures roadmap.

Line numbers are for the files as they stand on this branch.

## Summary

| # | Site | Primitive | Protects | Reachable from toke | HNDL exposure | Status |
|---|---|---|---|---|---|---|
| H1 | `std.tls` contexts, `build_ssl_ctx` (`src/stdlib/tls.c:269-275`) | TLS 1.3 KEX | all `std.tls` traffic | yes (`tls.connect`, `tls.listen`) | **mitigated** when both ends have OpenSSL 3.5+: X25519MLKEM768 is preferred | shipped (124.1); fallback now tested (C1180) |
| H2 | `std.http` HTTPS server contexts (`src/stdlib/http.c:4529`, `:4665`, `:4707`) | TLS 1.2/1.3 KEX | `http.servetls` traffic (ooke serves HTTPS through it) | yes | **mitigated** as H1 since 122.15: the explicit hybrid-first list (`tk_tls_set_groups`); TLS 1.2 still accepted | done (122.15, C1200) |
| H3 | `std.http` HTTPS client context (`src/stdlib/http.c:3444`) | TLS 1.2/1.3 KEX | outbound `http.get`/`post` traffic, including API keys in headers | yes | **mitigated**, as H2 | done (122.15, C1200) |
| H4 | `std.llm` HTTPS client context (`src/stdlib/llm.c:407`) | TLS 1.2/1.3 KEX | prompts, completions and provider API keys | yes | **mitigated**, as H2 | done (122.15, C1200) |
| H5 | `encrypt.x25519keypair` / `encrypt.x25519dh` (`src/stdlib/encrypt.h:75`, `:80`; `encrypt.c:842`, `:851`; glue `encrypt_glue.c:99`, `:107`; `stdlib/encrypt.tki:26-27`) | X25519 ECDH, hand-rolled libc core | whatever key a program derives from the shared secret | yes | **exposed by design** when a program uses the shared secret to protect long-lived data | needs the std.encrypt ML-KEM / hybrid API (122.5 part 3, blocked: owner) |
| H6 | `encrypt_rsa_encrypt` / `encrypt_rsa_decrypt` (`src/stdlib/encrypt.h:196`, `:201`; `encrypt.c:2879`, `:2898`) | RSA-2048 OAEP-SHA-256 | anything a C caller wraps with it | **no** — not in `stdlib/encrypt.tki`; no caller anywhere in `src/` outside `encrypt.c` | none today; latent if it is ever exposed | keep C-only; do not expose without an ML-KEM alternative |

**Nothing in the tree wraps a key with RSA-OAEP or X25519 on its own behalf.**
The only callers of `encrypt_x25519_*` are the toke-facing wrappers in
`encrypt_glue.c`; `encrypt_rsa_*` has no caller outside `encrypt.c`. The
at-rest envelope (`encrypt_seal` / `encrypt_open`, `encrypt.h:64-66`) is
symmetric (AES-256-GCM or ChaCha20-Poly1305) and takes its key from the caller,
so it has no asymmetric dependency of its own. The ADR's
"RSA-OAEP / X25519 protect long-lived data or wrap keys" (`encrypt.h:65,179` in
the original row; those lines have since moved) describes **capabilities the
library offers**, not uses the toke tree makes.

**Downstream.** A sweep of the toke-ooke checkout, ooke main, toke-website,
toke-test-programs and toke-mcp (toke-eval excluded, per the hidden-tests rule)
finds no program that imports `std.encrypt`. ooke's HTTPS listener goes through
`http.servetls` (H2), so H2 is the exposure that carries real traffic today.

## H1 — `std.tls`: hybrid KEX ships, fallback tested

`build_ssl_ctx` sets `TK_TLS_GROUPS_HYBRID`
(`X25519MLKEM768:X25519:secp256r1:x448:secp384r1`, `src/stdlib/tls.h`) on every
`std.tls` context; when OpenSSL rejects that list it retries with
`TK_TLS_GROUPS_CLASSICAL` (the same list without the hybrid). OpenSSL rejects a
groups list as a whole when one name in it is unknown (checked on 4.0.3), which
is what an OpenSSL older than 3.5 does with `X25519MLKEM768`, so the retry is
the fallback.

Evidence, on Homebrew OpenSSL 4.0.3 (the `-L/opt/homebrew/lib` build `tkc` links):

- `test/conform/C1180_tls_hybrid_kex_fallback.sh` (new): the linked library
  accepts the hybrid list; a `std.tls` client and server negotiate
  `X25519MLKEM768`. Compiled with a hybrid name the library does not know, the
  contexts still build, the handshake falls back to `x25519` and a payload
  round-trips. With no usable list at all `tls_connect` fails closed. Removing
  the classical retry from `tls.c` makes the test fail.
- `test/conform/C206_tls_negotiated_group.sh` (122.13): a classical-only peer is
  served over `x25519`, and `tls.group(conn)` reports which group each
  connection got.

Residual exposure: a peer, or a deployment, whose OpenSSL is older than 3.5.
Linux distributions in long-term support often ship the 3.0 series, where the
hybrid is unavailable and every `std.tls` connection is classical. A program
that must not run over a classical exchange checks `tls.group(conn)` and closes
the connection.

## H2–H4 — HTTPS in `std.http` and `std.llm`: no group list set

`std.http` (server `http.c:4529`, `:4665` (SNI contexts), `:4707`; client
`http.c:3444`) and `std.llm` (`llm.c:407`) create their own `SSL_CTX`s and never
call `SSL_CTX_set1_groups_list`. On OpenSSL 3.5 and later the library's own
default already prefers `X25519MLKEM768`; verified on 4.0.3 with
`openssl s_server` / `s_client` on loopback, both with default settings:
`Negotiated TLS1.3 group: X25519MLKEM768`. So on this host these contexts are
hybrid today, but only by inheritance:

- the result depends on the library's default and on any system `openssl.cnf`
  `Groups` setting, not on toke;
- unlike `std.tls`, there is no `tls.group`-style readback, so a server cannot
  tell which of its connections were classical;
- these contexts also allow TLS 1.2 (`http.c:4557`, `llm.c:409`, and
  `http.c:4711` from configuration), and a TLS 1.2 handshake has no
  post-quantum key exchange at all.

Recommended remediation (story **122.15**, filed by this batch): apply the same
`TK_TLS_GROUPS_HYBRID` / `TK_TLS_GROUPS_CLASSICAL` preference in these contexts
(the macros now live in `tls.h` for exactly this reuse) and make the negotiated
group observable on the server side. Not done here: 122.5 names
`build_ssl_ctx` only, and `http.c` is shared with in-flight HTTP work.

**Remediated by 122.15.** The two calls now live in one helper,
`tk_tls_set_groups` (`src/stdlib/tk_tls_groups.h`), which `build_ssl_ctx` and
all five `std.http` / `std.llm` contexts call; C1201 fails if a function in
`src/stdlib` creates an `SSL_CTX` without it. `TK_TLS_TRACE_GROUP=1` makes each
context report the list it received and each completed handshake the group it
negotiated, on stderr (a diagnostic switch, not a stdlib API). C1200 shows on
OpenSSL 4.0.x that the server (plain, SNI and configured contexts) and both
clients negotiate `X25519MLKEM768`, still do so under an `openssl.cnf` that
forces `x25519` on unlisted contexts, fall back to `x25519` when the library
lacks the hybrid, and fail closed when neither list is usable. A configured
`tls.curves` list still overrides the default. **Residual, owner decision:**
TLS 1.2 is still accepted by all of these contexts, so a peer that only speaks
1.2 gets a classical exchange.

## H5 — `encrypt.x25519dh`

A bare X25519 shared secret is recoverable from the two public keys once a CRQC
exists. Any key a program derives from it (for example through
`encrypt.hkdfsha256`) and any data that key protects is HNDL-exposed for as long
as the data must stay secret. Threat-model Part III, guidance item 3, already
tells programs not to use `x25519dh` alone for long-lived secrets. The fix is
an ML-KEM-768 or hybrid X25519 + ML-KEM-768 key encapsulation in the stdlib;
its API shape is not settled by ADR-0013 (e) and is proposed for owner decision
in [`pqc-signatures-roadmap.md`, appendix A](/docs/security/pqc-signatures-roadmap/#appendix-a--stdencrypt-pqc-api-proposal-owner-decision).

## H6 — RSA-OAEP

`encrypt_rsa_*` uses a bespoke binary key format, 2048-bit keys only, and is
C-only. Nothing calls it. It should stay unexposed: exposing an RSA key
transport to toke programs now would add an HNDL-vulnerable primitive with no
post-quantum counterpart beside it.

## Not HNDL exposures (and why)

| Site | Why it is not an HNDL exposure |
|---|---|
| Certificate and account keys: `tls_gen_self_signed_alg` (`tls.c:402`, P-384 default), ACME (`acme.c:137`, P-256, used at `:456` and `:803`) | In TLS 1.3 the certificate key only authenticates; the session key comes from the ephemeral exchange, so recording traffic and later breaking the certificate key does not decrypt it. These are signature-migration items (roadmap S2, S3) |
| `std.auth` JWT (`auth.c:89`, HS256 only) | symmetric HMAC |
| `encrypt.aes256gcm*`, `encrypt_seal`/`encrypt_open`, `encrypt.hkdfsha256` | symmetric / hash at 256-bit strength |
| Release signing (cosign keyless, `.github/workflows/release.yml:149-170`) | signatures (roadmap S4) |

## Priority

1. **H2–H4 (122.15) — done.** The only exposure that carried production
   traffic; it now reuses the H1 code path. Residual: the TLS 1.2 floor
   (owner decision).
2. **H5 (122.5 part 3, after the owner decision).** Gives programs a
   post-quantum way to establish long-lived keys, and is the trigger for
   deprecation stage 1 in threat-model Part III.
3. **H1 residual.** Nothing to change in toke; documentation already tells
   deployments to link OpenSSL 3.5 or later and programs to check `tls.group`.
4. **H6.** Keep C-only; no action.
