# docs/progress.md
## toke — Story Progress Tracker

**Project phase:** Phase 2 — Language Extensions
**Active milestone:** M3
**Gate 1:** PASS (2026-04-03) — 12.5% token reduction, 63.7% Pass@1
**Last updated:** see git log

---

## How to use this file

Update this file at the start and end of every development cycle.  
Statuses: `backlog` | `planned` | `in_progress` | `blocked` | `review` | `done`

---

## Phase 1 — Falsification

### Epic 1.1 — Language Specification Lock

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.1.1 | Character set finalisation | done | feature/spec-character-set | [x] 80 chars enumerated with class [x] Every symbol has named primary/secondary role [x] String delimiter assigned to `"` [x] Excluded chars listed with rationale [x] Frozen at M0 2026-03-27 [x] No conflicting dual roles at same position |
| 1.1.2 | Keyword table lock | done | feature/spec-keyword-table | [x] 12 keywords listed with exact lexical form [x] Each keyword's production rule identified [x] true/false documented as predefined identifiers [x] Prefix-conflict analysis: lp/let share `l`, resolved by second char [x] Underscore excluded, identifier rule updated [x] Frozen at M0 2026-03-27 |
| 1.1.3 | Symbol disambiguation rules | done | feature/spec-symbol-disambiguation | [x] 14 dual-role symbols documented [x] All rules LL(1) one-token lookahead [x] Two examples per symbol [x] Summary table [x] Frozen M0 2026-03-27 |
| 1.1.4 | Formal EBNF grammar | done | feature/spec-ebnf-grammar | [x] 65 productions covering all constructs [x] LL(1) verified [x] Unambiguous [x] All 8 open questions resolved [x] Committed to spec/grammar.ebnf [x] Frozen at M0 2026-03-27 |
| 1.1.5 | Profile 2 transformation rules | done | feature/spec-phase2-transform | [x] Type sigil rule (TYPE_IDENT→$name) [x] Array literal rule ([]→@()) [x] Array index rule (a[n]→a.get(n)) [x] Determinism proof [x] Round-trip spec [x] 20+ example pairs [x] Frozen M0 2026-03-28 |
| 1.1.6 | Spec review and alignment | done | feature/spec-review-m0 | [x] Cross-document review complete [x] 7 blocking issues resolved [x] grammar.ebnf fully populated (was stub) [x] character-set.md created [x] keywords.md created [x] symbol-disambiguation.md created [x] phase2-transform.md created [x] CastExpr production added [x] LoopStmt semicolon ambiguity fixed [x] spec-review-m0.md written [x] 4 warnings, 4 notes in risk register [x] Frozen M0 2026-03-27 |
| 1.1.7 | Meta-repo README and project landing page | done | feature/meta-readme (toke) | [x] Plain-language thesis [x] 7 sub-repo links [x] Phase/gate status table [x] Getting-started (build + conform) [x] Licence + contributing links [x] No placeholder text |

### Epic 1.2 — Reference Compiler Frontend

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.2.1 | Lexer implementation | done | feature/compiler-lexer | [x] 80-char set [x] 38 token kinds [x] W1010 [x] E1001 E1002 E1003 [x] 296 lines |
| 1.2.2 | Parser implementation | done | feature/compiler-parser | [x] All 47 non-terminals [x] LL(1) [x] AST with source positions [x] E2001–E2004 diagnostics [x] 394 lines |
| 1.2.3 | Import resolver | done | feature/compiler-import-resolver | [x] .tokei loading [x] E2030 unresolved [x] E2031 circular [x] std.* prefix [x] SymbolTable [x] 179 lines |
| 1.2.4 | Name resolver | done | feature/compiler-name-resolver | [x] Scope chain [x] Predefined ids [x] Import aliases [x] E3011 E3012 [x] All scope levels |
| 1.2.5 | Type checker | done | feature/compiler-type-checker | [x] 10/10 rules [x] E4031 E4010 E4011 E4025 E5001 E3020 [x] 346 lines (arena escape: conservative — Decl needs depth stamp) |
| 1.2.6 | Structured diagnostic emitter | done | feature/compiler-diag | [x] JSON schema 1.0 [x] fix field omitted when absent [x] --diag-text [x] 180 lines |
| 1.2.7 | Interface file emitter | done | feature/compiler-interface-emitter | [x] JSON .tokei [x] module/func/type/const [x] interface_hash [x] E9001 [x] 190 lines |
| 1.2.8 | LLVM IR backend | done | feature/compiler-llvm-backend | [x] Text IR all constructs [x] 3 targets via clang [x] E9002 E9003 [x] 396 lines (cast/field-offset/nested-break stubs) |
| 1.2.9 | CLI interface | done | feature/compiler-cli | [x] All flags [x] Full pipeline [x] Exit codes 0/1/2/3 [x] isatty diag [x] 186 lines |
| 1.2.10 | Conformance test suite (Profile 1) | done | test/compiler-conformance-suite | [x] 62 tests [x] L/G/D series [x] run_conform.sh [x] make conform wired |

