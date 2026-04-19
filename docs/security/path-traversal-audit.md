# Path Traversal and Input Sanitisation Audit

**Date:** 2026-04-19
**Story:** 57.4.3

## Scope

All file path operations in toke stdlib and ooke web server that handle paths derived from HTTP requests or user input.

## Findings and Fixes

### Fixed (this story)

| # | File | Severity | Issue | Fix |
|---|------|----------|-------|-----|
| 1-3,5 | `router.c` | CRITICAL/HIGH | `has_traversal()` check insufficient; no `realpath()` validation; symlinks could escape document root | Added `realpath()` + document root prefix validation in `router_static_serve`. All vhost and static handlers inherit the fix. |
| 4 | `http.c` | MEDIUM | `url_decode()` accepted `%00` null bytes | `%00` now silently dropped (skip, don't emit) |
| 4b | `encoding.c` | MEDIUM | `encoding_urldecode()` accepted `%00` null bytes | Same fix: `%00` silently dropped |

### Not vulnerable (verified safe)

| # | File | Notes |
|---|------|-------|
| 8 | `cache.c` | Disk cache filenames are FNV-1a hex hashes — no traversal possible |
| 9 | `file.c:nftw` | Correctly uses `FTW_PHYS` (no symlink following) |

### Accepted risk (no fix needed now)

| # | File | Severity | Notes |
|---|------|----------|-------|
| 6 | `file.c` | MEDIUM | All file ops accept arbitrary paths by design. Mitigated: ooke constructs paths from config, not user input. A future sandbox mechanism could be added. |
| 7 | `toke-website [section]/[slug].tk` | LOW | URL params in path construction, but mitigated by build-time rendering (routes are pre-rendered at startup, not live). |

## Verification

- `make tkc` compiles clean with `-Wall -Wextra -Wpedantic -Werror`
- `realpath()` validation ensures no symlink or `..` traversal escapes the document root
- Null byte injection blocked in both URL decoders
