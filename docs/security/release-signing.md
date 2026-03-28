# Release signing and SBOM verification

**Applies to:** tkc releases v1.0.0 and later
**Signing method:** sigstore cosign keyless (OIDC-backed, no private key)
**SBOM format:** SPDX 2.3 JSON

---

## Why we sign releases

The tkc compiler is the root of trust for the entire toke toolchain.  A
compromised compiler binary corrupts every toke program built with it, and by
extension every corpus entry generated from those programs.  Supply-chain
integrity therefore matters at the binary level, not just at the source level.

We use sigstore cosign keyless signing so that:

- There is no private key to rotate, leak, or lose.
- Every signing event is recorded in the Rekor transparency log, giving a
  permanent, auditable trail that neither maintainers nor attackers can erase.
- Verification requires only the public Rekor API and the signer's OIDC
  identity — no out-of-band key distribution.

---

## Signing method

Binaries are signed during the GitHub Actions release workflow
(`.github/workflows/release.yml`) using cosign keyless mode.

**How keyless signing works:**

1. GitHub Actions generates a short-lived OIDC token for the workflow run.
2. cosign exchanges that token with Fulcio (sigstore's certificate authority),
   which issues a short-lived X.509 certificate binding the signing key to the
   workflow's OIDC identity.
3. cosign signs the binary and writes the signature bundle (`tkc.sig`).
4. The signing event (certificate + signature) is appended to the Rekor
   transparency log.

The certificate expires in ten minutes.  Verification does not require the
certificate to be valid at verify-time; it only needs to have been valid at
sign-time, which Rekor proves.

**Signer OIDC identity:**

```
https://github.com/karwalski/tkc/.github/workflows/release.yml
```

**OIDC issuer:**

```
https://token.actions.githubusercontent.com
```

These two values are the verification anchors.  Any signature that does not
match both is invalid, regardless of whether it is otherwise cryptographically
well-formed.

---

## Verifying a downloaded binary

### Prerequisites

Install cosign:

```sh
# macOS
brew install cosign

# Linux (or inside CI)
curl -sSfL https://raw.githubusercontent.com/sigstore/cosign/main/install.sh \
  | sh -s -- -b /usr/local/bin
```

### Verification command

Replace `<tag>` with the release tag you downloaded (e.g. `v1.0.0`) and
`<binary>` with `tkc-linux-x86_64` or `tkc-macos-arm64`.

```sh
cosign verify-blob \
  --bundle tkc.sig \
  --certificate-identity "https://github.com/karwalski/tkc/.github/workflows/release.yml@refs/tags/<tag>" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  <binary>
```

A successful verification prints:

```
Verified OK
```

Any other output, or a non-zero exit code, means the binary has not been
signed by the release workflow or has been modified since signing.  Do not
use it.

### Downloading the release bundle

All required files (`tkc-linux-x86_64`, `tkc-macos-arm64`, `tkc.sig`) are
attached to each GitHub release on the Releases page.  Download them into the
same directory before running the verify command.

---

## Verifying the SBOM

The SBOM (`tkc-sbom.spdx.json`) is published alongside the binary.  Its
SHA-256 checksum is in `tkc-sbom.spdx.json.sha256`.

**Step 1 — Checksum integrity:**

```sh
sha256sum -c tkc-sbom.spdx.json.sha256
```

Expected output:

```
tkc-sbom.spdx.json: OK
```

**Step 2 — Inspect SBOM contents:**

```sh
syft convert tkc-sbom.spdx.json -o table
```

This prints every component identified in the binary.  For tkc you should see
only the compiler's own source files and glibc (or the macOS system libSystem)
as the sole runtime library component.  There are no third-party compiled
libraries.

---

## What the SBOM covers

tkc has **zero external compiled runtime dependencies**.  The only runtime
library it links against is:

| Component | Type | Provided by |
|-----------|------|-------------|
| libc (glibc on Linux, libSystem on macOS) | System library | OS vendor |

The SBOM documents:

- The tkc compiler source files compiled into the binary.
- The system C library (libc) identified from the ELF/Mach-O binary by syft.
- LLVM is **not** a compile-time or link-time dependency.  It is invoked
  as a subprocess at runtime (`clang`), so it appears in the SBOM only as a
  noted runtime tool, not as a linked library.

This means:

- There are no vendored third-party C libraries to patch.
- There are no transitive dependency trees to audit.
- The compiler's attack surface at the binary level is limited to the OS libc
  and the compiler's own source.

---

## Tamper test description

The release workflow includes an automated tamper-detection step.  After
signing the binary, it:

1. Copies the signed binary to `tkc-tampered`.
2. Appends a single byte (`x`) to `tkc-tampered`, changing its SHA-256 hash.
3. Attempts to verify `tkc-tampered` using the same signature bundle.
4. Asserts that cosign returns a **non-zero exit code**.

If cosign incorrectly accepts the tampered binary, the step fails with:

```
ERROR: cosign accepted a tampered binary — aborting release
```

This forces the workflow to stop before publishing any release artifacts,
ensuring that a defective signing configuration cannot reach users.

The tamper test validates end-to-end: it exercises the full verify path using
the same `--certificate-identity` and `--certificate-oidc-issuer` flags that
a user would run.  A pass here means the verify command works as documented.

---

## Signing key / identity summary

| Field | Value |
|-------|-------|
| Signing method | sigstore cosign keyless |
| Certificate authority | Fulcio (sigstore public instance) |
| Transparency log | Rekor (sigstore public instance) |
| OIDC issuer | `https://token.actions.githubusercontent.com` |
| Signer identity | `https://github.com/karwalski/tkc/.github/workflows/release.yml` |
| Private key stored | No — ephemeral per-run key, certificate expires in 10 minutes |
| Key rotation required | No |
