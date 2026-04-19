# Website Content Audit — toke-web (old) vs toke-website (new)

**Date:** 2026-04-19
**Story:** 57.2.1

---

## Summary

The old site (Astro/Starlight, archived at `archive/toke-web/`) and the new site (ooke-powered, `toke-website/`) have near-complete content parity. The new site has **more** content overall, but 3 old-site pages were not migrated.

## Page Mapping

### Path reorganisation (no content loss)

| Old path | New path | Status |
|----------|----------|--------|
| `reference/stdlib/*` (30 modules) | `stdlib/*` | Moved to top-level. All 30 modules present. |
| `reference/cookbook/*` (3 recipes) | `cookbook/*` | Moved to top-level. All 3 present. |
| `getting-started/*` (4 pages) | `getting-started/*` + duplicated into `learn/*` | All present. Learn section also includes the 10-lesson course. |
| `community/*` (2 pages) | `about/*` (contributing, enterprise) | Merged into about section. |
| `index.mdx` (splash page) | `index.html` | Fully ported including TokenViz tabs, best-practices comparison, cards, and CTA links. |

### Content only in old site (3 pages — needs migration)

| Old page | Content | Action |
|----------|---------|--------|
| `reference/phase2/overview.md` | "Encoding Design" — character set comparison (80 vs 56), why two profiles, transformation rules, sigil rationale | **Port** — valuable explainer for the default syntax design. Rename to `docs/decisions/encoding-design.md` or `docs/reference/encoding-design.md`. |
| `reference/phase2/grammar.md` | Phase 2 grammar rules — EBNF fragments for default syntax constructs (`$variant`, `@()`, sigils) | **Port** — useful grammar reference for default syntax. Merge into existing `reference/grammar` page or create `docs/reference/default-syntax-grammar.md`. |
| `reference/phase2/types.md` | Type system under default syntax — `$str` sigil convention, sum type variant syntax (`$ok`, `$err`) | **Port** — type reference for default syntax. Merge into existing `reference/types` page. |

### Content only in new site (not in old)

| Section | Pages | Notes |
|---------|-------|-------|
| `compiler/*` | 5 pages | binary-size, companion-file-spec, conventions, lint-rules, runtime-abi |
| `decisions/*` | 4 pages | ADR-0001, ADR-0002, ADR-0003, gate1-decision |
| `spec/*` | 5 pages | Full spec, semantics, errors, stdlib-signatures, implementation-delta |
| `stdlib/*` | 7 new modules | args, crypto_ext, json_stream, md, path, toml (not in old site) |
| `about/*` | 3 new pages | ecosystem, loke, ooke |
| `reference/*` | 2 new pages | glossary, stdlib-coverage |

### Content in both but potentially different

All shared pages (about/why, about/design, about/repos, learn/01-10, reference/*, getting-started/*) exist in both sites. The new site versions are the canonical source — they were updated during Epic 58 (website transition) and subsequent epics. The old site versions should not be merged back; they are frozen copies.

## Recommendations

1. **Port 3 phase2 pages** to the new site under `docs/reference/` or `docs/decisions/`. Update terminology: "Phase 2" → "default syntax", "Phase 1" → "legacy profile".
2. **Archive old site** — `archive/toke-web/` can remain as-is. No content will be lost after the 3 pages are ported.
3. **No content regression** — the new site has strictly more content than the old site (5 compiler pages, 4 decision records, 5 spec pages, 7 new stdlib modules, 3 ecosystem pages, 2 reference pages).