### Epic 1.7 — Compiler Security and SAST

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.7.1 | SAST on compiler C code | done | feature/security-sast-dco | [x] cppcheck CI [x] clang-tidy [x] PR annotations [x] 63-line workflow |
| 1.7.2 | Compiler input fuzzing | done | feature/security-fuzzing | [x] libFuzzer lexer+parser targets [x] ASAN+UBSAN [x] 5 corpus files [x] make fuzz [x] nightly CI |
| 1.7.3 | Secret scanning in corpus pipeline | done | feature/security-secret-dep-scan (toke-corpus, toke-model) | [x] gitleaks CI [x] .gitignore hardened [x] pre-commit docs (GitHub UI: enable secret scanning) |
| 1.7.4 | Dependency vulnerability scanning | done | feature/security-secret-dep-scan (toke-corpus, toke-model) | [x] Dependabot [x] pip-audit CI [x] pyproject.toml pinned (GitHub UI: enable Dependabot alerts) |
| 1.7.5 | Signed commits and DCO enforcement | done | feature/security-sast-dco | [x] dco.yml [x] retroactive signoff [x] CONTRIBUTING.md [x] README notice (GitHub App install: manual) |

### Epic 1.8 — Corpus Pipeline Threat Model

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.8.1 | Corpus pipeline threat model document | done | feature/security-threat-model-sandbox | [x] T1-T5 threat categories [x] likelihood/impact/mitigation [x] sign-off table |
| 1.8.2 | Sandboxed execution of generated programs | done | feature/security-threat-model-sandbox | [x] sandbox-exec (macOS) [x] Docker fallback [x] 10s timeout [x] clean env [x] setup docs |

### Epic 1.3 — Standard Library Core

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.3.1 | std.str | done | feature/stdlib-str | [x] 14/14 functions [x] str.tokei [x] C impl 222 lines [x] 22 test assertions |
| 1.3.2 | std.http | done | feature/stdlib-http | [x] Req/Res/HttpErr types [x] 5 route verbs [x] :param extraction [x] POSIX server [x] http.tokei |
| 1.3.3 | std.db | done | feature/stdlib-db | [x] 8/8 functions [x] SQLite3 backend [x] DbErr sum type [x] db.tokei [x] in-memory test suite |
| 1.3.4 | std.json | done | feature/stdlib-json | [x] 8/8 functions [x] JsonErr sum type [x] json.tokei [x] recursive key extractor [x] 7 test assertions |
| 1.3.5 | std.file | done | feature/stdlib-file | [x] 6/6 functions [x] FileErr sum type [x] file.tokei [x] 138 lines [x] 9 test assertions |

### Epic 1.9 — Remote Monitoring Console

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.9.1 | Job manager daemon | done | feature/monitor-console | Flask REST API, subprocess manager, jobs.json persistence |
| 1.9.2 | Web dashboard | done | feature/monitor-console | Dark-theme SPA, auto-refresh, log panel |
| 1.9.3 | Log streaming | done | feature/monitor-console | SSE /api/jobs/{id}/stream; EventSource in dashboard |
| 1.9.4 | Authentication and HTTPS | done | feature/monitor-console | TLS via TK_MONITOR_CERT/KEY; token query param for SSE |

### Epic 1.4 — Mac Studio Setup and Local Pipeline

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.4.1 | Mac Studio configuration | bypassed | — | Used EC2 cloud instead (Epic 8.1) |
| 1.4.2 | Local inference pipeline | bypassed | — | Cloud pipeline in Epic 8.1 |
| 1.4.3 | Corpus storage schema | done | — | Implemented in corpus pipeline (schema.json) |

### Epic 1.5 — Phase A Corpus Generation

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.5.1 | Task curriculum generator | done | — | 339 templates, 50K tasks (via Epic 8.1.3) |
| 1.5.2 | Four-language parallel generation | done | — | Multi-model pipeline: Haiku 4.5, GPT-4.1-mini, Grok-3-mini (via Epic 8.1) |
| 1.5.3 | Differential test harness | done | — | Python/C/Java majority agreement (via Epic 8.1.7) |
| 1.5.4 | Corpus quality metrics | done | — | 46,754 entries: A=26,978 B=9,776 C=5,000 D=5,000 |

### Epic 1.6 — Gate 1 Benchmark

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.6.1 | Held-out benchmark task set | done | feature/benchmark-held-out-tasks | [x] 500 tasks [x] 120 test inputs each [x] Gitignored [x] Generation script committed |
| 1.6.2 | Token efficiency measurement | done | — | [x] 12.5% reduction (8K vocab) [x] 13.1% reduction (32K vocab) [x] eval_report.json [x] Gate 1 threshold met (>10%) |
| 1.6.3 | Pass@1 measurement | done | — | [x] 1000-task benchmark (500 original + 500 expanded) [x] 923/1000 compiled (92.3%) [x] 588/923 Pass@1 (63.7%) [x] Gate 1 threshold met (>60%) [x] 7 codegen fixes applied (Epic 2.8) [x] Zero regressions |
| 1.6.4 | Gate 1 decision document | done | — | [x] gate1-decision.md in toke-spec/docs [x] Full results, verdict, and next steps [x] Gate 1: PASS (2026-04-03) |

