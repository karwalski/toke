# Release signing and SBOM verification

**Applies to:** tkc releases after v3.2.0 (the next tag). For v3.2.0 and earlier, see the
limitation below.
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

> **Limitation in v3.2.0 and earlier (toke 127.453).** Those releases' workflow signed,
> and ran syft on, a `tkc` it rebuilt on Linux in the signing job, not the binaries inside
> the published archives. So their `tkc.sig` and `tkc-sbom.spdx.json` do not verify a
> downloaded archive. For those releases, verify an archive against `checksums.txt`
> fetched over HTTPS from the release page (`shasum -a 256 -c checksums.txt
> --ignore-missing`). From the next release the workflow signs and SBOMs the published
> archives themselves, as described below.

## What is signed

Exactly what you download. The release job takes the four platform archives that the
build jobs of the same workflow run uploaded, and:

1. writes one SPDX SBOM per archive, `<archive>.spdx.json` (syft unpacks the archive; see
   "What the SBOM covers");
2. writes `checksums.txt`: the SHA-256 of each archive and each SBOM;
3. signs each archive and `checksums.txt` keyless, writing `<file>.cosign.bundle`;
4. verifies every bundle with the command below, then appends one byte to each file and
   requires verification to fail (the tamper test);
5. only then, and only for a `v*` tag, publishes the release.

Release assets:

| Asset | What it is |
|---|---|
| `tkc-<tag>-<platform>.tar.gz` | the archive (platforms: `linux-x86_64`, `linux-arm64`, `macos-arm64`, `macos-x86_64`) |
| `tkc-<tag>-<platform>.tar.gz.cosign.bundle` | its signature bundle (signature, certificate, Rekor entry) |
| `tkc-<tag>-<platform>.tar.gz.spdx.json` | its SBOM |
| `checksums.txt`, `checksums.txt.cosign.bundle` | SHA-256 of every archive and SBOM, and its signature bundle |

## Signing method

The archives are signed during the GitHub Actions release workflow
(`.github/workflows/release.yml`, the `release` job) using cosign keyless mode.

**How keyless signing works:**

1. GitHub Actions generates a short-lived OIDC token for the workflow run.
2. cosign exchanges that token with Fulcio (sigstore's certificate authority),
   which issues a short-lived X.509 certificate binding the signing key to the
   workflow's OIDC identity.
3. cosign signs each archive and `checksums.txt`, writing one bundle per file
   (`<file>.cosign.bundle`).
4. The signing event (certificate + signature) is appended to the Rekor
   transparency log.

The certificate expires in ten minutes.  Verification does not require the
certificate to be valid at verify-time; it only needs to have been valid at
sign-time, which Rekor proves.

**Signer OIDC identity:**

```
https://github.com/karwalski/toke/.github/workflows/release.yml@refs/tags/<tag>
```

**OIDC issuer:**

```
https://token.actions.githubusercontent.com
```

These two values are the verification anchors.  Any signature that does not
match both is invalid, regardless of whether it is otherwise cryptographically
well-formed.

---

## Verifying a download

### Prerequisites

Install cosign:

```sh
# macOS
brew install cosign

# Linux (or inside CI)
curl -sSfL https://raw.githubusercontent.com/sigstore/cosign/main/install.sh \
  | sh -s -- -b /usr/local/bin
```

### Verify an archive

Download the archive and its `.cosign.bundle` from the release page into one directory.
Replace `<tag>` with the release tag (e.g. `v3.3.0`) and `<platform>` with yours.

```sh
cosign verify-blob \
  --bundle tkc-<tag>-<platform>.tar.gz.cosign.bundle \
  --certificate-identity "https://github.com/karwalski/toke/.github/workflows/release.yml@refs/tags/<tag>" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  tkc-<tag>-<platform>.tar.gz
```

A successful verification prints:

```
Verified OK
```

Any other output, or a non-zero exit code, means the archive was not signed by the
release workflow at that tag or has been modified since signing. Do not use it.

### Or verify through checksums.txt

`checksums.txt` is signed the same way and lists every archive and SBOM:

```sh
cosign verify-blob \
  --bundle checksums.txt.cosign.bundle \
  --certificate-identity "https://github.com/karwalski/toke/.github/workflows/release.yml@refs/tags/<tag>" \
  --certificate-oidc-issuer "https://token.actions.githubusercontent.com" \
  checksums.txt
sha256sum -c --ignore-missing checksums.txt      # macOS: shasum -a 256 -c --ignore-missing checksums.txt
```

---

## Verifying the SBOM

Each archive has its own SBOM, `tkc-<tag>-<platform>.tar.gz.spdx.json`. Its SHA-256 is in
`checksums.txt`, so verifying `checksums.txt` as above and running `sha256sum -c` covers it.

Inspect its contents:

```sh
syft convert tkc-<tag>-<platform>.tar.gz.spdx.json -o table
```

The SBOM is taken of the archive as published: its root package is the archive, named
with the archive's SHA-256, and syft unpacks it and lists the `tkc` executable it finds
there. syft has no cataloger for C sources, so the stdlib C sources and the vendored
cmark, tomlc99 and miniz sources in `stdlib/vendor/` do not appear in it; their versions
and provenance are in `stdlib/vendor/README.md`.

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

- The vendored C sources (cmark, tomlc99, miniz) ship as sources in the archive and
  are compiled into a user's program only when it imports the module they back; they
  are tracked in `stdlib/vendor/README.md`, not in the SBOM.
- There are no transitive dependency trees to audit.
- The compiler's attack surface at the binary level is limited to the OS libc
  and the compiler's own source.

---

## Tamper test description

The release workflow includes an automated tamper-detection step.  For each
signed file (each archive and `checksums.txt`) it:

1. Verifies the untouched file with its bundle (the positive control).
2. Copies the file and appends a single byte (`x`), changing its SHA-256 hash.
3. Attempts to verify the copy using the same signature bundle.
4. Asserts that cosign returns a **non-zero exit code**.

If cosign incorrectly accepts a tampered file, the step fails with:

```
ERROR: cosign accepted a tampered <file>
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
| Signer identity | `https://github.com/karwalski/toke/.github/workflows/release.yml@refs/tags/<tag>` |
| Signed files | each `tkc-<tag>-<platform>.tar.gz` and `checksums.txt` |
| Private key stored | No — ephemeral per-run key, certificate expires in 10 minutes |
| Key rotation required | No |
