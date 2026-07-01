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

## Audit-120 cross-reference

**Added:** 2026-07-02 (Story 120.21, Epic 120)

The Epic 120 security audit re-examined path-handling and containment across the
compiler, runtime, and web layers and found several traversal / symlink /
decoding issues beyond the 57.4.3 scope. Per-area reports live under
[`audit-120/`](audit-120/); each finding is tracked as an Epic 121 remediation story.

| Finding | Area report | Severity / reach | Relation to this audit |
|---------|-------------|------------------|------------------------|
| WEB-02 `http.servedir` lacks symlink/allowed-root resolution (weaker than `router_static_serve`) | [`web-glue.md`](audit-120/web-glue.md) | Medium / remote-unauth | `servedir` did **not** inherit the 57.4.3 `realpath()` document-root fix |
| OOK-01 ooke static file server exposes the entire project directory | [`ooke.md`](audit-120/ooke.md) | High / remote-unauth | app-layer static serving is unrooted |
| PAR-09 Template engine layout path traversal (+ unbounded block/partial recursion) | [`parsers.md`](audit-120/parsers.md) | Medium / remote-auth | template layout path not contained |
| AMB-05 `file_rmdir_r` follows symlinks (`stat` not `lstat`) — recursive delete escapes the tree | [`ambient-authority.md`](audit-120/ambient-authority.md) | Medium / remote-auth | extends item 6 "accepted risk" for `file.c` |
| AMB-06 `path_join` / `file_join` do not normalize `..` — unsafe as containment primitives | [`ambient-authority.md`](audit-120/ambient-authority.md) | Medium / remote-auth | no safe join primitive exists yet |
| AMB-07 File open/read/write/copy follow symlinks (no `O_NOFOLLOW`); write-through-symlink + TOCTOU | [`ambient-authority.md`](audit-120/ambient-authority.md) | Low / remote-auth | extends item 6 "accepted risk" for `file.c` |
| WEB-06 Route params not percent-decoded; router query decoder keeps `%00` | [`web-glue.md`](audit-120/web-glue.md) | Info / remote-unauth | inconsistent decoding vs. the 57.4.3 `%00`-drop fix (item 4) |
| RUN-10 WAF URI validator does not decode; misses traversal / double-encoding; permits raw TAB | [`runtime-waf.md`](audit-120/runtime-waf.md) | Low / remote-unauth | the WAF-layer traversal check is bypassable (and the engine is dead code — RUN-01) |

The 57.4.3 `realpath()` + document-root fix in `router_static_serve` remains valid;
audit-120 shows it is **not uniformly applied** across the other serving/joining paths.