### Epic 1.10 — Phase 1 Integration and Conformance Review

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.10.1 | Phase 1 full integration and conformance review | done | docs/phase1-review (tkc) | [x] 62/62 conformance [x] all 9 compiler passes [x] stdlib tests pass [x] docs/phase1-review.md written [x] 6 TD items logged |

### Epic 2.6 — Responsible Disclosure and CVE Process

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.6.1 | Security policy and disclosure process | done | feature/security-disclosure | SECURITY.md in tkc/corpus/model; README linked |
| 2.6.2 | CVE coordination process | done | feature/security-disclosure | docs/security/cve-process.md |

### Epic 2.7 — Standard Library Expansion

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.7.1 | std.process | done | feature/stdlib-2.7-process-env (tkc + toke-stdlib) | [x] POSIX fork/exec/pipe/waitpid [x] ProcessHandle [x] process.tokei [x] 28/28 tests |
| 2.7.2 | std.env | done | feature/stdlib-2.7-process-env (tkc + toke-stdlib) | [x] getenv/setenv/unsetenv [x] EnvErrKind [x] env.tokei [x] tests pass |
| 2.7.3 | std.crypto | done | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) | [x] SHA-256 + HMAC-SHA-256 [x] self-contained [x] crypto.tokei [x] test vectors pass |
| 2.7.4 | std.time | done | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) | [x] clock_gettime [x] strftime [x] time.tokei [x] tests pass |
| 2.7.5 | std.test | done | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) | [x] assert/assert_eq/assert_ne [x] DIAGNOSTIC stderr format [x] test.tokei [x] tests pass |

### Epic 3.7 — Supply Chain Security and Release Signing

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 3.7.1 | SBOM generation for compiler releases | done | feature/supply-chain-3.7 (tkc) | SPDX JSON via syft; published as release artifact |
| 3.7.2 | Release binary signing | done | feature/supply-chain-3.7 (tkc) | cosign keyless; tamper test in CI; docs/security/release-signing.md |
| 3.7.3 | Reproducible builds for the compiler | done | 2026-04-03 | [x] REPRO_FLAGS (frandom-seed, ffile-prefix-map) [x] SOURCE_DATE_EPOCH [x] make repro-check (all .o bit-identical) [x] docs/security/reproducible-builds.md [x] macOS LC_UUID variance documented |
| 3.7.4 | Model release safety evaluation | done | feature/supply-chain-3.7 (toke-models) | [x] LlamaGuard eval process doc [x] safety_eval.py (dry-run verified) [x] 50 adversarial templates (5 categories) [x] docs/security/model-safety-evals.md |

### Epic 3.8 — Standard Library Production Hardening

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 3.8.1 | std.log structured logging | done | feature/stdlib-3.8-log (tkc + toke-stdlib) | [x] NDJSON stderr [x] TK_LOG_LEVEL [x] log.info/warn/error [x] 12/12 tests |
| 3.8.2 | stdlib performance benchmarks | done | feature/stdlib-3.8-bench (tkc) | [x] 9 modules benchmarked [x] baseline.txt (M-series) [x] make bench [x] bench.yml CI |
| 3.8.3 | stdlib conformance test coverage | done | feature/stdlib-3.8-bench (tkc) | [x] 100% function coverage [x] 30 gap tests [x] docs/stdlib-coverage.md |

### Epic 2.8 — LLVM Backend Correctness

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.8.1 | Fix LLVM IR emission for end-to-end compilation | done | feature/codegen-2.8 (tkc) | [x] 5 defects fixed: param SSA naming, let-bind child index, call return types, match arm index, cast emission [x] Terminator tracking [x] prepass_funcs() [x] 5/5 e2e tests pass [x] 79/79 conformance |
| 2.8.2 | Fix loop variable codegen (lp construct) | done | — | **[local]** Three bugs fixed: (1) NODE_LOOP_INIT had no emit_stmt handler — loop var never alloca'd/stored (2) loop step (i=i+1) never emitted before back-edge branch (3) break label used fragile `lbl-1` arithmetic — replaced with dedicated `break_lbl` field. [x] 90/90 conformance [x] 9/9 e2e [x] nested loops [x] nested break |

### Epic 2.9 — Tokenizer Pipeline Scaffolding

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.9.1 | Corpus preparation script (prepare.py) | done | feature/tokenizer-2.9 (toke-tokenizer) | [x] JSONL extraction [x] String placeholder replacement [x] SHA-256 dedup [x] Configurable train/valid split [x] 25/25 tests pass |
| 2.9.2 | BPE training wrapper (train.py) | done | feature/tokenizer-2.9 (toke-tokenizer) | [x] SentencePiece BPE wrapper [x] Toke-specific defaults [x] Dry-run mode [x] 17/17 tests pass |
| 2.9.3 | Tokenizer evaluation script (eval.py) | done | feature/tokenizer-2.9 (toke-tokenizer) | [x] vs cl100k_base comparison [x] Compression ratio, fertility, vocab utilization [x] JSON report [x] 32/32 tests pass |

