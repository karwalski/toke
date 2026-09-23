---
title: Architecture Decision Records
slug: index
section: decisions
order: 0
---

Architectural decisions for the toke project, recorded as ADRs.

## Decisions

- [ADR-0001 — Initial Language and Compiler Architecture](/docs/decisions/ADR-0001/) -- foundational architecture choices
- [ADR-0002 — Source File Extension](/docs/decisions/ADR-0002-file-extension/) -- `.tk` file extension decision
- [ADR-0003 — Serialization Format Strategy and Internationalisation](/docs/decisions/ADR-0003/) -- TOON, YAML/JSON, and i18n
- [ADR-0004 — String building uses interpolation + stdlib, never operator overloading](/docs/decisions/ADR-0004/) -- `+` is numeric-only
- [ADR-0005 — ooke is pure toke; native capability lives in toke core](/docs/decisions/ADR-0005/) -- the app contains zero C; gaps become reusable stdlib
- [ADR-0006 — Array mutation performance: capacity + copy-on-write](/docs/decisions/ADR-0006/) -- **proposed** (story 114.18); fixes O(N²) array building
- [ADR-0007 — `if` is an expression](/docs/decisions/ADR-0007/) -- v0.4 (116/A1); expression-`if` replaces the mut-flag idiom
- [ADR-0008 — `=` is assignment, `==` is equality](/docs/decisions/ADR-0008/) -- v0.4 (116/A3); breaking; removes the `=` overload
- [ADR-0009 — Backtrack-free parser (bounded lookahead) + `&&`/`||`](/docs/decisions/ADR-0009/) -- v0.4 (116/A2,A4,A5); honest grammar property
- [ADR-0010 — Ambient authority and the toke capability model](/docs/decisions/ADR-0010/) -- **ACCEPTED 2026-07-03** (Epic 124); deny-by-default capabilities, out-of-band grants, flip bundled with grant-aware harness
- [ADR-0011 — Injection-proof by construction: argv-only exec, auto-escaping templates, parameterized queries](/docs/decisions/ADR-0011/) -- **ACCEPTED 2026-07-03** (Epic 124); full context-aware auto-escape default; source-API changes folded into the idiom-v0.4 codemod
- [ADR-0012 — Spatial memory safety of emitted code: bounds-checked indexing + guarded division](/docs/decisions/ADR-0012/) -- **ACCEPTED 2026-07-03** (Epic 124); bounds/div/nil/canary; bounds-default flips bundled with `-O2` + `@nobounds`; option-type split to ADR-0014
- [ADR-0013 — Cryptographic agility: versioned formats, JWT alg-dispatch, hybrid post-quantum KEX](/docs/decisions/ADR-0013/) -- **ACCEPTED 2026-07-03** (Epic 124); envelope + JWT dispatch + hybrid KEX + committed signature PQC
- [ADR-0014 — Option / non-null as a first-class type (null-safety by construction)](/docs/decisions/ADR-0014/) -- **proposed, gated on Epic 123.5** ($none codegen fix); reuse `T!$none`+`mt`, no new syntax unless measurement forces it
- [ADR-0015 — The toke toolchain is C99 only; no vendored C++ dependencies](/docs/decisions/ADR-0015/) -- **ACCEPTED 2026-09-22** (Epic 135/135.0); `-std=c99`, zero C++ TUs; pdfium/podofo/Tesseract disqualified as vendored deps; escape hatches = C shim, or separate optional component
- [135.3 — Implementation route for `std.pdf` text extraction](/docs/decisions/135.3-pdf-extraction-route/) -- **decided 2026-09-23** (Epic 135/135.3); a purpose-built C99 extractor, because MuPDF is AGPL against this repository's Apache-2.0 and every other candidate is C++ under ADR-0015; items 1-4 of the story ship, embedded images do not
- [Gate 1 Decision Document](/docs/decisions/gate1-decision/) -- Gate 1 review and outcome
