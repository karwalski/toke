---
title: Changelog
slug: changelog
section: about
order: 1
---

A reverse-chronological record of major project milestones and compiler releases. The
per-change record for each release, with every breaking change and its migration, is
[`CHANGELOG.md`](https://github.com/karwalski/toke/blob/main/CHANGELOG.md) in the toke
repository; this page summarises it. Story numbers refer to the tracker,
[`docs/progress.md`](https://github.com/karwalski/toke/blob/main/docs/progress.md).

## 3.2.0 (unreleased)

**Not released.** Everything on `main` since the `v3.1.0` tag (2026-09-30). The version
is prepared on a release branch and is not tagged or published; until it is, a
downstream that needs one of these fixes pins a `main` commit.

**Breaking changes.** `CHANGELOG.md` § Unreleased lists each with its migration:

- `std.tls` clients verify the server's certificate chain and host name by default;
  `tls.insecureconfig` is the explicit opt-out (121.64).
- Text-format log lines are logfmt, with the message in a quoted `msg=` field (121.41).
- The stdlib `.tki` type names are lower case (`$fileerr`, `$dberr`, `$jsonval`, ...);
  an old spelling is E3011 with a deterministic `fix`, and `tkc --migrate` rewrites it
  (127.251).
- A match-arm label must name a type or a variant (127.292), and the `lasterr` accessors
  of `ocr`, `pdf` and `xlsx` are retired in favour of the `$err` arm (127.246).

**Compiler and tooling.**

- `tkc pkg init|add|remove|resolve|fetch|list`: `pkg.toml`, `pkg.lock`, a resolver in the
  import path and git-based fetch (76.1.3b, 76.1.3c, 76.1.3d; ADR-0004).
- `tkc --python-view`, a read-only Python-like view of a source file (96.7).
- `--emit-tkir`, `--from-tkir` and `--read-tkir`: a type-checked program as binary IR,
  compiled back with the same LLVM IR as from source (76.1.6c).
- tkc runs the clang found when it was built, by absolute path; `TKC_CC` overrides it and
  an unusable path is E9027 (121.28). A program over a compiler limit exits 1 (127.264).
- `--migrate` keeps comments, and declines with E9026 and exit 4 on source it should not
  touch (127.197, 127.198).
- Re-raising an error into another error type is E4076 at `--check` (127.275).
- A `let` inside a block goes out of scope at the end of the block (127.196).
- Strings carry their length, so `.len` and `str.charat` scans are linear (127.170).
- `m.get(k)` is an error union the checker enforces (127.244).
- A command-line reference generated from `tkc --help`:
  [Command-Line Reference](/docs/reference/cli/) (132.135).

**Standard library.**

- `std.router` dispatches `:param` and `*` routes (127.343) and carries request and
  response headers (121.90).
- Every `std.http` and `std.llm` TLS context sets an explicit key-exchange group list with
  a hybrid post-quantum group first (122.15).
- The WebSocket client no longer dies of SIGPIPE (127.347), nor does the stdlib HTTP test
  under a redirected stdout (127.341).
- HTTP/1.0 requests without keep-alive are closed (121.102); the stdlib TLS code builds
  against OpenSSL 4 (121.104); `toml` gains `loadfile` and real error messages (113.B.4).
- The standard library is MIT-licensed, with the licence text and SPDX headers in the
  tree (135.15a).

### Fixed for loke since 3.1.0

loke's IDs are from its handoff of 2026-11-09; the toke and ooke stories close them.

- **T-62** (loke F10.42) — an `@@str` row subscripted inline as a call argument no longer
  emits invalid IR (127.274).
- **T-64** — the HTTP client answers `$err`, not `$ok` with a short body, when a response
  is cut short: a body short of its `Content-Length`, a chunked body that ends before its
  last chunk, or a close-delimited body that ends in a read timeout or reset (138.23,
  138.31).
- **T-48 addendum** — passing an `httpclient` where a `str` URL is declared
  (`http.postheaders(http.client(url);...)`, `http.geturl(http.client(url))`) is E4031
  (138.26).
- **T-60, the spawn half** — the checker judges `std.process` handles in argument slots,
  and a `$err` arm of `@()` or a string beside `$ok:h h` is E4011; `process.badhandle()`
  is the failure value (138.21).
- An expression-`if` whose branches have different known types is E4011 (138.22).
- The client's `Host` header carries a non-default port (138.28).
- In a `mt` over a std call that returns a fielded record with an error (`httpresp`,
  `jsonval`, `csvrow`, `dbrow`, `wsmsg` and others), the `$ok` payload is typed
  (138.33).
- **O-4** (ooke) — an unknown template directive, a stray `end` and a block left open
  are `$tplerr`, not silently dropped content (138.29).
- **O-5** (ooke) — ooke's changelog names the real raw-output syntax, `{= key | raw =}`
  (138.30).

**Still open for loke:** `==` on arrays (T-60's other half, 138.20); a whole-request
deadline and a body-size cap for the HTTP client (T-63, 138.24); connecting to a given
address with host-name verification, and a resolver (T-63, 138.25); a header-carrying
GET (T-58 addendum, 138.27); scoped capability grants (T-59 / B9, 138.19).

## 3.1.0 — 2026-09-30

loke's handoff, the typed standard library, and security fixes. Tagged `v3.1.0`
(`86c432da`). Read the breaking changes in `CHANGELOG.md` § 3.1.0 before upgrading:
most of the release is the checker learning to reject programs that 3.0.0 compiled and
then ran wrongly.

- Standard-library calls are checked against the typed `.tki` interfaces: argument kinds
  and scalar returns (138.3, 138.11, 138.12, 138.13).
- HTTP client calls are separate from route registration (`http.clientget`,
  `http.geturl`, ...), and `http.postheaders` is typed and takes three arguments (138.3,
  138.8).
- The https client verifies server certificates (135.25).
- One regex engine for the checker and the runtime; unsupported syntax is E4075 at
  `--check` (137.9, 121.35).
- The default `max_funcs` and `max_structs` rise to 1024 (127.250).
- Security fixes across HTTP, templates, decoders, crypto and capability gates (Epic 121).

### Fixed for loke in 3.1.0

The 2026-09-27 handoff (Epic 138), with loke's IDs:

- **T-18** — `mt` binds a result's arms by label, not by position (138.2).
- **T-20** — `f64` (and narrow integer) returns cross module boundaries correctly (138.5).
- **T-25, T-54** — default caps: `max_funcs` and `max_structs` default to 1024, so
  loke's `--max-funcs`/`--max-structs` flags can go (138.15, 127.250). **T-56** — the
  import cap is unchanged, but going over it is E9010 at `--check`, naming the count, the
  limit and `--max-imports` (138.15).
- **B0 / T-58** — certificate verification: `http.postheaders`, `http.geturl` and
  `http.clientget` refuse an untrusted or mismatched certificate before sending a request
  (135.25).
- **B1** bind address: `http.serveon(host;port)` and friends (138.1). **B2** `mt` by
  label (138.2). **B3 / T-57** client calls separated from route registration:
  `http.clientget`, `http.geturl` (138.3). **B4** keychain status and throwaway keychains
  (138.4). **B5** `f64` across modules (138.5). **B6** `std.db` handles and zero values
  (138.6). **B7** an ooke release candidate (rc.2) that builds on current toke (138.7).
  **B8 / T-48** the typed three-argument `http.postheaders` (138.8).

## 3.0.0 — 2026-09-23

The v0.4 language boundary. Tagged `v3.0.0` (`0b0bc4e`). `VERSION` had read 2.8.0 since
2026-06-21 while the whole v0.4 language break shipped under it; 3.0.0 is the first
version number that names v0.4. Breaking changes since 2.8.0: `=` is assignment and `==`
is equality; expression-oriented `if`; `@(...)` array and map literals; the
deny-by-default capability broker and the `tkc.toml [capabilities]` manifest (ADR-0010);
escape-by-default templates (ADR-0011); `mut.` bindings; no-underscore stdlib names.
Require `>= 3.0.0` for anything written in v0.4 syntax.

## April 2026

- **(2026-04-15)** **Gate 2 ON HOLD**: Training corpus quality insufficient for reliable model training. Automated toke code generation produces too many syntax and semantic errors for the corpus to be useful without significant manual curation. Gate 2 evaluation paused until a higher-accuracy training corpus can be built, likely requiring local compute hardware (Mac Mini/Studio) for iterative retraining.
- **(2026-04-15)** **Documentation consolidation**: All documentation merged into `~/tk/docs/` as single source of truth. Research and planning documents moved to `~/tk/research/`.
- **(2026-04-12)** **Corpus overhaul complete**: 188,830 deduplicated records, 18,890 train + 994 eval rows in chat format with quality gates. Phase 1 data archived.
- **(2026-04-05)** **Syntax frozen** at `v0.2-syntax-lock`. 55-char profile is the default ("toke"). 80-char profile available under `--legacy`.
- **(2026-04-03)** **Gate 1 recorded PASS** (NOT PASSED since the owner decision of 2026-09-22, story 128.19): 12.5% token reduction (8K purpose-built BPE vs cl100k_base on the **same toke source** -- mean 172.9 vs 197.6 tokens/program over 46,754 validated toke programs; a tokenizer-lane figure, not a comparison with Python, and superseded on v0.4 text -- `docs/metrics-baseline.md`); **58.8% Pass@1** (588 of 1,000 solutions generated; Qwen 2.5 Coder 7B + LoRA, 1,000 held-out tasks). *(The original entry read "vs. Python/Go/Rust equivalents" -- corrected 2026-09-19, story 132.6: the figure never compared toke with another language. It also read "63.7% Pass@1" and "Both thresholds exceeded" -- corrected 2026-09-19, story 128.19: 63.7% was 588/**923**, computed after the 77 non-compiling solutions had been dropped from the denominator. The Pass@1 threshold of >= 60% was **not** met; the verdict was re-opened, and on 2026-09-22 the owner decided Gate 1 NOT PASSED; the token-reduction threshold of >= 10% was met. See `docs/decisions/gate1-decision.md`.)*

## Q1 2026

- **Repo consolidation**: 10 repositories reduced to 6.
- **30+ standard library modules** implemented in C with full `.tki` interface contracts.
- **Build system, test hardening, and integration test suites** completed.
- **8K BPE tokenizer trained**: 15.2% token reduction compared to `cl100k_base`.
- **Website and specification updates** for default syntax.

## Q4 2025

- **Phase A--D corpus**: 46,730 validated programs covering pure language, stdlib, and advanced patterns.
- **Compiler bootstrap**: `tkc` compiler producing valid executables with conformance tests.
- **toke-spec formalized**: Specification document locked for Gate 1 evaluation.

## Project Gates (from spec Section 21.5)

| Gate | Criteria | Status |
|------|----------|--------|
| Gate 1 (Month 8) | Token reduction >= 10%, Pass@1 >= 60% | **NOT PASSED** (owner decision 2026-09-22, story 128.19; recorded PASS Apr 2026 on a Pass@1 later corrected to 58.8%) |
| Gate 2 (Month 14) | 7B model >= 65% Pass@1, extended features maintain efficiency | **ON HOLD** |
| Gate 3 (Month 26) | Two+ LLM families >= 70% Pass@1, self-improvement loop running | Pending |
| Gate 4 (Month 32) | All benchmarks met, spec complete, consortium proposal ready | Pending |