### Epic 2.10 — Benchmark Harness and Reference Implementations

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.10.1 | Benchmark evaluation harness (run/score/report) | done | feature/benchmark-2.10-harness (toke-benchmark) | [x] Task discovery [x] Scoring/timeout [x] JSON reports [x] Dry-run [x] 32/32 tests pass |
| 2.10.2 | Phase A Python reference implementations (50+) | done | feature/benchmark-2.10-baselines (toke-benchmark) | [x] 60 tasks [x] run_baselines.py runner [x] solutions.py |
| 2.10.3 | Phase A C reference implementations (50+) | done | feature/benchmark-2.10-harness (toke-benchmark) | [x] 60 tasks in C11 [x] Makefile [x] Smoke tests pass |
| 2.10.4 | Benchmark CI workflow | done | feature/benchmark-2.10-harness (toke-benchmark) | [x] Unit tests [x] Python dry-run [x] C build+smoke [x] Manual dispatch |

### Epic 2.11 — Corpus Pipeline Test Infrastructure

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.11.1 | Corpus pipeline unit tests | done | feature/corpus-2.11-tests (toke-corpus) | [x] 93 tests across 5 files [x] curriculum, validate, schema, compile, vote |
| 2.11.2 | Corpus pipeline dry-run integration test | done | feature/corpus-2.11-tests (toke-corpus) | [x] 38 tests [x] Full pipeline mock [x] JSONL format validation [x] Error handling |

### Epic 2.12 — Specification Completion

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.12.1 | Error code registry (spec/errors.md) | done | feature/spec-2.12-errors (toke-spec) | [x] 34 error/warning codes [x] Stage, severity, message, fix field [x] Conformance test cross-refs |
| 2.12.2 | Formal semantics stub (spec/semantics.md) | done | feature/spec-2.12-semantics (toke-spec) | [x] 13 type kinds [x] Inference rules [x] Scoping [x] Error propagation [x] 556 lines |
| 2.12.3 | Standard library signatures (spec/stdlib-signatures.md) | done | feature/spec-2.12-stdlib (toke-spec) | [x] 11 modules [x] 61 functions [x] All error types documented |

### Epic 2.13 — Standard Library Documentation

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.13.1 | Complete stdlib module documentation | done | feature/stdlib-2.13-docs (toke-stdlib) | [x] 12 modules documented [x] 955 lines [x] Signatures, params, returns, examples |

### Epic 2.1 — Phase 2 Language Extensions

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.1.1 | Generic collection types (Map) | done | feature/lang-2.1-async (tkc) | [x] `[K:V]` map type [x] `[k:v; k2:v2]` map literal [x] NODE_MAP_TYPE/LIT/ENTRY [x] TY_MAP [x] `.len` on arrays/maps [x] E4040-E4043 [x] G032-G035 D015 |
| 2.1.2 | Async task model (spawn/await) | done | feature/lang-2.1-async (tkc) | [x] spawn/await/Task predefined identifiers [x] TY_TASK [x] E4050-E4052 [x] pthread runtime stubs [x] G036-G038 D016 |
| 2.1.3 | Minimal C FFI | done | feature/lang-2.1-ffi (tkc) | [x] Bodyless extern functions [x] `*T` pointer types [x] NODE_PTR_TYPE [x] TY_PTR [x] `declare` vs `define` in LLVM [x] E2010 E4060 [x] G026-G029 D013 |
| 2.1.4 | Module versioning | done | feature/lang-2.1-versioning (tkc) | [x] Optional `"ver"` string in imports [x] Semver validation [x] Version conflict detection [x] E2035-E2037 [x] G030-G031 D014 |

### Epic 4.6 — Security Audit and Hosted Service Readiness

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 4.6.1 | Third-party security audit of the compiler | done | feature/audit-4.6 (tkc) | [x] audit-scope.md (435 lines) [x] 4 scope areas [x] codebase map [x] TD items cross-referenced [x] accepted-risk register |
| 4.6.2 | SOC 2 readiness assessment (conditional on hosted service) | done | 2026-04-03 | [x] Draft gap analysis against TSC criteria [x] 8 gaps identified (all deferred to hosted service) [x] docs/security/soc2-readiness.md [x] Strong security foundations confirmed |

