# Dependency Update Tracking

**Date:** 2026-04-19
**Story:** 57.5.2

## Process

### Vendored C Libraries

Vendored libraries (cmark, tomlc99) are checked manually. No automated dependency tracking is available for vendored C source.

**Check schedule:** Before each Gate evaluation or release.

| Library | Current | Upstream | Check URL |
|---------|---------|----------|-----------|
| cmark | 0.31.1 | github.com/commonmark/cmark/releases | Check for security advisories |
| tomlc99 | 1.0 | Obsoleted — migrate to tomlc17 when convenient | github.com/cktan/tomlc17/releases |

### System Libraries

System libraries (OpenSSL, SQLite, zlib) are managed by the OS package manager (Homebrew on macOS, apt on Ubuntu).

**OpenSSL monitoring:** Subscribe to openssl-announce mailing list. Critical CVEs should trigger an immediate update.

```
# Check current versions
openssl version
sqlite3 --version
```

### npm Dependencies (toke-mcp)

Dependabot is configured in the GitHub repository.

```yaml
# .github/dependabot.yml
version: 2
updates:
  - package-ecosystem: "npm"
    directory: "/"
    schedule:
      interval: "weekly"
    open-pull-requests-limit: 5
```

### Python Dependencies (toke-model)

Dependabot is configured for pip.

```yaml
  - package-ecosystem: "pip"
    directory: "/"
    schedule:
      interval: "weekly"
    open-pull-requests-limit: 5
```

## CVE Response

1. Monitor NVD for CVEs affecting vendored/linked libraries
2. Critical (CVSS >= 9.0): Patch within 48 hours
3. High (CVSS >= 7.0): Patch within 1 week
4. Medium/Low: Patch in next release cycle

## Current Status

- OpenSSL 3.6.2: No known CVEs (as of 2026-04-19)
- SQLite 3.51.0: No known CVEs
- cmark 0.31.1: No known CVEs
- tomlc99: Upstream obsoleted; no security patches expected
