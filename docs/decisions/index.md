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
- [ADR-0004 — String building uses interpolation + stdlib, never operator overloading](/docs/decisions/ADR-0004/) -- `+` is never string concatenation
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
- [135.6 — Implementation route for `std.ocr`, the platform OCR binding](/docs/decisions/135.6-platform-ocr-route/) -- **decided 2026-09-23** (Epic 135/135.6); bind macOS Vision from plain C99 via `objc_msgSend`, no toolchain change; **Vision's confidence is a per-level constant (1.0 accurate, 0.5 fast) and carries no reliability signal**, which is an argument for 135.8 rather than something 135.6 can defer
- [96.7 — `tkc --python-view`, a read-only Python view of toke source](/docs/decisions/96.7-python-view/) -- **decided 2026-11-10** (story 96.7); a printer over the AST next to `--dump-ast`, **display only** (not executable, not round-trippable), deterministic; an executable variant is recorded as an owner option, not built
- [138.19 — Scoped capability grants (path, host, command)](/docs/decisions/138.19-scoped-capability-grants/) -- **ACCEPTED (D1–D4) 2026-11-15** (story 138.19, loke T-59/B9; register 3.4); a spec amendment to ADR-0010: per-class scope lists, direction-prefixed `net` scopes, scoped gates at the existing `TK_REQUIRE` sites, runtime flags narrow a baked set, CAP002, a warning release for quoted strings; **implemented**; D5 (OS-level enforcement) is 138.44
- [138.44 — OS-level enforcement of capability grants](/docs/decisions/138.44-os-enforcement/) -- **implemented, opt-in** (story 138.44; D5 of 138.19): `os_enforce = true` / `--cap-os-enforce` has the kernel enforce the baked grants behind the broker (macOS `sandbox_init`, Linux Landlock + seccomp), a superset of the broker that never refuses what it admits; CAP004 warns where the kernel cannot carry a grant
- [138.24 — HTTP client request controls, a header-carrying GET and a resolver](/docs/decisions/138.24-http-client-controls/) -- **accepted 2026-11-15** (stories 138.24, 138.25, 138.27, 138.34; owner answers 2.1-2.3); a spec amendment to the stdlib signatures: `http.withdeadline`, `http.withmaxbody`, `http.withconnectaddr` (opt-in, off by default), `http.getheaders`, `net.resolve` gated by `net`; `timeoutms` documented as per read
- [127.299 — Releasing a function's allocations](/docs/decisions/127.299-function-arena/) -- **design note 2026-11-23** (story 127.299; owner answer 1.13: (b) now, an ADR for (a)); options: per-function arenas with escape analysis, scope-exit frees, the `{arena}` block first, refcounting, the shipped model with a leak budget; measured on the Z77 harness; ships one provably safe slice (interpolation frees its number conversions, C2200); follow-ups 127.524 (§1 amendment) and 127.525 (`{arena}` ADR)
- [ADR-0016 — Releasing memory, first target: the explicit {arena} block, with S1 sealed strings](/docs/decisions/ADR-0016-arena-block/) -- **proposed 2026-11-24** (story 127.525; owner answer 1.13 (a)); a dynamic per-thread region stack behind the existing `ArenaStmt` (no grammar change, 53 productions), E5001 over the whole block with copy-out only for `<`, per-thread sealed chunks and an amended S1 rule, a load-bearing `.tki` retained marking; no code until accepted; seven owner questions
- [ADR-0017 — Structured concurrency: sc is the one construct, with typed task handles](/docs/decisions/ADR-0017-structured-concurrency/) -- **accepted 2026-11-28** (stories 127.553, 127.554; owner decision 2026-11-28, superseding D4=B for concurrency); `sc{}` the block, `sc f(x)` a task (call form), `sc():T` the handle type read by `h()`; joins on every exit edge, handles confined to their block (E4083), unread fallible results rejected (E4084), no cancellation; `std.task` withdrawn behind E2038 (an error at once); helping-join runtime; unread fallible results must be read on every path (owner resolutions 1-7)
- [Gate 1 Decision Document](/docs/decisions/gate1-decision/) -- Gate 1 review and outcome
- [Gate 2 Decision Record](/docs/decisions/gate2-decision/) -- **withdrawn 2026-11-15** (owner decision D4); the PASS recorded 2026-05-24 is withdrawn and Gate 2 has no standing verdict; Gate 3 never ran and is void