### Epic 5.1 — Project Website (tokelang.dev)

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 5.1.1 | Site scaffold and landing page | done | feature/web-5.1 (toke-web) | [x] Astro Starlight [x] Landing page with hero, token comparison, CTAs [x] Responsive [x] Search built-in |
| 5.1.2 | About page and project philosophy | done | feature/web-5.1 (toke-web) | [x] Why toke [x] Design principles [x] All repos listed |
| 5.1.3 | API specification browser | done | feature/web-5.1 (toke-web) | [x] Type system ref [x] Grammar ref [x] Error codes [x] 11 stdlib modules |
| 5.1.4 | Getting Started guide | done | feature/web-5.1 (toke-web) | [x] Install [x] Hello World [x] Language tour [x] Project structure |
| 5.1.5 | Human training course | done | feature/web-5.1 (toke-web) | [x] 10 lessons + overview [x] Exercises [x] Full project in lesson 10 |
| 5.1.6 | Web-based translator and comparison tool | on_hold | — | ON HOLD per user request. Needs tokenizer pipeline + translation engine. |
| 5.1.7 | Community and contribution hub | done | feature/web-5.1 (toke-web) | [x] Contributing guide [x] Enterprise adoption page |
| 5.1.8 | Site CI/CD and deployment | done | feature/web-5.1 (toke-web) | [x] GitHub Actions [x] Build + deploy [x] Pagefind search [x] Sitemap |

### Epic 5.2 — Phase 2 Website Updates

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 5.2.1 | Update website to present toke as one language (56-char set) | done | — | [x] All 38+ docs converted to Phase 2 syntax [x] Phase 1 tabs removed from homepage [x] Sidebar renamed to "How toke Was Built" [x] Phase 2 ref pages reframed as methodology [x] "available in Phase 2" removed from 11 stdlib pages [x] Deployed to Lightsail 2026-04-01 |

### Epic 2.14 — Corpus Phase 2 Transformation

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.14.1 | Phase 1→Phase 2 corpus transformation script | done | — | [x] transform/phase1_to_phase2.py [x] 55 unit tests [x] Context-aware tokenizer (not regex) [x] All 8 transformation rules [x] 46,754 entries processed [x] corpus_p2.jsonl output |

### Epic 2.15 — Model Training Pipeline (MLX)

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.15.1 | MLX QLoRA fine-tuning script | done | — | [x] finetune/train_mlx.py [x] Qwen 2.5 Coder 7B [x] rank 64, alpha 128 [x] cosine schedule [x] configs/7b_mlx.yaml |
| 2.15.2 | MLX data preparation and validation | done | — | [x] finetune/prepare_mlx_data.py [x] Existing ChatML data MLX-compatible [x] Validation mode |
| 2.15.3 | MLX adapter merging | done | — | [x] finetune/merge_mlx.py [x] mlx_lm.fuse integration [x] de-quantize option |

### Epic 2.16 — Benchmark Toke Integration

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.16.1 | Toke solution loader and model inference mode in benchmark harness | done | — | [x] load_toke_solutions() with tkc compilation [x] load_model_solutions() for Pass@1 [x] --model-endpoint CLI [x] Incremental build |

### Epic 2.17 — Research Review Document

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.17.1 | Research review request document | done | — | [x] toke-spec/docs/research-review-request.md [x] 8 design questions for reviewers [x] Preliminary results (12.5% token reduction) [x] Placeholders for final tokenizer + Pass@1 results |
| 2.17.2 | Update research review with final results | done | 2026-04-03 | [x] All 6 PLACEHOLDER markers filled with Gate 1 results [x] Status updated to Post-Gate 1 PASS [x] Stdlib 11→14 [x] Benchmark 500→1000 [x] Appendix B+C filled [x] RFC updated with Gate 1 results [x] stdlib-signatures.md completed (14 modules) |

### Epic 6.1 — Publish to Hugging Face

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 6.1.1 | Create Hugging Face organisation and model card | backlog | — | **[local]** Depends on Gate 1 pass |
| 6.1.2 | Upload model weights and tokenizer | backlog | — | **[local]** Upload adapter + tokenizer. Depends on 6.1.1 |
| 6.1.3 | Publish benchmark results and evaluation dataset | backlog | — | **[local]** Depends on 6.1.2, Gate 1 pass |
| 6.1.4 | Inference API and demo space | backlog | — | **[cloud/HF]** HuggingFace Spaces. Depends on 6.1.2 |

### Epic 6.2 — Repository Scaffolding

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 6.2.1 | Create toke-eval repository | done | 2026-04-03 | [x] toke_eval package (pass_at_k, token_efficiency, report) [x] pyproject.toml [x] README with usage examples |
| 6.2.2 | Create toke-model repository scaffold | done | 2026-04-03 | [x] Merged docs/terminology-rename to main [x] README updated for Gate 1 [x] Empty eval stubs removed (moved to toke-eval) |

### Epic 6.3 — Serialization Format Support (TOON / YAML / JSON Alternates)

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 6.3.1 | Review research on JSON alternates for LLM pipelines | done | — | [x] Research reviewed [x] TOON as default, YAML/JSON secondary, extensible modules [x] Format selection recommendation documented in memory |
| 6.3.2 | Implement TOON serialization module (std.toon) | done | — | [x] toon.tki interface [x] toon.h/toon.c C impl [x] parse/emit/field extraction [x] JSON↔TOON conversion [x] codegen mapping [x] 17 tests pass |
| 6.3.3 | Extend std.json and add std.yaml modules | done | — | [x] yaml.tki interface [x] yaml.h/yaml.c C impl [x] flat mappings + sequences [x] JSON↔YAML conversion [x] codegen mapping [x] 26 tests pass |
| 6.3.4 | Extensible format module interface | done | — | [x] Common .tki pattern: enc/dec/str/i64/f64/bool/arr + from_json/to_json [x] All 3 modules (json/toon/yaml) follow same interface [x] codegen resolve_stdlib_call extensible per module |
| 6.3.5 | String externalisation and i18n placeholder support | done | — | [x] i18n.tki interface [x] i18n.h/i18n.c C impl [x] TOON/YAML/JSON bundle loading with locale fallback [x] {placeholder} substitution [x] codegen mapping [x] 15 tests pass |
| 6.3.6 | Document serialization strategy across website, spec, and repos | done | — | [x] Website stdlib pages: toon.md, yaml.md, i18n.md [x] Data Formats reference page with format hierarchy and i18n [x] ADR-0003 in toke-spec (serialization strategy + i18n) [x] tkc README stdlib table with 14 modules [x] Website builds clean (44 pages) |

### Epic 7.1 — Website Code Example Correctness

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 7.1.1 | Rewrite website examples to remove underscores from identifiers | on_hold | — | **[local]** 28+ files affected; parked until spec locked |
| 7.1.2 | Fix match arm return syntax in website examples | on_hold | — | **[local]** Parked until spec locked |
| 7.1.3 | Fix error variant construction and propagation syntax | on_hold | — | **[local]** Parked until spec locked |
| 7.1.4 | Fix typed empty collection literals in website examples | on_hold | — | **[local]** Parked until spec locked |
| 7.1.5 | Fix loop init and spawn/await syntax in website examples | on_hold | — | **[local]** Parked until spec locked |
| 7.1.6 | Implement array indexing `arr[i]` in compiler | done | — | [x] PtrLocal tracking [x] NODE_IDENT for ptr-typed locals [x] Function param/call codegen for array types [x] e2e_array_index test |
| 7.1.7 | Implement void return type in compiler | done | — | [x] Void return verification [x] e2e_void test |
| 7.1.8 | Fix struct literal with field access expression crash | done | — | [x] Struct type registry [x] prepass_structs [x] Multi-field GEP codegen [x] Field access codegen [x] e2e_struct_field tests |
| 7.1.9 | Website code example conformance test suite | on_hold | — | **[local]** Automated CI test; depends on 7.1.1–7.1.5; parked until spec locked |
| 7.2.1 | Review `.tk` file extension decision | done | — | [x] ADR-0002 in toke-spec [x] Linguist conflict documented [x] .gitattributes mitigation [x] Decision: keep .tk |
| 7.3.1 | Audit empty stub files across all repos | done | — | 12 empty files found, 3 stories with false done status, 1 missing from tracking |
| 8.1.1 | Provision cloud instance and deploy toolchain | done | — | Setup/deploy/verify scripts in infra/ |
| 8.1.2 | Multi-provider API client layer | done | — | 4 providers + 29 tests |
| 8.1.3 | Task curriculum generator | done | — | 339 templates, 50K tasks, 25 tests |
| 8.1.4 | Generation prompt templates | done | — | 7 prompts, syntax-verified against compiler |
| 8.1.5 | Model capability trial framework | done | — | Trial runner + scorer, 23 tests |
| 8.1.6 | Model pool manager and task router | done | — | Category routing, auto-rebalance, 18 tests |
| 8.1.7 | Validation pipeline | done | — | Compiler + diff test + quality + dedup, 36 tests |
| 8.1.8 | Correction loop and escalation engine | done | — | 3-attempt retries, tier escalation, 17 tests |
| 8.1.9 | Corpus writer and metrics dashboard | done | — | Schema output, checkpointing, metrics, 19 tests |
| 8.1.10 | Orchestrator main loop | done | — | Full pipeline entry point, 10 integration tests |

---

## Completed Stories

| ID | Story | Completed | Branch |
|----|-------|-----------|--------|
| 1.1.1 | Character set finalisation | 2026-03-27 | feature/spec-character-set (toke-spec) |
| 1.1.2 | Keyword table lock | 2026-03-27 | feature/spec-keyword-table (toke-spec) |
| 1.1.3 | Symbol disambiguation rules | 2026-03-27 | feature/spec-symbol-disambiguation (toke-spec) |
| 1.1.4 | Formal EBNF grammar | 2026-03-27 | feature/spec-ebnf-grammar (toke-spec) |
| 1.1.5 | Profile 2 transformation rules | 2026-03-28 | feature/spec-phase2-transform (toke-spec) |
| 1.1.6 | Spec review and alignment | 2026-03-27 | feature/spec-review-m0 (toke-spec) |
| 1.2.1 | Lexer implementation | 2026-03-28 | feature/compiler-lexer (tkc) |
| 1.2.2 | Parser implementation | 2026-03-28 | feature/compiler-parser (tkc) |
| 1.2.3 | Import resolver | 2026-03-28 | feature/compiler-import-resolver (tkc) |
| 1.2.4 | Name resolver | 2026-03-28 | feature/compiler-name-resolver (tkc) |
| 1.2.5 | Type checker | 2026-03-28 | feature/compiler-type-checker (tkc) |
| 1.2.6 | Structured diagnostic emitter | 2026-03-28 | feature/compiler-diag (tkc) |
| 1.2.7 | Interface file emitter | 2026-03-28 | feature/compiler-interface-emitter (tkc) |
| 1.2.8 | LLVM IR backend | 2026-03-28 | feature/compiler-llvm-backend (tkc) |
| 1.2.9 | CLI interface | 2026-03-28 | feature/compiler-cli (tkc) |
| 1.2.10 | Conformance test suite (Profile 1) | 2026-03-28 | test/compiler-conformance-suite (tkc) |
| 1.6.1 | Held-out benchmark task set | 2026-03-27 | feature/benchmark-held-out-tasks (toke-benchmark) |
| 1.10.1 | Phase 1 integration and conformance review | 2026-03-28 | docs/phase1-review (tkc) |
| 2.7.1 | std.process | 2026-03-28 | feature/stdlib-2.7-process-env (tkc + toke-stdlib) |
| 2.7.2 | std.env | 2026-03-28 | feature/stdlib-2.7-process-env (tkc + toke-stdlib) |
| 2.7.3 | std.crypto | 2026-03-28 | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) |
| 2.7.4 | std.time | 2026-03-28 | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) |
| 1.1.7 | Meta-repo README and project landing page | 2026-03-28 | feature/meta-readme (toke) |
| 2.7.5 | std.test | 2026-03-28 | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) |
| 3.7.1 | SBOM generation for compiler releases | 2026-03-28 | feature/supply-chain-3.7 (tkc) |
| 3.7.2 | Release binary signing | 2026-03-28 | feature/supply-chain-3.7 (tkc) |
| 3.7.4 | Model release safety evaluation | 2026-03-28 | feature/supply-chain-3.7 (toke-model) |
| 3.8.1 | std.log structured logging | 2026-03-28 | feature/stdlib-3.8-log (tkc + toke-stdlib) |
| 3.8.2 | stdlib performance benchmarks | 2026-03-28 | feature/stdlib-3.8-bench (tkc) |
| 3.8.3 | stdlib conformance test coverage | 2026-03-28 | feature/stdlib-3.8-bench (tkc) |
| 4.6.1 | Third-party security audit readiness | 2026-03-28 | feature/audit-4.6 (tkc) |
| 2.1.1 | Generic collection types (Map) | 2026-03-29 | feature/lang-2.1-async (tkc) |
| 2.1.2 | Async task model (spawn/await) | 2026-03-29 | feature/lang-2.1-async (tkc) |
| 2.1.3 | Minimal C FFI | 2026-03-29 | feature/lang-2.1-ffi (tkc) |
| 2.1.4 | Module versioning | 2026-03-29 | feature/lang-2.1-versioning (tkc) |
| 2.8.1 | Fix LLVM IR emission for end-to-end compilation | 2026-03-29 | feature/codegen-2.8 (tkc) |
| 2.9.1 | Corpus preparation script (prepare.py) | 2026-03-29 | feature/tokenizer-2.9 (toke-tokenizer) |
| 2.10.2 | Phase A Python reference implementations | 2026-03-29 | feature/benchmark-2.10-baselines (toke-benchmark) |
| 2.11.1 | Corpus pipeline unit tests | 2026-03-29 | feature/corpus-2.11-tests (toke-corpus) |
| 2.12.1 | Error code registry | 2026-03-29 | feature/spec-2.12-errors (toke-spec) |
| 2.12.3 | Standard library signatures | 2026-03-29 | feature/spec-2.12-stdlib (toke-spec) |
| 2.13.1 | Complete stdlib module documentation | 2026-03-29 | feature/stdlib-2.13-docs (toke-stdlib) |
| 2.9.2 | BPE training wrapper (train.py) | 2026-03-29 | feature/tokenizer-2.9 (toke-tokenizer) |
| 2.9.3 | Tokenizer evaluation script (eval.py) | 2026-03-29 | feature/tokenizer-2.9 (toke-tokenizer) |
| 2.10.1 | Benchmark evaluation harness | 2026-03-29 | feature/benchmark-2.10-harness (toke-benchmark) |
| 2.10.3 | Phase A C reference implementations | 2026-03-29 | feature/benchmark-2.10-harness (toke-benchmark) |
| 2.10.4 | Benchmark CI workflow | 2026-03-29 | feature/benchmark-2.10-harness (toke-benchmark) |
| 2.11.2 | Corpus pipeline dry-run integration test | 2026-03-29 | feature/corpus-2.11-tests (toke-corpus) |
| 2.12.2 | Formal semantics stub | 2026-03-29 | feature/spec-2.12-semantics (toke-spec) |
| 5.1.1 | Site scaffold and landing page | 2026-03-29 | feature/web-5.1 (toke-web) |
| 5.1.2 | About page and project philosophy | 2026-03-29 | feature/web-5.1 (toke-web) |
| 5.1.3 | API specification browser | 2026-03-29 | feature/web-5.1 (toke-web) |
| 5.1.4 | Getting Started guide | 2026-03-29 | feature/web-5.1 (toke-web) |
| 5.1.5 | Human training course | 2026-03-29 | feature/web-5.1 (toke-web) |
| 5.1.7 | Community and contribution hub | 2026-03-29 | feature/web-5.1 (toke-web) |
| 5.1.8 | Site CI/CD and deployment | 2026-03-29 | feature/web-5.1 (toke-web) |
| 7.3.1 | Audit empty stub files across all repos | 2026-03-30 | — |
| 8.1.1 | Provision cloud instance and deploy toolchain | 2026-03-30 | — |
| 8.1.2 | Multi-provider API client layer | 2026-03-30 | — |
| 8.1.3 | Task curriculum generator | 2026-03-30 | — |
| 8.1.4 | Generation prompt templates | 2026-03-30 | — |
| 8.1.5 | Model capability trial framework | 2026-03-30 | — |
| 8.1.6 | Model pool manager and task router | 2026-03-30 | — |
| 8.1.7 | Validation pipeline | 2026-03-30 | — |
| 8.1.8 | Correction loop and escalation engine | 2026-03-30 | — |
| 8.1.9 | Corpus writer and metrics dashboard | 2026-03-30 | — |
| 8.1.10 | Orchestrator main loop | 2026-03-30 | — |
| 1.5.1 | Task curriculum generator | 2026-03-31 | — |
| 1.5.2 | Four-language parallel generation | 2026-03-31 | — |
| 1.5.3 | Differential test harness | 2026-03-31 | — |
| 1.5.4 | Corpus quality metrics | 2026-04-01 | — |
| 1.6.2 | Token efficiency measurement | 2026-04-01 | — |
| 5.2.1 | Update website for Phase 2 | 2026-04-01 | — (toke-web) |
| 2.14.1 | Phase 1→Phase 2 corpus transformation script | 2026-04-01 | — (toke-corpus) |
| 2.15.1 | MLX QLoRA fine-tuning script | 2026-04-01 | — (toke-models) |
| 2.15.2 | MLX data preparation and validation | 2026-04-01 | — (toke-models) |
| 2.15.3 | MLX adapter merging | 2026-04-01 | — (toke-models) |
| 2.16.1 | Toke solution loader and model inference | 2026-04-01 | — (toke-benchmark) |
| 2.17.1 | Research review request document | 2026-04-01 | — (toke-spec) |
| 7.1.6 | Implement array indexing in compiler | 2026-04-01 | — (tkc) |
| 7.1.7 | Implement void return type in compiler | 2026-04-01 | — (tkc) |
| 7.1.8 | Fix struct literal crash | 2026-04-01 | — (tkc) |
| 7.2.1 | Review .tk file extension decision | 2026-04-01 | — (toke-spec) |
| 1.6.3 | Pass@1 measurement | 2026-04-03 | — (toke-benchmark) |
| 1.6.4 | Gate 1 decision document | 2026-04-03 | — (toke-spec) |
| 2.8.1 | LLVM IR backend codegen fixes | 2026-04-03 | — (tkc) |
| 2.8.2 | Runtime library extensions | 2026-04-03 | — (tkc) |
| 6.3.1 | Research review: JSON alternates | 2026-04-03 | — |
| 6.3.2 | Implement std.toon module | 2026-04-03 | — (tkc) |
| 6.3.3 | Add std.yaml module | 2026-04-03 | — (tkc) |
| 6.3.4 | Extensible format module interface | 2026-04-03 | — (tkc) |
| 6.3.5 | String externalisation and i18n | 2026-04-03 | — (tkc) |
| 6.3.6 | Document serialization strategy | 2026-04-03 | — (toke-spec, toke-web, tkc) |
| 2.17.2 | Update research review with final results | 2026-04-03 | — (toke-spec) |
| 6.2.1 | Create toke-eval repository | 2026-04-03 | — (toke-eval) |
| 6.2.2 | Finalise toke-models repo scaffold | 2026-04-03 | — (toke-models) |
| 3.7.3 | Reproducible builds for the compiler | 2026-04-03 | — (tkc) |
| 4.6.2 | SOC 2 readiness assessment (draft) | 2026-04-03 | — (tkc) |

---

## Active Blockers

None. Gate 1 passed 2026-04-03.

## Compute Summary

All Phase 2 work runs locally (Mac Studio M4 Max). No cloud instances needed.

| Compute | Stories |
|---------|---------|
| **local** | 6.1.1-6.1.3, 7.1.1-7.1.5 (on_hold), 7.1.9 (on_hold) |
| **cloud/HF** | 6.1.4 (HuggingFace Spaces demo) |
| **no compute** | All EC2 instances can be suspended |
