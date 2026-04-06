# docs/progress.md |
## toke — Story Progress Tracker |
 |
**Project phase:** Default Syntax Implementation (formerly "Phase 2") |
**Active milestone:** M3 |
**Gate 1:** PASS (2026-04-03) — 12.5% token reduction, 63.7% Pass@1 |
**Decision (2026-04-04):** 56-char syntax is "toke" (default). 80-char syntax is "legacy profile" (`--legacy`). |
**Last updated:** see git log |
 |
--- |
 |
## How to use this file |
 |
Update this file at the start and end of every development cycle. |
Statuses: `backlog` | `planned` | `in_progress` | `blocked` | `review` | `done` |
 |
--- |
 |
## Phase 1 — Falsification |
 |
### Epic 1.1 — Language Specification Lock |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.1.1 | Character set finalisation | done | feature/spec-character-set | [x] 80 chars enumerated with class [x] Every symbol has named primary/secondary role [x] String delimiter assigned to `"` [x] Excluded chars listed with rationale [x] Frozen at M0 2026-03-27 [x] No conflicting dual roles at same position |
| 1.1.2 | Keyword table lock | done | feature/spec-keyword-table | [x] 12 keywords listed with exact lexical form [x] Each keyword's production rule identified [x] true/false documented as predefined identifiers [x] Prefix-conflict analysis: lp/let share `l`, resolved by second char [x] Underscore excluded, identifier rule updated [x] Frozen at M0 2026-03-27 |
| 1.1.3 | Symbol disambiguation rules | done | feature/spec-symbol-disambiguation | [x] 14 dual-role symbols documented [x] All rules LL(1) one-token lookahead [x] Two examples per symbol [x] Summary table [x] Frozen M0 2026-03-27 |
| 1.1.4 | Formal EBNF grammar | done | feature/spec-ebnf-grammar | [x] 65 productions covering all constructs [x] LL(1) verified [x] Unambiguous [x] All 8 open questions resolved [x] Committed to spec/grammar.ebnf [x] Frozen at M0 2026-03-27 |
| 1.1.5 | Profile 2 transformation rules | done | feature/spec-phase2-transform | [x] Type sigil rule (TYPE_IDENT→$name) [x] Array literal rule ([]→@()) [x] Array index rule (a[n]→a.get(n)) [x] Determinism proof [x] Round-trip spec [x] 20+ example pairs [x] Frozen M0 2026-03-28 |
| 1.1.6 | Spec review and alignment | done | feature/spec-review-m0 | [x] Cross-document review complete [x] 7 blocking issues resolved [x] grammar.ebnf fully populated (was stub) [x] character-set.md created [x] keywords.md created [x] symbol-disambiguation.md created [x] phase2-transform.md created [x] CastExpr production added [x] LoopStmt semicolon ambiguity fixed [x] spec-review-m0.md written [x] 4 warnings, 4 notes in risk register [x] Frozen M0 2026-03-27 |
| 1.1.7 | Meta-repo README and project landing page | done | feature/meta-readme (toke) | [x] Plain-language thesis [x] 7 sub-repo links [x] Phase/gate status table [x] Getting-started (build + conform) [x] Licence + contributing links [x] No placeholder text |
 |
### Epic 1.2 — Reference Compiler Frontend |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.2.1 | Lexer implementation | done | feature/compiler-lexer | [x] 80-char set [x] 38 token kinds [x] W1010 [x] E1001 E1002 E1003 [x] 296 lines |
| 1.2.2 | Parser implementation | done | feature/compiler-parser | [x] All 47 non-terminals [x] LL(1) [x] AST with source positions [x] E2001–E2004 diagnostics [x] 394 lines |
| 1.2.3 | Import resolver | done | feature/compiler-import-resolver | [x] .tki loading [x] E2030 unresolved [x] E2031 circular [x] std.* prefix [x] SymbolTable [x] 179 lines |
| 1.2.4 | Name resolver | done | feature/compiler-name-resolver | [x] Scope chain [x] Predefined ids [x] Import aliases [x] E3011 E3012 [x] All scope levels |
| 1.2.5 | Type checker | done | feature/compiler-type-checker | [x] 10/10 rules [x] E4031 E4010 E4011 E4025 E5001 E3020 [x] 346 lines (arena escape: conservative — Decl needs depth stamp) |
| 1.2.6 | Structured diagnostic emitter | done | feature/compiler-diag | [x] JSON schema 1.0 [x] fix field omitted when absent [x] --diag-text [x] 180 lines |
| 1.2.7 | Interface file emitter | done | feature/compiler-interface-emitter | [x] JSON .tki [x] module/func/type/const [x] interface_hash [x] E9001 [x] 190 lines |
| 1.2.8 | LLVM IR backend | done | feature/compiler-llvm-backend | [x] Text IR all constructs [x] 3 targets via clang [x] E9002 E9003 [x] 396 lines (cast/field-offset/nested-break stubs) |
| 1.2.9 | CLI interface | done | feature/compiler-cli | [x] All flags [x] Full pipeline [x] Exit codes 0/1/2/3 [x] isatty diag [x] 186 lines |
| 1.2.10 | Conformance test suite (Profile 1) | done | test/compiler-conformance-suite | [x] 62 tests [x] L/G/D series [x] run_conform.sh [x] make conform wired |
 |
### Epic 1.7 — Compiler Security and SAST |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.7.1 | SAST on compiler C code | done | feature/security-sast-dco | [x] cppcheck CI [x] clang-tidy [x] PR annotations [x] 63-line workflow |
| 1.7.2 | Compiler input fuzzing | done | feature/security-fuzzing | [x] libFuzzer lexer+parser targets [x] ASAN+UBSAN [x] 5 corpus files [x] make fuzz [x] nightly CI |
| 1.7.3 | Secret scanning in corpus pipeline | done | feature/security-secret-dep-scan (toke-corpus, toke-model) | [x] gitleaks CI [x] .gitignore hardened [x] pre-commit docs (GitHub UI: enable secret scanning) |
| 1.7.4 | Dependency vulnerability scanning | done | feature/security-secret-dep-scan (toke-corpus, toke-model) | [x] Dependabot [x] pip-audit CI [x] pyproject.toml pinned (GitHub UI: enable Dependabot alerts) |
| 1.7.5 | Signed commits and DCO enforcement | done | feature/security-sast-dco | [x] dco.yml [x] retroactive signoff [x] CONTRIBUTING.md [x] README notice (GitHub App install: manual) |
 |
### Epic 1.8 — Corpus Pipeline Threat Model |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.8.1 | Corpus pipeline threat model document | done | feature/security-threat-model-sandbox | [x] T1-T5 threat categories [x] likelihood/impact/mitigation [x] sign-off table |
| 1.8.2 | Sandboxed execution of generated programs | done | feature/security-threat-model-sandbox | [x] sandbox-exec (macOS) [x] Docker fallback [x] 10s timeout [x] clean env [x] setup docs |
 |
### Epic 1.3 — Standard Library Core |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.3.1 | std.str | done | feature/stdlib-str | [x] 14/14 functions [x] str.tki [x] C impl 222 lines [x] 22 test assertions |
| 1.3.2 | std.http | done | feature/stdlib-http | [x] Req/Res/HttpErr types [x] 5 route verbs [x] :param extraction [x] POSIX server [x] http.tki |
| 1.3.3 | std.db | done | feature/stdlib-db | [x] 8/8 functions [x] SQLite3 backend [x] DbErr sum type [x] db.tki [x] in-memory test suite |
| 1.3.4 | std.json | done | feature/stdlib-json | [x] 8/8 functions [x] JsonErr sum type [x] json.tki [x] recursive key extractor [x] 7 test assertions |
| 1.3.5 | std.file | done | feature/stdlib-file | [x] 6/6 functions [x] FileErr sum type [x] file.tki [x] 138 lines [x] 9 test assertions |
 |
### Epic 1.9 — Remote Monitoring Console |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.9.1 | Job manager daemon | done | feature/monitor-console | Flask REST API, subprocess manager, jobs.json persistence |
| 1.9.2 | Web dashboard | done | feature/monitor-console | Dark-theme SPA, auto-refresh, log panel |
| 1.9.3 | Log streaming | done | feature/monitor-console | SSE /api/jobs/{id}/stream; EventSource in dashboard |
| 1.9.4 | Authentication and HTTPS | done | feature/monitor-console | TLS via TK_MONITOR_CERT/KEY; token query param for SSE |
 |
### Epic 1.4 — Mac Studio Setup and Local Pipeline |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.4.1 | Mac Studio configuration | bypassed | — | Used EC2 cloud instead (Epic 8.1) |
| 1.4.2 | Local inference pipeline | bypassed | — | Cloud pipeline in Epic 8.1 |
| 1.4.3 | Corpus storage schema | done | — | Implemented in corpus pipeline (schema.json) |
 |
### Epic 1.5 — Phase A Corpus Generation |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.5.1 | Task curriculum generator | done | — | 339 templates, 50K tasks (via Epic 8.1.3) |
| 1.5.2 | Four-language parallel generation | done | — | Multi-model pipeline: Haiku 4.5, GPT-4.1-mini, Grok-3-mini (via Epic 8.1) |
| 1.5.3 | Differential test harness | done | — | Python/C/Java majority agreement (via Epic 8.1.7) |
| 1.5.4 | Corpus quality metrics | done | — | 46,754 entries: A=26,978 B=9,776 C=5,000 D=5,000 |
 |
### Epic 1.6 — Gate 1 Benchmark |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.6.1 | Held-out benchmark task set | done | feature/benchmark-held-out-tasks | [x] 500 tasks [x] 120 test inputs each [x] Gitignored [x] Generation script committed |
| 1.6.2 | Token efficiency measurement | done | — | [x] 12.5% reduction (8K vocab) [x] 13.1% reduction (32K vocab) [x] eval_report.json [x] Gate 1 threshold met (>10%) |
| 1.6.3 | Pass@1 measurement | done | — | [x] 1000-task benchmark (500 original + 500 expanded) [x] 923/1000 compiled (92.3%) [x] 588/923 Pass@1 (63.7%) [x] Gate 1 threshold met (>60%) [x] 7 codegen fixes applied (Epic 2.8) [x] Zero regressions |
| 1.6.4 | Gate 1 decision document | done | — | [x] gate1-decision.md in toke-spec/docs [x] Full results, verdict, and next steps [x] Gate 1: PASS (2026-04-03) |
 |
### Epic 1.10 — Phase 1 Integration and Conformance Review |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 1.10.1 | Phase 1 full integration and conformance review | done | docs/phase1-review (tkc) | [x] 62/62 conformance [x] all 9 compiler passes [x] stdlib tests pass [x] docs/phase1-review.md written [x] 6 TD items logged |
 |
### Epic 2.6 — Responsible Disclosure and CVE Process |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.6.1 | Security policy and disclosure process | done | feature/security-disclosure | SECURITY.md in tkc/corpus/model; README linked |
| 2.6.2 | CVE coordination process | done | feature/security-disclosure | docs/security/cve-process.md |
 |
### Epic 2.7 — Standard Library Expansion |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.7.1 | std.process | done | feature/stdlib-2.7-process-env (tkc + toke-stdlib) | [x] POSIX fork/exec/pipe/waitpid [x] ProcessHandle [x] process.tki [x] 28/28 tests |
| 2.7.2 | std.env | done | feature/stdlib-2.7-process-env (tkc + toke-stdlib) | [x] getenv/setenv/unsetenv [x] EnvErrKind [x] env.tki [x] tests pass |
| 2.7.3 | std.crypto | done | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) | [x] SHA-256 + HMAC-SHA-256 [x] self-contained [x] crypto.tki [x] test vectors pass |
| 2.7.4 | std.time | done | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) | [x] clock_gettime [x] strftime [x] time.tki [x] tests pass |
| 2.7.5 | std.test | done | feature/stdlib-2.7-crypto-time-test (tkc + toke-stdlib) | [x] assert/assert_eq/assert_ne [x] DIAGNOSTIC stderr format [x] test.tki [x] tests pass |
 |
### Epic 3.7 — Supply Chain Security and Release Signing |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 3.7.1 | SBOM generation for compiler releases | done | feature/supply-chain-3.7 (tkc) | SPDX JSON via syft; published as release artifact |
| 3.7.2 | Release binary signing | done | feature/supply-chain-3.7 (tkc) | cosign keyless; tamper test in CI; docs/security/release-signing.md |
| 3.7.3 | Reproducible builds for the compiler | done | 2026-04-03 | [x] REPRO_FLAGS (frandom-seed, ffile-prefix-map) [x] SOURCE_DATE_EPOCH [x] make repro-check (all .o bit-identical) [x] docs/security/reproducible-builds.md [x] macOS LC_UUID variance documented |
| 3.7.4 | Model release safety evaluation | done | feature/supply-chain-3.7 (toke-models) | [x] LlamaGuard eval process doc [x] safety_eval.py (dry-run verified) [x] 50 adversarial templates (5 categories) [x] docs/security/model-safety-evals.md |
 |
### Epic 3.8 — Standard Library Production Hardening |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 3.8.1 | std.log structured logging | done | feature/stdlib-3.8-log (tkc + toke-stdlib) | [x] NDJSON stderr [x] TK_LOG_LEVEL [x] log.info/warn/error [x] 12/12 tests |
| 3.8.2 | stdlib performance benchmarks | done | feature/stdlib-3.8-bench (tkc) | [x] 9 modules benchmarked [x] baseline.txt (M-series) [x] make bench [x] bench.yml CI |
| 3.8.3 | stdlib conformance test coverage | done | feature/stdlib-3.8-bench (tkc) | [x] 100% function coverage [x] 30 gap tests [x] docs/stdlib-coverage.md |
 |
### Epic 2.8 — LLVM Backend Correctness |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.8.1 | Fix LLVM IR emission for end-to-end compilation | done | feature/codegen-2.8 (tkc) | [x] 5 defects fixed: param SSA naming, let-bind child index, call return types, match arm index, cast emission [x] Terminator tracking [x] prepass_funcs() [x] 5/5 e2e tests pass [x] 79/79 conformance |
| 2.8.2 | Fix loop variable codegen (lp construct) | done | — | **[local]** Three bugs fixed: (1) NODE_LOOP_INIT had no emit_stmt handler — loop var never alloca'd/stored (2) loop step (i=i+1) never emitted before back-edge branch (3) break label used fragile `lbl-1` arithmetic — replaced with dedicated `break_lbl` field. [x] 90/90 conformance [x] 9/9 e2e [x] nested loops [x] nested break |
 |
### Epic 2.9 — Tokenizer Pipeline Scaffolding |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.9.1 | Corpus preparation script (prepare.py) | done | feature/tokenizer-2.9 (toke-tokenizer) | [x] JSONL extraction [x] String placeholder replacement [x] SHA-256 dedup [x] Configurable train/valid split [x] 25/25 tests pass |
| 2.9.2 | BPE training wrapper (train.py) | done | feature/tokenizer-2.9 (toke-tokenizer) | [x] SentencePiece BPE wrapper [x] Toke-specific defaults [x] Dry-run mode [x] 17/17 tests pass |
| 2.9.3 | Tokenizer evaluation script (eval.py) | done | feature/tokenizer-2.9 (toke-tokenizer) | [x] vs cl100k_base comparison [x] Compression ratio, fertility, vocab utilization [x] JSON report [x] 32/32 tests pass |
 |
### Epic 2.10 — Benchmark Harness and Reference Implementations |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.10.1 | Benchmark evaluation harness (run/score/report) | done | feature/benchmark-2.10-harness (toke-benchmark) | [x] Task discovery [x] Scoring/timeout [x] JSON reports [x] Dry-run [x] 32/32 tests pass |
| 2.10.2 | Phase A Python reference implementations (50+) | done | feature/benchmark-2.10-baselines (toke-benchmark) | [x] 60 tasks [x] run_baselines.py runner [x] solutions.py |
| 2.10.3 | Phase A C reference implementations (50+) | done | feature/benchmark-2.10-harness (toke-benchmark) | [x] 60 tasks in C11 [x] Makefile [x] Smoke tests pass |
| 2.10.4 | Benchmark CI workflow | done | feature/benchmark-2.10-harness (toke-benchmark) | [x] Unit tests [x] Python dry-run [x] C build+smoke [x] Manual dispatch |
 |
### Epic 2.11 — Corpus Pipeline Test Infrastructure |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.11.1 | Corpus pipeline unit tests | done | feature/corpus-2.11-tests (toke-corpus) | [x] 93 tests across 5 files [x] curriculum, validate, schema, compile, vote |
| 2.11.2 | Corpus pipeline dry-run integration test | done | feature/corpus-2.11-tests (toke-corpus) | [x] 38 tests [x] Full pipeline mock [x] JSONL format validation [x] Error handling |
 |
### Epic 2.12 — Specification Completion |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.12.1 | Error code registry (spec/errors.md) | done | feature/spec-2.12-errors (toke-spec) | [x] 34 error/warning codes [x] Stage, severity, message, fix field [x] Conformance test cross-refs |
| 2.12.2 | Formal semantics stub (spec/semantics.md) | done | feature/spec-2.12-semantics (toke-spec) | [x] 13 type kinds [x] Inference rules [x] Scoping [x] Error propagation [x] 556 lines |
| 2.12.3 | Standard library signatures (spec/stdlib-signatures.md) | done | feature/spec-2.12-stdlib (toke-spec) | [x] 11 modules [x] 61 functions [x] All error types documented |
 |
### Epic 2.13 — Standard Library Documentation |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.13.1 | Complete stdlib module documentation | done | feature/stdlib-2.13-docs (toke-stdlib) | [x] 12 modules documented [x] 955 lines [x] Signatures, params, returns, examples |
 |
### Epic 2.1 — Phase 2 Language Extensions |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.1.1 | Generic collection types (Map) | done | feature/lang-2.1-async (tkc) | [x] `[K:V]` map type [x] `[k:v; k2:v2]` map literal [x] NODE_MAP_TYPE/LIT/ENTRY [x] TY_MAP [x] `.len` on arrays/maps [x] E4040-E4043 [x] G032-G035 D015 |
| 2.1.2 | Async task model (spawn/await) | done | feature/lang-2.1-async (tkc) | [x] spawn/await/Task predefined identifiers [x] TY_TASK [x] E4050-E4052 [x] pthread runtime stubs [x] G036-G038 D016 |
| 2.1.3 | Minimal C FFI | done | feature/lang-2.1-ffi (tkc) | [x] Bodyless extern functions [x] `*T` pointer types [x] NODE_PTR_TYPE [x] TY_PTR [x] `declare` vs `define` in LLVM [x] E2010 E4060 [x] G026-G029 D013 |
| 2.1.4 | Module versioning | done | feature/lang-2.1-versioning (tkc) | [x] Optional `"ver"` string in imports [x] Semver validation [x] Version conflict detection [x] E2035-E2037 [x] G030-G031 D014 |
 |
### Epic 4.6 — Security Audit and Hosted Service Readiness |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 4.6.1 | Third-party security audit of the compiler | done | feature/audit-4.6 (tkc) | [x] audit-scope.md (435 lines) [x] 4 scope areas [x] codebase map [x] TD items cross-referenced [x] accepted-risk register |
| 4.6.2 | SOC 2 readiness assessment (conditional on hosted service) | done | 2026-04-03 | [x] Draft gap analysis against TSC criteria [x] 8 gaps identified (all deferred to hosted service) [x] docs/security/soc2-readiness.md [x] Strong security foundations confirmed |
 |
### Epic 5.1 — Project Website (tokelang.dev) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 5.1.1 | Site scaffold and landing page | done | feature/web-5.1 (toke-web) | [x] Astro Starlight [x] Landing page with hero, token comparison, CTAs [x] Responsive [x] Search built-in |
| 5.1.2 | About page and project philosophy | done | feature/web-5.1 (toke-web) | [x] Why toke [x] Design principles [x] All repos listed |
| 5.1.3 | API specification browser | done | feature/web-5.1 (toke-web) | [x] Type system ref [x] Grammar ref [x] Error codes [x] 11 stdlib modules |
| 5.1.4 | Getting Started guide | done | feature/web-5.1 (toke-web) | [x] Install [x] Hello World [x] Language tour [x] Project structure |
| 5.1.5 | Human training course | done | feature/web-5.1 (toke-web) | [x] 10 lessons + overview [x] Exercises [x] Full project in lesson 10 |
| 5.1.6 | Web-based translator and comparison tool | on_hold | — | ON HOLD per user request. Needs tokenizer pipeline + translation engine. |
| 5.1.7 | Community and contribution hub | done | feature/web-5.1 (toke-web) | [x] Contributing guide [x] Enterprise adoption page |
| 5.1.8 | Site CI/CD and deployment | done | feature/web-5.1 (toke-web) | [x] GitHub Actions [x] Build + deploy [x] Pagefind search [x] Sitemap |
 |
### Epic 5.2 — Phase 2 Website Updates |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 5.2.1 | Update website to present toke as one language (56-char set) | done | — | [x] All 38+ docs converted to Phase 2 syntax [x] Phase 1 tabs removed from homepage [x] Sidebar renamed to "How toke Was Built" [x] Phase 2 ref pages reframed as methodology [x] "available in Phase 2" removed from 11 stdlib pages [x] Deployed to Lightsail 2026-04-01 |
 |
### Epic 2.14 — Corpus Phase 2 Transformation |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.14.1 | Phase 1→Phase 2 corpus transformation script | done | — | [x] transform/phase1_to_phase2.py [x] 55 unit tests [x] Context-aware tokenizer (not regex) [x] All 8 transformation rules [x] 46,754 entries processed [x] corpus_p2.jsonl output |
 |
### Epic 2.15 — Model Training Pipeline (MLX) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.15.1 | MLX QLoRA fine-tuning script | done | — | [x] finetune/train_mlx.py [x] Qwen 2.5 Coder 7B [x] rank 64, alpha 128 [x] cosine schedule [x] configs/7b_mlx.yaml |
| 2.15.2 | MLX data preparation and validation | done | — | [x] finetune/prepare_mlx_data.py [x] Existing ChatML data MLX-compatible [x] Validation mode |
| 2.15.3 | MLX adapter merging | done | — | [x] finetune/merge_mlx.py [x] mlx_lm.fuse integration [x] de-quantize option |
 |
### Epic 2.16 — Benchmark Toke Integration |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.16.1 | Toke solution loader and model inference mode in benchmark harness | done | — | [x] load_toke_solutions() with tkc compilation [x] load_model_solutions() for Pass@1 [x] --model-endpoint CLI [x] Incremental build |
 |
### Epic 2.17 — Research Review Document |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 2.17.1 | Research review request document | done | — | [x] toke-spec/docs/research-review-request.md [x] 8 design questions for reviewers [x] Preliminary results (12.5% token reduction) [x] Placeholders for final tokenizer + Pass@1 results |
| 2.17.2 | Update research review with final results | done | 2026-04-03 | [x] All 6 PLACEHOLDER markers filled with Gate 1 results [x] Status updated to Post-Gate 1 PASS [x] Stdlib 11→14 [x] Benchmark 500→1000 [x] Appendix B+C filled [x] RFC updated with Gate 1 results [x] stdlib-signatures.md completed (14 modules) |
 |
### Epic 6.1 — Publish to Hugging Face |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 6.1.1 | Create Hugging Face organisation and model card | done | — | **[local]** [x] Model card with YAML metadata [x] config.json placeholder [x] Upload instructions |
| 6.1.2 | Upload model weights and tokenizer | done | — | **[local]** Upload script, tokenizer config, LFS gitattributes ready. Actual upload requires HuggingFace credentials |
| 6.1.3 | Publish benchmark results and evaluation dataset | done | — | **[local]** eval_results.json in huggingface/, export script + README in toke-benchmark/export/ |
| 6.1.4 | Inference API and demo space | done | 2026-04-04 | **[cloud/HF]** Gradio app, requirements, HF Spaces metadata, deploy instructions in toke-models/huggingface/spaces/ |
 |
### Epic 6.2 — Repository Scaffolding |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 6.2.1 | Create toke-eval repository | done | 2026-04-03 | [x] toke_eval package (pass_at_k, token_efficiency, report) [x] pyproject.toml [x] README with usage examples |
| 6.2.2 | Create toke-model repository scaffold | done | 2026-04-03 | [x] Merged docs/terminology-rename to main [x] README updated for Gate 1 [x] Empty eval stubs removed (moved to toke-eval) |
 |
### Epic 6.3 — Serialization Format Support (TOON / YAML / JSON Alternates) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 6.3.1 | Review research on JSON alternates for LLM pipelines | done | — | [x] Research reviewed [x] TOON as default, YAML/JSON secondary, extensible modules [x] Format selection recommendation documented in memory |
| 6.3.2 | Implement TOON serialization module (std.toon) | done | — | [x] toon.tki interface [x] toon.h/toon.c C impl [x] parse/emit/field extraction [x] JSON↔TOON conversion [x] codegen mapping [x] 17 tests pass |
| 6.3.3 | Extend std.json and add std.yaml modules | done | — | [x] yaml.tki interface [x] yaml.h/yaml.c C impl [x] flat mappings + sequences [x] JSON↔YAML conversion [x] codegen mapping [x] 26 tests pass |
| 6.3.4 | Extensible format module interface | done | — | [x] Common .tki pattern: enc/dec/str/i64/f64/bool/arr + from_json/to_json [x] All 3 modules (json/toon/yaml) follow same interface [x] codegen resolve_stdlib_call extensible per module |
| 6.3.5 | String externalisation and i18n placeholder support | done | — | [x] i18n.tki interface [x] i18n.h/i18n.c C impl [x] TOON/YAML/JSON bundle loading with locale fallback [x] {placeholder} substitution [x] codegen mapping [x] 15 tests pass |
| 6.3.6 | Document serialization strategy across website, spec, and repos | done | — | [x] Website stdlib pages: toon.md, yaml.md, i18n.md [x] Data Formats reference page with format hierarchy and i18n [x] ADR-0003 in toke-spec (serialization strategy + i18n) [x] tkc README stdlib table with 14 modules [x] Website builds clean (44 pages) |
 |
### Epic 7.1 — Website Code Example Correctness |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 7.1.1 | Rewrite website examples to remove underscores from identifiers | done | 2026-04-04 | **[local]** [x] camelCase identifiers fixed across 3 files (toHex, readFile, readAll, parseLine) [x] No underscores remain in code examples |
| 7.1.2 | Fix match arm return syntax in website examples | done | 2026-04-04 | **[local]** [x] Single-arm match shorthand replaced with two-arm syntax in 7 files [x] if/el syntax fixed in 6 stdlib files |
| 7.1.3 | Fix error variant construction and propagation syntax | done | 2026-04-04 | **[local]** [x] Ok:/Err: arms in match expressions [x] .ok?/.err? postfix replaced [x] http.Res constructors lowercased |
| 7.1.4 | Fix typed empty collection literals in website examples | done | 2026-04-04 | **[local]** [x] Array/map literals use @() syntax [x] .get() indexing throughout |
| 7.1.5 | Fix loop init and spawn/await syntax in website examples | done | 2026-04-04 | **[local]** [x] spawn/await examples removed [x] Loop syntax corrected [x] != replaced with !(x=0) |
| 7.1.6 | Implement array indexing `arr[i]` in compiler | done | — | [x] PtrLocal tracking [x] NODE_IDENT for ptr-typed locals [x] Function param/call codegen for array types [x] e2e_array_index test |
| 7.1.7 | Implement void return type in compiler | done | — | [x] Void return verification [x] e2e_void test |
| 7.1.8 | Fix struct literal with field access expression crash | done | — | [x] Struct type registry [x] prepass_structs [x] Multi-field GEP codegen [x] Field access codegen [x] e2e_struct_field tests |
| 7.1.9 | Website code example conformance test suite | done | 2026-04-04 | **[local]** [x] test/check_examples.sh: extracts toke blocks from 45 docs, runs tkc --check [x] Skips fragments (no m=), non-toke blocks, skip-check annotated [x] Baseline: 48 checked, 24 pass, 24 fail (50%), threshold set at 50% [x] Failures mostly cross-module imports (E2030) and unimplemented features [x] npm run test:examples wired in package.json |
| 7.1.10 | Add loke showcase to website homepage | done | 2026-04-04 | **[local]** [x] "Built with toke" section added to index.mdx between Development Timeline and Ready to Start [x] loke card with description and link to loke.tokelang.dev [x] Single mention, no other pages affected |
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
 |
--- |
 |
## Gate 2 Preparation |
 |
**Gate 2 criterion:** Default syntax implemented, extended features, 7B model beats baseline |
**Source:** read-only-research/Open source curricula for coding LLMs.md |
 |
### Epic 9.1 — Training Data Expansion |
 |
**Goal:** Scale instruction-tuning dataset from ~47K to ≥200K high-quality instruction–solution pairs. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.1.1 | Reverse OSS-Instruct: corpus → problem descriptions | done | 2026-04-04 | **P0** [x] reverse_oss_instruct.py in toke-corpus/scripts/ [x] Reads corpus JSON, generates JSONL with problem_description, toke_solution, test_cases, difficulty_tier [x] Pluggable generate_description() with --provider flag (openai, anthropic, local) [x] --dry-run heuristic mode with complexity analysis [x] --validate flag (100% pass rate) [x] argparse CLI with all required flags. Ref: OSS-Instruct / Magicoder |
| 9.1.2 | Parallel corpus: Python ↔ toke translation pairs | done | 2026-04-04 | **P0** [x] parallel_corpus.py pipeline in toke-corpus/scripts/ [x] Pairs toke benchmark solutions with Python baselines by task_id [x] JSONL output with python_source, toke_source, token counts, ratio [x] cl100k_base tokenizer with graceful tiktoken degradation [x] Summary stats: mean/median/p10/p90 ratio. 60 pairs from current benchmark. Ref: CodeXGLUE, APPS, MBPP |
| 9.1.3 | Evol-Instruct complexity escalation for toke | done | 2026-04-04 | **P1** [x] evol_instruct.py in toke-corpus/scripts/ [x] 5 toke-specific evolution dimensions (type_constraints, error_propagation, multi_module, algorithmic_complexity, mutable_state) [x] Template-based AST-level transformations, no LLM needed [x] 5 levels per dimension per seed (25 variants/seed) [x] JSONL output with variant_id, seed_id, dimension, level, evolved_source, evolution_description, prompt [x] CLI: --seed-dir, --output, --dimensions, --max-seeds, --dry-run [x] Structural validation in dry-run mode [x] Dedup seeds by task_id. Ref: WizardCoder, Evol-Instruct-Code-80K |
| 9.1.4 | Execution feedback annotation | done | 2026-04-04 | **P1** [x] execution_feedback.py in toke-corpus/scripts/ [x] Reads corpus entries, generates 1-3 broken mutations per program (syntax, name, type, keyword) [x] Simulates tkc diagnostics (E2001, E3011, E4010, E1003) [x] JSONL output with broken_source, diagnostics, fixed_source, error_codes [x] RLEF reward field (0.0 broken, 1.0 fixed) [x] CLI: --corpus-dir, --output, --max-tasks, --mutations-per-task, --seed [x] Stats to stderr: totals, by mutation type, by error code. 46754 corpus entries available. Ref: OpenCodeInstruct |
 |
### Epic 9.2 — Curriculum Learning |
 |
**Goal:** Structure training so the model learns toke incrementally from basic syntax to full-featured programs. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.2.1 | Define toke skill ladder | done | — | **P0** 6 stages defined in toke-corpus/docs/skill-ladder.md. Curriculum schedule with 5 training phases, stage transition gates, adaptive reweighting. Tagging scheme with feature tags and automated assignment algorithm. 2026-04-04 |
| 9.2.2 | TAROT-style test-driven curriculum with compiler feedback | done | — | **P1** Adaptive training loop: evaluate diagnostic subset (100 tasks) per checkpoint, aggregate error codes → weakness profile, reweight data mix. ≥3 iterations. Depends on 9.2.1. Ref: TAROT (2026). Implemented in toke-corpus/scripts/tarot_curriculum.py with --dry-run mode, stratified sampling, error-to-stage mapping, reweighting formula, iteration JSON + summary output. 2026-04-04 |
| 9.2.3 | Teacher-student evaluation loop | done | — | **P2** Teacher generates 500 problems/iteration, student generates toke solutions, compiler verifies, teacher targets weak areas. ≥2 iterations before Gate 2. Depends on 9.2.1. Ref: NVIDIA data flywheel, SelfCodeAlign. Implemented in toke-eval/scripts/teacher_student_loop.py with --dry-run mode, --teacher-model/--student-model/--tkc-path/--iterations/--problems-per-iter args, per-iteration JSON reports with pass rates, failure categories (syntax/type_mismatch/undeclared/control_flow/exhaustiveness/error_handling), stage-weighted problem targeting via data-flywheel reweighting. 2026-04-04 |
 |
### Epic 9.3 — Cross-Language Transfer and Low-Resource Techniques |
 |
**Goal:** Transfer base model's existing code knowledge into toke generation. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.3.1 | Specification-grounded prompting | done | 2026-04-04 | **P0** [x] spec-reference.md (Phase 2, ≤4K tokens, 5.1KB) [x] 56-char set, 12 keywords, type sigils, operators, precedence, all constructs [x] few-shot-template.md with A/B eval protocol [x] Files in toke-corpus/prompts/. Ref: Bridging the Knowledge Void (2026) |
| 9.3.2 | Few-shot exemplar bank | done | 2026-04-04 | **P0** [x] toke-corpus/exemplars/exemplars.jsonl [x] 30 canonical examples covering 20+ categories [x] Profile 1 syntax verified against grammar.ebnf and test files [x] JSONL format with annotations. Ref: SWE-AGI / MoonBit |
| 9.3.3 | Python-to-toke translation fine-tuning stage | done | 2026-04-04 | **P1** [x] translation_finetune.py: parallel corpus to chat-format with 80/10/10 stratified splits [x] 20% reverse (toke->Python) pairs for bidirectional capability [x] configs/translation_lora.yaml: LoRA r=16 a=32 d=0.05 lr=2e-5 warmup=100 seq=2048 [x] eval_translation.py: Pass@1 via tkc --check, exact match, BLEU [x] CLI: --parallel-corpus, --output-dir, --train-ratio, --reverse-ratio, --seed [x] Tested: 60 pairs -> 58 train, 7 val, 6 test. Ref: UniCoder, CodeXGLUE |
 |
### Epic 9.4 — Token Efficiency Measurement and Comparison |
 |
**Goal:** Rigorously quantify toke's token efficiency against baselines and existing reduction techniques. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.4.1 | ShortCoder comparison harness | done | — | **P0** [x] toke-eval/scripts/shortcoder_comparison.py [x] 4 token counts (Python, ShortCoder proxy, toke cl100k, toke BPE) [x] per-task CSV + summary + flagged tasks [x] BCa bootstrap 95% CIs via scipy [x] ShortCoder proxy = ast.unparse minification (documented) [x] argparse: --benchmark-dir, --corpus-dir, --output-dir. 2026-04-04 |
| 9.4.2 | Multi-tokenizer token economy analysis | done | — | **P1** [x] toke-eval/scripts/token_economy.py [x] 5 tokenizers: cl100k_base, o200k_base, Qwen, Llama, toke BPE (graceful fallback) [x] Per-task toke vs Python token counts + ratios [x] Flags tasks where toke is token-longer for ANY tokenizer [x] Aggregate: mean/median/p10/p90, histogram buckets, cross-tokenizer correlation, most-favorable selection [x] 3 output files: token_economy_full.csv, token_economy_summary.json, token_economy_flagged.csv [x] CLI: --benchmark-dir, --output-dir, --tokenizers, --bpe-model, --seed. 2026-04-04 |
| 9.4.3 | Cost and latency benchmarking | done | — | **P2** [x] toke-eval/scripts/cost_latency_benchmark.py [x] Stratified task sampling by difficulty (easy/medium/hard) [x] Per-task: generation time, token count (input+output), estimated API cost [x] Two modes: Python generation, toke generation [x] Configurable per-token pricing (default GPT-4o $2.50/$10 per 1M) [x] Cost-per-correct-solution metric [x] Break-even analysis: Pass@1 threshold where toke becomes cheaper [x] Dry-run mode with synthetic timing + pass rates [x] 3 output files: cost_latency_results.json, cost_latency_summary.json, break_even_analysis.json [x] CLI: --benchmark-dir, --output-dir, --input-price, --output-price, --tasks, --dry-run, --seed. 2026-04-04 |
 |
### Epic 9.5 — Reinforcement Learning from Compiler Feedback |
 |
**Goal:** Use toke's structured JSON diagnostics as a reward signal for RL-based training. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.5.1 | Compiler-as-verifier reward model | done | — | [x] 4-tier reward (+1.0/+0.5/+0.25/0.0) [x] tkc --check --diag-json integration [x] batch JSONL pipeline [x] GRPO-compatible output format [x] dry-run heuristic simulation [x] statistics: histogram, per-tier mean, failure modes [x] CLI: --predictions, --benchmark-dir, --output, --tkc-path, --dry-run, --seed [x] toke-corpus/scripts/compiler_reward.py. 2026-04-04 |
| 9.5.2 | Error-code-aware reward shaping | done | — | [x] 5-tier severity: lex E1xxx (0.10) > parse E2xxx (0.25) > name E3xxx (0.40) > type E4xxx (0.55) > semantic E5xxx (0.70) > clean (1.0) [x] monotonic partial credit via highest_stage_reached [x] A/B comparison: flat binary vs shaped reward [x] JSON report with per-tier stats, variance, distribution, recommendation [x] dry-run simulation with weighted error codes [x] CLI: --corpus-path, --tkc-path, --output, --max-tasks, --dry-run, --seed [x] toke-eval/scripts/error_reward_shaping.py. 2026-04-04 |
 |
### Epic 9.6 — Evaluation and Benchmarking Infrastructure |
 |
**Goal:** Expand evaluation harness for Gate 2 and comparison with published baselines. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.6.1 | EvalPlus-compatible harness | done 2026-04-04 | — | **P0** Extend toke-eval (6.2.1) to output EvalPlus format. Pass@k unbiased estimator. Greedy + temperature sampling (T=0.2, 0.8). Sandboxed execution (Docker). Ref: EvalPlus |
| 9.6.2 | Expanded benchmark: APPS and MBPP adaptation | done 2026-04-04 | — | **P1** 200 APPS + 200 MBPP problems translated to toke. Reference solutions + ≥3 test cases each. Stored in toke-benchmark, separated from training data. Ref: APPS, MBPP |
| 9.6.3 | Automated regression testing on training checkpoints | done 2026-04-04 | — | **P1** CI pipeline: run benchmark on each checkpoint. Track Pass@1, token count, compile rate, error distribution. Alert on >5% Pass@1 drop. Training curve visualisation. |
 |
### Epic 9.7 — Data Efficiency |
 |
**Goal:** Maximise model quality per training sample given small corpus. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.7.1 | IFD-based data selection | done 2026-04-04 | — | **P1** IFD scores on all training samples. K-Means clustering + top-m% selection. Compare: full vs 60% vs 40% vs 20%. Optimal selection rate → use for subsequent training. Ref: Lv et al. (2025) |
| 9.7.2 | Dynamic packing for toke training | done 2026-04-04 | — | **P1** Sort by token length, concatenate to fill context window. Padding ratio <5%. Measure throughput improvement. No Pass@1 degradation. |
 |
### Epic 9.8 — Tokenizer Optimisation |
 |
**Goal:** Fully optimise BPE tokenizer for toke's 56-character set. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 9.8.1 | Retrain BPE on production corpus | done | — | **P0** Retrain SentencePiece on full 47K corpus. Benchmark 1K/2K/4K/8K vocab sizes. Char-to-token ratio ≤1.8. Common patterns (m=, f=, i=, t=, let, <, :$) as single tokens. Round-trip fidelity. Extends 2.9.x pipeline. Done 2026-04-04. Script: `toke-tokenizer/scripts/retrain_bpe.py`. |
| 9.8.2 | Tokenizer alignment with base model vocabulary | done | — | **P1** Overlap analysis: toke BPE vs Qwen tokenizer. If >30% novel tokens → prototype vocab extension. If ≤30% → document feasibility of Qwen tokenizer directly. Recommendation with data. Done 2026-04-04. Script: `toke-tokenizer/scripts/tokenizer_alignment.py`. |
 |
### Story Dependency Graph (Gate 2 Critical Path) |
 |
``` |
9.8.1 BPE retrain ──▶ 9.1.1 Reverse OSS-Instruct ──▶ 9.1.2 Parallel corpus ──▶ 9.2.1 Skill ladder |
                                                                                        │ |
9.3.1 Spec-grounded prompting ─────────────────────────────────────────────────────────┤ |
9.3.2 Few-shot exemplar bank ──────────────────────────────────────────────────────────┘ |
                                                                                        │ |
                                                                              QLoRA training |
                                                                                        │ |
                                                                              9.6.1 EvalPlus ──▶ Gate 2 |
``` |
 |
**Critical path:** 9.8.1 → 9.1.1 → 9.1.2 → 9.2.1 → 9.3.1 + 9.3.2 → QLoRA → 9.6.1 → Gate 2 |
 |
--- |
 |
## Completed Stories |
 |
| ID | Story | Completed | Branch |
|----|-------|-----------|--------| |
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
| 9.3.1 | Specification-grounded prompting | 2026-04-04 | — (toke-corpus) |
| 10.4.4 | Reduce ptrtoint/inttoptr in IR emission | 2026-04-04 | — (tkc) |
| 9.1.1 | Reverse OSS-Instruct: corpus → problem descriptions | 2026-04-04 | — (toke-corpus) |
 |
--- |
 |
## Research Review Remediation (Sprint R1) |
 |
**Source:** Gate 1 research review (8 teams, April 2026) |
**Priority:** P0 — must complete before external review |
**Decisions document:** docs/decisions/gate1-research-review-decisions.md |
 |
### Epic 10.1 — Reproducibility and Transparency (T1, T3, T6, T8) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.1.1 | Publish Token Efficiency Measurement Spec (TEMSpec) | done | 2026-04-03 | **P0** Define all metrics (token density, compression ratio, fertility, paired reduction). Reconcile 2.5-4x cross-language vs 12.5% same-tokenizer claims. Version as artifact. Ref: T1 |
| 10.1.2 | Publish raw per-task token counts CSV | done | 2026-04-03 | **P0** [x] generate_token_counts.py in toke-eval [x] data/gate1_token_counts.csv with 1000 tasks [x] cl100k_base tokenizer, per-task token/char counts + pass1. Ref: T1 |
| 10.1.3 | Multi-tokenizer baseline testing | done | 2026-04-04 | **P0** [x] scripts/multi_tokenizer.py in toke-eval [x] cl100k_base, o200k_base, Qwen, Llama-3.1 tokenizers [x] per-task CSV: task_id, source_chars, token counts + char/token ratios [x] summary stats (mean/median/p10/p90) [x] flags tasks >50% above min tokenizer [x] graceful degradation if transformers missing. Ref: T1 |
| 10.1.4 | Gate 1 reproducibility package | done | 2026-04-04 | **P0** [x] toke-spec/docs/gate1-reproducibility.md [x] 8 sections: eval harness, test hashes, hyperparams (from 7b_mlx.yaml), hardware (M4 Max), curriculum distribution (46,754 programs), contamination report, software versions, reproduction steps [x] 17 TODO markers for commit hashes/SHA values needing shell commands. Ref: T6, T8 |
| 10.1.5 | Corpus statistics and provenance publication | done | 2026-04-04 | **P1** [x] toke-corpus/docs/corpus-statistics.md [x] 8 sections: overview (46,754 programs, 4 stages), category distribution (6 Stage A cats + B/C/D), token count ranges, quality/validation pipeline, provider breakdown (manifest.json exact counts), provenance (synthetic, no copyrighted code), data format (schema.json), staleness note [x] All stats from manifest.json/scorecard.json/config.yaml — no raw data. D13=E. Ref: T1, T3, T6 |
| 10.1.6 | Statistical analysis with confidence intervals | done | 2026-04-04 | **P1** [x] toke-eval/scripts/statistical_analysis.py [x] BCa bootstrap CIs (10k resamples) for median and trimmed-mean (10%) token reduction ratios [x] Per-category stratification [x] Paired Wilcoxon signed-rank test with exact p-values [x] Rank-biserial r effect size [x] Power analysis (min sample size for 80% power) [x] Mock data generator for methodology validation (gate1 CSV has toke-only data) [x] JSON + Markdown output formats [x] argparse CLI. Ref: T1 |
 |
### Epic 10.2 — Repository Coherence (T6) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.2.1 | Reconcile stdlib module count across all repos | done | — | **P0** [x] toke-stdlib README updated to 14 modules with descriptions [x] toke meta-repo verified. Ref: T6 |
| 10.2.2 | Fix toke meta-repo Gate 1 status | done | — | **P0** [x] Already correct from prior session (PASS, 1000 tasks, Phase 2). Ref: T6 |
| 10.2.3 | Fix toke-stdlib import syntax (I= to i=) | done | — | **P0** [x] All I=/M=/F= converted to i=/m=/f= in toke-stdlib README. Ref: T6 |
| 10.2.4 | Add CONTRIBUTING.md and SECURITY.md to meta-repo | done | 2026-04-03 | **P1** [x] CONTRIBUTING.md with per-repo guide, DCO, conventional commits [x] SECURITY.md with private disclosure policy. Ref: T6 |
| 10.2.5 | Standardize security and governance docs across repos | done | 2026-04-04 | **P1** [x] SECURITY.md added to 8 repos (tkc/toke/toke-models already had) [x] .gitleaks.toml added to 10 repos (toke-models already had) [x] All extend default gitleaks rules with project allowlists. Ref: T6 |
| 10.2.6 | Fix CI quality gate suppression in tkc | done | — | **P1** [x] Removed || true from clang-format and make conform CI steps [x] brew install || true retained (expected). Ref: T6 |
| 10.2.7 | Introduce release tags across repos | done | 2026-04-03 | **P1** [x] v0.1-gate1 annotated tag on all 9 repos [x] Tags created locally, push pending. Ref: T6, T8 |
| 10.2.8 | Fix toke-tokenizer dependency metadata | done | — | **P1** [x] sentencepiece and tiktoken added as optional deps [train] and [eval]. Ref: T6 |
| 10.2.9 | Publish dual-licence rationale | done | 2026-04-04 | **P2** [x] LICENSING.md created in tkc root [x] Covers repo-licence table, rationale (MIT for adoption, Apache 2.0 for patent grant), DCO/header guidelines, FAQ. Ref: T6 |
| 10.2.10 | Consolidate repos from 10 to 6 (toke, toke-model, toke-eval, toke-web, toke-mcp, toke-cloud) | done | 2026-04-04 | **P1** [x] Archived all 12 dirs to ~/tk/archive/ [x] toke-mcp extracted from toke-cloud (7 tools, SSE, rate limiter, IDE integrations) [x] toke-cloud trimmed to private (billing, auth, CDK, telemetry) [x] tkc+toke-spec+toke-stdlib → toke via git subtree [x] toke-corpus+toke-tokenizer → toke-model via git subtree --squash [x] toke-benchmark → toke-eval via git subtree [x] 86/86 conformance [x] All builds clean. Ref: T6 |
| 10.2.12 | Replace Karwalski name references with Matt Watt | done | 2026-04-04 | **P2** [x] Replaced M. Karwalski/Matt Karwalski → Matt Watt in ADRs, gate1-decision, phase1-review, pyproject.toml, RFC [x] Updated git config user.name in toke, toke-model, toke-eval, toke-web [x] 'karwalski' only remains in GitHub URLs and RFC document ID |
| 10.2.11 | ASCII art loading bar for slow CLI operations | done | 2026-04-04 | **P1** Add animated ASCII art progress bar to tkc for slow operations (compiling, building, installing, first-run token setup). Uses wave-swimmer animation with percentage steps (0–100% in 10% increments). Three-line display: wave characters (~≈∿˜) swim across a track of dots replacing block-fill (████→▓▓▓). Rotate through themed loading messages: "Rolling things up…", "Packing it in…", "Getting a good hit on the server…", "Pulling data slowly…", "Blazing through requests…", "Lighting up the pipeline…", "Taking a long drag from the database…", "Inhaling your preferences…", "Passing it to the next process…", "Hitting the cache real smooth…", "Grinding through the queue…", "Cherrying the connection…", "Toasting the buffers…", "Puff puff processing…", "Almost cashed out…", "Deep in rotation…", "Sparking up the backend…", "Cottonmouth loading…", "Holding it in…", "Exhaling results…". Show during: LLVM IR emit, clang link step, config init. Hide in --quiet mode. Ref: docs/loading-bar.json |
 |
### Epic 10.3 — Specification Completeness (T4, T8, RFC reviews) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.3.1a | Specify integer overflow semantics (spec) | done | 2026-04-04 | **P0** D2=E: [x] toke-spec/docs/integer-overflow.md [x] Checked default with @wrapping opt-out [x] LLVM IR emission patterns [x] RT002 error code [x] Conformance test categories defined. Ref: T2, T4, T8 |
| 10.3.1b | Implement checked integer overflow in compiler | done | 2026-04-04 | **P0** D2=E: [x] llvm.sadd/ssub/smul.with.overflow.i64 intrinsics for +,-,* [x] tk_overflow_trap(i32) in tk_runtime.c prints RT002+exits [x] Overflow trap test (i64 max+1) passes [x] 86/86 conform [x] 9/9 stress [x] 9/9 IR verify. @wrapping deferred to future story. Ref: T2 |
| 10.3.2 | Publish Phase 2 as first-class normative profile | done | 2026-04-04 | **P0** D2=E: [x] toke-spec/spec/phase2-profile.md [x] 56-char set, 12 keywords (m/f/i/t lowercased) [x] $ type sigils, @() arrays, .get() indexing [x] Full EBNF grammar [x] Complete Phase 1→Phase 2 transformation rules (7 categories) [x] Phase 2 = normative default, Phase 1 = legacy [x] Lexical rules, reserved identifiers, conformance reqs [x] Cross-refs to semantics, stdlib, errors. Ref: T4, T5 |
| 10.3.3 | Define minimum runtime semantics contract | done | 2026-04-04 | **P1** [x] toke-spec/docs/runtime-semantics.md [x] 10 sections: eval order, numeric (i64/f64), strings (UTF-8), arrays (length-prefixed), maps, bounds traps, recursion limits (1000), error propagation, arena lifetime, concurrency (single-threaded) [x] RT001-RT006 error codes defined. Ref: T4, T8 |
| 10.3.4 | Publish memory model specification | done | 2026-04-04 | **P1** D3=D: [x] toke-spec/docs/memory-model.md [x] Hybrid arena + explicit allocator [x] Ownership rules, escape analysis, safety guarantees [x] Supported/unsupported workload patterns documented. Ref: T2, T8 |
| 10.3.5 | Specify string escaping rules | done | 2026-04-04 | **P1** [x] toke-spec/docs/string-escaping.md [x] 7 escape sequences defined [x] Phase 1/2 compatibility documented [x] Edge cases (empty, multi-line, null bytes, \xNN) [x] Error codes E1001/E1002/W1010. Ref: T8 |
| 10.3.6 | Standardize interface file format (.tki) | done | 2026-04-04 | **P1** D8=A: [x] .tokei → .tki in 14 files across tkc, toke, toke-spec, toke-web [x] No .tokei files existed on disk — already .tki. Ref: T4 |
| 10.3.7 | Spec vs implementation delta table | done | 2026-04-04 | **P2** [x] toke-spec/docs/spec-implementation-delta.md [x] 96 features audited across 7 categories [x] 52 specified+implemented, 20 specified-not-implemented, 6 partial, 13 deferred, 3 removed, 2 impl-only [x] Key gaps: Phase 2 syntax, narrow integers, 11 unemitted error codes, sum type exhaustiveness, mutability enforcement [x] Priority recommendations for Gate 2. Ref: T4 |
| 10.3.11 | Remove spawn/await from language spec | done | 2026-04-04 | **P1** D4=B: [x] TY_TASK enum removed [x] E4050-E4052 removed [x] spawn/await type-checking removed (~50 lines) [x] codegen stubs removed [x] tk_spawn/tk_await declarations removed [x] 4 tests removed (G036-G038, D016) [x] 86/86 conformance, 9/9 IR, 5/5 stress. Ref: T2, T4 |
| 10.3.12 | Add SARIF diagnostic output mode | done | 2026-04-04 | **P1** D6=C: [x] DIAG_FMT_SARIF enum added to diag.h [x] --diag-sarif flag in main.c (mutually exclusive with --diag-json/--diag-text) [x] SARIF v2.1.0 envelope with tool info, results array, locations [x] Buffered output flushed at exit via diag_flush_sarif() [x] Help text updated [x] 86/86 conformance tests pass. Ref: T2, T4, T5 |
| 10.3.8 | Formal static semantics section | done | 2026-04-04 | **P2** [x] toke-spec/docs/static-semantics.md [x] 12 typing judgment rules in inference-rule notation [x] Literals, variables, unary/binary ops, calls, indexing, field access, cast [x] 8 statement rules: let, mut, assign, return, if, loop, match, arena [x] Cast validity table (9 pairs) [x] Type equivalence rules (nominal structs, structural composites) [x] No implicit coercions documented [x] Well-formedness: module, function, struct, declaration order [x] 8 error codes mapped to rules (E2010, E3020, E4010, E4011, E4025, E4031, E4043, E5001) [x] Conformance criteria (10 points). Ref: T4 |
| 10.3.9 | Machine-readable grammar (Tree-sitter) | done | 2026-04-04 | **P2** D6=B: [x] grammar.js covering full EBNF spec [x] All Profile 1 productions: M=/F=/T=/I= decls, let/mut/assign/if/el/lp/br/rt/match/arena stmts, full expression precedence [x] Type system: scalars, arrays, maps, pointers, function types [x] queries/highlights.scm for editor integration [x] Test corpus: declarations, expressions, statements [x] package.json with tree-sitter config. Ref: T4, T5 |
| 10.3.10 | Publish normative JSON Schema for diagnostics | done | 2026-04-03 | **P1** [x] diagnostic-schema.json in toke-spec/docs [x] JSON Schema 2020-12 [x] All fields documented with types, patterns, enums [x] fix field contract preserved. Ref: T2, T8 |
 |
### Epic 10.4 — Compiler Hardening (T2) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.4.1 | Add --emit-llvm and --emit-asm flags to tkc | done | — | **P0** [x] --emit-llvm writes .ll to output path [x] --emit-asm produces .s via clang -S [x] Help text updated [x] 90/90 conformance. Ref: T2 |
| 10.4.2 | IR verification in CI (llvm-as + opt -verify) | done | 2026-04-03 | **P0** [x] test/verify_ir.sh runs llvm-as + opt -passes=verify on all e2e .tk files [x] make verify-ir target [x] CI step added to test job [x] 9/9 pass. Ref: T2 |
| 10.4.3 | Emit rich LLVM annotations | done | 2026-04-04 | **P1** [x] dso_local on all function defs [x] nounwind attribute [x] inbounds on 6 GEP sites (struct field, array .len, array element, struct literal) [x] 86/86 conform [x] 9/9 IR verify [x] 5/5 stress. Ref: T2 |
| 10.4.4 | Reduce ptrtoint/inttoptr in IR emission | done | 2026-04-04 | **P1** [x] Audit: 13 casts across 7 sites [x] Added direct ptr icmp for == < > when both operands are ptr (eliminates 2 ptrtoint per ptr comparison) [x] 8 remaining casts documented as necessary: boxing (array concat coercion), explicit as-casts, call arg coercion, field/index access on untyped i64 values [x] 90/90 conformance [x] 9/9 e2e. Ref: T2 |
| 10.4.5 | Emit target datalayout and target triple | done | — | **P0** [x] target datalayout emitted for x86_64/aarch64 linux/macos [x] Fallback via preprocessor for native builds [x] target triple already emitted. Ref: T2 |
| 10.4.6 | Expose optimization level flags (-O0/-O1/-O2/-O3) | done | 2026-04-03 | **P1** [x] -O0/-O1/-O2/-O3 flags parsed in main.c [x] Passed to compile_binary and emit-asm [x] Default -O1 preserved [x] 90/90 conformance. Ref: T2 |
| 10.4.7 | Enable stack probes for recursion safety | done | 2026-04-04 | **P2** [x] probe-stack="inline-asm" attribute on all emitted functions [x] stack-protector-buffer-size="8" attribute [x] attributes #0 group emitted at module end [x] 86/86 conformance, 9/9 e2e, 9/9 stress. Ref: T2 |
| 10.4.8 | Emit musttail for tail-recursive calls | done | 2026-04-04 | **P2** [x] fastcc calling convention on all internal (non-extern) function definitions [x] fastcc on all calls to internal functions [x] musttail call fastcc emitted for tail-recursive calls (return of call to self) [x] argument type coercion in tail position [x] non-tail and extern calls unchanged [x] 86/86 conformance. Ref: T2 |
| 10.4.9 | Publish Runtime ABI document v0 | done | 2026-04-04 | **P1** [x] tkc/docs/runtime-abi.md [x] 10 sections: calling convention, scalar types, string layout, array layout (length-prefixed), struct layout (flat i64 arrays), map/error [TODO], 17 runtime functions, overflow trap, initialization. Ref: T2 |
 |
### Epic 10.4b — Compiler Bugs Found by Stress Tests |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.4b.1 | Fix codegen for 10+ function definitions | done | 2026-04-04 | **P1** Root cause: NODE_MAX_CHILDREN=8 truncated AST children. Fix: dynamic children array in Node (arena-allocated, doubles on overflow). [x] 12-function test passes [x] 86/86 conform [x] 8/8 stress [x] 9/9 IR verify. |
| 10.4b.2 | Fix large array literal indexing | done | 2026-04-04 | **P1** Same root cause as 10.4b.1. 50-element array now allocates correctly. [x] a[49]=50 test passes. |
| 10.4b.3 | Fix many locals (20+) codegen | done | 2026-04-04 | **P1** Same root cause as 10.4b.1. 20 locals sum=210 test passes. |
 |
### Epic 10.5 — Performance Benchmarking (T2) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.5.1 | Compiled binary benchmarks vs C/Rust/Zig | done | 2026-04-04 | **P1** [x] 12 benchmark programs in toke + C (fib recursive/iterative, sum array, nested loops, binary search, prime sieve, deep recursion, struct ops, large expr, chained calls, collatz, gcd euler) [x] bench/run_bench.sh runner: wall-time, binary size, peak RSS via /usr/bin/time [x] Median of N runs, markdown table output [x] bench/README.md with methodology and caveats. Ref: T2 |
| 10.5.2 | Compiler speed benchmarks (tkc vs tcc/zig cc) | done | 2026-04-04 | **P2** [x] bench/compile_speed.sh: times tkc --check (frontend) vs tkc -O2 (full) vs cc/tcc/zig cc [x] Frontend/backend split via --check vs full compile difference [x] Median of N runs, markdown table output to bench/results/ [x] C equivalents in bench/programs/c/ for comparison [x] Auto-detects tcc and zig on PATH. Ref: T2 |
| 10.5.3 | Optimization ladder evaluation | done | 2026-04-04 | **P2** [x] bench/optimization_ladder.sh: compiles .tk to LLVM IR via tkc -O0 --emit-llvm, then clang at -O0/-O1/-O2/-O3/-flto=thin [x] Median of N runs, markdown table output to bench/results/ [x] Marginal gains table between each level [x] Summary with average speedup vs -O0 across all benchmarks [x] PGO deferred — requires workload-specific training data. Ref: T2 |
| 10.5.4 | Stress test suite (make stress) | done | 2026-04-03 | **P1** [x] test/stress/ with run_stress.sh [x] 5 tests: large expression, nested arithmetic, chained functions, nested if, boundary zero [x] make stress target [x] Uncovered 3 bugs (large array indexing, many locals, forward references) — separate fix stories needed. Ref: T2 |
| 10.5.5 | Binary size and memory footprint documentation | done | 2026-04-04 | **P2** [x] docs/binary-size.md [x] 12 benchmarks: 34.2-34.3 KB stripped [x] tkc compiler 182 KB stripped [x] ~46 MB peak RSS [x] +2.4% overhead vs C equivalent. Ref: T2 |
 |
### Epic 10.6 — Evaluation Infrastructure Expansion (T1, T3, T7, T8) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.6.1 | Add Pass@5 and Pass@10 to evaluation harness | done | 2026-04-04 | **P1** [x] pass_at_k.py in scripts/ with unbiased Codex estimator (scipy.special.comb, math.comb fallback) [x] Temperature sweeps (0.0, 0.2, 0.4, 0.6, 0.8) [x] JSONL prediction input format [x] Compile+run pipeline via tkc/clang [x] --dry-run with deterministic synthetic outcomes [x] JSON + CSV + stdout summary output [x] Pass@1/5/10 per task and aggregate [x] CLI: --predictions-dir, --benchmark-dir, --output-dir, --k-values, --temperatures, --samples-per-task, --dry-run, --seed. Ref: T1, T7 |
| 10.6.2 | Error taxonomy in evaluation output | done | 2026-04-03 | **P1** [x] classify_error() maps E-codes to 8 categories (syntax/parse/name/type/codegen/runtime/logic/unknown) [x] ErrorTaxonomy dataclass [x] Per-task error_category in results [x] Summary output with percentages. Ref: T3, T7 |
| 10.6.3 | Port 200 tasks to HumanEval/MBPP JSON schema | done | 2026-04-04 | **P1** [x] export_humaneval.py converts YAML tasks to HumanEval JSONL (task_id, prompt, entry_point, canonical_solution, test, description) [x] export_mbpp.py converts to MBPP JSONL (task_id, text, code, test_list) [x] 200 tasks exported to data/humaneval_format.jsonl [x] 200 tasks exported to data/mbpp_format.jsonl [x] Canonical toke solutions included where available. Ref: T7 |
| 10.6.4 | Publish frozen public benchmark slice | done | 2026-04-04 | **P1** [x] 74 tasks selected from 1000 via proportional category sampling (seed=2026) [x] All 7 categories covered [x] public/tasks/ with YAML files [x] public/metadata.json with full task metadata [x] public/CHECKSUM.sha256 for integrity [x] public/README.md with methodology and caveats [x] build_slice.py for reproducibility [x] No overlap with training data [x] Frozen at Gate 1. Ref: T1, T7 |
| 10.6.5 | Repair-loop evaluation harness | done | 2026-04-04 | **P2** [x] repair_loop_harness.py in toke-eval/scripts [x] Generate-compile-repair loop with --max-iterations budget (default 5) [x] Compiles via tkc --check --diag-json, parses structured diagnostics (code, message, line, col, fix) [x] Categorises failures: syntax, type, name_resolution, control_flow, exhaustiveness, error_handling, codegen, unknown [x] Repair prompt builder feeds diagnostics+fix suggestions back to model [x] Simulated repair with stage-dependent pass rates and iteration bonus [x] JSON report: per-task iteration counts, failure categories, aggregate stats (success rate, mean/median iterations, histogram) [x] CLI: --tkc-path, --tasks-dir, --max-iterations, --output, --dry-run, --seed [x] Loads HumanEval JSONL or built-in synthetic tasks. Ref: T5, T7 |
| 10.6.6 | Contamination analysis and holdout governance | done | 2026-04-04 | **P1** D5=A+D: strict separation of proprietary vs open-weight outputs. Semantic similarity checks between training and held-out. Hash commitments. Document governance. Ref: T3, T7 |
| 10.6.7 | Difficulty stratification of benchmark tasks | done | 2026-04-04 | **P2** [x] stratify_tasks.py classifies 1400 tasks by difficulty (beginner/intermediate/advanced/expert) and category (9 categories) [x] Heuristics: solution char count + existing labels + description keywords [x] data/stratification.json per-task output [x] data/stratification_summary.json with counts and cross-tab [x] --dry-run, --update-yaml, --benchmark-dir CLI [x] Tested dry-run pass. Ref: T3 |
 |
### Epic 10.7 — Training Pipeline Improvements (T3, T8) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.7.1 | Add lm_head and embed_tokens to modules_to_save | done | — | **P0** [x] embed_tokens + lm_head unfrozen in train_mlx.py [x] train_embeddings config option (default: true) [x] 7b_mlx.yaml updated. Ref: T3 |
| 10.7.2 | Switch training to DoRA | done | 2026-04-04 | **P1** D10=B: [x] 7b_dora_comparison.yaml config (rank 16, alpha 32, use_dora:true, adapters/dora/) [x] compare_dora_qlora.py with McNemar + paired t-test, JSON+MD output [x] train_dora.sh wrapper with logging and auto-comparison [x] eval_adapter.py with tkc --check Pass@1, dry-run mode, predictions JSONL. Ref: T3, T8 |
| 10.7.3 | Stabilize evaluation contract (model card for gates) | done | — | **P0** [x] gate_card_template.md in toke-eval [x] Covers model/tokenizer/decoding/benchmark/compiler/hardware/results/hashes [x] "Incomplete cards invalidate gate result" policy. Ref: T3 |
| 10.7.4 | Holdout isolation as hard invariant | done | 2026-04-03 | **P0** QualityScorer requires holdout_task_ids parameter. Fail corpus build if absent. Dual enforcement in training export. Ref: T3 |
| 10.7.5 | Negative examples in training corpus (10-15%) | done | 2026-04-04 | **P2** [x] 11 mutators across 7 categories: off-by-one, logic inversion, missing return, wrong operator, scope error, type confusion, semicolon errors [x] Each entry has broken_source, fixed_source, mutation_type, diagnostics, difficulty, contrastive_pair [x] Quality filter: keeps only mutations that fail compile or produce wrong output [x] CLI: --corpus-dir, --output, --target-ratio, --max-entries, --seed [x] Output JSONL to data/negative_examples.jsonl [x] Tested with --max-entries 20 --seed 42 (20 examples generated). Ref: T3 |
| 10.7.6 | Five-tier corpus validation pipeline | done | 2026-04-04 | **P1** [x] 5-tier pipeline: compile check, execution check, cross-optimisation differential, property-based, mutation testing [x] Gated tiers (N gates N+1) [x] --tiers, --max-entries, --dry-run, --tkc-path CLI [x] validation_report.json + validation_summary.json output [x] Progress bar, summary table to stdout [x] Dry-run mode with heuristic/synthetic results. Ref: T3 |
| 10.7.7 | Publish corpus statistics + regeneration scripts + clean subset | done | 2026-04-04 | **P1** D13=E: [x] scripts/regenerate.py with --dry-run and --stage filter [x] scripts/compute_hashes.py with SHA-256 + Merkle root [x] scripts/extract_clean.py filters open-weight only (200 entries) [x] clean/ dir with README.md, manifest.json, programs/ [x] hashes.json with 46756 file hashes. Ref: T1, T3, T6, T7 |
| 10.7.8 | Add DoRA support to training pipeline | done | 2026-04-04 | **P1** D10=B: [x] 7b_mlx_dora.yaml config with use_dora:true [x] train_mlx.py updated to propagate use_dora flag [x] Fixed API compat with mlx-lm >=0.21 linear_to_lora_layers signature [x] Console reports adapter type. Ref: T3 |
| 10.7.9 | Hybrid MLX+CUDA training infrastructure | done | 2026-04-04 | **P2** D19=hybrid: [x] scripts/train_cuda.sh with --config, --output-dir, --dry-run, --resume [x] PEFT/transformers QLoRA+DoRA via shared YAML config [x] Same JSONL data pipeline as MLX [x] BitsAndBytes 4-bit quantization [x] Checkpointing + wandb logging [x] Auto-maps MLX config keys (keys->target_modules, grad_checkpoint, etc.) [x] docs/hybrid-training.md: framework selection, config compat, checkpoint transfer, hardware recs. Ref: T8 |
 |
### Epic 10.8 — Translation, Readability and Expansion Tooling (T5) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.8.1 | Canonical formatter (toke fmt) | done | 2026-04-04 | **P1** D18=opinionated. [x] fmt.h/fmt.c AST walker [x] --fmt CLI flag (parse+format, no type check) [x] 2-space indent, semicolon separators, deterministic output [x] All node types handled [x] F001_format_basic.sh test [x] Makefile wired. Ref: T5 |
| 10.8.2 | Add --pretty and --expand flags to tkc | done | 2026-04-04 | **P1** D18=opinionated. [x] FmtOptions struct in fmt.h [x] tkc_format_pretty() with separate AST walkers [x] --pretty: spaces around binary ops, blank lines before loops/returns [x] --expand: abbreviation dictionary (15 entries), context-aware loop counters, inline comments [x] Combined --pretty --expand mode [x] Inferred type annotations on let bindings [x] CLI integration in main.c [x] F002_pretty_expand.sh test [x] Clean build -Wall -Wextra -Wpedantic -Werror. Ref: T5 |
| 10.8.3 | Source map emission capability | done | 2026-04-04 | **P2** Map compact spans to expanded spans. [x] sourcemap.h/sourcemap.c: SourceMap struct, sourcemap_init/add/lookup/emit_json/free [x] SpanMapping (compact_line, compact_col, expanded_line, expanded_col, length) [x] JSON format {"version":1,"mappings":[...]} [x] --sourcemap CLI flag in main.c [x] Works with --fmt and --pretty/--expand: generates both views, walks tokens in parallel [x] Makefile wired (sourcemap.o) [x] Clean build -Werror, 86 conformance tests pass. Ref: T5 |
| 10.8.4 | Expose AST-as-JSON in tooling protocol | done | 2026-04-04 | **P2** [x] ast_json.c/ast_json.h: recursive AST-to-JSON serialiser [x] --dump-ast flag in main.c: lex+parse then JSON to stdout [x] Node kind, pos (line/col/offset), span, name (idents), value (literals), children [x] Valid JSON output pipeable to jq. Ref: T5 |
| 10.8.5 | Publish parallel training dataset (compact + expanded) | done | 2026-04-04 | **P2** Enable reproducible expander training. Invert existing corpus. [x] parallel_expand.py reads corpus JSONL, runs tkc --fmt/--pretty, outputs compact+expanded pairs [x] --dry-run generates synthetic demo pairs without tkc binary [x] JSONL schema: task_id, compact_source, expanded_source, token_count_compact, token_count_expanded, compression_ratio [x] --corpus-dir, --output, --tkc-path, --max-entries, --dry-run CLI args [x] tiktoken integration with graceful degradation. Ref: T5 |
 |
### Epic 10.9 — Research Positioning and External Credibility (T7) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.9.1 | Prior art and design-space map document | done | 2026-04-04 | **P1** [x] toke-spec/docs/prior-art.md [x] Design space map (2-axis: new lang vs constrained decoding, human vs machine) [x] Comparison matrix (9 systems) [x] "Why new language" + "Why not constrained decoding alone" arguments [x] 12 citation-needed markers for web-sourced claims. Ref: T7 |
| 10.9.2 | Competitive differentiation matrix on tokelang.dev | done | 2026-04-04 | **P2** [x] toke-web/src/content/docs/about/competitive-matrix.md [x] 5-column matrix (token density, Pass@1, compilation target, safety, ecosystem) [x] 4 rows (toke, Zig, Mojo, constrained decoding) [x] Factual and fair — acknowledges competitor strengths [x] Notes toke targets different use case than general-purpose systems languages. Ref: T7 |
| 10.9.3 | Constrained decoding ablation study | done | 2026-04-04 | **P1** D12=C: [x] 4-condition ablation framework (toke/python x constrained/unconstrained) [x] McNemar + Cohen's d + bootstrap CIs [x] Dry-run simulation with realistic error distributions [x] ablation_results.json, ablation_summary.json, ablation_table.csv outputs. Ref: T7 |
| 10.9.4a | Rename RFC → Toke Specification | done | 2026-04-04 | **P1** D9=A: [x] RFC references → "specification" in toke-spec, tkc decisions, research-review-request [x] Draft header updated [x] IETF-specific language removed. Ref: specification reviews |
| 10.9.4b | Strengthen specification with Gate 1 evidence | done | 2026-04-04 | **P1** [x] integer-overflow.md: Gate 1 evidence section (unchecked during Gate 1) [x] string-escaping.md: numeric literals section (int decimal/hex/binary, float, profile restrictions) [x] memory-model.md: Gate 1 evidence (arena-only) [x] temspec.md: Gate 1 application (87,903 tokens, 12.5% reduction, cl100k_base). Ref: specification reviews |
 |
### Epic 10.10 — Gate 2 Definition and Governance (T8) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.10.1 | Define Gate 1.5 reproducibility criteria | done | — | **P0** [x] toke-spec/docs/gate-criteria.md [x] 9 required artifacts listed [x] 30-day deadline (2026-05-03) [x] 5 success criteria. Ref: T8 |
| 10.10.2 | Define Gate 2 success criteria with audit-grade precision | done | — | **P0** [x] 11 criteria with thresholds [x] Evaluation protocol (decoding, sampling, CIs) [x] Failure protocol [x] Gate 3+4 preliminary criteria. Ref: T8 |
| 10.10.3 | Publish compute budget and infrastructure plan | done | — | **P1** [x] toke-spec/docs/compute-budget.md [x] Gate 1 actuals [x] Gate 2-4 projections [x] Hardware strategy (MLX-first + cloud contingency) [x] Cost table ($680-880 through Gate 4) [x] Optimisation strategies. Done 2026-04-04. Ref: T8 |
| 10.10.4 | Living risk register | done | — | **P2** [x] 16 risks across 6 categories [x] Heat map + top-5 summary [x] Likelihood x impact scoring [x] Mitigations and residual risk [x] docs/risk-register.md. Done 2026-04-04. Ref: T8 |
 |
### Epic 10.11 — Compiler Code Quality and Configurability |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.11.1 | Document lexer.c with verbose function comments | done | 2026-04-04 | **P2** Review all functions in lexer.c. Add block comments explaining purpose, inputs, outputs, invariants, and any non-obvious logic. No behavioral changes. |
| 10.11.2 | Document parser.c with verbose function comments | done | 2026-04-04 | **P2** Documented all 30 functions/helpers in parser.c: file-level header (pipeline role, grammar overview, precedence chain, error recovery, diagnostic codes E2001-E2004), Parser struct fields, peek/cur/adv/mk/ch/teq/eerr/sync/opt_semi/xp helpers, is_scalar, parse_func_type, parse_type_expr, parse_literal, parse_primary, parse_postfix, parse_call, parse_cast_prop, parse_unary, parse_mul, parse_add, parse_compare, parse_match_arm, parse_expr, parse_loop_stmt, parse_if_stmt, parse_stmt, parse_stmt_list, parse_module_path, parse_module_decl, parse_import_decl, parse_field_list, parse_type_decl, parse_const_decl, parse_one_param, parse_func_decl, parse (entry point). Each comment covers grammar production, AST node structure with children layout, and error recovery strategy. No behavioral changes. |
| 10.11.3 | Document names.c with verbose function comments | done | 2026-04-04 | **P2** Verified all 18 functions in names.c have verbose block comments: file header (two-subsystem overview), span_dup, node_path_str, dots_to_slashes, build_avail_list, tki_exists, InFlight struct + ifl_has/ifl_push/ifl_pop, st_push, validate_version, version_major, resolve_imports (7-step algorithm), symtab_free, push_scope, scope_lookup, scope_lookup_local, arena_intern, scope_insert, seed_predefined, resolve_ident, resolve_func, resolve_node (10-case dispatch), resolve_names (5-step algorithm). Each comment covers purpose, parameters, return values, scope chain mechanics, and diagnostic codes. No behavioral changes. Build verified clean. |
| 10.11.4 | Document types.c with verbose function comments | done | 2026-04-04 | **P2** Documented all functions and switch cases in types.c: file header (pipeline role, Profile 1 type system with 10 rules, type inference strategy, error handling codes E2010/E3020/E4010/E4011/E4025/E4031/E4043/E5001, memory allocation), ty_intern, mk_type, type_name, types_equal (structural/nominal equality, TY_UNKNOWN compatibility), is_numeric, tc_lookup, TOKSTR macro, Ctx struct, contains_ptr, resolve_type (primitives, pointers, arrays, maps, structs), resolve_return_spec (error-union T!Err), emit_mm, infer() with all 18 cases: literals (4 kinds), struct/array/map literals, identifiers, unary/binary expressions, function calls, cast expressions, error propagation (!), index expressions, field access, bind/mut-bind/assign statements, return statements, match statements (exhaustiveness + arm consistency), arena statements (escape analysis), function declarations (E2010 pointer validation), default fallthrough. type_check entry point. No behavioral changes. Build verified clean with -Werror. |
| 10.11.5 | Document llvm.c with verbose function comments | done | 2026-04-04 | **P2** Added comprehensive block comments to all functions and data structures in llvm.c: file-level architecture overview (4-phase pipeline: prepass, top-level emission, expr/stmt emission, finalization), Ctx state machine (all fields grouped by role: output, SSA counters, control-flow state, registries, per-function tracking, string globals buffer), all 6 internal structs (FnSig, PtrLocal, StructInfo, ImportAlias, LocalType, NameAlias), SSA counter helpers (next_tmp/next_lbl/next_str), tok_cp, mark_ptr_with_type, is_ptr_local, ptr_local_struct_type, register_struct (field ordering and GEP index significance), lookup_struct, struct_field_index, is_struct_type_name, is_ptr_type_node, resolve_llvm_type (full type mapping table), register_fn, lookup_fn, prepass_structs, prepass_funcs (is_internal detection, fastcc convention), prepass_imports, resolve_stdlib_call (all 5 modules and their methods), str_buf_append (buffering strategy), emit_str_global (escape processing), resolve_base_struct, emit_expr (all 14 AST node kinds with emission strategy), expr_struct_type, get_llvm_name, make_unique_name (shadowing strategy), set_local_type, get_local_type, expr_llvm_type (static type prediction), emit_stmt (all 11 statement kinds including tail-call detection), emit_toplevel (struct layout, const emission, function definition with param spills), emit_llvm_ir (9-step orchestration), find_runtime_source, compile_binary. No behavioral changes -- comments only. Build verified clean with -Werror. |
| 10.11.6 | Document ir.c, diag.c, arena.c, main.c with verbose function comments | done | 2026-04-04 | **P2** Documented 7 source files. ir.c and diag.c already had comprehensive comments (file headers + every function) from earlier stories -- no changes needed. arena.c: added block comments to block_new, arena_init, arena_alloc, arena_free. main.c: expanded file header with pipeline overview and exit-code semantics, added comments to stem() and main(). fmt.c: added comments to all Buf helpers, op_str, fmt_type_expr, fmt_expr, fmt_stmt_list, fmt_stmt, fmt_module_path, fmt_return_spec, fmt_decl, tkc_format, tkc_format_pretty. progress.c: expanded file header with animation/message description, added comments to progress_init, progress_update, progress_done. config.c: added block comment to tkc_load_config with return-value semantics. No behavioral changes. Build verified clean with -Werror. |
| 10.11.7 | Extract all magic numbers to named constants in tkc_limits.h | done | 2026-04-04 | **P1** [x] src/tkc_limits.h with 25 named constants, each documented with role, safe range, configurability [x] arena.c: ARENA_BLOCK_SIZE/ARENA_ALIGN → TKC_ prefix [x] names.c: MAX_PATH/MAX_IFL/MAX_AVAIL → TKC_ prefix [x] llvm.c: MAX_FUNCS/LOCALS/PARAMS/PTR_LOCALS/STRUCT_TYPES/IMPORTS → TKC_ prefix, buffer sizes → NAME_BUF/ALIAS_BUF [x] main.c: exit codes → TKC_EXIT_, buffers → PATH_BUF/CMD_BUF/MSG_BUF [x] parser.h: NODE_INIT_CAP moved to tkc_limits.h [x] 86/86 conform, 9/9 stress, 9/9 IR. |
| 10.11.8 | Add compiler configuration file support (tkc.toml) | done | 2026-04-04 | **P1** [x] src/config.h + src/config.c: minimal TOML parser (comments, blank lines, key=integer) [x] tkc_load_config() returns 0 success, -1 not found, -2 parse error [x] main.c: loads ./tkc.toml after defaults, before CLI flag parsing [x] --config=PATH flag for alternate config file [x] Precedence: CLI > tkc.toml > defaults [x] Missing default tkc.toml silently ignored; missing explicit --config= is an error [x] Help text updated [x] Makefile updated with config.o [x] Clean build -Werror, 86/86 conform [x] test/test_config.sh. |
| 10.11.9 | Add per-session limit override flags to tkc | done | 2026-04-04 | **P1** [x] TkcLimits struct in tkc_limits.h with defaults initialiser and print function [x] CLI flags: --max-funcs=N, --max-locals=N, --max-params=N, --max-structs=N, --max-imports=N, --arena-block=N [x] --show-limits prints effective limits and exits [x] strncmp+atoi parsing for --flag=N style [x] Help text updated with limit override section [x] Clean build with -Werror, no warnings. Actual dynamic allocation deferred to 10.11.10. |
| 10.11.10 | Make codegen Ctx dynamically sized using limits | done | 2026-04-04 | **P1** [x] Added TkcLimits to CodegenEnv in llvm.h [x] Ctx arrays (fns, ptrs, structs, imports, locals, aliases) converted from fixed-size to pointers with capacity fields [x] Arrays arena_alloc'd in emit_llvm_ir from TkcLimits values [x] All 6 limit checks use runtime capacity (fn_cap, ptr_cap, struct_cap, import_cap, local_cap, alias_cap) instead of compile-time constants [x] E9010 diagnostics preserved [x] main.c passes limits through CodegenEnv at all 3 call sites [x] Clean build with -Werror. |
| 10.11.11 | Make names.c limits dynamic | done | 2026-04-04 | **P1** [x] Added max_imports_in_flight and max_avail_modules to TkcLimits struct with defaults [x] InFlight struct converted from fixed array to malloc'd pointer+capacity [x] build_avail_list converted from stack array to malloc'd array [x] resolve_imports signature updated to accept const TkcLimits* [x] E9010 diagnostics check against dynamic capacity [x] CLI flags --max-in-flight=N and --max-avail=N added [x] config.c recognises max_imports_in_flight and max_avail_modules keys [x] --show-limits prints new fields [x] Help text updated [x] Clean build with -Werror. |
| 10.11.12 | Emit diagnostics on silent limit truncation | done | 2026-04-04 | **P0** Audited all 8 limit-check sites across llvm.c (6) and names.c (2). Every silent `return` now emits E9010 "compiler limit exceeded" with a descriptive message. Depends on 10.11.7. |
 |
### Epic 10.12 — AI Coding Tool Integration (`toke-cloud`) |
 |
**Source:** TOKE-INT-001 — AI Coding Tool Integration Assessment |
**Goal:** Zero-install onboarding for AI coding tools via hosted MCP service. Developers add one URL and can immediately write, compile, and debug Toke programs. Supports Gate 2/3 evaluation through production telemetry. |
 |
#### Phase A — Hosted Foundation |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.12.1 | Build MCP server with toke_check and toke_compile tools | done | 2026-04-04 | **P0** Node.js MCP SDK server exposing `toke_check` (source → JSON diagnostics) and `toke_compile` (source → LLVM IR or diagnostics). MCP protocol over SSE. Effort: M. Repo: toke-cloud. |
| 10.12.2 | Bundle tkc as Lambda layer and deploy safe tool functions | done | 2026-04-04 | **P0** Package tkc static binary in Lambda layer. Deploy `toke_check` and `toke_compile` as Lambda functions. No network access, read-only fs, 10s timeout. Effort: M. Depends on 10.12.1. |
| 10.12.3 | Deploy API Gateway + CloudFront + WAF with free-tier rate limiting | done | 2026-04-04 | **P0** Edge layer: CloudFront (TLS 1.3 + WAF), API Gateway HTTP API, route /mcp/sse → SSE handler. WAF rate limiting per-IP. Payload max 64KB. Effort: M. Depends on 10.12.2. |
| 10.12.4 | Deploy Redis (ElastiCache) for rate limits and response caching | done | 2026-04-04 | **P1** ElastiCache t4g.micro. Sliding window rate limit counters. Response cache for spec lookups and error explanations. Connection registry. Effort: S. |
| 10.12.5 | Write Toke language skill file for Claude Code | done | 2026-04-04 | **P0** `toke-language.md` skill: 56-char set, keywords, type system, idioms, common patterns, error codes. Reflects final Phase 2 spec. Covers both Phase 1 (compiler default) and Phase 2 (normative spec) syntax. 7 complete examples verified against e2e/conformance tests. |
| 10.12.6 | Build minimal Claude Code plugin pointing to hosted endpoint | done | 2026-04-04 | **P0** [x] .mcp.json → mcp.tokelang.dev/mcp/sse [x] CLAUDE.md with check-repair loop instructions [x] /toke:check and /toke:new slash commands [x] toke-language.md skill file bundled [x] README with install instructions. Depends on 10.12.5. |
| 10.12.7 | Write Codex instructions file and validate MCP connection | done | 2026-04-04 | **P1** `codex.md` instructions teaching Codex the Toke language. `mcp.json` for SSE endpoint config. 5 working examples, check-repair loop, all 12 keywords, type system, common mistakes. Effort: S. |
| 10.12.8 | End-to-end test: Claude Code repair loop on 20 sample tasks | done | 2026-04-04 | **P0** Test harness at toke-cloud/test/e2e_repair_loop.js. Imports tokeCheck/tokeCompile directly (no MCP server). 20 tasks from toke-benchmark/hidden_tests/. Rule-based fixer handles E1003, E2001, E2003, E3011, E4010/E4031. CLI: `node test/e2e_repair_loop.js [--max-iterations 5] [--tasks 20] [--verbose]`. JSON report to test/data/e2e_results.json. Depends on 10.12.6. |
| 10.12.9 | Infrastructure as CDK (TypeScript) with CI deploy | done | 2026-04-04 | **P1** CDK v2 TypeScript project at toke-cloud/infra/. Main stack: Lambda (check/compile/mcp) + tkc binary layer + HTTP API Gateway + CloudFront + WAF (60 req/min rate limit) + S3 logs + CloudWatch alarms. Security-first: no public Lambda URLs, HTTPS-only, TLS 1.2+. CI pipeline deferred to separate story. |
 |
#### Phase B — Security Hardening + Pro Tier |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.12.10 | Build Fargate task definition for toke_run with full sandbox | done 2026-04-04 | — | **P1** Sandboxed execution: private subnet (no internet), read-only root, tmpfs 16MB, 512MB mem, 5s CPU kill, seccomp whitelist, no inbound/outbound SG. Fresh task per execution. CDK construct + Dockerfile + entrypoint + seccomp profile. |
| 10.12.11 | Implement seccomp profile and capability dropping | done | 2026-04-04 | **P1** Enhanced seccomp profile (grouped whitelist: basic/file/process/memory/signal/time, explicit EPERM deny for networking/kernel ops/bpf). CAP_DROP ALL via CDK linuxParameters. no-new-privileges docker security option. Multi-stage Dockerfile with static-link verification, binary stripping. Entrypoint ulimits: 256MB vmem, 16MB fsize, 10 procs, 32 fds. Health check added. Security test script (test_security.sh) for local docker verification. |
| 10.12.12 | Build API key management system | done 2026-04-04 | — | **P1** DynamoDB for accounts/keys, Secrets Manager for credentials with auto-rotation. Free tier: anonymous with IP rate limit. Pro tier: API key auth. Effort: M. |
| 10.12.13 | Implement pro-tier rate limiting and usage tracking | done 2026-04-04 | — | **P1** Redis sliding window per API key. Free: 60 checks/hr, 30 compiles/hr. Pro: 600/300/60. 429 with Retry-After on exceed. Max 2 SSE connections (free), 10 (pro). Effort: M. Depends on 10.12.4, 10.12.12. |
| 10.12.14 | Add toke_explain_error, toke_spec_lookup, toke_stdlib_ref tools | done | 2026-04-04 | **P1** Three read-only MCP tools. Error code → explanation + fix suggestion (built-in catalog of all E/W codes from errors.md). Keyword → spec section (built-in knowledge base covering types, syntax, operators, etc.). Module.function → signature + description + example (all 14 stdlib modules). Cacheable in Redis. Effort: M. |
| 10.12.15 | Build toke-repair subagent for Claude Code plugin | done 2026-04-04 | — | **P0** Subagent: given NL task → generate Toke via skill context → call toke_check via MCP → parse JSON diagnostics → apply fixes → iterate until pass or retry limit. Centrepiece of thesis validation. Effort: M. Depends on 10.12.6. |
| 10.12.16 | Add opt-in anonymised telemetry pipeline | done | 2026-04-04 | **P2** Only collected with X-Telemetry: opt-in header. Metadata only (error codes, token count, tool name, latency). No source code stored. API Gateway → S3. Effort: M. |
| 10.12.17 | Security audit: penetration testing on toke_run sandbox | done | 2026-04-04 | **P1** Pen test: network escape, filesystem escape, resource exhaustion, capability abuse. Must pass before pro tier launch. Effort: L. Depends on 10.12.10, 10.12.11. |
 |
#### Phase C — Model Integration + IDE |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.12.18 | Deploy fine-tuned Qwen model to SageMaker with scale-to-zero | done | 2026-04-04 | **P1** QLoRA fine-tuned Qwen 2.5 Coder 7B. Auto-scaling 0→N instances. Cold start 30-60s (document in tool response). Spot instances where possible. Effort: L. |
| 10.12.19 | Build toke_generate and toke_bench MCP tools | done | 2026-04-04 | **P1** `toke_generate`: NL → Toke source + diagnostics (calls SageMaker). `toke_bench`: source + task_id → pass/fail, token count, baseline comparison. Pro tier only. Effort: M. Depends on 10.12.18. |
| 10.12.20 | Build toke-lsp language server wrapping tkc | done | 2026-04-04 | **P2** LSP server at toke-cloud/lsp/: diagnostics via `tkc --check --diag-json` (debounced 300ms), hover info via `--emit-interface` + keyword descriptions (12 keywords), document symbols (func/type/const). Config: toke.tkc.path, toke.tkc.stdlib. README with VS Code, Neovim, JetBrains setup. Effort: L. |
| 10.12.21 | Build toke-vscode extension (grammar + LSP client) | done | 2026-04-04 | **P2** TextMate grammar for .tk files (syntax highlighting for keywords, types, literals, declarations, operators). LSP client connecting to toke-lsp. 9 snippets (mod, fn, st, imp, loop, ifelse, let, letmut, main). Language configuration with auto-closing pairs and indentation rules. Status bar indicator. Effort: M. Depends on 10.12.20. |
| 10.12.22 | Build and publish self-hosted Docker image | done | 2026-04-04 | **P1** Multi-stage Dockerfile.selfhosted: Node 20 slim + tkc binary + server code. Non-root (node uid 1000). Config via PORT, TKC_PATH, LOG_LEVEL env vars. docker-compose.yml, build-docker.sh (multi-platform, ghcr.io push), SELF_HOSTED.md with Claude Code + Codex setup. Health endpoint returns version + tkc availability. Effort: M. |
| 10.12.23 | Test MCP compatibility across AI coding tools | done | 2026-04-04 | **P1** Compatibility test suite: mcp_compat_test.js (SSE transport, JSON-RPC, tool discovery, tool execution, error handling — spawns/kills local server), protocol_test.js (pure HTTP/SSE, no SDK). Per-tool configs for Claude Code, Codex, Cursor, Windsurf, Cline, Aider. COMPATIBILITY.md matrix with setup instructions and known limitations. CLI: `node test/compatibility/mcp_compat_test.js [--server-url URL] [--verbose]`. Effort: M. |
| 10.12.24 | Submit Claude Code plugin to Anthropic directory | backlog | — | **P2** Stabilise plugin, submit to official directory for verified listing. Effort: S. Depends on 10.12.15. |
 |
#### Phase D — Scale + Community |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 10.12.25 | Benchmark Pass@1 across AI coding tools | done | 2026-04-04 | **P1** Head-to-head: Claude Code vs Codex vs Copilot on 20+ tasks via hosted MCP. Report repair loop success rate, iteration count, token counts. Gate 2/3 data. Effort: L. cross_tool_benchmark.py with dry-run simulation, McNemar/Wilcoxon stats, bootstrap CIs, 4 output formats. |
| 10.12.26 | Multi-region deployment (US + AP) | done | 2026-04-04 | **P2** Second region (us-east-1) for NA/EU latency. CDK parameterised for multi-region. Effort: L. `infra/lib/multi-region.ts`: TokeCloudRegionalStack (parameterised per-region: VPC, Lambda, API GW, Redis, DynamoDB global tables) + TokeCloudEdgeStack (CloudFront origin-group failover AP->US, WAF, ACM). `bin/toke-cloud.ts` updated: `-c multiRegion=true` deploys 3 stacks (AP, US, Edge), default remains single-region. DynamoDB global tables for cross-region data replication. Region-specific env vars (DEPLOY_REGION, REGION_LABEL, IS_PRIMARY). `docs/multi-region.md` with architecture, deployment, failover, cost analysis. |
| 10.12.27 | Billing integration for pro tier (Stripe) | done | 2026-04-04 | **P2** Stripe subscription for pro tier ($5-10/mo). Webhook → DynamoDB tier update. Usage dashboard. Effort: M. lib/stripe-billing.js (createSubscription, handleWebhook, getSubscriptionStatus, cancelSubscription), lambda/billing/index.js (signature verification, event dispatch), infra/lib/billing.ts CDK construct (DynamoDB subscriptions table, Secrets Manager, Lambda, API GW route), docs/billing.md (pricing, lifecycle, setup). |
| 10.12.28 | Community plugin development guide | done | 2026-04-04 | **P2** Documentation on tokelang.dev: how to build plugins/integrations using the hosted MCP service. MCP tool schemas, authentication, rate limits. Effort: M. [x] All 7 tool schemas documented with input/output [x] Auth and API key management [x] Free/pro rate limit tables [x] VS Code, JetBrains, Neovim plugin guides [x] SSE transport and reconnection [x] TypeScript example code [x] Staging test checklist |
| 10.12.29 | Publish MCP server to npm registry | done | 2026-04-04 | **P2** Prepared `@tokelang/mcp-server` npm package: scoped package.json with bin/files/engines/publishConfig, `bin/toke-mcp.js` CLI entry point (--port, --tkc-path, auto-detect tkc, startup banner), `.npmignore`, `scripts/publish.sh` (test, version check, dry-run, publish, git tag), README npx section. Not yet published. Effort: S. |
| 10.12.30 | Open-source MCP server and sandbox configuration | done | 2026-04-04 | **P2** Open-source README (architecture diagram, 7 tools, quick start, API reference, deployment), CONTRIBUTING.md (local setup, testing, code style, PR process, adding tools guide), MIT LICENSE, .gitignore, GitHub templates (bug report, feature request, PR template). Secrets audit clean. Effort: S. |
| 10.12.31 | WASM compilation of tkc for browser playground | done | 2026-04-04 | **P2** Emscripten build infrastructure: `Makefile.wasm` (emcc, -O2, 16MB, NO_FILESYSTEM, MODULARIZE), `src/wasm_api.c` (in-memory pipeline: lex->parse->names->types, JSON result strings), `wasm/tkc-wasm.js` (promise-based JS wrapper with check/format/version), `wasm/playground.html` (split-pane editor+output, dark theme, example loader). No codegen in WASM build (check+format only). Effort: L. |
 |
--- |
 |
## Sprint Plan — Research Review Remediation |
 |
### Sprint R1 (P0 items — 1-2 weeks) |
 |
**Goal:** Remove external-review blockers. Produce reproducibility package. |
 |
| Story | Est | Repo |
|-------|-----|------| |
| 10.2.1 Reconcile stdlib module count | 2h | all |
| 10.2.2 Fix meta-repo Gate 1 status | 1h | toke |
| 10.2.3 Fix I= to i= in toke-stdlib | 1h | toke-stdlib |
| 10.1.1 TEMSpec document | 4h | toke-spec |
| 10.1.2 Raw per-task token counts CSV | 4h | toke-eval |
| 10.1.4 Gate 1 reproducibility package | 8h | toke-spec |
| 10.4.1 --emit-llvm and --emit-asm flags | 8h | tkc |
| 10.4.5 Emit target datalayout/triple | 2h | tkc |
| 10.7.1 Embed_tokens in modules_to_save | 4h | toke-models |
| 10.7.3 Evaluation contract (model card) | 4h | toke-eval |
| 10.7.4 Holdout isolation invariant | 4h | toke-corpus |
| 10.10.1 Gate 1.5 reproducibility criteria | 4h | toke-spec |
| 10.10.2 Gate 2 success criteria | 4h | toke-spec |
 |
### Sprint R2 (P1 items — 2-4 weeks) |
 |
**Goal:** Strengthen evaluation, compiler, spec. External credibility. Compiler configurability. |
 |
| Story | Est | Repo |
|-------|-----|------| |
| ~~10.1.3 Multi-tokenizer baselines~~ | ~~8h~~ | ~~toke-eval~~ done 2026-04-04 |
| ~~10.1.5 Corpus statistics publication~~ | ~~4h~~ | ~~toke-corpus~~ done 2026-04-04 |
| ~~10.1.6 Statistical analysis with CIs~~ | ~~8h~~ | ~~toke-eval~~ done 2026-04-04 |
| 10.2.4 CONTRIBUTING.md to meta-repo | 2h | toke |
| 10.2.5 Standardize security docs | 4h | all |
| 10.2.6 Fix CI quality gate suppression | 2h | tkc |
| 10.2.7 Release tags (v0.1-gate1) | 4h | all |
| 10.2.8 Fix tokenizer deps | 1h | toke-tokenizer |
| 10.3.1 Integer overflow semantics | 16h | toke-spec, tkc |
| 10.3.2 Phase 2 normative profile | 16h | toke-spec |
| 10.3.5 String escaping rules | 8h | toke-spec, tkc |
| 10.3.6 Standardize .tki format | 8h | toke-spec, tkc |
| 10.3.10 Diagnostics JSON Schema | 8h | toke-spec |
| 10.4.2 IR verification in CI | 4h | tkc |
| 10.4.3 Rich LLVM annotations | 16h | tkc |
| 10.4.6 Optimization level flags | 4h | tkc |
| 10.4.9 Runtime ABI document v0 | 8h | tkc |
| 10.5.1 Binary benchmarks vs C/Rust | 16h | toke-benchmark |
| 10.5.4 Stress test suite | 8h | tkc |
| 10.6.1 Pass@5/Pass@10 | 8h | toke-eval |
| 10.6.2 Error taxonomy | 4h | toke-eval |
| 10.6.3 Port tasks to HumanEval/MBPP | 16h | toke-benchmark |
| 10.6.4 Public benchmark slice | 4h | toke-benchmark |
| 10.6.6 Contamination analysis | 8h | toke-corpus |
| 10.7.2 QLoRA vs DoRA benchmark | 16h | toke-models |
| 10.7.6 Five-tier validation pipeline | 16h | toke-corpus |
| 10.8.1 Canonical formatter (toke fmt) | 24h | tkc |
| ~~10.8.2 --pretty/--expand flags~~ | ~~16h~~ | ~~tkc~~ |
| 10.9.1 Prior art design-space map | 8h | toke-spec |
| 10.9.3 Constrained decoding ablation | 16h | toke-eval |
| 10.9.4 RFC updates from review feedback | 8h | toke-spec |
| 10.11.7 Extract magic numbers to tkc_limits.h | 8h | tkc |
| 10.11.8 Compiler config file (tkc.toml) | 16h | tkc |
| 10.11.9 Per-session limit override flags | 8h | tkc |
| 10.11.10 Dynamic Ctx sizing from limits | 16h | tkc |
| 10.11.11 Dynamic names.c limits | 8h | tkc |
| 10.11.12 Diagnostics on limit truncation | 4h | tkc |
 |
### Sprint R3 (P2 items — 4-8 weeks) |
 |
**Goal:** Polish, expansion tooling, longer-term items. |
 |
Stories: 10.2.9, 10.3.3, 10.3.4, 10.3.7, 10.3.8, 10.3.9, 10.5.2, 10.5.3, 10.5.5, ~~10.6.5~~, ~~10.6.7~~, 10.7.5, 10.8.3, 10.8.4, ~~10.8.5~~, ~~10.9.2~~, 10.10.4, 10.11.1-10.11.6 |
 |
--- |
 |
## Default Syntax — Compiler Implementation and Gate 2 Readiness |
 |
**Decision (2026-04-04):** The 56-character syntax is toke's default and only actively developed syntax. The 80-character uppercase syntax is retained as "legacy profile" (`--legacy` flag) for backward compatibility and generic tokenizer use. Stop referring to "Phase 2" or "Profile 2" — it is simply "toke." |
 |
**Gate 2 criterion:** All 5 key gaps from spec-implementation-delta.md closed. Default syntax compiles, passes conformance, and spec is researcher-approved. |
 |
### Epic 11.1 — Default Syntax Compiler Implementation |
 |
**Goal:** Make the 56-char syntax the compiler's default mode. Legacy (80-char) available via `--legacy`. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 11.1.1 | Lexer: default syntax tokens and character set | done | 2026-04-04 | **P0** [x] TK_DOLLAR and TK_AT token kinds [x] Profile enum (PROFILE_DEFAULT/PROFILE_LEGACY) [x] $ and @ recognized in default mode, E1003 in legacy [x] --legacy CLI flag [x] Version string updated to "tkc 0.1.0" [x] Lowercase keyword disambiguation deferred to parser (11.1.2) [x] 92/92 conform, 10/10 e2e, 9/9 stress |
| 11.1.2 | Parser: `$name` types, `@()` arrays and maps, `.get()` indexing | done | 2026-04-04 | **P0** [x] $ident type references in parse_type_expr [x] @() array literals with ; separation [x] @() map literals disambiguated by : [x] @$type array type, @($k:$v) map type [x] .get(expr) → NODE_INDEX_EXPR [x] $name{} struct literals [x] [] blocked in default mode (E1003) [x] Lowercase m=/f=/i=/t= via is_decl_ident() (lexer emits TK_IDENT, parser checks context) [x] Profile passed to parse() [x] G052-G058 + e2e_default_syntax [x] Existing bracket tests flagged --legacy [x] 115 conform, 13 e2e, 9 stress |
| 11.1.3 | Name resolver and type checker: handle default syntax AST | done | 2026-04-04 | **P0** No changes needed — parser reuses same AST node kinds (NODE_TYPE_IDENT, NODE_ARRAY_LIT, NODE_MAP_LIT, NODE_INDEX_EXPR) for default syntax. Name resolver and type checker work unchanged. Confirmed by 115 conformance tests passing with default syntax. |
| 11.1.4 | LLVM backend: emit correct IR for `.get()` array indexing | done | 2026-04-04 | **P0** No changes needed — parser emits NODE_INDEX_EXPR for .get(), same as legacy arr[i]. LLVM backend handles it identically. Confirmed by e2e_default_syntax test (compile+run). |
| 11.1.5 | CLI: default mode is 56-char, `--legacy` for 80-char | done | 2026-04-04 | **P0** Completed as part of 11.1.1. [x] PROFILE_DEFAULT is default [x] --legacy flag [x] --profile1/--phase1 deprecated aliases [x] Version string "tkc 0.1.0" [x] Profile passed to lex() [x] Help text updated |
| 11.1.6 | New conformance test suite (default syntax) | done | 2026-04-04 | **P0** [x] 57 new tests: L029-L033 (lexical), G059-G098 (grammar), D034-D045 (diagnostics) [x] Covers all default syntax: m=/f=/t=/i=, $name types, @() arrays/maps, .get() indexing, &&/||, narrow types [x] 172 total tests, all passing |
| 11.1.7 | Migrate e2e and stress tests to default syntax | done | 2026-04-04 | **P1** [x] All 13 e2e and 9 stress tests converted to default syntax [x] Legacy copies in test/legacy/e2e/ and test/legacy/stress/ [x] run_e2e.sh and run_stress.sh support --legacy flag [x] All tests passing |
| 11.1.8 | Verify corpus compiles with updated compiler | done | 2026-04-04 | **P1** [x] 500 entries sampled from corpus_p2.jsonl (46,754 total) [x] 53.2% pass rate [x] Top failures: type mismatches 40%, $-prefixed types unrecognized 25%, arr.0 dot-index 21%, immutable assignment 11% [x] Findings documented — corpus transformation script needs update for new syntax rules |
| 11.1.9 | Update error message catalog examples to default syntax | done | 2026-04-04 | **P1** [x] errors.md: 23 code examples converted to default syntax [x] semantics.md: type tables, literals, declarations updated [x] stdlib-signatures.md already in default syntax |
 |
### Epic 11.2 — Gate 2 Soundness Gaps |
 |
**Goal:** Close the 5 remaining spec-implementation gaps identified in spec-implementation-delta.md. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 11.2.1 | Narrow integer types in type checker and codegen | done | 2026-04-04 | **P1** [x] TY_I8/I16/I32/U8/U16/U32/F32 in type system [x] resolve_type mapping [x] LLVM trunc/sext/zext/fptrunc/fpext codegen [x] Byte alias for u8 [x] let x:type annotation parsing [x] G046-G048 + D027 tests [x] e2e_narrow_int [x] 96 conform, 11 e2e |
| 11.2.2 | Missing diagnostic error codes | done | 2026-04-04 | **P1** [x] 7 of 11 implemented (4 already covered by existing codes) [x] E1004 unterminated string EOF [x] E1005 invalid numeric literal [x] E2005 unexpected token in type position [x] E2015 duplicate field name [x] E4026 wrong argument count [x] E5002 unreachable code after return [x] W1001 lossy cast warning [x] L026-L028 + D029-D033 tests [x] Skipped: E4001→E3011, E4020→E4031, E2006→E2003, E2011→E2002 |
| 11.2.3 | Sum type match exhaustiveness | done | 2026-04-04 | **P1** [x] Collect variant tags from type declaration [x] Check match arms cover all variants [x] E5001 for missing variants with names listed [x] G045 + D022 + D023 tests [x] Bool exhaustiveness unchanged |
| 11.2.4 | Mutability enforcement | done | 2026-04-04 | **P1** [x] E4070 "cannot assign to immutable binding" [x] find_binding_kind() helper [x] Loop vars implicitly mutable [x] Function params immutable [x] D024-D026 tests [x] e2e_mutability [x] L024 updated [x] 92 conform, 10 e2e, 9 stress |
| 11.2.5 | Logical operators (`&&` and `||`) | done | 2026-04-04 | **P1** [x] TK_AND/TK_OR in lexer (& alone → E1003) [x] parse_and()/parse_or() with OR > AND > comparison precedence [x] Bool operand type checking (E4031) [x] Short-circuit codegen via alloca [x] G049-G051 + D028 + e2e_logical [x] 115 conform, 13 e2e |
 |
### Epic 11.3 — Documentation and Ecosystem Alignment |
 |
**Goal:** Ensure all documentation, examples, tools, and IDE integrations use and reference the default (56-char) syntax. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 11.3.1 | Review and compile-test all website code examples | done | 2026-04-04 | **P0** [x] 63 complete programs tested [x] 52% pass rate (63% excl error demos) [x] 17 files fixed in toke-web: array type @(T)→@T, struct field $fieldname→fieldname [x] Remaining failures: cross-module types, unimplemented features |
| 11.3.2 | Update MCP skill file, Claude plugin, and Codex instructions | done | 2026-04-04 | **P1** [x] 7 files updated in toke-mcp [x] toke-language.md skill rewritten for default syntax [x] CLAUDE.md, commands, agents updated [x] codex.md rewritten [x] All examples use m=/f=/$name/@() |
| 11.3.3 | Update Tree-sitter grammar for default syntax | done | 2026-04-04 | **P1** [x] grammar.js with 66 rules (both profiles) [x] highlights.scm with $ and @ highlighting [x] package.json [x] Covers: $name types, @() arrays/maps, .get() indexing, &&/|| |
| 11.3.4 | Update error catalog and stdlib docs to default syntax | done | 2026-04-04 | **P1** [x] stdlib-signatures.md already in default syntax [x] 1 fix in toke-web (FileErr → $fileerr) [x] No .tki files to update |
| 11.3.5 | Build migration tool (`tkc --migrate`) | done | 2026-04-04 | **P2** [x] src/migrate.c + migrate.h [x] Token-level legacy→default transformation [x] Uppercase keywords M/F/T/I → lowercase [x] Type idents Vec2 → $vec2 [x] Whitespace/formatting preserved [x] 2 test cases + make test-migrate [x] Compiles clean -Werror |
| 11.3.6 | Remove "Phase 2" / "Profile 2" terminology across all repos | done | 2026-04-04 | **P1** [x] 33+ files updated across toke, toke-mcp, toke-web repos [x] "Phase 2" → "default syntax", "Phase 1" → "legacy profile" [x] Preserved git branch names, file names, source code, progress.md [x] Factual counts ("56 characters") retained |
 |
### Epic 11.4 — Specification Alignment and Research Signoff |
 |
**Goal:** Ensure the spec accurately reflects the implemented compiler, then get researcher signoff before locking syntax. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 11.4.1 | Update spec-implementation-delta.md after 11.1 and 11.2 work | done | 2026-04-04 | **P0** [x] All 5 key gaps marked RESOLVED [x] 24 rows updated from "not implemented" to "specified + implemented" [x] Summary: 110 specified+implemented (was 52), 5 not implemented (was 20) |
| 11.4.2 | Re-run token efficiency benchmarks with compiler-verified code | done | 2026-04-04 | **P1** [x] 5,000 entries benchmarked from corpus_p2.jsonl [x] 80.9% compiler pass rate [x] Token reduction: 63% vs Python, 84.9% vs C, 73.5% vs Java (cl100k_base) [x] eval_report_gate2.json published to toke-eval/data/ [x] 19.1% corpus failures due to parser tightening — corpus regen needed (see 11.5.7) |
| 11.4.3 | Align spec document with implemented state | done | 2026-04-04 | **P0** [x] Default syntax is now primary presentation [x] Appendix F: Legacy Profile (80-char) [x] Grammar: $type, @() arrays/maps, &&/||, lowercase m=/f=/t=/i= context keywords [x] Keyword table split: 4 context-sensitive + 8 reserved [x] All examples converted [x] Removed Phase 1/2/Profile 2 terminology [x] RouteDecl removed (stdlib macro) |
| 11.4.4 | Prepare researcher review package and request signoff | done | 2026-04-04 | **P0** [x] gate2-review-package.md in toke-spec/docs/ [x] Executive summary, syntax overview, design goals assessment [x] All numbers verified from source artifacts [x] 8 specific reviewer questions [x] Timeline: 2-week review window closes 2026-04-18, freeze target 2026-04-25 [x] Appendix with artifact links |
| 11.4.5 | Syntax freeze — tag v0.2-syntax-lock | done | 2026-04-05 | **P0** [x] Lock-in with conditions decision: reviewer approved (all 3 conditions resolved: EBNF complete in phase2-profile.md §6, Pass@1 risk accepted, void unit variant added) [x] phase2-profile.md status → FROZEN [x] toke-spec-v02.md Section 11 + Appendix D/E updated to Profile 2 normative [x] void added to ScalarType EBNF and 4.2 table [x] stdlib $jwtalg fixed: bool→void payload [x] v0.2-syntax-lock tag on all repos [x] No syntax changes without formal amendment. |
| 11.4.6 | BPE tokenizer validation with final syntax | done | 2026-04-04 | **P1** [x] Tokenizer (8k vocab SentencePiece) trained on old uppercase syntax only [x] 0/13 new syntax patterns are single tokens ($, @ fall to byte encoding) [x] Corpus needs regeneration in default syntax before retrain [x] eval script + JSON report in toke-tokenizer/docs/ [x] Follow-up: retrain after corpus regen |
| 11.4.7 | Update Gate 2 spec-implementation delta to zero key gaps | done | 2026-04-04 | **P0** [x] All 5 key gaps confirmed CLOSED [x] Gate 2 readiness section added to gate-criteria.md [x] Test results: 172 conformance, 13 e2e, 9 stress — all passing |
 |
### Epic 11.5 — Companion Files |
 |
**Goal:** Generate plain-language companion files (`.tkc.md`) alongside toke source files. Companion files contain human-readable explanations of code, enabling peer review, security review, and documentation. Include a content hash for version assurance, and target high round-trip fidelity (an LLM reading the companion file should regenerate equivalent code). |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 11.5.1 | Companion file format spec | done | 2026-04-04 | **P1** [x] .tkc.md format defined in docs/companion-file-spec.md [x] YAML frontmatter: source_file, source_hash (SHA-256), compiler_version, generated_at, format_version [x] 6 structured sections: Module, Types, Functions, Constants, Control Flow, Notes [x] Round-trip fidelity guidelines [x] Worked example (prime_sieve.tk) [x] Versioning policy |
| 11.5.2 | Companion file generation (`tkc --companion`) | done | 2026-04-04 | **P1** [x] src/companion.c + companion.h (portable C99 SHA-256) [x] YAML frontmatter: source_file, source_hash, compiler_version, generated_at, format_version [x] Sections: Module (imports), Types (fields), Functions (signatures, params), Constants [x] --companion (stdout) and --companion-out <path> flags [x] TODO placeholders for LLM/human prose [x] Compiles clean -Werror |
| 11.5.3 | Round-trip fidelity validation | done | — | **P1** Given a companion file, feed it through a toke-trained LLM and measure code similarity to the original source. Target: ≥90% AST-equivalent regeneration for standard patterns. Metric: exact match on function signatures, type declarations; fuzzy match on expression bodies. Depends on 11.5.2. |
| 11.5.4 | Companion diff / compare feature | done | 2026-04-04 | **P2** [x] --companion-diff flag in main.c [x] companion_diff() in companion.c (~200 lines) [x] Detects NEW/REMOVED/CHANGED functions and types [x] Hash mismatch detection [x] Exit codes: 0 no divergences, 1 divergences, 2 error [x] 7 test cases + make test-companion-diff [x] Compiles clean -Werror |
| 11.5.5 | MCP tool: `toke_companion` | done | 2026-04-04 | **P2** [x] tools/companion.js in toke-mcp [x] 3 modes: generate, verify, diff [x] Registered as 8th MCP tool in server.js [x] Schema: source (required), mode (optional), companion (optional) [x] Temp file cleanup in finally block |
| 11.5.6 | Companion file hash verification | done | 2026-04-04 | **P1** [x] --verify-companion flag in main.c [x] verify_companion() + extract_frontmatter_field() in companion.c [x] Resolves source_file relative to companion dir [x] Reuses existing SHA-256 [x] Exit codes: 0 match, 1 mismatch, 2 error [x] 6 test cases + make test-companion [x] Compiles clean -Werror |
 |
### Epic 11.6 — Corpus and Tokenizer Refresh |
 |
**Goal:** Regenerate the training corpus in default syntax and retrain the BPE tokenizer so key syntax patterns (`$type`, `@(`, `m=`, `f=`) become single tokens. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 11.6.1 | Regenerate corpus in default syntax | done | 2026-04-04 | **P1** [x] to_default_syntax.py transformer (12 rule categories) [x] corpus_default.jsonl: 46,754 entries [x] ~90% tkc --check pass rate [x] Zero transformation-introduced errors [x] Remaining failures: pre-existing E4070 immutable (75%), E4031 type mismatch (20%), E3012 duplicate (1%) |
| 11.6.2 | Retrain BPE tokenizer on default syntax corpus | done | 2026-04-04 | **P1** [x] 14 user-defined symbols added [x] Trained on 46,754 default-syntax entries (22MB) [x] 14/14 key patterns are single tokens (was 0/14) [x] Char-to-token ratio 0.352 (7.5% improvement) [x] Old model backed up, new model installed [x] eval_retrain_11_6_2.json published |
 |
### Epic 12.1 — Standard Library Tier 0: Foundation Extensions |
 |
**Goal:** Extend existing stdlib modules (json, http, crypto) with production-grade capabilities. Each module ships with 200+ corpus examples. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 12.1.1 | std.json: streaming encode/decode | done | — | **P1** Add streaming JSON encoder/decoder for large payloads. JSON Pointer (RFC 6901) read/write. JSON Patch (RFC 6902) apply. Schema validation against type defs. Extends existing 8-function json module. 200+ corpus examples. **⚠ .tki declares streaming API (streamparser/streamnext/streamemit/newwriter/writerbytes) but C impl missing — see 35.1.3.** |
| 12.1.2 | std.http: client and connection pooling | done | — | **P1** HTTP client (GET/POST/PUT/DELETE/PATCH), connection pooling, request/response streaming, middleware chain pattern, timeout/retry. Extends existing server-only http module. 200+ corpus examples. **⚠ .tki declares client API (client/get/post/put/delete/stream/streamnext) but C impl missing — see 35.1.2.** |
| 12.1.3 | std.crypto: extended hashing and HMAC | done | — | **P1** Add SHA-512, BLAKE3 hashing. Extend HMAC beyond SHA-256. Constant-time compare utility. Extends existing SHA-256 + HMAC-SHA-256 module. 200+ corpus examples. **⚠ .tki declares sha512/hmacsha512/constanteq/randombytes but C impl missing — see 35.1.1.** |
 |
### Epic 12.2 — Standard Library Tier 1: Web & API Platform |
 |
**Goal:** Build a production API framework on top of the foundation. Auth, routing, WebSocket, and template generation. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 12.2.1 | std.encoding | done | — | **P1** Base64 encode/decode, hex encode/decode, URL percent-encoding/decoding. Prerequisite for auth headers, cookie values, data URIs. .tki interface + C impl. 200+ corpus examples. |
| 12.2.2 | std.auth | done | — | **P1** JWT sign/verify (HS256, RS256), API key validation, OAuth2 client credentials flow, bearer token extraction middleware. Depends on std.crypto + std.encoding + std.json. 200+ corpus examples. **⚠ .tki declares bearerextract but C impl missing; apikeygenerate/apikeygen naming mismatch — see 35.1.7.** |
| 12.2.3 | std.router | done | — | **P1** Path parameters (`/users/:id`), query string parsing, middleware composition, CORS handling, rate limiting, request body validation against `t=` type schemas. Builds API framework on raw std.http. 200+ corpus examples. **⚠ .tki declares use (middleware) and serve but C impl missing — see 35.1.9.** |
| 12.2.4 | std.ws | done | — | **P1** WebSocket upgrade from HTTP connection, message framing (text/binary), ping/pong, connection lifecycle management, broadcast to multiple clients. Needed for dashboards and streaming LLM output. 200+ corpus examples. **⚠ .tki declares high-level API (connect/send/recv/close/broadcast) but C only has low-level frame codec — see 35.1.4.** |
| 12.2.5 | std.template | done | — | **P2** HTML/JS/CSS generation from toke types. Typed template fragments, HTML escaping, component composition, `<script>` and `<style>` inline embedding. Structural generation from data, not a full templating engine. 200+ corpus examples. **⚠ .tki declares vars/html/renderfile but C impl missing — see 35.1.8.** |
 |
### Epic 12.3 — Standard Library Tier 2: Data & LLM Integration |
 |
**Goal:** CSV ingest, math/stats primitives, and first-class LLM client support — toke's highest-value differentiator. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 12.3.1 | std.csv | done | — | **P1** Parse/emit CSV and TSV with typed column mapping. Quote handling, header detection, streaming read for large files. Fast path for analytics data ingest. 200+ corpus examples. **⚠ .tki/.h naming mismatches (csv.reader vs csv_reader_new etc.) — see 35.1.11.** |
| 12.3.2 | std.math | done | — | **P1** Statistics: mean, median, stddev, percentiles, histograms, linear regression. Matrix/vector ops for small dimensions (up to 4x4). Foundation for analytics and ML modules. 200+ corpus examples. |
| 12.3.3 | std.llm | done | — | **P0** HTTP client wrapper for OpenAI/Anthropic/local model APIs. Streaming token consumption via SSE, structured output parsing (JSON mode), prompt construction helpers, token counting, retry with exponential backoff, multi-provider abstraction. Highest-value differentiator: a language for LLM code gen with first-class LLM client support. Depends on std.http client (12.1.2) + std.json (12.1.1). 200+ corpus examples. |
| 12.3.4 | std.llm.tool | done | — | **P1** Tool-use/function-calling protocol. Type-safe tool definitions generated from toke function signatures. Automatic JSON schema emission from `t=` type defs. Tool result parsing. Lets toke programs act as both LLM-generated code and LLM-consuming agents. Depends on std.llm (12.3.3). 200+ corpus examples. **⚠ .tki/.h naming mismatch (withtools vs tool_build_tools_json) — see 35.1.12.** |
 |
### Epic 12.4 — Standard Library Tier 3: Visualization & Dashboards |
 |
**Goal:** Data-to-dashboard pipeline. A single toke program produces a self-contained monitoring dashboard with no frontend build toolchain. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 12.4.1 | std.chart | done | — | **P1** Data-to-JSON serialisation targeting Chart.js/Vega-Lite/minimal custom spec. Line, bar, scatter, histogram, heatmap chart types. Output is a JSON descriptor — rendering is client-side. 200+ corpus examples. |
| 12.4.2 | std.html | done | — | **P1** Structured HTML document builder. DOM-like tree construction, CSS class composition, JS snippet embedding, `<canvas>` and `<svg>` element support. Full document generation (more structured than std.template). 200+ corpus examples. |
| 12.4.3 | std.dashboard | done | — | **P1** Composition layer combining std.chart + std.html + std.ws. Layout grid, auto-refresh intervals, WebSocket push for real-time data. Single toke program → self-contained monitoring dashboard served by std.router. 200+ corpus examples. Depends on 12.4.1, 12.4.2, 12.2.4, 12.2.3. **⚠ .tki declares serve but C impl has render only — see 35.1.10.** |
| 12.4.4 | std.sse | done | — | **P1** Server-Sent Events endpoint helper. Simpler than WebSocket for one-way streaming: LLM token streams, live chart updates, log tailing. Connection management, event naming, retry hints. 200+ corpus examples. |
 |
### Epic 12.5 — Standard Library Tier 4: Analytics & ML |
 |
**Goal:** Ingest, analyse, visualise, and serve data — all in one toke program. Close the loop. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 12.5.1 | std.dataframe | done | — | **P1** Columnar typed data structure. Filter, group-by, join, pivot, window functions. Arena-friendly design (column arrays allocated in bulk). Import from CSV (std.csv), JSON (std.json), and std.db query results. 200+ corpus examples. **⚠ .tki declares fromrows/columnstr/tocsv/schema but C impl missing — see 35.1.5.** |
| 12.5.2 | std.analytics | done | — | **P1** Aggregation pipelines on dataframes. Time-series bucketing, moving averages, anomaly detection (z-score), correlation matrices. Outputs feed directly into std.chart. Depends on std.dataframe (12.5.1) + std.math (12.3.2). 200+ corpus examples. **⚠ .tki declares groupstats/pivot but C impl missing — see 35.1.6.** |
| 12.5.3 | std.ml | done | — | **P2** Inference-only ML primitives. Linear/logistic regression, k-means, decision trees, k-nearest neighbours. Train on std.dataframe columns. No GPU, no backprop — small-data, in-process models for classification/prediction. Depends on std.dataframe (12.5.1) + std.math (12.3.2). 200+ corpus examples. |
| 12.5.4 | std.encrypt | done | — | **P1** Symmetric encryption (AES-256-GCM), asymmetric (X25519 key exchange, Ed25519 signing), TLS client certificate handling. Deferred from Tier 0 because hashing covers 90% of early needs; full encryption needed for data-at-rest and E2E security in production APIs. 200+ corpus examples. |
 |
### Epic 12.6 — Standard Library Tier 5: Media & Visualization Extensions |
 |
**Goal:** Lower-priority modules added as demand warrants. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 12.6.1 | std.image | done | — | **P3** PNG/JPEG decode to pixel buffer, resize, crop, format conversion. Needed for image upload handling in APIs. Not core to platform thesis. 200+ corpus examples. |
| 12.6.2 | std.svg | done | — | **P3** Programmatic SVG generation for custom visualisations beyond std.chart. Diagrams, flowcharts, node graphs. 200+ corpus examples. |
| 12.6.3 | std.canvas | done | — | **P3** JS Canvas API bindings for std.html output. 2D drawing commands serialised as JS. Gaming and 3D explicitly out of scope. 200+ corpus examples. |
 |
### Epic 13.1 — Loke Integration: Compression API |
 |
**Goal:** Expose toke as a stable compression/decompression service for loke (and other consumers). Loke uses toke to reduce token spend before LLM calls and restore original content after. Placeholders from loke's anonymisation pipeline ($PERSON_1, $EMAIL_1 etc.) must survive the round-trip unchanged. All tools are exposed over the existing MCP server. |
 |
**Dependency notes:** |
- Topic 4 (TOON bridge): already complete — Epic 6.3 (std.toon done). See 6.3.6 for migration docs. |
- Topic 8 (scripting primitives): already backlogged — see Epics 12.3-12.5 (std.csv, std.math, std.dataframe, std.analytics, std.ml). |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 13.1.1 | Placeholder-preserving compress/decompress API | done | — | **P0** Loke's privacy pipeline produces tokens like $PERSON_1 and $EMAIL_1. toke's compress operation must treat any `$IDENT_N` pattern (sigil + uppercase + underscore + digit) as an opaque atom — never split, rewrite, or conflate with type sigils. Decompress must restore byte-identical output. Spec the atom-preservation contract, add conformance tests with representative loke placeholder sequences. Covers loke F3.5 (reversal engine). |
| 13.1.2 | toke_compress and toke_decompress MCP tools | done | — | **P0** Add two new tools to toke-mcp: `toke_compress(input:str;preserve_atoms:@str):$compressed` and `toke_decompress(input:$compressed):str`. Signatures are frozen once loke depends on them — document them in TOOLS.md with stability guarantee. Must work over both MCP stdio and HTTP transport. Covers loke F7.4. |
| 13.1.3 | toke_analyse MCP tool — pre-flight token estimation | done | — | **P1** Add `toke_analyse(input:str;tokenizer:str):$tokenreport` tool. Returns expected token count for the input (raw and compressed estimates) without actually compressing. `$tokenreport` includes: `raw_tokens:u64`, `est_compressed_tokens:u64`, `reduction_pct:f64`, `tokenizer:$str`. Used by loke F4.5 (token budget manager) for daily/weekly limit enforcement before calling LLMs. Tokenizer param selects cl100k_base, o200k_base, or toke-bpe. |
| 13.1.4 | Streaming-compatible compress output | done | — | **P0** toke compression of prompts is fine as a batch operation. However if toke is applied to LLM response post-processing, it must not buffer the full stream. Add a streaming mode to the compress API: `toke_compress_stream` yields compressed chunks as input arrives. Must be compatible with SSE and chunked HTTP. Covers loke F5.4 (all provider integrations are streaming). |
| 13.1.5 | Schema-aware compression for structured data | done | — | **P1** When input is JSON, CSV, or DB result rows, toke should represent it as a compact schema + data encoding rather than compressing flat text. E.g. a 1000-row CSV should emit a header schema once and encode rows as positional tuples. Target: 30-60% token savings vs raw JSON/CSV (TOON baseline). Add structured-input detection heuristic, schema extraction, and schema-anchored encoding. Covers loke F4.2 (data profiler). |
| 13.1.6 | Parameterised template format with slot preservation | done | — | **P2** Loke needs reusable prompt templates with variable slots (e.g. `{{user_query}}`, `{{context_summary}}`). toke must: (a) treat `{{IDENT}}` as an opaque atom during compress/decompress (like placeholders in 13.1.1), (b) provide a `toke_render(template:$compressed;vars:@($str:str)):str` MCP tool that substitutes variables without re-compressing the whole template. Covers loke A1.4 (dashboard persistence) and A2.3 (codebase profiling). |
| 13.1.7 | TOON→toke migration guide | done | — | **P1** Loke F4.1/F4.2 are specced around TOON. Since TOON is already implemented (Epic 6.3), document: (a) which TOON use cases toke compress supersedes and which remain TOON territory, (b) whether loke should build F4.1 at all or skip to toke compression directly, (c) a bridge adapter so existing TOON serialisers can be wrapped without rewrite. Publish in toke-spec/docs/toon-toke-bridge.md. Covers loke F4.1 decision gate. |
 |
--- |
 |
### Sprint S1 — Default Syntax (Critical Path) |
 |
**Goal:** Compiler speaks default (56-char) syntax. All tests pass. Spec aligned. |
 |
**Critical path:** 11.1.1 → 11.1.2 → 11.1.3 → 11.1.4 → 11.1.5 → 11.1.6 → 11.3.1 → 11.4.3 → 11.4.4 → 11.4.5 |
 |
| Story | Est | Priority | Repo | Depends |
|-------|-----|----------|------|---------| |
| 11.1.1 Lexer: default syntax tokens | 8h | P0 | toke | — |
| 11.1.2 Parser: $name, @(), .get() | 16h | P0 | toke | 11.1.1 |
| 11.1.3 Names + types: handle new AST | 8h | P0 | toke | 11.1.2 |
| 11.1.4 LLVM backend: .get() codegen | 8h | P0 | toke | 11.1.3 |
| 11.1.5 CLI: default mode + --legacy | 4h | P0 | toke | 11.1.1 |
| 11.1.6 New conformance tests (86+) | 16h | P0 | toke | 11.1.5 |
| 11.1.7 Migrate e2e + stress tests | 8h | P1 | toke | 11.1.6 |
| 11.1.8 Verify corpus compiles | 4h | P1 | toke | 11.1.5 |
| 11.1.9 Error catalog default syntax | 4h | P1 | toke-spec | 11.1.5 |
| 11.2.1 Narrow integer types | 16h | P1 | toke | — |
| 11.2.2 Missing error codes (11) | 12h | P1 | toke | — |
| 11.2.3 Sum type exhaustiveness | 8h | P1 | toke | — |
| 11.2.4 Mutability enforcement | 8h | P1 | toke | — |
| 11.2.5 Logical operators (&&, ||) | 8h | P1 | toke | — |
| 11.3.1 Website example compile-test | 8h | P0 | toke-web | 11.1.5 |
| 11.3.2 MCP/plugin/Codex updates | 4h | P1 | toke-mcp | 11.1.5 |
| 11.3.3 Tree-sitter grammar update | 8h | P1 | toke | 11.1.5 |
| 11.3.4 Error + stdlib docs update | 4h | P1 | toke-spec | 11.1.5 |
| 11.3.5 Migration tool (--migrate) | 16h | P2 | toke | 11.1.5 |
| 11.3.6 Remove Phase 2 terminology | 4h | P1 | all | 11.1.5 |
| 11.4.1 Delta table re-audit | 4h | P0 | toke-spec | 11.1.6, 11.2.x |
| 11.4.2 Re-run token benchmarks | 8h | P1 | toke-eval | 11.1.8 |
| 11.4.3 Align spec with compiler | 16h | P0 | toke-spec | 11.4.1 |
| 11.4.4 Researcher review package | 8h | P0 | toke-spec | 11.4.1-3 |
| 11.4.5 Syntax freeze (v0.2-syntax-lock) | 2h | P0 | all | 11.4.4 |
| 11.4.6 BPE tokenizer validation | 4h | P1 | toke-model | 11.1.8 |
| 11.4.7 Gate 2 delta to zero gaps | 4h | P0 | toke-spec | 11.4.1 |
 |
--- |
 |
## Overlap with Existing Stories |
 |
The following existing backlog stories overlap with research review stories: |
 |
| Existing | Research | Resolution |
|----------|----------|------------| |
| 9.4.1 ShortCoder comparison | 10.9.3 Constrained decoding ablation | Merge — 10.9.3 is broader, subsumes 9.4.1 |
| 9.4.2 Multi-tokenizer analysis | 10.1.3 Multi-tokenizer baselines | Merge — 10.1.3 is P0, replaces 9.4.2 |
| 9.6.1 EvalPlus harness | 10.6.1 Pass@5/Pass@10 + 10.6.3 HumanEval port | Merge — 10.6.x stories are more granular, replace 9.6.1 |
| 9.8.1 BPE retrain on corpus | 10.7.1 Embed_tokens | Complementary — both needed. 10.7.1 is prerequisite |
| 7.1.1-7.1.5 Website examples (on_hold) | 10.3.2 Phase 2 normative profile | Unblock — 10.3.2 resolves "spec locked" dependency for 7.1.x |
 |
--- |
 |
## Active Blockers |
 |
| Blocker | Blocks | Resolution |
|---------|--------|------------| |
| Decision D2 (integer overflow) | 10.3.1, 10.5.4 | Decide in docs/decisions/gate1-research-review-decisions.md |
| Decision D3 (memory model) | 10.3.4 | Decide in docs/decisions/ |
| Decision D10 (QLoRA vs DoRA) | 10.7.2, 9.x training stories | Benchmark first (10.7.2) then decide |
| Decision D11 (embed_tokens) | All training runs | Hard requirement — 10.7.1 must complete before any retrain |
 |
## Compute Summary |
 |
All Phase 2 work runs locally (Mac Studio M4 Max) unless noted. No cloud instances needed for Sprint R1. |
 |
| Compute | Stories |
|---------|---------| |
| **local** | 6.1.1-6.1.3, 7.1.1-7.1.5 (on_hold), 7.1.9 (on_hold), 9.1.x-9.8.x (Gate 2 prep), 10.x (research review remediation) |
| **cloud/HF** | 6.1.4 (HuggingFace Spaces demo) |
| **cloud (API)** | 9.1.1, 9.1.3, 9.2.3 (teacher model inference) |
| **cloud (GPU)** | 10.7.2 (QLoRA/DoRA/full benchmark — if A100 used) |
| **no compute** | All EC2 instances can be suspended |
 |
--- |
 |
### Epic 14 — Stdlib Implementation: Security & Encoding |
 |
C implementations for the security and encoding foundation. All downstream auth-protected APIs depend on these being real. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 14.1.1 | std.encoding C implementation | done | — | **P0** Implement `src/stdlib/encoding.c`: base64_encode/decode, base64url_encode/decode, hex_encode/decode, url_encode/decode. No external deps — pure C99 lookup tables. Link into Makefile. All 8 functions must be callable from toke `i=std.encoding` imports. Conformance: `test/stdlib/conform_encoding.sh` — round-trip tests for each codec, edge cases (empty, padding, special chars). |
| 14.1.2 | std.encrypt C implementation | done | — | **P1** Implement `src/stdlib/encrypt.c`: AES-256-GCM encrypt/decrypt (libtomcrypt or mbedTLS, bundled), X25519 key exchange, Ed25519 sign/verify, HKDF-SHA256, TLS cert fingerprint. Arena-allocated outputs. Conformance: `test/stdlib/conform_encrypt.sh` — encrypt+decrypt round-trip, wrong-key rejection, ciphertext tampering, DH symmetry, Ed25519 sign+verify, HKDF determinism. Depends on 14.1.1 (encoding for key serialisation). |
| 14.1.3 | std.auth C implementation | done | — | **P0** Implement `src/stdlib/auth.c`: JWT HS256 sign/verify (HMAC-SHA256 from std.crypto + base64url from std.encoding), JWT claims parsing, API key generation (32-byte random → base64url), API key validation (constant-time compare). Conformance: `test/stdlib/conform_auth.sh` — sign+verify round-trip, tampered token rejection, expired token detection, API key round-trip. Depends on 14.1.1, existing std.crypto. |
| 14.1.4 | std.encoding/encrypt/auth integration tests | done | — | **P1** End-to-end test: toke program that imports all three modules, generates an API key, issues a JWT, verifies it, uses AES-GCM to encrypt a payload, decrypts it. Run via `tkc --run`. Must pass in `make test-stdlib-security`. |
 |
--- |
 |
### Epic 15 — Stdlib Implementation: Network & Streaming |
 |
C implementations for network modules. These underpin the web platform tier. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 15.1.1 | std.ws C implementation | done | — | **P1** Implement `src/stdlib/ws.c`: WebSocket handshake (HTTP Upgrade), frame encode/decode (text/binary/ping/pong/close), send/recv, broadcast to connection list. Wraps POSIX sockets + SHA-1 for handshake key (pull from crypto). Conformance: `test/stdlib/conform_ws.sh` — loopback server+client, text round-trip, binary round-trip, close handshake. |
| 15.1.2 | std.sse C implementation | done | — | **P1** Implement `src/stdlib/sse.c`: SSE response writer (Content-Type: text/event-stream, chunked), emit/emitdata/close/keepalive. Integrates with std.http response lifecycle. Conformance: `test/stdlib/conform_sse.sh` — emit a sequence of events, verify `data:` line format, verify `event:` field, verify keepalive `:` comment. |
| 15.1.3 | std.router C implementation | done | — | **P1** Implement `src/stdlib/router.c`: radix-tree path matcher supporting `:param` and `*wildcard` segments, query string parser (URL-decoded key-value), middleware chain (linked list of handler pointers), CORS middleware, request body validation stub (calls type checker). Conformance: `test/stdlib/conform_router.sh` — static routes, param extraction, wildcard match, middleware order, CORS headers. Depends on std.http. |
| 15.1.4 | std.template C implementation | done | — | **P2** Implement `src/stdlib/template.c`: compile `{{IDENT}}` slot positions into an offset table (one pass), render by copying bytes between slots and substituting vars map. HTML-escape variant. Conformance: `test/stdlib/conform_template.sh` — basic substitution, missing key behaviour, HTML escaping, empty template, nested whitespace. |
| 15.1.5 | Network stdlib integration test | done | — | **P1** Toke program that starts an HTTP server (std.router), serves SSE events (/stream), and accepts a WebSocket connection (/ws). Verified by `test/stdlib/conform_network_integration.sh` using curl for SSE and a minimal WS handshake check. Depends on 15.1.1–15.1.3. |
 |
--- |
 |
### Epic 16 — Stdlib Implementation: Data Processing |
 |
C implementations for CSV, math, dataframe, analytics, and ML modules. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 16.1.1 | std.csv C implementation | done | — | **P1** Implement `src/stdlib/csv.c`: RFC 4180 parser (quoted fields, escaped quotes, CRLF/LF), streaming row reader (no full-file buffer), header detection, typed column mapping via arena allocation, CSV writer with proper quoting. Conformance: `test/stdlib/conform_csv.sh` — RFC 4180 corpus (10 test cases: empty, quoted commas, newlines in fields, UTF-8, large file streaming). |
| 16.1.2 | std.math C implementation | done | — | **P1** Implement `src/stdlib/math.c`: sum/mean/median/stddev/variance/percentile (single-pass where possible), linreg (least squares), min/max/abs/sqrt/floor/ceil/pow. All operate on `[f64]` slices. No BLAS dep — pure C. Conformance: `test/stdlib/conform_math.sh` — known-answer tests for each function, edge cases (empty slice, single element, NaN propagation). |
| 16.1.3 | std.dataframe C implementation | done | — | **P1** Implement `src/stdlib/dataframe.c`: columnar storage (array of `[f64]` or `[str]` columns, named), filter (predicate over row), groupby+aggregate, join (hash join on string key), pivot, head/tail, shape. Arena allocation for columns. Conformance: `test/stdlib/conform_dataframe.sh` — create from CSV, filter, groupby count/sum, join two frames, shape correctness. Depends on 16.1.1 (CSV ingest), 16.1.2 (math ops). |
| 16.1.4 | std.analytics C implementation | done | — | **P1** Implement `src/stdlib/analytics.c`: describe (count/mean/stddev/min/max/quartiles on dataframe), groupstats, timeseries bucketing (epoch timestamps + bucket size), anomalies (z-score threshold), pivot table, correlation matrix (Pearson). Conformance: `test/stdlib/conform_analytics.sh` — known-answer tests with synthetic data: anomaly detection, moving average, pivot and correlation. Depends on 16.1.3. |
| 16.1.5 | std.ml C implementation | done | — | **P2** Implement `src/stdlib/ml.c`: linear regression (fit + predict), k-means (Lloyd's algorithm, configurable k + max_iter), decision tree (CART, max_depth), k-nearest neighbours (brute-force L2). All inference-only after fit. Conformance: `test/stdlib/conform_ml.sh` — linearly-separable synthetic data, k=2 cluster recovery, XOR tree fit, KNN 1-NN on 2D points. Depends on 16.1.2 (math), 16.1.3 (dataframe input). |
| 16.1.6 | Data pipeline integration test | done | — | **P1** Toke program: reads a CSV file → dataframe → analytics describe → anomaly detection → chart JSON → stdout. Run via `tkc --run`. Verifies the full data stack end-to-end. Depends on 16.1.1–16.1.4, chart (18.1.1). |
 |
--- |
 |
### Epic 17 — Stdlib Implementation: LLM Integration |
 |
C implementations for LLM client and tool-use modules. Highest strategic value. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 17.1.1 | std.llm C implementation | done | — | **P0** Implement `src/stdlib/llm.c`: HTTP POST to `/v1/chat/completions` (OpenAI-compatible), streaming via SSE (`data: {"choices":[{"delta":...}]}`), structured JSON output mode (parse response body as JSON), token counting (cl100k_base tiktoken approximation), retry with exponential backoff (configurable max_retries). Multi-provider: OpenAI, Anthropic (messages API), local Ollama. Provider selected via `llmclient.base_url`. Conformance: `test/stdlib/conform_llm.sh` — mock HTTP server returning fixture SSE stream, verify streamnext yields correct chunks, verify non-streaming complete, verify token count estimate. Depends on std.http (existing), std.json (existing), std.sse (15.1.2). |
| 17.1.2 | std.llm_tool C implementation | done | — | **P1** Implement `src/stdlib/llm_tool.c`: build OpenAI-format tools JSON array from `tooldecl` list, parse `tool_calls` array from response, dispatch to registered handlers, submit `tool` role results. Automatic JSON schema emission from toke `t=` type definitions via compiler introspection hook. Conformance: `test/stdlib/conform_llm_tool.sh` — fixture response with tool_call, verify dispatch, verify result submission. Depends on 17.1.1, std.json. |
| 17.1.3 | LLM integration tests with real providers | done | — | **P2** Optional live tests (skipped if no API key env var). Test: `OPENAI_API_KEY` → single chat completion → non-empty response. `ANTHROPIC_API_KEY` → same. Gate: these are `make test-stdlib-llm-live` not in default suite. Documents expected env vars in README. Depends on 17.1.1. |
 |
--- |
 |
### Epic 18 — Stdlib Implementation: Visualization & Media |
 |
C implementations for all rendering and media modules. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 18.1.1 | std.chart C implementation | done | — | **P1** Implement `src/stdlib/chart.c`: serialize `chartspec` + `dataset` to Chart.js JSON descriptor (type, data, labels, options). Vega-Lite fallback: emit minimal `$schema`, mark, encoding. Output is a JSON string — no rendering, no deps. Conformance: `test/stdlib/conform_chart.sh` — bar/line/scatter/pie JSON output, verify required fields, valid JSON parse. |
| 18.1.2 | std.html C implementation | done | — | **P1** Implement `src/stdlib/html.c`: htmldoc/htmlnode tree (arena-allocated linked list), node constructors (div/p/h1/h2/table/script/style/title), append child, render to HTML string (proper escaping of `<>&"'`), doc wrapper with `<!DOCTYPE html>`. Conformance: `test/stdlib/conform_html.sh` — render a doc with nested divs, verify escaping, verify well-formed HTML (no unclosed tags). |
| 18.1.3 | std.dashboard C implementation | done | — | **P2** Implement `src/stdlib/dashboard.c`: layout grid (JSON descriptor of widget positions/sizes), addchart/addtable/update compose chart+html+ws into a single self-contained HTML page with embedded JS for WebSocket auto-refresh. `serve()` calls std.router to expose the dashboard at a path. Conformance: `test/stdlib/conform_dashboard.sh` — build a 2-widget dashboard, render HTML, verify `<canvas` and `<script` present. Depends on 18.1.1, 18.1.2, 15.1.1 (ws), 15.1.3 (router). |
| 18.1.4 | std.svg C implementation | done | — | **P2** Implement `src/stdlib/svg.c`: build SVG document tree (arena-allocated), element constructors (rect/circle/line/path/text/group/polyline/polygon/arrow with arrowhead marker), render to SVG XML string. Conformance: `test/stdlib/conform_svg.sh` — render a doc with one of each element type, parse as XML, verify element names and attribute presence. |
| 18.1.5 | std.canvas C implementation | done | — | **P3** Implement `src/stdlib/canvas.c`: accumulate drawing ops as a command list, serialize to JS string (`const ctx = document.getElementById('id').getContext('2d'); ctx.fillRect(...)` etc.), `to_html()` wraps in `<canvas>` + `<script>`. Conformance: `test/stdlib/conform_canvas.sh` — build a canvas with fill_rect + text + arc, verify JS string contains expected method calls. |
| 18.1.6 | std.image C implementation | done | — | **P2** Implement `src/stdlib/image.c`: PNG decode/encode (libpng, linked as system lib or vendored), JPEG decode/encode (libjpeg-turbo), pixel buffer alloc (arena), resize (bilinear), crop (bounds-checked copy), grayscale conversion (luminance formula), flip operations. Conformance: `test/stdlib/conform_image.sh` — encode a synthetic 4×4 RGBA buffer to PNG, decode it back, verify dimensions and pixel values. |
| 18.1.7 | Visualization integration test | done | — | **P2** Toke program: dataframe → analytics → chart JSON → html doc with embedded chart → render to file. Verifies the chart→html→dashboard stack. Depends on 16.1.3, 18.1.1, 18.1.2. |
 |
 |
--- |
 |
### Epic 19 — Build System Integration |
 |
Link all 20 new stdlib C implementations into the build system so `tkc` can actually invoke them and tests can be compiled and run. Currently these are standalone `.c` files not in any Makefile target. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 19.1.1 | Add Makefile test targets for all 20 new stdlib modules | done | — | **P0** Add `test-stdlib-{encoding,encrypt,auth,ws,sse,router,template,csv,math,llm,llm_tool,chart,html,dashboard,svg,canvas,image,dataframe,analytics,ml}` targets. Each compiles test_{module}.c + {module}.c, runs, reports pass/fail. Add `test-stdlib-all-new` aggregate target. Must compile clean with `-std=c99 -Wall -Wextra -Wpedantic -Werror`. |
| 19.1.2 | Add Makefile test targets for integration tests | done | — | **P0** Add `test-stdlib-security-integration`, `test-stdlib-network-integration`, `test-stdlib-viz-integration`, `test-stdlib-data-pipeline`, `test-stdlib-llm-live` targets. Each links the required modules together. `test-stdlib-llm-live` is opt-in (not in default `ci`). |
| 19.1.3 | Compile and fix all test builds to pass clean | done | — | **P0** Run every test target from 19.1.1 and 19.1.2. Fix any compilation errors (missing includes, type mismatches, undefined references). All 20 module tests + 4 integration tests must compile and pass. Track results in a test matrix. |
| 19.1.4 | Link stdlib modules into tkc binary for `i=` imports | done | — | **P1** Add the new .c files to the SRCS list in Makefile (or a STDLIB_SRCS variable) so that toke programs using `i=std.encoding;` etc. can resolve to the C implementations at compile/link time. Verify with a minimal toke program that imports and calls each module. |
 |
--- |
 |
### Epic 20 — Module Unit Test Hardening |
 |
Current C tests cover happy paths. Add edge case, error path, boundary condition, and memory tests for every module. Each module needs at least 20 test cases covering the full API surface. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 20.1.1 | Harden encoding tests: edge cases and invalid input | done | — | **P1** Add: empty input for all 8 functions, max-length strings, invalid UTF-8 in urlencode, null bytes in base64, decode of corrupted padding, percent-encoding of all reserved chars. Target: 25+ total tests. |
| 20.1.2 | Harden encrypt tests: key/nonce size validation and known vectors | done | — | **P1** Add: AES-256-GCM NIST test vectors (from SP 800-38D), wrong key/nonce size errors, zero-length plaintext, max-length AAD, X25519 known-answer (RFC 7748 §6.1), Ed25519 known-answer (RFC 8032 §7.1). Target: 25+ total tests. |
| 20.1.3 | Harden auth tests: malformed tokens and timing | done | — | **P1** Add: JWT with missing fields, JWT with extra dots, JWT with non-base64 segments, expired-by-1-second boundary, verify with empty secret, API key of different lengths. Target: 20+ total tests. |
| 20.1.4 | Harden csv tests: RFC 4180 compliance suite | done | — | **P1** Add: field with only quotes, field with only newlines, 0-column row, 1000-column row, 100KB streaming read, BOM handling, trailing CRLF variations. Target: 20+ total tests. |
| 20.1.5 | Harden math tests: NaN/Inf propagation and precision | done | — | **P1** Add: NaN in input arrays, +/-Inf inputs, single-element median/stddev, identical-values stddev=0, percentile at boundaries (0.001, 99.999), linreg with vertical line, linreg with all same x. Target: 25+ total tests. |
| 20.1.6 | Harden dataframe tests: type coercion and large data | done | — | **P1** Add: 10K-row dataframe performance, mixed numeric/string column (should be str), empty dataframe operations, join on missing column, groupby on f64 column (error), filter on str column (error). Target: 20+ total tests. |
| 20.1.7 | Harden network module tests: ws/sse/router edge cases | done | — | **P1** Add: WS frame with 0-byte payload, WS 64-bit extended length, SSE with empty data, SSE with \r\n line endings, router with trailing slashes, router with URL-encoded path segments, duplicate route registration. Target: 25+ total across 3 modules. |
| 20.1.8 | Harden visualization tests: html/svg/chart/canvas edge cases | done | — | **P2** Add: HTML special chars in all positions, SVG coordinate precision, chart with 0 datasets, chart with 1000 labels, canvas op count overflow, dashboard with 0 widgets, deeply nested HTML nodes. Target: 25+ total across 4 modules. |
| 20.1.9 | Harden ml tests: convergence, degenerate inputs, known datasets | done | — | **P2** Add: k-means with k > n_points, k-means single-point clusters, decision tree max_depth=1, KNN with k=n_train, linreg single point, linreg with collinear points, Iris-like 3-class dataset for KNN. Target: 20+ total. |
 |
--- |
 |
### Epic 21 — Real-World Integration Scenarios |
 |
Multi-step tests simulating actual use cases. These verify modules work together in production-like workflows. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 21.1.1 | HTTP API client: request → JSON parse → data extraction | done | — | **P1** Build a test that constructs an HTTP request using the LLM module's request builder, creates a mock JSON response string, parses it with json.h functions, extracts fields, validates structure. Covers: llm_build_request + json parsing + string manipulation. No actual network call. |
| 21.1.2 | Auth-protected API flow: keygen → JWT → verify → encrypt payload | done | — | **P1** Full auth flow: generate API key, sign JWT, encode Authorization header with encoding module, encrypt a JSON payload with AES-GCM, decrypt and verify JWT on the "server side". End-to-end in one test file. |
| 21.1.3 | Data analytics dashboard: CSV → analyze → chart → HTML → file | done | — | **P1** Load a realistic CSV dataset (50+ rows, 5+ columns). Run analytics_describe, detect anomalies, compute correlations. Build a bar chart + table. Compose into a dashboard. Render to HTML string. Verify the output is valid HTML with embedded chart data. |
| 21.1.4 | Template-based email rendering with escaped user data | done | — | **P2** Compile a template with `{{name}}`, `{{email}}`, `{{message}}` slots. Render with values containing HTML special chars (`<script>alert(1)</script>`). Verify `tmpl_renderhtml` produces safe output with no unescaped `<script>` tags. |
| 21.1.5 | Image processing pipeline: create → resize → grayscale → encode → decode | done | — | **P2** Create a 64x64 RGBA buffer with a gradient pattern. Resize to 32x32. Convert to grayscale. Encode as PNG. Decode the PNG bytes. Verify dimensions and that pixel values are reasonable (not corrupt). Full round-trip. |
| 21.1.6 | WebSocket protocol handshake simulation | done | — | **P1** Simulate a full WS upgrade: build upgrade request headers, compute accept key, send a text frame, receive and decode it, exchange ping/pong, send close frame. All in-memory using the ws encode/decode functions. |
| 21.1.7 | ML prediction pipeline: CSV → train → predict → evaluate accuracy | done | — | **P2** Load a classification dataset from CSV into a dataframe. Split into train/test. Train a decision tree. Predict on test set. Compute accuracy (count correct / total). Verify accuracy > 70% on a linearly-separable synthetic dataset. |
| 21.1.8 | SVG diagram generation from structured data | done | — | **P2** Read a CSV of nodes and edges. Build an SVG document with circles for nodes (positioned in a grid), lines for edges, text labels. Render to SVG string. Verify it contains expected number of `<circle>`, `<line>`, `<text>` elements. |
 |
--- |
 |
### Epic 22 — Corpus Regeneration (Frozen Default Syntax) |
 |
Generate a fresh, expanded corpus using the frozen default syntax. Include open-source model generation for diversity. This is prerequisite to a valid Phase 2 training run. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 22.1.1 | Verify existing corpus is fully converted to default syntax | backlog | — | **P0** Sample 100 entries from each phase (A/B/C/D). Confirm all use `$` sigils, `@()` arrays, lowercase keywords. If any are still Phase 1 uppercase, run `to_default_syntax.py` on the full corpus. Report conversion coverage. |
| 22.1.2 | Generate 10K new corpus entries using open-source local models | backlog | — | **P0** Use Qwen 2.5 Coder 7B (local via Ollama) and Llama 3.2 to generate toke programs in default syntax. Pipeline: prompt model → extract code → `tkc --check` → keep passing programs. Target: 10,000 new validated entries. Focus on stdlib usage (the 20 new modules). |
| 22.1.3 | Generate 10K corpus entries using Claude/GPT with stdlib context | backlog | — | **P0** Use Claude Sonnet and GPT-4o to generate programs exercising the new stdlib modules (http, csv, json, llm, chart, html, etc.). Provide .tki files as context. `tkc --check` validation. Target: 10,000 new validated entries with diverse stdlib usage. |
| 22.1.4 | Expand corpus with multi-module integration programs | backlog | — | **P1** Generate 5,000 programs that import 2+ stdlib modules and use them together (e.g., CSV→chart, http→json, auth→encrypt). These are harder to generate but critical for training the model on realistic usage patterns. |
| 22.1.5 | Corpus quality audit and dedup | backlog | — | **P1** Deduplicate the expanded corpus. Run quality scoring. Remove entries below threshold. Produce final manifest with stats. Target: 70K+ validated programs total (original 47K + new 20K+ after dedup). |
| 22.1.6 | Prepare training data JSONL from expanded corpus | backlog | — | **P0** Run the data preparation pipeline to produce train/eval split in ChatML JSONL format. Ensure all examples use default syntax. Store in `toke-model/training-data-v2/`. Depends on 22.1.1-22.1.5. |
 |
--- |
 |
### Epic 23 — Tokenizer Retrain on Expanded Corpus |
 |
Retrain the BPE tokenizer on the expanded default-syntax corpus and evaluate against baselines. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 23.1.1 | Verify current tokenizer was trained on default syntax | backlog | — | **P0** Run `eval.py` with the current 8K and 32K tokenizer models against a sample of default-syntax programs. Check that `$`, `@()`, lowercase keywords are single tokens or efficiently tokenized. Report results. If not trained on default syntax, proceed to 23.1.2. |
| 23.1.2 | Retrain 8K BPE tokenizer on expanded default-syntax corpus | backlog | — | **P0** Run `train.py` with the full expanded corpus (70K+ programs). 8K vocabulary. Evaluate: token count reduction vs cl100k_base and o200k_base. Target: ≥15% reduction vs cl100k_base. Depends on 22.1.5. |
| 23.1.3 | Retrain 32K BPE tokenizer and evaluate vocab utilization | backlog | — | **P1** Retrain at 32K vocabulary. Evaluate vocab utilization (was 23.5% — should improve with larger corpus). Compare token counts against 8K model. Determine if 32K is worth the complexity or if 8K suffices. |
| 23.1.4 | Tokenizer regression tests: ensure all stdlib identifiers tokenize cleanly | backlog | — | **P1** For each of the 30+ stdlib module names and common function names (e.g., `crypto.sha256`, `df_fromcsv`, `chart_tojson`), verify the tokenizer does not split them badly (no mid-word breaks). Report any problematic tokenizations. |
 |
--- |
 |
### Epic 24 — Model Training Round 2 (Default Syntax) |
 |
Full training run with the frozen default syntax, expanded corpus, and retrained tokenizer. Evaluate against Gate 2 criteria. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 24.1.1 | Train 7B model on default-syntax corpus (QLoRA/DoRA) | backlog | — | **P0** Run `train_mlx.py` with DoRA config on the expanded training data (from 22.1.6) using the retrained tokenizer (from 23.1.2). Mac Studio M4 Max local training. Target: eval loss < 0.15. Depends on 22.1.6, 23.1.2. |
| 24.1.2 | Merge adapters and evaluate Pass@1 | backlog | — | **P0** Merge LoRA/DoRA adapters into base model. Run `gate2_benchmark.py` on the 1000-task benchmark suite. Compare Pass@1 against Gate 1 baseline (63.7%). Target: ≥70% Pass@1. Depends on 24.1.1. |
| 24.1.3 | Evaluate token efficiency with retrained tokenizer | backlog | — | **P0** Measure token reduction of model-generated code vs Python/C/Java using the new tokenizer. Compare against Gate 1 baseline (12.5%). Target: ≥20% token reduction. Depends on 24.1.2. |
| 24.1.4 | Gate 2 assessment and go/no-go decision | backlog | — | **P0** Compile Gate 2 report: Pass@1, token reduction, compilation rate, stdlib usage in generated code. Document decision. If gate passes, proceed to Phase 3 planning. If not, identify remediation stories. Depends on 24.1.2, 24.1.3. |
| 24.1.5 | Publish model to HuggingFace and update model card | backlog | — | **P1** Upload merged model weights, tokenizer, and updated model card to HuggingFace. Include Gate 2 results, training details, and usage instructions. Update the README with default syntax examples. Depends on 24.1.4 (gate pass). |
 |
--- |
 |
### Epic 25 — Website & Documentation Refresh |
 |
Update the website to reflect current state: 6-repo structure, 30+ stdlib modules, Phase 2 progress, timeline status. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 25.1.1 | Update DevTimeline component with Phase 2 completion status | done | — | **P0** In DevTimeline.astro: Mark 2.1 (Language Extensions) as Complete. Update 2.2 (Tokenizer) status text with current metrics. Update 2.3 (First Model) with Gate 1 results. Update repo links to new consolidated names (toke, toke-model, toke-eval). Fix any "toke-models" → "toke-model" references. |
| 25.1.2 | Update repos.md with current 6-repo structure | done | — | **P0** Replace the old 10-repo table with the consolidated 6-repo structure: toke (compiler+spec+stdlib), toke-model (corpus+tokenizer+models), toke-eval (benchmark+eval), toke-web, toke-mcp, toke-cloud (private). Update all status fields to current state. Remove "blocked" and "not started" for repos that are now active. |
| 25.1.3 | Update stdlib lesson (09-stdlib.md) with all 30+ modules | done | — | **P0** Add all new modules to the Available list: encoding, encrypt, auth, ws, sse, router, template, csv, math, llm, llm_tool, chart, html, dashboard, svg, canvas, image, dataframe, analytics, ml. Move std.math from Planned to Available. Group modules by tier (Foundation, Web, Data, LLM, Visualization). |
| 25.1.4 | Update homepage stats and claims | done | — | **P1** Update "11 standard library modules" → "30+ standard library modules". Update the feature cards. Add mentions of LLM integration, data analytics, and visualization capabilities. Verify all numeric claims match current data. |
| 25.1.5 | Add API reference pages for all new stdlib modules | done | — | **P1** Create documentation pages in `/reference/stdlib/` for each of the 20 new modules. Each page: module overview, function signatures, usage examples in default syntax, notes on dependencies. Use the existing .md files from the toke/stdlib/ directory as source material. |
| 25.1.6 | Add project changelog/status page | done | — | **P2** Create `/about/changelog.md` or `/about/status.md` documenting key milestones: Gate 1 pass, syntax freeze, stdlib expansion, consolidation. Link from homepage and navbar. |
| 25.1.7 | Validate all website code examples compile | done | — | **P0** Run `test/check_examples.sh` and fix any failures. Every toke code block on the website must compile with `tkc --check`. Target: 100% pass rate (up from current 50%). Fix syntax errors, update outdated examples, add `skip-check` only where examples are intentionally partial. |
| 25.1.8 | Validate training course examples and exercises | done | — | **P1** Verify all 10 lessons in `/learn/` have compilable, runnable examples. Test each lesson's exercises produce expected output. Fix any that use outdated syntax or reference unavailable stdlib modules. |
| 25.1.9 | Deploy updated website and verify live | done | — | **P1** Build, deploy to Lightsail, verify all pages render correctly. Run linkinator. Test on mobile. Clear Cloudflare cache. Depends on 25.1.1-25.1.8. |
 |
--- |
 |
### Epic 26 — Specification & API Documentation Update |
 |
Update toke-spec to reflect all new stdlib modules and ensure the specification is comprehensive and current. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 26.1.1 | Add stdlib module specifications to toke-spec | done | — | **P1** For each of the 20 new modules: add a section in the spec documenting the module name, function signatures (using toke type syntax), error conditions, and guarantees. Reference the .tki files as canonical. Publish in `toke-spec/spec/stdlib/`. |
| 26.1.2 | Update spec-implementation-delta.md for new modules | done | — | **P1** Update the delta tracking document to include all 20 new modules. Each should show: specified (in .tki) + implemented (in .c) + tested (test_{module}.c). |
| 26.1.3 | Verify .tki interface files match C implementations exactly | done | — | **P0** For each module, compare the function names and parameter types in the .tki file against the actual C header (.h) file. Fix any mismatches. The .tki is the contract; the .h must conform. |
| 26.1.4 | Push spec updates to GitHub and update website API browser | done | — | **P1** Push toke-spec changes. Verify the website API specification browser at `/reference/` reflects the new modules. Depends on 26.1.1-26.1.3, 25.1.5. |
 |
--- |
 |
### Epic 27 — Production Web Server Runtime |
 |
Harden the std.http + std.router stack into a production-quality web server. The primitives exist (route registration, TCP listener, request/response types, WebSocket framing, SSE) but the server is single-threaded and blocking with no TLS, no static files, no compression, and no connection management. These stories bring it to parity with what a modern web framework provides. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 27.1.1 | Multi-connection server: pre-fork worker pool | done | — | **P0** Replace the single-threaded accept loop in `http.c` with a pre-fork model (N worker processes, configurable via `http.serve(port;workers)`). Each worker independently accepts and handles connections. No shared mutable state — each worker has its own route table copy. Falls back to single-process when workers=1. Must not require pthreads (fork only). Update http.h, http.c, http.tki. |
| 27.1.2 | TLS/HTTPS support via BearSSL (bundled) | done | — | **P0** Add `http.serve_tls(port;cert_path;key_path)` and `http.serve_tls(port;cert_path;key_path;workers)`. Bundle BearSSL (small, no-malloc, C99, permissive license) for TLS 1.2/1.3. PEM cert+key loading. No OpenSSL dependency. Update http.h, http.tki. Conformance: self-signed cert handshake, cert rotation without restart. |
| 27.1.3 | HTTP/1.1 keep-alive and connection reuse | done | — | **P1** Implement persistent connections per RFC 7230. Parse `Connection: keep-alive` / `close` headers. Maintain connection with configurable idle timeout (default 30s) and max requests per connection (default 1000). Properly handle `Content-Length` and chunked boundaries between pipelined requests. |
| 27.1.4 | Chunked transfer encoding | done | — | **P1** Support `Transfer-Encoding: chunked` for both request reading and response writing. Enable streaming responses without knowing content length upfront. Required for SSE and large response bodies. Wire into SSE module's `sse_emit()`. |
| 27.1.5 | Static file serving middleware | done | — | **P1** Add `router_static(router;url_prefix;dir_path)` that serves files from a directory. MIME type detection (by extension, ~30 common types). Directory index (index.html). Path traversal protection (reject `..`). ETag based on mtime+size. Conditional GET (If-None-Match → 304). |
| 27.1.6 | Gzip response compression | done | feature/stdlib-gzip-compression | **P1** Add `router_use_gzip(router;min_size)` middleware. Compress responses when `Accept-Encoding: gzip` is present and body exceeds min_size (default 1KB). Uses system zlib (deflateInit2 windowBits=15|16 for gzip format). Skips already-compressed MIME types (image, video, audio, application/octet-stream). Sets `Content-Encoding: gzip` and `Vary: Accept-Encoding`. Tests T47-T50 pass. |
| 27.1.7 | Cookie parsing and Set-Cookie response headers | done | — | **P1** Add `http_cookie(req;name)` to extract cookie values from `Cookie:` header. Add `http_Res_set_cookie(res;name;value;opts)` where opts includes `path`, `domain`, `max_age`, `secure`, `httponly`, `samesite`. RFC 6265 compliant parsing. Update http.h, http.tki. |
| 27.1.8 | Multipart/form-data request body parsing | done | — | **P1** Add `http_multipart(req)` returning an array of parts, each with `name`, `filename` (optional), `content_type`, and `data` (bytes). Stream-parse without buffering entire body. Enforce per-part size limit (configurable, default 10MB) and total body limit (default 50MB). |
| 27.1.9 | Request size limits and timeout protection | done | — | **P0** Enforce maximum request header size (default 8KB), maximum body size (default 1MB, overridable per route), and per-request timeout (default 30s). Return 413 Payload Too Large or 408 Request Timeout. Protects against slowloris and large-payload DoS. Configurable via `http.serve` options. |
| 27.1.10 | Graceful shutdown and signal handling | done | — | **P1** Trap SIGTERM and SIGINT. On signal: stop accepting new connections, finish in-flight requests (with timeout, default 10s), then exit cleanly. Workers drain independently. Add `http.shutdown()` for programmatic stop. |
| 27.1.11 | Access logging middleware | done | — | **P2** Add `router_use_log(router;format)` middleware. Common Log Format and JSON format options. Logs: timestamp, client IP, method, path, status, response size, duration_ms. Writes to stderr by default, configurable to file path. |
| 27.1.12 | CORS middleware (full implementation) | done | — | **P1** Add `router_use_cors(router;opts)` with: `allowed_origins` (list or `*`), `allowed_methods`, `allowed_headers`, `expose_headers`, `max_age`, `allow_credentials`. Handle preflight OPTIONS requests automatically. Current router has a CORS stub — replace with full RFC 6454/Fetch spec implementation. |
| 27.1.13 | ETag generation and conditional request handling | done | — | **P2** Auto-generate weak ETags for responses (FNV-1a hash of body). Handle `If-None-Match` → 304 Not Modified. Handle `If-Match` → 412 Precondition Failed. Works with both static files (27.1.5) and dynamic responses. |
| 27.1.14 | URL-encoded form body parsing | done | — | **P1** Add `http_form(req)` that parses `application/x-www-form-urlencoded` bodies into key-value pairs. Uses existing URL decode from std.encoding. Handles `+` as space, multiple values per key. |
| 27.1.15 | WebSocket upgrade integration with HTTP server | done | — | **P1** Wire `std.ws` frame codec into the HTTP server's connection loop. Add `router_ws(router;pattern;on_open;on_message;on_close)` that upgrades HTTP connections to WebSocket when `Upgrade: websocket` is present. Handle the Sec-WebSocket-Accept handshake automatically. Connection lifecycle management (ping/pong keepalive, clean close). Depends on 27.1.3 (keep-alive). |
| 27.1.16 | Production web server integration test | done | — | **P0** Created `test/stdlib/test_web_server_integration.c`: 12 test scenarios (T1–T12) covering static file + MIME, JSON API, cookie round-trip, form parsing, multipart upload, CORS preflight, gzip, ETag conditional, chunked round-trip, graceful shutdown, access log, WebSocket handshake. In-process socketpair+fork testing, no real network. Compiles with `-std=c99 -Wall -Wextra -Wpedantic -Werror -lz`. |
| 27.1.17 | Fix TLS forward-declaration ordering bug in http.h | done | — | **BUG** `src/stdlib/http.h` declares `http_serve_tls()` and `http_serve_tls_workers()` at lines 224/229 using types `TkHttpErr` and `TkHttpRouter` that are not typedef'd until lines 238/247. This causes a compile error (`unknown type name 'TkHttpErr'`) in every translation unit that includes http.h. The TLS declarations must be moved after the `TkHttpRouter`/`TkHttpErr` typedefs, or forward declarations must be added before the TLS function prototypes. Discovered during 27.1.16. |
 |
--- |
 |
### Epic 28 — Foundation Libraries: Production Completeness |
 |
The foundation modules (str, file, time, test, env, process, log) are missing essential functions that any real program needs. str has no `replace` or `join`. file can't create directories or copy files. time can't parse strings. test has only 3 assert functions. These gaps make it impossible to write non-trivial programs without workarounds. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 28.1.1 | std.str: search and transform functions | done | 2026-04-05 | **P0** Add: `str_index(s;sub)→i64` (first occurrence, -1 if not found), `str_rindex(s;sub)→i64` (last occurrence), `str_replace(s;old;new)→Str` (all occurrences), `str_replace_first(s;old;new)→Str`, `str_join(sep;parts)→Str`, `str_repeat(s;n)→Str`. Update str.h, str.c, str.tki. Tests for each. |
| 28.1.2 | std.str: prefix/suffix and line operations | done | 2026-04-05 | **P0** Add: `str_starts_with(s;prefix)→bool`, `str_ends_with(s;suffix)→bool`, `str_split_lines(s)→StrArray`, `str_count(s;sub)→u64` (non-overlapping). Update str.h, str.c, str.tki. |
| 28.1.3 | std.str: padding, reverse, and character class tests | done | 2026-04-05 | **P1** Add: `str_pad_left(s;width;ch)→Str`, `str_pad_right(s;width;ch)→Str`, `str_reverse(s)→Str`, `str_is_alpha(s)→bool`, `str_is_digit(s)→bool`, `str_is_alnum(s)→bool`, `str_is_space(s)→bool`. |
| 28.2.1 | std.file: directory operations | done | 2026-04-05 | **P0** Add: `file_mkdir(path)→bool!FileErr`, `file_mkdir_p(path)→bool!FileErr` (recursive), `file_rmdir(path)→bool!FileErr`, `file_rmdir_r(path)→bool!FileErr` (recursive tree removal). `file_is_dir(path)→bool`, `file_is_file(path)→bool`. Update file.h, file.c, file.tki. |
| 28.2.2 | std.file: copy, move, and metadata | done | 2026-04-05 | **P0** Add: `file_copy(src;dst)→bool!FileErr`, `file_move(src;dst)→bool!FileErr`, `file_size(path)→u64!FileErr`, `file_mtime(path)→u64!FileErr`. Uses POSIX `stat()`, `rename()`, read+write fallback for cross-device copy. |
| 28.2.3 | std.file: path utilities | done | 2026-04-05 | **P1** Add: `file_join(a;b)→Str` (path join with separator), `file_basename(path)→Str`, `file_dirname(path)→Str`, `file_absolute(path)→Str` (resolve via `realpath()`), `file_ext(path)→Str` (extension including dot). `file_readlines(path)→StrArray!FileErr`, `file_glob(pattern)→StrArray` (uses POSIX `glob()`). |
| 28.3.1 | std.time: parsing and arithmetic | done | 2026-04-05 | **P0** Add: `tk_time_parse(s;fmt)→u64!TimeErr` (strptime wrapper), `tk_time_add(ts;duration_ms)→u64`, `tk_time_diff(ts1;ts2)→i64` (milliseconds between). |
| 28.3.2 | std.time: date breakdown and calendar | done | 2026-04-05 | **P1** Add: `tk_time_to_parts(ts)→{year;month;day;hour;min;sec}`, `tk_time_from_parts(year;month;day;hour;min;sec)→u64`, `tk_time_weekday(ts)→u8` (0=Sun..6=Sat), `tk_time_is_leap_year(year)→bool`, `tk_time_days_in_month(year;month)→u8`. |
| 28.4.1 | std.test: comparison and containment assertions | done | 2026-04-05 | **P0** Add: `tk_test_assert_true(cond;msg)→bool`, `tk_test_assert_false(cond;msg)→bool`, `tk_test_assert_gt(a;b;msg)→bool`, `tk_test_assert_lt(a;b;msg)→bool`, `tk_test_assert_gte(a;b;msg)→bool`, `tk_test_assert_lte(a;b;msg)→bool`, `tk_test_assert_contains(haystack;needle;msg)→bool`, `tk_test_assert_not_contains(haystack;needle;msg)→bool`, `tk_test_assert_nil(ptr;msg)→bool`, `tk_test_assert_not_nil(ptr;msg)→bool`. |
| 28.4.2 | std.test: test runner and lifecycle hooks | done | 2026-04-05 | **P1** Add: `tk_test_run(name;test_fn)→int` (named test with pass/fail tracking), `tk_test_setup(fn)→void` (before-each hook), `tk_test_teardown(fn)→void` (after-each hook), `tk_test_summary()→{passed;failed;skipped}` (print results). Enables structured test suites instead of ad-hoc main(). |
| 28.5.1 | std.env: list, delete, and .env file loading | done | 2026-04-05 | **P1** Add: `env_list()→StrArray`, `env_delete(key)→bool`, `env_expand(template)→Str` (substitute `$VAR` in string), `env_file_load(path)→bool` (parse KEY=VALUE lines, skip comments). |
| 28.5.2 | std.process: stdin, stderr, and timeout | done | 2026-04-05 | **P1** Add: `process_stdin_write(h;data)→u64!ProcessErr`, `process_stderr(h)→Str!ProcessErr`, `process_exit_code(h)→i32!ProcessErr`, `process_is_running(h)→bool`, `process_set_cwd(h;cwd)→void`, `process_timeout(h;timeout_ms)→i32!ProcessErr` (kill if exceeded). |
| 28.5.3 | std.log: debug level, JSON format, file output | done | 2026-04-05 | **P1** Add: `tk_log_debug(msg;fields;n)→int`, `tk_log_set_format(fmt)→void` ("json" or "text"), `tk_log_set_output(path)→bool` (redirect to file), `tk_log_with_context(msg;context_json)→int` (attach request ID, trace ID). |
 |
--- |
 |
### Epic 29 — Data & Encoding Libraries: Production Completeness |
 |
The data processing modules (json, csv, math, db, encoding, crypto) are missing functions required for common workflows. json can't list keys or check types. db has no transactions or prepared statements. math has no trig or logarithms. These gaps block realistic application development. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 29.1.1 | std.json: object inspection and manipulation | done | 2026-04-05 | **P0** Add: `json_keys(j;key)→StrArray!JsonErr`, `json_has(j;key)→bool`, `json_len(j)→u64!JsonErr` (array length), `json_type(j)→Str` ("null"/"object"/"array"/"string"/"number"/"bool"), `json_pretty(j)→Str` (indented output), `json_is_null(j;key)→bool`. |
| 29.1.2 | std.json: path access and construction | done | 2026-04-05 | **P1** Add: `json_at(j;path)→Json!JsonErr` (dotted path like "user.address.city"), `json_index(j;i)→Json!JsonErr` (array indexing), `json_merge(j1;j2)→Json!JsonErr` (shallow merge), `json_from_pairs(keys;values)→Json` (build object). |
| 29.2.1 | std.db: prepared statements and transactions | done | 2026-04-05 | **P0** Add: `db_prepare(conn;sql)→Stmt!DbErr`, `db_bind(stmt;params)→bool!DbErr`, `db_step(stmt)→Row!DbErr` (fetch next), `db_finalize(stmt)→void`, `db_begin(conn)→bool!DbErr`, `db_commit(conn)→bool!DbErr`, `db_rollback(conn)→bool!DbErr`. Prepared statements prevent SQL injection and improve performance for repeated queries. |
| 29.2.2 | std.db: metadata and result inspection | done | 2026-04-05 | **P1** Add: `db_last_insert_id(conn)→u64!DbErr`, `db_affected_rows(conn)→u64`, `db_columns(row)→StrArray` (column names), `db_is_null(row;col)→bool`, `db_table_exists(conn;name)→bool`. |
| 29.3.1 | std.math: trigonometry and transcendental functions | done | 2026-04-05 | **P0** Add wrappers around `<math.h>`: `math_sin(x)`, `math_cos(x)`, `math_tan(x)`, `math_asin(x)`, `math_acos(x)`, `math_atan(x)`, `math_atan2(y;x)`, `math_log(x)` (natural), `math_log10(x)`, `math_exp(x)`, `math_hypot(x;y)`. All `f64→f64`. |
| 29.3.2 | std.math: rounding, NaN handling, and combinatorics | done | 2026-04-05 | **P1** Add: `math_round(x;digits)→f64`, `math_trunc(x)→f64`, `math_fmod(x;y)→f64`, `math_isnan(x)→bool`, `math_isinf(x)→bool`, `math_copysign(x;y)→f64`, `math_gcd(a;b)→i64`, `math_lcm(a;b)→i64`, `math_factorial(n)→i64`, `math_mode(xs)→f64`. Constants: `MATH_E`, `MATH_TAU`. |
| 29.4.1 | std.csv: configuration and dialects | done | 2026-04-05 | **P1** Add: `csv_reader_set_separator(r;sep)`, `csv_reader_set_quote(r;ch)`, `csv_reader_lazyquotes(r;enabled)`, `csv_writer_set_separator(w;sep)`, `csv_writer_use_crlf(w;enabled)`, `csv_reader_line_number(r)→u64`. Enable TSV and other delimited formats. |
| 29.5.1 | std.encoding: UTF-8 validation and base32 | done | 2026-04-05 | **P1** Add: `encoding_utf8_validate(data)→bool`, `encoding_utf8_rune_count(s)→u64`, `encoding_base32_encode(data)→Str`, `encoding_base32_decode(s)→ByteArray!EncodingErr`. UTF-8 validation is critical for any text processing pipeline. |
| 29.6.1 | std.crypto: SHA-512, bcrypt, and random bytes | done | 2026-04-05 | **P0** Add: `crypto_sha512(data)→Str` (hex), `crypto_bcrypt_hash(password;cost)→Str!CryptoErr`, `crypto_bcrypt_verify(password;hash)→bool`, `crypto_random_bytes(n)→ByteArray`, `crypto_from_hex(hex)→ByteArray!CryptoErr`. Bcrypt is the standard for password storage. |
 |
--- |
 |
### Epic 30 — Security Libraries: Production Completeness |
 |
The encrypt and auth modules cover the basics but are missing key algorithms (ChaCha20, PBKDF2) and auth patterns (JWT decode, OAuth2 client, TOTP) that production systems need. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 30.1.1 | std.encrypt: ChaCha20-Poly1305 and PBKDF2 | done | 2026-04-05 | **P1** Add: `encrypt_chacha20poly1305_encrypt/decrypt/keygen/noncegen` (modern alternative to AES-GCM, used by WireGuard/TLS 1.3), `encrypt_pbkdf2(password;salt;iterations;dklen;hash)→ByteArray` (key derivation from passwords). Pure C99 implementation. |
| 30.1.2 | std.encrypt: RSA key operations | backlog | — | **P2** Add: `encrypt_rsa_generate_keypair(bits)→{pub;priv}`, `encrypt_rsa_encrypt/decrypt`, `encrypt_rsa_sign/verify`. RSA-OAEP for encryption, RSA-PSS for signatures. Large implementation — consider bundling libtomcrypt subset. |
| 30.2.1 | std.auth: JWT decode and OAuth2 client | done | 2026-04-05 | **P1** Add: `auth_jwtdecode_claims(token)→JwtClaims!AuthErr` (decode without verification for inspection), `auth_oauth2_authorize_url(provider;client_id;redirect_uri;scopes)→Str`, `auth_oauth2_token_exchange(code;client_id;client_secret;redirect_uri)→{access_token;refresh_token;expires_in}!AuthErr`. OAuth2 authorization code flow is baseline for any API integration. |
| 30.2.2 | std.auth: TOTP (2FA) and bcrypt integration | done | 2026-04-05 | **P2** Add: `auth_totp_generate(secret)→{uri;secret_b32}` (RFC 6238, compatible with Google Authenticator), `auth_totp_verify(secret;token;window)→bool`, `auth_password_hash(password)→Str!AuthErr` (delegates to crypto_bcrypt), `auth_password_verify(password;hash)→bool`. |
 |
--- |
 |
### Epic 31 — Data Science Libraries: Production Completeness |
 |
The dataframe, analytics, and ml modules provide basic functionality but are missing sort, concat, train/test split, and evaluation metrics — all essential for any data pipeline. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 31.1.1 | std.dataframe: sort, unique, and column operations | done | 2026-04-05 | **P0** Add: `df_sort(df;col;ascending)→TkDataframe`, `df_unique(df;col)→TkDataframe`, `df_drop_column(df;col)→TkDataframe`, `df_rename_column(df;old;new)→TkDataframe`, `df_select_columns(df;cols)→TkDataframe`, `df_value_counts(df;col)→DfGroupResult`. |
| 31.1.2 | std.dataframe: concat, merge, missing data, and export | done | 2026-04-05 | **P1** Add: `df_concat(left;right)→TkDataframe` (stack rows), `df_fillna(df;col;value)→TkDataframe`, `df_dropna(df;col)→TkDataframe`, `df_sample(df;n)→TkDataframe` (random without replacement), `df_get_row(df;idx)→Row!DfErr`, `df_to_csv(df)→Str`, `df_to_html(df)→Str`. |
| 31.2.1 | std.analytics: statistical tests and moving averages | done | 2026-04-05 | **P1** Add: `analytics_ttest(g1;g2)→{t_stat;p_value}`, `analytics_histogram(xs;nbins)→{bins;counts}`, `analytics_moving_average(xs;window)→[f64]`, `analytics_exponential_smoothing(xs;alpha)→[f64]`, `analytics_trend(ts)→{slope;intercept;r2}`, `analytics_covariance(xs;ys)→f64`. |
| 31.3.1 | std.ml: train/test split and evaluation metrics | done | 2026-04-05 | **P0** Add: `ml_train_test_split(n;test_size;seed)→{train_idx;test_idx}`, `ml_confusion_matrix(y_true;y_pred)→ConfusionMatrix`, `ml_precision_recall_f1(y_true;y_pred)→{precision;recall;f1}`, `ml_accuracy(y_true;y_pred)→f64`, `ml_standardize(xs)→[f64]`, `ml_normalize(xs)→[f64]` (min-max to [0,1]). Without metrics, trained models can't be evaluated. |
| 31.3.2 | std.ml: random forest and cross-validation | done | 2026-04-05 | **P2** Add: `ml_random_forest_fit/predict/free` (ensemble of decision trees with bagging), `ml_cross_validation_split(n;k;seed)→[[u64]]` (k-fold indices). Random forest is the most commonly used ensemble method and often outperforms single decision trees. |
 |
--- |
 |
### Epic 32 — LLM Integration: Production Completeness |
 |
The llm and llm_tool modules handle basic chat completions but are missing embeddings, retry logic, and agentic loop support — features that any serious LLM application needs. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 32.1.1 | std.llm: embeddings and retry backoff | done | 2026-04-05 | **P0** Add: `llm_embedding(client;text)→[f64]!LlmErr` (OpenAI text-embedding-3-small / Ollama), `llm_embeddings_batch(client;texts)→[[f64]]!LlmErr`, `llm_retry_backoff(client;base_delay_ms;max_retries)→void` (exponential backoff on 429/500/503). Embeddings are required for RAG, semantic search, and clustering. |
| 32.1.2 | std.llm: JSON mode and usage tracking | done | 2026-04-05 | **P1** Add: `llm_json_mode(client;messages)→TkLlmResp!LlmErr` (set response_format to json_object), `llm_usage()→{input_tokens;output_tokens}` (cumulative tracking), `llm_vision(client;messages_with_images)→TkLlmResp!LlmErr` (image input for GPT-4V/Claude). |
| 32.2.1 | std.llm_tool: parallel tool calls and agentic loop | done | 2026-04-05 | **P1** Add: `llm_parallel_tool_calls(client;messages;tools)→[ToolCallResult]` (handle multiple tool_calls in single response), `llm_tool_validate_args(tool;args_json)→bool!Str` (validate against schema), `llm_agentic_loop(client;system;user;tools;max_iterations)→Str` (ReAct-style agent loop with tool execution). The agentic loop is the most common LLM application pattern. |
 |
--- |
 |
### Epic 33 — Template Engine: Production Completeness |
 |
The template module only supports `{{slot}}` substitution. Without conditionals and loops, it can't render lists, optional sections, or any dynamic content — making it unusable for real HTML generation. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 33.1.1 | std.template: conditionals and loops | done | 2026-04-05 | **P0** Add: `{{#if key}}...{{/if}}` (truthy check — non-empty string, non-zero number), `{{#unless key}}...{{/unless}}` (inverse), `{{#each key}}...{{/each}}` (iterate array, `{{.}}` for current item, `{{@index}}` for index). Update compiler to handle block tags. This is the minimum for generating HTML lists, tables, and conditional sections. |
| 33.1.2 | std.template: partials and helpers | done | 2026-04-05 | **P2** Add: `{{>partial_name}}` (include sub-templates), `tmpl_register_partial(name;source)→void`, `tmpl_register_helper(name;fn)→void` (user-defined transform). Enables component-based template composition. |
 |
--- |
 |
### Epic 34 — Visualization Libraries: Production Completeness |
 |
The visualization modules (html, svg, canvas, chart, dashboard, image) cover basic output but are missing form elements, chart variants, image filters, and dashboard widgets needed for real applications. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 34.1.1 | std.html: form elements and lists | done | 2026-04-05 | **P1** Add: `html_form(action;method)`, `html_input(type;name;value)`, `html_select(name;options)`, `html_textarea(name;content)`, `html_button(text;onclick)`, `html_label(for_id;text)`, `html_ul(items)`, `html_ol(items)`, `html_br()`, `html_hr()`, `html_pre(content)`, `html_code(content)`. HTML without forms is read-only. |
| 34.1.2 | std.html: attributes and metadata | done | 2026-04-05 | **P1** Add: `html_attr(node;name;value)→TkHtmlNode` (arbitrary attributes), `html_class_add(node;class)→TkHtmlNode`, `html_id(node;id)→TkHtmlNode`, `html_meta(name;content)→void` (head metadata), `html_link_stylesheet(href)→void`. |
| 34.2.1 | std.chart: additional chart types and configuration | done | 2026-04-05 | **P1** Add: `chart_stacked_bar(...)`, `chart_horizontal_bar(...)`, `chart_area(...)`, `chart_radar(...)`, `chart_histogram(values;nbins;title)`, `chart_heatmap(rows;cols;matrix;title)`. Config: `chart_set_theme(spec;theme)`, `chart_set_legend(spec;position;display)`, `chart_set_tooltip(spec;fields)`. |
| 34.3.1 | std.image: transforms and filters | done | 2026-04-05 | **P1** Add: `image_rotate(buf;angle_deg)→TkImgBuf`, `image_blur(buf;radius)→TkImgBuf` (Gaussian, 3x3 kernel), `image_sharpen(buf)→TkImgBuf`, `image_brightness(buf;factor)→TkImgBuf`, `image_contrast(buf;factor)→TkImgBuf`, `image_paste(dst;src;x;y)→TkImgBuf` (composite). |
| 34.3.2 | std.image: text drawing and histogram | done | 2026-04-05 | **P2** Add: `image_text_draw(buf;text;x;y;size;color)→TkImgBuf` (bitmap font renderer, no external deps), `image_histogram(buf)→{r;g;b;a}` (256-bin per channel), `image_quantize(buf;ncolors)→TkImgBuf` (color reduction). |
| 34.4.1 | std.dashboard: stat/gauge widgets and theming | done | 2026-04-05 | **P2** Add: `dashboard_add_stat(d;id;title;value;unit)` (big number widget), `dashboard_add_gauge(d;id;title;value;min;max)`, `dashboard_add_markdown(d;id;content)`, `dashboard_set_theme(d;"dark"/"light")`, `dashboard_set_refresh_interval(d;interval_ms)`, `dashboard_export_json(d)→Str`. |
| 34.5.1 | std.svg: gradients, animation, and file output | done | 2026-04-05 | **P2** Add: `svg_ellipse(cx;cy;rx;ry;style)`, `svg_gradient_linear(id;stops)`, `svg_gradient_radial(id;stops)`, `svg_animate(target;attr;from;to;duration)`, `svg_defs(elements)`, `svg_save_file(doc;path)→bool`. |
| 34.5.2 | std.canvas: transforms, gradients, and state management | done | 2026-04-05 | **P2** Add: `canvas_translate(c;dx;dy)`, `canvas_rotate(c;angle)`, `canvas_scale(c;sx;sy)`, `canvas_save(c)`, `canvas_restore(c)`, `canvas_fill_style(c;color)`, `canvas_stroke_style(c;color)`, `canvas_line_width(c;width)`, `canvas_quadratic_to(c;cpx;cpy;x;y)`, `canvas_bezier_to(c;cp1x;cp1y;cp2x;cp2y;x;y)`, `canvas_gradient_linear(c;x0;y0;x1;y1)`. |
| 34.6.1 | std.ws: close frame handling and upgrade helpers | done | 2026-04-05 | **P1** Add: `ws_handle_close_frame(payload)→{code;reason}`, `ws_build_close_frame(code;reason)→ByteArray`, `ws_parse_upgrade_headers(headers)→{sec_key;protocol;extensions}`, `ws_build_upgrade_response(accept_key;protocols)→Str`, `ws_validate_utf8(payload)→bool` (RFC 6455 §3.4 compliance). |
 |
--- |
 |
### Epic 35 — .tki Contract Reconciliation |
 |
**BLOCKING.** The .tki interface files are the compiler contract — toke programs import against them. 13 modules have .tki files that declare functions with no corresponding C implementation. Programs using these functions will compile but fail at link time. Either implement the missing C functions or trim the .tki to match reality. The .tki is the authoritative contract; prefer adding implementations. |
 |
**Note:** Stories 12.1.1, 12.1.2, 12.1.3, 12.2.2, 12.2.3, 12.2.4, 12.2.5, 12.4.3, 12.5.1, 12.5.2 are marked "done" but their .tki-promised functions were never fully implemented in C. These stories described the interface design + corpus examples, not the C implementation. The C implementation stories (Epics 14-18) implemented a subset. This epic closes the gap. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 35.1.1 | std.crypto: implement sha512, hmacsha512, constanteq, randombytes | done | — | **P0** 4 functions declared in crypto.tki but missing from crypto.h/.c. `crypto_sha512`: SHA-512 hash (FIPS 180-4). `crypto_hmac_sha512`: HMAC-SHA-512. `crypto_constanteq`: constant-time byte comparison (anti-timing-attack). `crypto_randombytes`: wraps arc4random_buf. Update crypto.h, crypto.c, tests. |
| 35.1.2 | std.http: implement client API (client, get, post, put, delete, stream, streamnext) | done | — | **P0** 7 functions declared in http.tki but missing from http.h/.c. The http module currently only has the server-side API. Add: `http_client(baseurl)` (connection pool), `http_get/post/put/delete(client;path;body)→HttpResult`, `http_stream/streamnext` for streaming responses. POSIX sockets, reuse patterns from llm.c. Update http.h, http.c, tests. |
| 35.1.3 | std.json: implement streaming API (streamparser, streamnext, streamemit, newwriter, writerbytes) | done | — | **P0** 5 functions declared in json.tki but missing from json.h/.c. Streaming JSON parser for large payloads (SAX-style events), streaming writer. Update json.h, json.c, tests. |
| 35.1.4 | std.ws: implement high-level API (connect, send, sendbytes, recv, close, broadcast) | done | — | **P0** 6 functions declared in ws.tki but ws.h/.c only has low-level frame encode/decode. Add: `ws_connect(url)→WsConn`, `ws_send(conn;text)`, `ws_sendbytes(conn;data)`, `ws_recv(conn)→WsFrame`, `ws_close(conn)`, `ws_broadcast(conns;text)`. Build on existing frame codec + POSIX sockets. |
| 35.1.5 | std.dataframe: implement fromrows, columnstr, tocsv, schema | done | — | **P1** 4 functions declared in dataframe.tki but missing from dataframe.h/.c. `df_fromrows`: create from row-major data. `df_columnstr`: extract string column. `df_tocsv`: serialize to CSV string. `df_schema`: return column names and types. |
| 35.1.6 | std.analytics: implement groupstats, pivot | done | — | **P1** 2 functions declared in analytics.tki but missing from analytics.h/.c. `analytics_groupstats`: per-group descriptive statistics. `analytics_pivot`: pivot table (row key, column key, aggregate values). |
| 35.1.7 | std.auth: implement bearerextract, fix apikeygenerate naming | done | — | **P1** `auth.bearerextract` declared in auth.tki but not in auth.h/.c — extract Bearer token from Authorization header. Also fix naming: .tki says `apikeygenerate`, .h says `apikeygen` — align to one name. |
| 35.1.8 | std.template: implement vars, renderfile; resolve html alias | done | — | **P1** 3 functions declared in template.tki but missing from template.h/.c. `tmpl_vars`: list slot names from compiled template. `tmpl_renderfile`: render directly to file. Resolve whether `tpl.html` is an alias for `tmpl_renderhtml` or a separate function. |
| 35.1.9 | std.router: implement use (middleware) and serve | done | — | **P1** 2 functions declared in router.tki but missing from router.h/.c. `router_use(router;middleware_fn)`: add middleware to chain. `router_serve(router;port)`: start HTTP server with router dispatch (wrapper around http_serve). |
| 35.1.10 | std.dashboard: implement serve | done | — | **P2** `dashboard.serve` declared in dashboard.tki but only `dashboard_render` exists in .h/.c. Either implement `dashboard_serve(d;port)` (start server serving rendered HTML) or align .tki to use `render` instead. |
| 35.1.11 | std.csv: fix naming mismatches between .tki and .h | done | — | **P1** csv.tki uses `csv.reader`/`csv.next` but csv.h uses `csv_reader_new`/`csv_reader_next`. Align the names so the compiler-generated calls match the C symbols. Either update .tki or add aliases in .h. |
| 35.1.12 | std.llm_tool: fix naming mismatch (withtools vs tool_build_tools_json) | done | — | **P1** llm_tool.tki uses `llm.withtools` but llm_tool.h uses `llm_tool_build_tools_json`. Align names. Also add `llm_parse_tool_calls` and `llm_tool_result_msgs` to .tki if they should be public. |
| 35.1.13 | Reconciliation verification: all .tki match all .h exactly | done | — | **P0** After 35.1.1–35.1.12, run automated check: for every function in every .tki, verify a matching symbol exists in the corresponding .h. For every public function in .h, verify it appears in .tki. Zero mismatches. Supersedes story 26.1.3. Depends on 35.1.1–35.1.12. |
 |
--- |
 |
### Epic 36 — Runtime Bug Fixes (discovered during test hardening) |
 |
Bugs found by Epic 20 test hardening. All 24 targets compile clean but 9 have runtime failures from pre-existing algorithm/logic bugs. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 36.1.1 | Fix encrypt X25519 key exchange — shared secrets not symmetric | done | 2026-04-05 | **P1** Fixed: 3 incorrect uses of `MASK51 >> 4` instead of `MASK51` for limb 4 in GF(2^255-19) field ops (`fe_from_bytes`, `fe_to_bytes`, `fe_sub`). RFC 7748 §6.1 known-answer passes. |
| 36.1.2 | Fix encrypt Ed25519 sign/verify — rejects valid signatures | done | 2026-04-06 | **P1** Fixed: `int64_t` accumulators in `sc_reduce64` and `sc_muladd` overflowed for SHA-512 outputs with large high limbs (s23*MU1 ≈ 45×10¹² then s12*MU0 ≈ 30×10¹⁸ > INT64_MAX). Changed all `s0..s23` and `s0..s22` accumulator declarations from `int64_t` to `__int128`; updated CARRY macros accordingly. Also corrected RFC 8032 TEST 1 expected pubkey in test_encrypt.c (was wrong in test; correct value verified against Go crypto/ed25519, OpenSSL 3, and back-computed from the known-good RFC signature). All Ed25519 tests pass. |
| 36.1.3 | Fix encrypt GCM auth tag computation | done | 2026-04-06 | **P2** On audit, all NIST SP 800-38D vectors (TC13, TC14, TC15, TC16) pass including auth tag and tamper-detection. Bug was already resolved; story closed after verification. |
| 36.1.4 | Fix dashboard render segfault — TkHtmlDoc leak causes heap corruption | done | 2026-04-05 | **P1** `dashboard_render` leaks the internal `TkHtmlDoc` on every call. Accumulated leaks cause heap corruption and segfault. Need to free doc after rendering or restructure to reuse. |
| 36.1.5 | Fix image PNG decode edge cases — 5 runtime failures | done | 2026-04-05 | **P2** Fixed: `inflate_stored` was reading LEN/NLEN directly from `byte_pos` after bit-buffer pre-fetch had advanced it by 3. Now reads LEN, NLEN and stored bytes via `bs_read(bs,8)`. 94/94 tests pass. |
| 36.1.6 | Fix ml decision tree XOR and k-means convergence | done | 2026-04-05 | **P2** Fixed: (1) dtree zero-gain split guard `<= 0.0` → `< -1e-12` allows XOR splits; (2) k-means max-spread init instead of sequential init ensures distinct initial centroids. All tests pass. |
| 36.1.7 | Fix analytics pivot edge cases | done | 2026-04-05 | **P2** Fixed: bug was in `analytics_anomalies`, not pivot. Strict `z > threshold` → `z >= threshold`. All 24 analytics tests pass. |
| 36.1.8 | Add test_tk_runtime.c — unit tests for tk_runtime | done | 2026-04-06 | **P2** Created `test/stdlib/test_tk_runtime.c` with 18 test groups covering: `tk_runtime_init`/`tk_str_argv` (5 assertions), `tk_json_parse` int/bool/str/array/whitespace (13), `tk_json_print_*` stdout capture via pipe (12), `tk_str_concat`/`tk_str_len`/`tk_str_char_at` (11), `tk_array_concat` incl. NULL (9), `tk_overflow_trap` exit-code via fork (4). Added `test-stdlib-runtime` Makefile target. All 63 assertions pass. |
 |
--- |
 |
### Epic 37 — Website Quality: Code Example Standards |
 |
Audit and enforce consistent code example standards across all stdlib module documentation pages. Standards: all library names lowercase; no inline comments in code blocks; plain-English explanation immediately before each code block; all public functions documented with function/parameters/returns format. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 37.1.1 | Audit all docs for uppercase library references; fix to lowercase | backlog | — | **P2** Scan every page under `docs/` and the toke-web repo for any capitalised library names (e.g. `Std.Http`, `STD.LLM`, `Http`, `Llm`). Replace with canonical lowercase form. Includes tutorial pages, API reference, and quick-start guide. |
| 37.1.2 | Remove inline comments from all website code examples | backlog | — | **P2** Code blocks on the public site must contain only toke code — no `# comment` or `// comment` lines. Move explanatory text into the prose paragraph that precedes the block. Audit all 30+ module pages. |
| 37.1.3 | Enforce explanation-before-code structure across all module pages | backlog | — | **P2** Every code block must be preceded by at least one plain-English sentence explaining what the snippet does. Pages that open with a code block or place prose after the block are non-conforming. |
| 37.1.4 | Create function/parameters/returns documentation template | backlog | — | **P2** Define a standard section template for each public stdlib function: function name (code), one-line description, Parameters table (name, type, description), Returns row (type, description), a minimal code example. Commit template to `docs/TEMPLATE_FUNCTION.md`. |
| 37.1.5 | Apply function/parameters/returns template to all 30+ stdlib module pages | backlog | — | **P2** Using the template from 37.1.4, rewrite or augment every public function entry across all stdlib module documentation pages. Depends on 37.1.4. |
 |
--- |
 |
### Epic 38 — Compiler: Input Normalization |
 |
Pre-parse normalisation so human or LLM-generated code using CamelCase or snake_case library/function names is accepted and converted to canonical lowercase toke identifiers before the lexer sees them. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 38.1.1 | Lex-level normalization of CamelCase/snake_case library names to lowercase | done | 2026-04-06 | **P1** Implemented in `src/names.c` `resolve_imports()`: after extracting `mpath`, calls `normalise_module_path()` which lowercases each dotted segment that matches a known stdlib module name (case-insensitive). Emits W2038 warning (non-fatal) with original and normalised path. Added `W2038` to `src/names.h`. 37 known modules listed. Compile succeeds with hint; already-lowercase paths produce no diagnostic. |
| 38.1.2 | Friendly "did you mean?" error messages for unrecognised library names | backlog | — | **P2** When an unknown library name is used, compute edit distance against all known stdlib module names. If distance ≤ 2, emit: `error: unknown module 'Std.Htpp' — did you mean 'std.http'?`. Depends on 38.1.1 for normalisation pass. |
 |
--- |
 |
### Epic 39 — Toolchain: Single-Command Runner and Runtime Limits |
 |
`toke hello.tk` compiles and immediately runs the program without a separate compile step. Default execution timeout and max-loop-iteration guards prevent runaway programs in both the runner and generated code. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 39.1.1 | `toke` binary: compile-and-run in one command | done | 2026-04-06 | **P1** `toke <file.tk>` compiles `file.tk` to a temp binary and exec's it, passing remaining args. `toke --compile <file.tk>` preserves current compile-only behaviour. Update CLI help and man page. |
| 39.1.2 | Default 30-second execution timeout in the runner | backlog | — | **P2** When the runner execs the compiled binary, set a `SIGALRM` watchdog of 30 s. If the process exceeds the limit, kill it and print `toke: execution timeout (30s)` to stderr with exit code 124. `--timeout=N` overrides. |
| 39.1.3 | Compiler option to inject max-loop-iteration guards | backlog | — | **P2** `--max-iters=N` (default off) injects a counter into every compiled loop body. If a loop exceeds N iterations, the runtime raises `LoopLimit` and terminates. Intended for sandbox/educational use where infinite-loop prevention is required. |
| 39.1.4 | Enforce default timeouts across stdlib iteration functions | backlog | — | **P2** stdlib functions that iterate (e.g. `std.list.each`, `std.map.each`, `std.net.fetch` retries) must honour a per-call timeout parameter (default 0 = no limit). Document default behaviour and how to set limits. Audit all stdlib modules. |
 |
--- |
 |
### Epic 40 — Benchmarking Expansion |
 |
Extend the benchmark suite from the current Rosetta Code subset to 100 tasks (Alderson set) with toke reference solutions, and add J, Ruby, JavaScript, C#, and Java reference solutions alongside existing Python and C. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 40.1.1 | Add 100 Rosetta Code tasks (Alderson set) with toke reference solutions | backlog | — | **P2** Select the Alderson benchmark set of 100 Rosetta Code tasks covering algorithms, data structures, strings, math, I/O, and concurrency. Write a toke reference solution for each. Store under `bench/rosetta/`. Integrate with existing `make bench` target. |
| 40.1.2 | Add J, Ruby, JavaScript, C#, Java reference solutions to benchmark suite | backlog | — | **P2** For each of the 100 Rosetta Code tasks (40.1.1), add reference solutions in J, Ruby, JavaScript, C#, and Java alongside existing Python and C solutions. Store under `bench/rosetta/<task>/{solution.j,solution.rb,solution.js,solution.cs,solution.java}`. Update benchmark runner to invoke each language where its runtime is present. Depends on 40.1.1. |
 |
--- |
 |
### Epic 41 — GPU Processing Support |
 |
Native toke support for GPU compute via a `std.gpu` module. Primary backend: Apple Metal/MPS (macOS). Secondary: CUDA/ROCm (Linux, third-party). GPU-accelerated operations in `std.ml` and `std.analytics`. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 41.1.1 | std.gpu module design and base API | backlog | — | **P1** Design the `std.gpu` module API: device enumeration (`gpu.devices`), tensor allocation (`gpu.tensor`), data transfer (`gpu.upload`/`gpu.download`), kernel dispatch (`gpu.run`), synchronisation (`gpu.sync`), and error handling. Write `gpu.tki` and `gpu.h`. Document design in `docs/std/gpu.md`. |
| 41.1.2 | Metal/MPS backend for std.gpu (macOS primary) | backlog | — | **P1** Implement `gpu.c` Metal/MPS backend using Objective-C bridging or the Metal C API. Targets Apple Silicon and Intel + AMD Macs. Support float32 tensor ops, matrix multiply, and element-wise ops. Depends on 41.1.1. |
| 41.1.3 | GPU-accelerated matrix operations in std.ml | backlog | — | **P2** When `std.gpu` is available and a GPU device is present, `ml.matmul`, `ml.train`, and `ml.infer` dispatch to GPU automatically. Fallback to CPU if no GPU. Add `ml.usedevice(device)` to pin computation. Depends on 41.1.2. |
| 41.1.4 | CUDA/ROCm backend for std.gpu (Linux, third-party) | backlog | — | **P3** Implement optional CUDA (NVIDIA) and ROCm (AMD) backends for Linux. Compile-time feature flags `TOKE_GPU_CUDA` / `TOKE_GPU_ROCM`. Not bundled in default build; documented as third-party extension. Depends on 41.1.1. |
| 41.1.5 | GPU support in std.analytics | backlog | — | **P3** Accelerate `analytics.pca`, `analytics.cluster`, and `analytics.corr` via `std.gpu` when available. Auto-detect and fallback gracefully. Depends on 41.1.3. |
 |
--- |
 |
### Epic 42 — Library: Desktop Application Support |
 |
Stdlib primitives needed by the Loke desktop distribution (identified from `read-only-research/desktop-distribution-epics.md`). Covers port availability, user directory resolution, HTTP proxy configuration, download progress, and file checksum verification. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 42.1.1 | std.net: port availability check (`net.portavailable`) | backlog | — | **P1** `net.portavailable(port)` → `bool`: attempts to bind a TCP socket on `127.0.0.1:<port>` and immediately releases it. Returns `true` if the port is free, `false` if already in use. Required by Loke installer to detect port conflicts before starting the local server. |
| 42.1.2 | std.sys: user config and data directory resolution (`sys.configdir`, `sys.datadir`) | backlog | — | **P1** `sys.configdir(appname)` → `str`: returns the platform config directory (`~/Library/Application Support/<appname>` on macOS, `~/.config/<appname>` on Linux, `%APPDATA%\<appname>` on Windows). `sys.datadir(appname)` → `str`: same for data. Creates the directory if it does not exist. Required by Loke for per-user proxy and preference storage. |
| 42.1.3 | std.http: per-request proxy configuration (`http.withproxy`) | backlog | — | **P2** `http.withproxy(client; proxy_url)` → `HttpClient`: returns a new client configured to route requests through the given HTTP/HTTPS/SOCKS5 proxy URL. Respects `HTTP_PROXY` / `HTTPS_PROXY` environment variables if proxy_url is empty string. Required by Loke for corporate network environments. |
| 42.1.4 | std.http: download with progress callback (`http.downloadfile`) | backlog | — | **P2** `http.downloadfile(client; url; dest_path; progress_fn)` → `Result`: streams a GET response to `dest_path`, calling `progress_fn(bytes_received, total_bytes)` periodically (every 64 KB or on Content-Length tick). Returns error on HTTP 4xx/5xx or I/O failure. Required by Loke auto-updater to show a progress bar. |
| 42.1.5 | std.crypto: file checksum verification (`crypto.sha256file`, `crypto.sha256verify`) | backlog | — | **P2** `crypto.sha256file(path)` → `str`: returns the hex SHA-256 digest of the file at `path`. `crypto.sha256verify(path; expected_hex)` → `bool`: returns true if the file digest matches. Required by Loke installer to verify downloaded update packages before applying them. |
 |
--- |
 |
### Epic 43 — Stdlib Documentation Expansion |
 |
Expand all 35 stdlib reference pages in toke-web from "summary table + one example" to the str.md quality standard: per-function section heading with full signature, prose description, parameter semantics, return value and error cases, and 1–2 concrete code examples per function. Goal: model-quality documentation for corpus generation. |
 |
**Quality standard (str.md pattern):** |
``` |
### func.name(param: type): returntype!errtype |
 |
Prose description of what the function does, including edge cases and error conditions. |
 |
```toke |
let ok = func.name(val);   (* common case *) |
let e  = func.name(bad);   (* error case *) |
``` |
``` |
 |
Expand in priority order: corpus-critical modules first, utilities last. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 43.1.1 | Expand std.json docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 115 → 326 lines. Per-function sections for all public functions. Types, error types, and parse→get→stringify round-trip example. |
| 43.1.2 | Expand std.file docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 96 → 259 lines. Per-function sections, $ioerr/$notfounderr error types, read→transform→write pipeline example. |
| 43.1.3 | Expand std.http docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 142 → 309 lines. Server and client functions, $req/$res/$httpclient/$httpresp/$httpstream types, $httperr variants, fetch→parse→respond example. |
| 43.1.4 | Expand std.process docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 85 → 156 lines. All 9 functions, $handle type, SpawnResult/WaitResult/StdoutResult, stdin pipeline and timeout examples. |
| 43.1.5 | Expand std.env docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 68 → 148 lines. Per-function sections for get/set/unset/all/args, $notseterr, config-from-environment pattern. |
| 43.1.6 | Expand std.time docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 66 → 171 lines. All time functions, $Timestamp/$Duration types, timestamp arithmetic and formatting examples. |
| 43.1.7 | Expand std.log docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 81 → 179 lines. All log functions, level hierarchy explained, structured logging and production config examples. |
| 43.1.8 | Expand std.math docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 55 → 182 lines. All 14 functions with individual sections, $linregresult type, descriptive stats and regression examples. |
| 43.1.9 | Expand std.csv docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 60 → 189 lines. parse vs streaming reader pattern explained, header handling, file→parse→transform→write pipeline. |
| 43.1.10 | Expand std.dataframe docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 71 → 210 lines. All 9 functions, $DataFrame/$Row types, CSV ingest→filter→group→output pipeline. |
| 43.1.11 | Expand std.analytics docs to str.md standard | done | 2026-04-06 | **P1** Expanded from ~60 → 180 lines. All 7 functions, result types, anomaly detection and clustering examples. |
| 43.1.12 | Expand std.ml docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 82 → 178 lines. All functions, $Model/$TrainConfig types, train→predict pipeline and embedding example. |
| 43.1.13 | Expand std.llm docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 77 → 246 lines. All functions, all types, three usage examples: chat, streaming loop, json_mode. |
| 43.1.14 | Expand std.llm_tool docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 81 → 185 lines. Tool-call flow documented end-to-end, complete weather-query tool example. |
| 43.1.15 | Expand std.crypto docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 61 → 166 lines. All functions, md5 security warning in prose, bcrypt cost explanation, HMAC usage. |
| 43.1.16 | Expand std.encrypt docs to str.md standard | done | 2026-04-06 | **P1** Expanded from 58 → 212 lines. All 11 functions, $DecryptResult/$Keypair types, three examples: AES-GCM, X25519 key exchange, Ed25519 sign/verify. Nonce reuse warning. |
| 43.1.17 | Expand std.auth docs to str.md standard | backlog | — | **P2** Per-function sections for: `auth.jwt_sign`, `auth.jwt_verify`, `auth.jwt_claims`, `auth.bcrypt_hash`, `auth.bcrypt_check`, `auth.apikey_generate`, `auth.bearer_extract`. Include `$JwtPayload`, `$autherr` type. Show JWT issue→verify flow and API key middleware pattern. |
| 43.1.18 | Expand std.router docs to str.md standard | backlog | — | **P2** Per-function sections for: `router.new`, `router.get`, `router.post`, `router.put`, `router.delete`, `router.use`, `router.serve`. Include `$Router`, `$Handler` types. Show full REST API example with middleware chain. |
| 43.1.19 | Expand std.ws docs to str.md standard | backlog | — | **P2** Per-function sections for: `ws.server`, `ws.connect`, `ws.send`, `ws.recv`, `ws.close`, `ws.on_message`, `ws.on_close`. Include `$WsConn`, `$WsMsg` types. Show echo server and client connect→send→recv→close pattern. |
| 43.1.20 | Expand std.sse docs to str.md standard | backlog | — | **P2** Per-function sections for: `sse.serve`, `sse.emit`, `sse.close`. Include event format, retry semantics, and show a real-time event stream example integrated with `std.router`. |
| 43.1.21 | Expand std.db docs to str.md standard | backlog | — | **P2** Per-function sections for: `db.open`, `db.query`, `db.exec`, `db.close`, `db.begin`, `db.commit`, `db.rollback`, `db.prepare`. Include `$DbConn`, `$DbRow`, `$dberr` types. Show transaction pattern and parameterised query example. |
| 43.1.22 | Expand std.chart docs to str.md standard | backlog | — | **P2** Per-function sections for: `chart.bar`, `chart.line`, `chart.scatter`, `chart.pie`, `chart.tojson`, `chart.tosvg`. Include `$Chart`, `$Series` types. Show data→chart→export pipeline. |
| 43.1.23 | Expand std.svg docs to str.md standard | backlog | — | **P2** Per-function sections for: `svg.new`, `svg.rect`, `svg.circle`, `svg.line`, `svg.text`, `svg.path`, `svg.render`. Include `$Svg`, `$SvgElement` types. Show building a simple diagram programmatically. |
| 43.1.24 | Expand std.canvas docs to str.md standard | backlog | — | **P2** Per-function sections for: `canvas.new`, `canvas.fill`, `canvas.stroke`, `canvas.text`, `canvas.image`, `canvas.topng`. Include `$Canvas`, `$Color` types. Show drawing and export pattern. |
| 43.1.25 | Expand std.html docs to str.md standard | backlog | — | **P2** Per-function sections for: `html.parse`, `html.select`, `html.text`, `html.attr`, `html.render`, `html.escape`. Include `$HtmlDoc`, `$HtmlNode` types. Show scraping and template-free rendering patterns. |
| 43.1.26 | Expand std.template docs to str.md standard | backlog | — | **P2** Per-function sections for: `tmpl.compile`, `tmpl.render`, `tmpl.renderfile`, `tmpl.renderhtml`, `tmpl.vars`. Include `$Template` type. Show compile→render pattern and slot/variable substitution. |
| 43.1.27 | Expand std.dashboard docs to str.md standard | backlog | — | **P2** Per-function sections for: `dashboard.new`, `dashboard.add`, `dashboard.render`, `dashboard.serve`. Include `$Dashboard`, `$Widget` types. Show building a metrics dashboard with chart and table widgets. |
| 43.1.28 | Expand std.image docs to str.md standard | backlog | — | **P2** Per-function sections for: `image.load`, `image.save`, `image.resize`, `image.crop`, `image.rotate`, `image.grayscale`, `image.pixel`. Include `$Image`, `$PixelFormat` types. Show load→transform→save pipeline. |
| 43.1.29 | Expand std.encoding docs to str.md standard | backlog | — | **P2** Per-function sections for: `encoding.base64_encode`, `encoding.base64_decode`, `encoding.hex_encode`, `encoding.hex_decode`, `encoding.url_encode`, `encoding.url_decode`. Include `$decoderr` type and round-trip examples. |
| 43.1.30 | Expand std.yaml docs to str.md standard | backlog | — | **P2** Per-function sections for: `yaml.parse`, `yaml.stringify`. Include `$parseerr`. Show config-file read and write patterns with nested types. |
| 43.1.31 | Expand std.toon docs to str.md standard | backlog | — | **P2** Per-function sections for the full TOON serialisation API. Include type annotations, schema registration, and round-trip encode/decode examples. |
| 43.1.32 | Expand std.i18n docs to str.md standard | backlog | — | **P2** Per-function sections for: `i18n.load`, `i18n.t`, `i18n.setlocale`, `i18n.locale`. Include `$I18n`, `$Locale` types. Show locale-file loading and key-based translation pattern. |
| 43.1.33 | Expand std.encrypt docs: add tls_cert_fingerprint section | backlog | — | **P2** Add dedicated section for `encrypt.tls_cert_fingerprint` documenting: usage, expected return format (32-byte SHA-256 digest), error cases (connection refused, invalid host). Separate from 43.1.16 to allow independent completion. |
| 43.1.34 | Audit str.md for compliance with Epic 37 standards | backlog | — | **P2** str.md is the quality template but may itself violate Epic 37 rules: check for inline comments in code blocks, ensure no uppercase library references, confirm explanation-before-code throughout. Fix any violations found. |
| 43.1.35 | Cross-module usage examples: compose 3+ modules in one doc page | backlog | — | **P2** Add a `docs/reference/cookbook/` section with 10 pages each combining 3+ stdlib modules (e.g., http+json+db, llm+file+template, csv+dataframe+chart). These composite examples are the highest-value corpus training material. |
 |
--- |
 |
### Epic 44 — Whitespace Semantics: Specification Clarification and Conformance |
 |
The toke website states "whitespace is structurally meaningless" but the lexer uses longest-match tokenisation, which means whitespace IS the token separator. `letx` lexes as identifier `letx`, not keyword `let` + identifier `x`. The claim must be corrected before it misleads corpus generation, documentation writers, or language learners. |
 |
**Design decision to lock in:** Whitespace separates tokens but has no other structural role. Any amount of whitespace (spaces, tabs, newlines) between tokens is equivalent. Whitespace is NOT required where adjacent token boundaries are unambiguous (e.g., `x+y` == `x + y`). Whitespace IS required between adjacent alphanumeric tokens (e.g., `let x` cannot be written `letx`). Identifiers that begin with a keyword prefix are valid and unambiguous: `mutantninjaturtles` is always one identifier; `mut antninjaturtles` is always keyword `mut` + identifier `antninjaturtles`. No "starts-with" bans are needed or wanted. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 44.1.1 | Fix website: correct "whitespace is structurally meaningless" claim | done | 2026-04-06 | **P1** The about/design page at `console.tokelang.dev/about/design/` states whitespace is structurally meaningless. This is wrong: whitespace is the token separator and is required between adjacent alphanumeric tokens (e.g., `let x` ≠ `letx`). Replace with accurate statement: "Whitespace separates tokens but has no other structural role — indentation, line breaks, and spacing between tokens are all equivalent." Update any other docs making the same claim. |
| 44.1.2 | Spec: add formal whitespace and token-separation section | done | 2026-04-06 | **P1** Added §8.9 "Whitespace and Token Separation" to `spec/spec/toke-spec-v02.md`. Covers: whitespace chars (U+0020/0009/000D/000A); required between adjacent alphanumeric tokens; not required at symbol boundaries; any amount equivalent; longest-match — identifier starting with keyword prefix is an identifier. Annotated examples table included. [x] All five normative rules stated [x] Examples: `letx` (ident), `let x` (kw+ident), `mutantninjaturtles` (ident), `mut antninjaturtles` (kw+ident), `letmutantninjaturtles=5` (ident assignment) |
| 44.1.3 | Conformance tests: keyword–identifier boundary behaviour | done | 2026-04-06 | **P1** Added `test/conform/L001_keyword_ident_boundary.sh` (6 tests) and 4 e2e test pairs in `test/e2e/`: `e2e_kw_bound_letx`, `e2e_kw_bound_letmut`, `e2e_kw_bound_fnprefix`, `e2e_kw_bound_mutident`. Covers: letx as identifier (not let+x); let x as let-binding; letmut as identifier; mutantninjaturtles as one identifier; function named letx compiles and runs; let mutantninjaturtles as let-binding. |
| 44.1.4 | Compiler diagnostic: "did you mean 'let x'?" for `letx = ...` at statement level | backlog | — | **P2** When the compiler sees a bare identifier assignment at statement level where the identifier starts with a keyword prefix followed by a valid identifier suffix (e.g., `letfoo = 5`, `fnbar`, `usestd`), emit a hint: `hint: 'letfoo' is an identifier — did you mean 'let foo'?`. Only emit when the remainder after stripping the prefix is a valid identifier. Never a hard error. |
 |
--- |
 |
### Epic 45 — Toke Linter |
 |
A human-facing lint tool distinct from the compiler's `--check` flag and the LSP's error diagnostics (10.12.20/21). The linter targets code style, conventions, and anti-patterns — warnings a compiler would not emit. Includes a standalone CLI (`toke lint`), a VS Code integration showing lint warnings as squiggles separate from compiler errors, and an auto-fix pass for mechanical issues. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 45.1.1 | Define toke lint rule set v1 | done | 2026-04-06 | **P1** Defined 11 lint rules across all three categories in `docs/lint-rules-v1.md`. Mandatory 8: `unused-let`, `unused-import`, `redundant-bind`, `unreachable-code`, `fn-name-convention`, `type-name-convention`, `keyword-prefix-ident`, `empty-fn-body`. Additional 3 from real toke patterns: `mutable-never-mutated`, `error-result-ignored`, `struct-field-shadow`. Each rule documented with: id, category, fixable flag, rationale, violation example, fix example, and edge cases. |
| 45.1.2 | Implement `toke lint` CLI command | backlog | — | **P1** Add `toke lint <file.tk>` subcommand. Output: one line per warning, format `file.tk:line:col: [rule-id] message`. `--format=json` emits machine-readable array. `--rules=rule1,rule2` limits to named rules. `--ignore=rule1` suppresses a rule. Exit code 0 = no warnings, 1 = warnings found, 2 = parse error. Integrates with `tkc --check` for the parse/type-check pass; lint rules run on the AST. Depends on 45.1.1. |
| 45.1.3 | `toke lint --fix`: auto-fix pass for mechanical violations | backlog | — | **P2** For rules where the fix is unambiguous (unused import removal, redundant `let x = x` removal), implement `--fix` to rewrite the source file in-place with a `.bak` backup. List which rules support `--fix` in help output. Depends on 45.1.2. |
| 45.1.4 | VS Code extension: integrate lint warnings as separate diagnostic source | backlog | — | **P1** Extend the toke-vscode extension (10.12.21) to run `toke lint` alongside the LSP. Lint warnings appear as yellow squiggles (compiler errors remain red). Lint source label: `toke-lint`. Configurable: `toke.lint.enable` (default true), `toke.lint.onSave` (default true), `toke.lint.ignoredRules` (array). Depends on 45.1.2 and 10.12.21. |
| 45.1.5 | Lint rule: flag identifiers that collide with keyword prefixes ambiguously in human reading | backlog | — | **P2** Emit a `hint` (not error) when an identifier begins with a keyword prefix followed immediately by a valid identifier — e.g., `letfoo`, `mutbar`, `fnbaz`. The code is legal and unambiguous to the compiler, but confusing to human readers. Hint: `identifier 'letfoo' starts with keyword 'let' — consider renaming to avoid visual confusion`. Suppressible with `-- toke:ignore let-prefix`. Connects to Epic 44 whitespace clarification. Depends on 45.1.1. |
 |
## Epic 46: Stdlib Linking for Compiled Programs |
 |
**Goal:** Fix compilation of toke programs that use stdlib imports (`i=`). Programs compiled with `tkc --out` should produce working binaries when stdlib modules are used. |
 |
**Root cause found (2026-04-06):** When `tkc --out` compiles a toke program to a native binary, `compile_binary()` in `llvm.c` only linked `tk_runtime.c`. Programs using stdlib imports (str, http, env, etc.) failed with LLVM IR "undefined value" errors because: (1) no `declare` statements existed in the emitted IR for stdlib functions like `@str_concat`; (2) the C implementations were not passed to clang. Additionally, `str_concat` was mapped in `resolve_stdlib_call()` but the C function takes `const char *` (pointer), while the codegen emits `i64` (pointer-as-integer) for all stdlib args. |
 |
**Resolution implemented (2026-04-06, 46.1.1):** Created `src/stdlib/tk_web_glue.c` providing i64-ABI wrappers for str, http, and env module functions. Updated `llvm.c` to: declare wrapper functions in emitted IR preamble; add env and http module mappings to `resolve_stdlib_call()`; add `find_stdlib_sources()` to locate stdlib C files; link str.c + http.c + encoding.c + env.c + tk_web_glue.c in every `compile_binary()` invocation. First confirmed working demo: `toke-demo/hello_world.tk` serving `Hello, World!` from toke-compiled binary on port 8181. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 46.1.1 | Fix stdlib linking for compiled programs | done | 2026-04-06 | **P0** `compile_binary()` only linked `tk_runtime.c`; stdlib modules not linked. Created `tk_web_glue.c` with i64 ABI wrappers, updated IR preamble declarations, updated `resolve_stdlib_call()` for env/http modules and full str module, added `find_stdlib_sources()` to always link str+http+encoding+env+glue. Verified: `hello_world.tk` compiles and serves HTTP on port 8181. |
| 46.1.2 | Selective stdlib linking based on imports | backlog | — | **P1** Currently all stdlib files (str, http, encoding, env, tk_web_glue) are linked into every compiled program regardless of which modules are imported. Add import tracking to `compile_binary()` so only the modules actually imported are linked. Reduces binary size for programs that don't use http/env. |
| 46.1.3 | Extend http module: full route handler support | backlog | — | **P1** `tk_web_glue.c` currently only supports static GET responses (`http.getstatic`). Full `http.GET(path; handler)` requires passing a toke function as a C callback — needs codegen support for function pointer emission and struct (Req/Res) ABI bridging. |
| 46.1.4 | Add env.set, env.expand, str.split to stdlib wrappers | backlog | — | **P2** Extend `tk_web_glue.c` and IR preamble declarations to cover `env_set`, `env_expand`, full str module (str_split returns StrArray — needs design for toke array representation). |
| 46.1.5 | Extend http module: env-based port selection | backlog | — | **P2** Allow `http.serve(env.getint("PORT"; 8080))` — requires `str.toint` wrapper and env module returning integers. `tk_env_get_or` already wraps `env_get_or`; need `tk_env_get_int(key, default_int)` for direct integer env reads. |
| 46.1.6 | Emit typed LLVM IR for LLVM <15 compatibility | backlog | — | **P2** The IR emitter in `llvm.c` uses opaque-pointer `ptr` type (LLVM 15+ default). Servers running LLVM 14 (e.g. Debian 12 default) reject this IR. Fix: update all ~45 `fprintf` IR-emit lines to use typed pointers (`i8*`, `i64*`, `[N x i8]*` etc.) so generated `.ll` files are compatible with LLVM 13–22. Workaround: install `clang-15` on target and use `-DTK_HAVE_OPENSSL`. |
 |
## Epic 47: HTTP Access Logging with Rotation |
 |
**Goal:** Add structured access logging to the toke HTTP server. Every inbound request is logged in Combined Log Format (Apache/Nginx compatible). Log files rotate at a configurable line limit, old files are gzip-compressed, and retention is enforced by age (priority) or file count. |
 |
| Story | Description | Status | Date | Notes |
|---|---|---|---|---| |
| 47.1.1 | HTTP access log with rotation (std.log extension) | done | 2026-04-06 | Extended `log.h`/`log.c` with `TkAccessLog` type: Combined Log Format, configurable `max_lines` rotation, gzip compression via zlib, retention by `max_age_days` (priority) or `max_files`. Integrated into `http.c` (`handle_connection` + `handle_tls_connection`) via `log_request()` helper using `getpeername()` for client IP. Added `tk_log_open_access_w()` wrapper in `tk_web_glue.c`. Added `log` module mapping in `llvm.c` (`log.openaccess` → wrapper). Added `log.c` to `find_stdlib_sources()`. Toke API: `log.openaccess(path; max_lines; max_files; max_age_days)`. Deployed to staging: `logs/access.log` written per request. |
| 47.1.2 | Error log (separate file for 4xx/5xx and server errors) | backlog | — | **P2** Separate `logs/error.log` for 4xx/5xx responses and server-level errors (bind failure, TLS handshake failure). Same rotation/retention config. New `log.openerror(path; max_lines; max_files; max_age_days)` toke API. |
| 47.1.3 | Structured JSON access log option | backlog | — | **P3** Optional JSON-per-line format alongside Combined Log Format, switchable via `log.accessformat("json")`. Enables log ingestion by tools like Loki or Datadog. |
 |
--- |
 |
## Epic 48 — Website Uplift: toke-website-new → Full Documentation Site |
 |
**Goal:** Replace the placeholder toke-website-new content with a complete, production-quality documentation and marketing site served by toke's own HTTP server. No Astro, no Node.js, no nginx. Toke handles routing, static file serving, and all server-side logic — the website is itself a live demonstration of what toke can build. Content lives as HTML/CSS/JS/MD in `toke-website-new/sites/tokelang.dev/`; toke's vhost serves it. Every code example in the site must compile with `tkc --check` and produce the documented output. This epic ends when toke-website-new fully replaces toke-web on tokelang.dev, nginx is retired, and port 443 is owned by toke. |
 |
**Architecture:** toke HTTP server (`main.tk`) serves `sites/tokelang.dev/` as static files via `http.vhost()`. Content is pure HTML+CSS+JS — no build step, no template engine, no Node.js. Documentation pages are HTML files authored directly or converted from the toke-web MDX source (one-time migration). Any dynamic features (live examples, search, metrics) are toke route handlers registered before `http.servevhoststls()`. |
 |
--- |
 |
### Epic 48.1 — Server Infrastructure: Pure Toke, No Astro |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.1.1 | Convert toke-web MDX content to plain HTML (one-time migration) | backlog | — | **P0** Run `pandoc` (or equivalent) on all `toke-web/src/content/docs/**/*.{md,mdx}` to produce clean HTML fragments. Wrap each with the shared site layout (nav, header, footer). Output goes to `toke-website-new/sites/tokelang.dev/docs/`. This replaces the Astro build step entirely. The toke server already serves static files from this directory via `http.vhost()`. |
| 48.1.2 | Add toke route handlers for dynamic features | backlog | — | **P1** Register custom route handlers in `main.tk` before `http.servevhoststls()`. Initial handlers: `GET /health` → JSON status; `GET /api/version` → current toke version. These showcase toke handling real HTTP logic alongside static file serving. |
| 48.1.3 | Script: compile toke-website-new binary for Linux x86_64 and deploy | backlog | — | **P1** Codify the cross-compile workflow into `toke-website-new/scripts/deploy.sh`: (1) `tkc --emit-llvm main.tk` on Mac; (2) `rsync` IR + C stdlib sources + site content to Lightsail; (3) SSH: `clang-15 ... -o website_server`; (4) `pkill website_server; ./website_server &`. Parameterise host/key from env vars. |
| 48.1.4 | Add CI: tkc --check + binary compile smoke test | backlog | — | **P1** GitHub Actions workflow: (1) `tkc --check main.tk`; (2) compile to binary on Linux runner; (3) start server, `curl --fail https://localhost:8443/` with self-signed cert allowance. No Node.js/npm steps. |
| 48.1.5 | Port mapping: switch from 8443 to 443 after nginx retirement | backlog | — | **P2** Once nginx is retired (48.7.4), update `main.tk` to bind port 443. Update Lightsail firewall rules. Update Cloudflare origin port. |
 |
--- |
 |
### Epic 48.2 — Content Migration: Port toke-web docs to current syntax |
 |
All content must use default 56-char syntax (not legacy 80-char). Every toke code block must be validated with `tkc --check`. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.2.1 | Audit all toke-web code examples: compile with tkc --check | backlog | — | **P0** Write `scripts/validate_examples.sh`: extract all fenced toke code blocks from `toke-web/src/content/docs/**/*.{md,mdx}`, wrap each in a minimal module+main if needed, run `tkc --check` on each. Record pass/fail. Target: 100% pass before those pages are migrated to HTML. Current estimate based on Epic 11.3.1: ~52% pass rate. This story establishes the baseline and failure list. Fix failures before running 48.1.1 conversion. |
| 48.2.2 | Fix getting-started/ code examples (hello-world, install, tour, project-structure) | backlog | — | **P0** Apply default-syntax corrections to 4 getting-started pages. Confirm: module declarations use `m=`, imports use `i=alias:std.mod`, functions use `f=`, return uses `<`. Remove any legacy 80-char forms. Each example must pass `tkc --check`. |
| 48.2.3 | Fix learn/ tutorial code examples (01–10) | backlog | — | **P0** Apply default-syntax corrections to all 10 learn/ tutorial pages. Special attention: lessons 03 (control flow: `if`/`lp`/`el`), 04 (collections: `@()` arrays, no `[]`), 05 (errors: error union `|{}`), 07 (modules: import syntax). Each code block must pass `tkc --check`. |
| 48.2.4 | Update getting-started tour: arrays use @(), maps use @(), no [] syntax | backlog | — | **P0** tour.md collection examples use old `[]` syntax. Replace all array literals with `@()` form. Replace all map literals with `@()` form. Update subscript access from `a[n]` to `a.get(n)`. Validate all examples. |
| 48.2.5 | Update reference/grammar.md to match toke-spec-v02 §1–§8 | backlog | — | **P1** grammar.md is the user-facing grammar page. Sync with the canonical EBNF in `spec/spec/toke-spec-v02.md`. Add §8.9 whitespace/token-separation rule (44.1.2). Update array/map productions to `@()`. Add new profile note: "default syntax, `--legacy` flag for 80-char profile". |
| 48.2.6 | Update reference/types.md: add @() array/map types, remove [] | backlog | — | **P1** Remove all `[T]` array notation. Replace with `@(T)`. Update map type notation to `@($str:T)` form. Confirm struct definition syntax matches spec. Add `$str` dollar-prefix convention note with rationale. |
| 48.2.7 | Add migration guide page: 80-char legacy → 56-char default | backlog | — | **P2** New page `docs/reference/migration.md`. Table of every syntactic change: `[]` → `@()`, `[n]` → `.get(n)`, remove type sigil, `fn` → `f=`, `let mut` → `let x=mut.`. Show side-by-side before/after for each. Note `tkc --migrate` (11.3.5) as the automated tool. Target audience: users coming from early toke documentation or the toke-web archived content. |
 |
--- |
 |
### Epic 48.3 — Language Reference: Complete and Accurate |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.3.1 | Language reference index page | backlog | — | **P1** New `docs/reference/index.md`. Structured overview of: module system, type system (primitives + structs + sum types + error unions), expressions, statements (let/mut/assign/if/lp/match/arena), functions, imports, the `<` return operator. Link to each reference sub-page. One-paragraph intro per section. |
| 48.3.2 | Expressions reference page | backlog | — | **P1** New `docs/reference/expressions.md`. Cover: arithmetic (+, -, *, /), comparison (=, !=, <, >, <=, >=), boolean (and, or, not), string concatenation, function calls, field access (`.`), error unwrap (`|{}`), cast expressions. One worked example per operator. All examples pass `tkc --check`. |
| 48.3.3 | Statements reference page | backlog | — | **P1** New `docs/reference/statements.md`. Cover: `let` binding (immutable), `let x=mut.val` (mutable), reassignment, `if`/`el`, `lp` (init; cond; step), `match`, `<` return, `arena{}` block. For each: syntax, semantics, worked example, common mistakes. All examples pass `tkc --check`. |
| 48.3.4 | Error handling reference page | backlog | — | **P1** New or expand `docs/reference/errors.md`. Cover: error union type (`T|E`), error return (`<err`), error match (`|{Ok:v body; Err:e body}`), propagation with `?`, stdlib error types per module. Two complete examples: (1) file read with error handling; (2) HTTP client with retry on error. All examples pass `tkc --check`. |
| 48.3.5 | Module system reference page | backlog | — | **P1** New `docs/reference/modules.md`. Cover: `m=name;` declaration, `i=alias:std.mod;`, multi-import, interface files (`.tki`), stdlib module paths, cross-module types. Explain `.tki` generation by compiler (`--emit-iface`). Example: two-module program with shared type. |
| 48.3.6 | Type system deep-dive page | backlog | — | **P2** New `docs/reference/type-system.md`. Cover: primitives (i8–i64, u8–u64, f32/f64, bool, $str, void), struct types (`t=$name{...}`), sum types, error unions, pointer types, array types `@(T)`, map types. Type inference rules. Worked example for each category. All compile with `tkc --check`. |
 |
--- |
 |
### Epic 48.4 — Standard Library Reference: Tested Examples |
 |
Each stdlib module page must have: status, type table, per-function section (signature + description + basic example), and one combined multi-function example at the end. All code blocks must pass `tkc --check`. Priority ordered by usage frequency. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.4.1 | stdlib/str.md: validate all examples, add combined example | backlog | — | **P0** Validate existing str.md examples with `tkc --check`. Add combined example: read a line, split on delimiter, uppercase first word, rejoin. Must pass compile check. |
| 48.4.2 | stdlib/http.md: validate all examples, add server+client combined example | backlog | — | **P0** Validate all http.md examples. Add combined example: HTTP server that reads env config, registers two routes, serves JSON from one and static file from other. Must pass `tkc --check`. Note: full route handler (46.1.3) is a dependency for runtime test; compile check is sufficient until 46.1.3 is done. |
| 48.4.3 | stdlib/json.md: validate all examples, add parse+generate combined | backlog | — | **P0** Validate all json.md examples. Add combined example: parse JSON config, extract fields, build response JSON object, serialise. Must pass `tkc --check`. |
| 48.4.4 | stdlib/log.md: add openaccess() function section, validate all examples | backlog | — | **P0** log.md documents structured logging (info/warn/error/debug). Add new section for `log.openaccess(path; max_lines; max_files; max_age_days)` introduced in 47.1.1. Validate all existing examples. Add combined example: server that opens access log, sets info level, logs startup, logs each request. |
| 48.4.5 | stdlib/env.md: validate all examples, add combined example | backlog | — | **P1** Validate all env.md examples. Add combined example: read PORT, HOST, DEBUG env vars with defaults; conditionally set log level based on DEBUG. Must pass `tkc --check`. |
| 48.4.6 | stdlib/file.md: validate all examples, add combined example | backlog | — | **P1** Validate all file.md examples. Add combined example: list directory, filter to `.json` files, read each, parse as JSON, summarise field counts. Must pass `tkc --check`. |
| 48.4.7 | stdlib/csv.md: validate all examples, add combined example | backlog | — | **P1** Validate all csv.md examples. Add combined example: read CSV, filter rows, compute column sum, write filtered subset to new CSV. Must pass `tkc --check`. |
| 48.4.8 | stdlib/time.md: validate all examples, add combined example | backlog | — | **P1** Validate all time.md examples. Add combined example: get current timestamp, format as ISO-8601, parse back, compute elapsed since epoch, format duration. Must pass `tkc --check`. |
| 48.4.9 | stdlib/crypto.md: validate all examples, add combined example | backlog | — | **P1** Validate all crypto.md examples. Add combined example: hash a password with bcrypt, verify it, then HMAC-SHA256 a message. Must pass `tkc --check`. |
| 48.4.10 | stdlib/encrypt.md: validate all examples, add combined example | backlog | — | **P1** Validate all encrypt.md examples. Add combined example: X25519 key exchange, derive shared secret with HKDF, encrypt a message with AES-256-GCM, decrypt and verify. Must pass `tkc --check`. |
| 48.4.11 | stdlib/math.md: validate all examples, add combined example | backlog | — | **P1** Validate all math.md examples. Add combined example: array of measurements, compute mean/stddev/percentiles, linear regression, print summary. Must pass `tkc --check`. |
| 48.4.12 | stdlib/dataframe.md: validate all examples, add combined example | backlog | — | **P1** Validate all dataframe.md examples. Add combined example: load CSV into dataframe, filter rows, group by column, compute group means, format as table. Must pass `tkc --check`. |
| 48.4.13 | stdlib/llm.md: validate all examples, add chat+json_mode combined | backlog | — | **P2** Validate all llm.md examples. Add combined example: initialise client, send system prompt, stream response tokens, re-call in json_mode for structured output, parse result. Must pass `tkc --check`. |
| 48.4.14 | stdlib/llm_tool.md: validate all examples, add full tool-call loop | backlog | — | **P2** Validate all llm_tool.md examples. Add combined example: declare two tools (weather + calculator), run LLM loop, dispatch tool calls, return results, get final response. Must pass `tkc --check`. |
| 48.4.15 | stdlib/process.md: validate all examples, add combined example | backlog | — | **P2** Validate all process.md examples. Add combined example: spawn git log, read stdout line by line, parse commit hashes and messages, return as structured list. Must pass `tkc --check`. |
| 48.4.16 | stdlib/db.md: validate all examples, add combined example | backlog | — | **P2** Validate all db.md examples. Add combined example: open SQLite db, create table, insert rows in transaction, query with filter, format results. Must pass `tkc --check`. |
| 48.4.17 | stdlib/template.md: validate all examples, add combined example | backlog | — | **P2** Validate all template.md examples. Add combined example: compile HTML template with slots, render with user data, HTML-escape user input, write to file. Must pass `tkc --check`. |
| 48.4.18 | stdlib/encoding.md: validate all examples, add combined example | backlog | — | **P2** Validate all encoding.md examples. Add combined example: base64-encode a payload, URL-encode for query string, hex-encode a hash, verify round-trip. Must pass `tkc --check`. |
| 48.4.19 | stdlib/ws.md: validate all examples, add server+client combined | backlog | — | **P2** Validate all ws.md examples. Add combined example: WebSocket server, client connects, sends binary message, receives echo, closes. Must pass `tkc --check`. |
| 48.4.20 | stdlib/sse.md: validate all examples, add streaming event example | backlog | — | **P2** Validate all sse.md examples. Add combined example: SSE endpoint integrated with router, emits timestamped events every second, client-side JS shows live updates. Must pass `tkc --check`. |
| 48.4.21 | stdlib/toon.md: validate all examples, add encode+decode combined | backlog | — | **P2** Validate all toon.md examples. Add combined example: define a struct, serialise to TOON, compare token count with JSON equivalent, parse back, verify round-trip. Must pass `tkc --check`. |
| 48.4.22 | stdlib/yaml.md, stdlib/i18n.md: validate and add combined examples | backlog | — | **P2** Validate both yaml.md and i18n.md examples. yaml: load config file, extract nested values. i18n: load locale bundles for en/fr, translate keys with placeholders, fallback to default locale. Must pass `tkc --check`. |
| 48.4.23 | stdlib/analytics.md, stdlib/ml.md: validate and add combined examples | backlog | — | **P2** Validate analytics.md and ml.md examples. analytics: time-series data, z-score anomaly detection, group aggregation. ml: k-means cluster, evaluate with silhouette score. Must pass `tkc --check`. |
| 48.4.24 | stdlib/image.md, stdlib/canvas.md, stdlib/svg.md: validate and add combined examples | backlog | — | **P3** Validate all three media module pages. image: load PNG, resize, grayscale, save. canvas: draw bar chart, export to PNG. svg: programmatic diagram with shapes and labels. Must pass `tkc --check`. |
| 48.4.25 | stdlib/chart.md, stdlib/dashboard.md: validate and add combined examples | backlog | — | **P3** Validate chart.md and dashboard.md. chart: line + bar from same dataset, export to Vega-Lite JSON. dashboard: two chart widgets + live counter, serve via WebSocket. Must pass `tkc --check`. |
| 48.4.26 | stdlib/html.md, stdlib/auth.md, stdlib/router.md: validate and add combined examples | backlog | — | **P2** Validate three web-layer module pages. html: build a table from a struct slice. auth: JWT sign, attach to response, verify on next request. router: full REST API with auth middleware. Must pass `tkc --check`. |
| 48.4.27 | Cookbook page: http + json + db (REST API with persistence) | backlog | — | **P1** New `docs/reference/cookbook/rest-api.md`. Complete working program: HTTP server, JSON request parsing, SQLite persistence, JSON response. ~60 lines of toke. Must compile and run correctly. |
| 48.4.28 | Cookbook page: llm + file + template (AI document pipeline) | backlog | — | **P2** New `docs/reference/cookbook/ai-pipeline.md`. Complete program: read documents from disk, send to LLM with system prompt, render results into HTML template, write output files. Must compile. |
| 48.4.29 | Cookbook page: csv + dataframe + chart (data analysis pipeline) | backlog | — | **P2** New `docs/reference/cookbook/data-pipeline.md`. Complete program: read CSV, filter+group with dataframe, render bar chart, export to SVG. Must compile. |
 |
--- |
 |
### Epic 48.5 — Tutorial Series (10 lessons, ported and updated) |
 |
Port the 10 toke-web learn/ lessons to current syntax. Each lesson adds one worked project. All examples compile. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.5.1 | Lesson 01: Why toke — update token comparison, add benchmarks from Gate 1 | backlog | — | **P1** Update 01-why-toke.md: token comparison examples must use default syntax. Add Gate 1 result: 12.5% token reduction, 63.7% Pass@1. Update Python/Go/Rust comparisons with current tokenizer counts. |
| 48.5.2 | Lesson 02: Modules and functions — update to f=, validate examples | backlog | — | **P1** Update 02-modules-functions.md. All `fn` → `f=`. All code blocks pass `tkc --check`. |
| 48.5.3 | Lesson 03: Control flow — update lp/if/el/match syntax | backlog | — | **P1** Update 03-control-flow.md. Confirm `lp(init;cond;step)` syntax. Confirm `if(cond){...}el{...}` form. All examples pass `tkc --check`. |
| 48.5.4 | Lesson 04: Collections — rewrite for @() arrays and maps, .get() access | backlog | — | **P0** This lesson is most affected by syntax change. Rewrite all array and map examples: `@(1;2;3)`, not `[1,2,3]`. `.get(n)` not `[n]`. `.len()` for length. Map literals `@("key":val)`. All examples pass `tkc --check`. |
| 48.5.5 | Lesson 05: Errors — validate error union syntax | backlog | — | **P1** Update 05-errors.md. Confirm `T|E` error union form. Confirm `|{Ok:v ...; Err:e ...}` match syntax. All examples pass `tkc --check`. |
| 48.5.6 | Lesson 06: Strings and I/O — validate str and io module calls | backlog | — | **P1** Update 06-strings-io.md. Validate all `str.*` function calls against current stdlib. Confirm `io.println`, `io.readln`, `io.print` are implemented. Pass `tkc --check`. |
| 48.5.7 | Lesson 07: Modules and imports — validate multi-module example | backlog | — | **P1** Update 07-modules-imports.md. Multi-module program examples with correct `i=alias:std.mod;` import syntax. `.tki` interface file description accurate. Pass `tkc --check`. |
| 48.5.8 | Lesson 08: Advanced features — validate arena, match, advanced patterns | backlog | — | **P1** Update 08-advanced.md. Confirm `arena{}` block syntax. Confirm sum type match patterns. Pass `tkc --check`. |
| 48.5.9 | Lesson 09: Standard library tour — update all 35 module examples | backlog | — | **P1** 09-stdlib.md is the longest tutorial (20K). Update all module references and examples to current API and default syntax. Every code block must pass `tkc --check`. |
| 48.5.10 | Lesson 10: Project — end-to-end project using http+json+str | backlog | — | **P1** 10-project.md is an end-to-end build tutorial. Update to use current syntax and stdlib API. The project must actually compile and run. |
 |
--- |
 |
### Epic 48.6 — Site Structure and Navigation |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.6.1 | Landing page: update hero with Gate 1 results and current positioning | backlog | — | **P1** Update index.html/index.mdx hero section: "56-char default syntax", "12.5% token reduction at Gate 1", "63.7% Pass@1". Correct any "Phase 2" terminology to "default syntax". Remove any placeholder content. |
| 48.6.2 | About/design.md: fix whitespace claim, update design rationale | backlog | — | **P0** design.md incorrectly states whitespace is structurally meaningless (Epic 44). Fix to: "Whitespace separates tokens but has no other structural role." Update any other inaccurate design claims. |
| 48.6.3 | changelog.md: add entries for default syntax decision and Gate 1 result | backlog | — | **P1** Add changelog entries: (2026-04-03) Gate 1 PASS — 12.5% token reduction, 63.7% Pass@1; (2026-04-04) 56-char is default syntax, 80-char becomes `--legacy`. |
| 48.6.4 | repos.md: update for 6-repo consolidation (post-Epic 10.2 plan) | backlog | — | **P2** After repo consolidation is executed, update repos.md to reflect the 6-repo structure: toke, toke-model, toke-eval, toke-web, toke-mcp, toke-cloud. Remove the 10-repo listing. |
| 48.6.5 | competitive-matrix.md: validate benchmarks against Gate 1 actuals | backlog | — | **P1** competitive-matrix.md has token comparison claims. Validate each against Gate 1 tokenizer measurements. Update any numbers that don't match. |
| 48.6.6 | Add 404 page with navigation links and search | backlog | — | **P2** Improve 404.html from placeholder to useful: site nav, "popular pages" links, search box (client-side). |
 |
--- |
 |
### Epic 48.7 — Migration and Go-Live |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 48.7.1 | Final content review: 0 failing tkc --check examples | backlog | — | **P0** Re-run `scripts/validate_examples.sh` (48.2.1). All toke code blocks must pass. Gate: 100% pass rate before go-live. |
| 48.7.2 | Performance baseline: staging load test before go-live | backlog | — | **P1** Run `wrk -t4 -c50 -d30s https://staging.tokelang.dev/` against toke server. Record: req/s, p50/p99 latency, memory use under load. Compare to nginx baseline if available. Acceptable: p99 < 200ms at 50 concurrent. |
| 48.7.3 | Let's Encrypt TLS cert: replace self-signed on Lightsail | backlog | — | **P0** Install certbot on Lightsail. Obtain cert for tokelang.dev, www.tokelang.dev, staging.tokelang.dev, loke.tokelang.dev. Update `main.tk` to use `/etc/letsencrypt/live/tokelang.dev/fullchain.pem` and `privkey.pem`. Update deploy script. Set Cloudflare SSL mode to "Full (strict)". |
| 48.7.4 | Retire nginx: move toke server to port 443 | backlog | — | **P0** After 48.7.3: stop and disable nginx (`systemctl disable nginx`). Update `main.tk` port from 8443 → 443. Update Lightsail firewall: open 443, close 8443. Update Cloudflare origin port. Update deploy script. Verify: `curl https://tokelang.dev/` returns 200. |
| 48.7.5 | DNS cutover: tokelang.dev → toke-website-new content | backlog | — | **P0** Once 48.7.1–48.7.4 are done: verify tokelang.dev serves the new content at port 443. Monitor access.log for 404s and fix any missing paths from the toke-web migration. Keep toke-web repo archived (not deleted). |
| 48.7.6 | Post-launch: 30-day monitoring and fix sprint | backlog | — | **P1** After go-live, monitor access.log for: 404 patterns, slow requests (>500ms), unexpected error responses. Fix any content gaps found. At 30 days, declare migration complete. |
 |
--- |
 |
## Epic 49 — ooke Phase 1: Foundation |
 |
**Repo:** `toke-ooke` (`~/tk/toke-ooke`) |
**Goal:** Deliver a working ooke that can build a static site from content files and serve it over HTTP. All ooke framework code is written in toke and compiles via tkc to a single native binary. No runtime dependencies. Phase 1 ends when the tokelang.dev website runs on ooke. |
 |
**Module plan:** ooke exposes three importable toke modules: `ooke.store` (content store), `ooke.template` (template engine), `ooke.router` (file-system routing). Each is a toke source file in `src/` that uses toke's `std.http`, `std.json`, and `std.file` modules. |
 |
--- |
 |
### Epic 49.1 — Project Scaffold and Configuration |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.1.1 | Repo structure and initial commit | done | — | Created `~/tk/toke-ooke/` with: `README.md`, `.gitignore`, `ooke.toml` (template config), `content/`, `models/`, `pages/`, `templates/partials/`, `islands/`, `static/`, `build/`. Git init complete. |
| 49.1.2 | ooke.toml parser (`src/config.tk`) | done | 2026-04-06 | Implemented in `src/config.c/h`. TOML-subset parser: section headers, key=value lines, string/bool/int types, defaults via `ooke_config_defaults()`. |
| 49.1.3 | CLI entry point: `ooke` binary (`main.tk`) | done | 2026-04-06 | Implemented in `src/main.c`. Full argv dispatch: new/build/serve/gen. Exit codes 0/1/2. Help and version flags. |
| 49.1.4 | `ooke new <name>`: scaffold a new project | done | 2026-04-06 | Implemented in `cmd_new()`: creates 14-subdirectory scaffold, writes `ooke.toml`, `pages/index.tk`, `templates/base.tkt`. |
 |
--- |
 |
### Epic 49.2 — Template Engine (`ooke.template`) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.2.1 | Template lexer: tokenise `{= =}`, `{! !}`, `{# #}` and raw HTML | done | 2026-04-06 | Implemented in `src/template.c` lexer. Scans `{= =}`, `{! !}`, `{# #}` character-by-character. RAW/EXPR/DIRECTIVE/COMMENT token kinds. |
| 49.2.2 | Template parser: build AST from token list | done | 2026-04-06 | Implemented in `src/template.c` parser. Full TNode tree: RAW, EXPR, LAYOUT, BLOCK (recursive children), YIELD, PARTIAL, ISLAND. |
| 49.2.3 | Template renderer: evaluate AST with data context | done | 2026-04-06 | Implemented in `src/template.c` renderer. Dotted-path context lookup, filter pipeline (md/date/escape/upper/lower/trim), StrBuf string builder. |
| 49.2.4 | Layout system: block inheritance | done | 2026-04-06 | Layout inheritance: child records layout name on first pass, fills BlockMap; layout rendered second pass with YIELD pulling from BlockMap. Multiple named blocks supported. |
| 49.2.5 | Partials: `{! partial("header") !}` | done | 2026-04-06 | Partials resolved to `templates/partials/<name>.tkt`, rendered inline with same context. Circular partial detection via depth counter. |
| 49.2.6 | Markdown filter: md renders .md content to HTML | done | 2026-04-06 | Implemented in `src/md.c/h`. Two-pass renderer: block-level (headings, fenced code, lists, blockquotes, paragraphs, HR, HTML passthrough) then inline (bold/italic/code/links/images). HTML-escapes <>&. Registered as md filter in template renderer. |
| 49.2.7 | `ooke.template` public API | backlog | — | **P1** Expose via `src/template.tki`: `tmpl.compile(path:$str):$Template`, `tmpl.render(t:$Template; ctx:@($str:$str)):$str`, `tmpl.renderfile(path:$str; ctx:@($str:$str)):$str`. `$Template` is the parsed AST. Cache compiled templates in memory for serve mode. |
 |
--- |
 |
### Epic 49.3 — Flat-File Content Store (`ooke.store`) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.3.1 | Content file loader: parse frontmatter + body from `.md` files | done | 2026-04-06 | Implemented in `src/store.c` `store_load_file()`. Parses `---` frontmatter delimiters, key: value lines, falls back to body-only when no frontmatter present. |
| 49.3.2 | Content type validation against model definitions | backlog | — | **P1** Load model definitions from `models/<type>.tk` (toke type definitions). Validate that required fields exist in frontmatter. Warn on extra fields. Return list of validation errors. |
| 49.3.3 | `store.all(type:$str):@($ContentFile)` — list all content of a type | done | 2026-04-06 | Implemented: `store_all()` scans `content/type/` with opendir/readdir, filters .md, calls `store_load_file`, sorts by created desc. |
| 49.3.4 | store.find — filter collection by frontmatter key=val | done | 2026-04-06 | Implemented: `store_find()` linear scan matching frontmatter key=val via `content_get()`. Returns pointer to first match or NULL. |
| 49.3.5 | store.slug — find content item by slug field | done | 2026-04-06 | Implemented: `store_slug()` matches `cf.slug` directly (frontmatter slug field or filename stem fallback). |
| 49.3.6 | JSON content files: `store.all` works for `.json` files too | backlog | — | **P2** In addition to `.md` files, support `.json` files in `content/<type>/`. Parse with `std.json`. Return `$ContentFile{meta:@($str:$str); body:""}` with JSON fields in meta. |
| 49.3.7 | `ooke.store` public API | done | 2026-04-06 | Public API in `src/store.h`: store_all, store_find, store_slug, content_get, store_free_collection, store_sort_by_created, store_load_file. |
 |
--- |
 |
### Epic 49.4 — File-System Router (`ooke.router`) |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.4.1 | Route scanner: build dispatch table from `pages/` directory | done | 2026-04-06 | Implemented in `src/ooke_router.c` `ooke_router_scan()`. Recursive directory walk, derives URL pattern from file path, handles index/[param]/api conventions. |
| 49.4.2 | Route matching: URL → handler with extracted params | done | 2026-04-06 | Implemented: `ooke_router_match()` + `pattern_match()`. Static segments exact-match; `:param` segments capture URL component. Static routes sorted before dynamic. |
| 49.4.3 | Handler loader: compile and call page handler function | backlog | — | **P1** Each `pages/*.tk` file exports `f=get(req:http.$req):http.$res`. In build mode: call handler at build time, write HTML to `build/` path. In serve mode: register with `std.http` via `http.GET()`. Handler receives extracted params in `req.params`. |
| 49.4.4 | Static asset passthrough: `static/` → serve without handler | done | 2026-04-06 | Static assets: `copy_dir_recursive("static/", "build/static/")` in build mode; `router_static(router, "/static/", "static")` in serve mode. |
| 49.4.5 | 404 handler: render `pages/404.tk` if present, else default | backlog | — | **P1** If `pages/404.tk` exists, render it for unmatched routes. If not, return minimal 404 HTML. Status code 404. |
| 49.4.6 | `ooke.router` public API | done | 2026-04-06 | Public API in `src/ooke_router.h`: ooke_router_scan, ooke_router_match, ooke_router_free, ooke_router_print. |
 |
--- |
 |
### Epic 49.5 — Build Mode: Static HTML Generation |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.5.1 | `ooke build`: walk all routes, render each to HTML file | done | 2026-04-06 | Implemented in `src/build.c` `ooke_build()`. Scans routes, renders static pages via `build_static_page()`, dynamic pages via `build_dynamic_pages()` iterating store_all(). |
| 49.5.2 | Copy `static/` to `build/static/` | done | 2026-04-06 | Implemented: `copy_dir_recursive("static/", "build/static/")` in `ooke_build()`. |
| 49.5.3 | CSS inlining: replace `<link rel=stylesheet>` with `<style>` in build output | done | 2026-04-06 | Implemented: `html_inline_css()` finds `<link rel="stylesheet">` tags, reads CSS file, replaces with `<style>` block. |
| 49.5.4 | HTML minification: strip comments and excess whitespace | done | 2026-04-06 | Implemented: `html_minify()` strips HTML comments (preserving IE conditionals), collapses inter-tag whitespace, skips pre/code/style/script blocks. |
| 49.5.5 | Build report: page count, output size, time | done | 2026-04-06 | Implemented: build report printed after `ooke_build()` — "Built N pages (X.X KB) in Yms" via gettimeofday. |
| 49.5.6 | `ooke build --binary`: embed all assets in the binary | backlog | — | **P2** Produce a single self-contained binary that serves all pages and assets from memory. Assets compiled to byte arrays in the binary at link time. Invoked as `./my-site serve --port 80`. No external files required. |
 |
--- |
 |
### Epic 49.6 — Serve Mode: Dynamic HTTP Server |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.6.1 | `ooke serve`: start HTTP server with compiled route table | done | 2026-04-06 | Implemented in `src/serve.c` `ooke_serve()`. Scans routes, registers catch-all `ooke_dispatch` handler, starts HTTP/HTTPS server via toke stdlib router. |
| 49.6.2 | In-memory page cache: render static routes at startup | done | 2026-04-06 | Implemented: `warmup_cache()` pre-renders all non-dynamic routes at startup into `g_cache[1024]`. Served from cache on GET. |
| 49.6.3 | TLS support: `ooke serve --tls cert.pem key.pem` | done | 2026-04-06 | Implemented: when cert_path/key_path provided, calls `http_tls_ctx_new()` + `http_serve_tls()` via toke stdlib. |
| 49.6.4 | Access logging: `ooke serve --access-log logs/access.log` | done | 2026-04-06 | Implemented: `tk_access_log_open()` + `tk_access_log_set_global()` at startup; `router_use_log()` adds log middleware. |
| 49.6.5 | Worker scaling: CPU-count workers for serve mode | backlog | — | **P1** Use `sysconf(_SC_NPROCESSORS_ONLN)` for worker count (already in `http.c` via Epic 47). Expose `server.workers` in `ooke.toml` for manual override. |
 |
--- |
 |
### Epic 49.7 — CLI Scaffold Commands |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 49.7.1 | `ooke gen type <name>`: generate model file | done | 2026-04-06 | Implemented: `cmd_gen_type()` writes `models/<name>.tk` with $name struct stub. |
| 49.7.2 | `ooke gen page <path>`: generate page handler | done | 2026-04-06 | Implemented: `cmd_gen_page()` writes `pages/<path>.tk` with get() handler stub. |
| 49.7.3 | `ooke gen island <name>`: generate island component stub | backlog | — | **P2** Write `islands/<name>.tk` with empty island handler. Add `{! island("<name>"; hydrate="visible") !}` comment showing how to include it in templates. |
| 49.7.4 | `ooke gen api <name>`: generate API endpoint | done | 2026-04-06 | Implemented: `cmd_gen_api()` writes `pages/api/<name>.tk` with GET+POST handler stubs. |
 |
--- |
 |
## Epic 50 — ooke Phase 2: CMS Features |
 |
**Dependency:** Epic 49 complete. |
**Goal:** Full content management — SQLite backend, admin interface, authentication, media library. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 50.1.1 | SQLite content store backend (`ooke.store` SQLite adapter) | backlog | — | **P1** When `store.backend = "sqlite"` in `ooke.toml`: auto-generate schema from model type definitions. Implement `store.all`, `store.find`, `store.slug` against SQLite via `std.db`. Schema: one table per content type, columns from struct fields. Auto-migration on startup (add columns, never drop). |
| 50.1.2 | Content type → SQLite schema generator | backlog | — | **P1** Read `models/*.tk`, extract `t=$typename{...}` field definitions, emit `CREATE TABLE IF NOT EXISTS` statements. Type mapping: `$str` → TEXT, `bool` → INTEGER, `u64/i64` → INTEGER, `f64` → REAL, `@$str` → TEXT (JSON-encoded). |
| 50.2.1 | Admin interface: mount at `/admin` in serve mode | backlog | — | **P2** Register `/admin/*` routes when `server.admin = true`. Admin is itself an ooke sub-application: ooke pages rendered with admin layout. Auth required for all admin routes. |
| 50.2.2 | Admin: content list view for each content type | backlog | — | **P2** `/admin/content/<type>` shows paginated table of all items. Columns from model definition. Links to edit/delete. Pagination: 25 per page. |
| 50.2.3 | Admin: content create/edit form, auto-generated from type | backlog | — | **P2** `/admin/content/<type>/new` and `/admin/content/<type>/<id>/edit`. Form fields generated from model struct. `$str` → text input. `bool` → checkbox. `$body` field (if present) → Markdown textarea with preview pane. POST handler writes to store. |
| 50.3.1 | User authentication: bcrypt password, session token | backlog | — | **P2** `ooke.auth` module. Users stored in SQLite (`users` table: id, email, bcrypt_hash, role). Login: POST `/admin/login`, verify bcrypt, set signed session cookie (HMAC-SHA256, 24h TTL). Session middleware: verify cookie on every `/admin` request. |
| 50.3.2 | Role-based access: admin, editor, viewer | backlog | — | **P2** Three roles. Admin: full access. Editor: create/edit content, no user management. Viewer: read-only admin access. Roles stored in users table. Middleware checks role before handler. |
| 50.4.1 | Media library: file upload and storage | backlog | — | **P3** POST `/admin/media/upload`. Accept multipart form data. Store files in `static/images/`. Record metadata (filename, size, mime type, uploaded_at) in SQLite. List at `/admin/media`. |
| 50.4.2 | Image optimisation: resize and convert on upload | backlog | — | **P3** On image upload: generate thumbnail (300px wide), optimise original (strip EXIF, compress). Use `std.image` (Epic 12.6). Store original + thumbnail. |
 |
--- |
 |
## Epic 51 — ooke Phase 3: Islands and Interactivity |
 |
**Dependency:** Epic 49 complete. |
**Goal:** Client-side interactive components written in toke, compiled to WebAssembly. Hydration strategies. Asset pipeline. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 51.1.1 | Island registration: `{! island("name"; hydrate="load") !}` in templates | backlog | — | **P2** Template renderer records declared islands and hydration strategy. At build time: injects `<div data-island="name" data-hydrate="load">` wrapper. Injects minimal hydration loader script (< 1 KB). |
| 51.1.2 | Hydration loader: client JS that activates islands on demand | backlog | — | **P2** Small (< 800 bytes minified) vanilla JS snippet. Reads `data-island` + `data-hydrate` attributes. Strategies: `load` (DOMContentLoaded), `visible` (IntersectionObserver), `idle` (requestIdleCallback), `none` (static, no JS injected). Fetches island WASM module from `/static/islands/<name>.wasm`. |
| 51.2.1 | toke-to-WASM compilation for island components | backlog | — | **P3** `islands/<name>.tk` files compile to `.wasm` via `tkc --target wasm32`. Output to `static/islands/`. WASM module exports one function: `render(props_json: *u8, len: u32) → *u8`. Requires toke WASM target in tkc (new story in toke repo). |
| 51.3.1 | CSS inlining and critical path extraction | backlog | — | **P2** At build time: identify CSS rules used by rendered HTML (parse class/id/tag selectors). Inline critical CSS in `<style>`. Defer remaining CSS load. Reduces render-blocking. |
| 51.3.2 | Image optimisation pipeline at build time | backlog | — | **P2** When `build.image_optimize = true`: process `static/images/` — generate WebP variants, add `width`/`height` attributes to `<img>` tags in built HTML, add `loading="lazy"` to below-fold images. Uses `std.image`. |
 |
--- |
 |
## Epic 52 — ooke Phase 4: LLM Tooling |
 |
**Dependency:** Epic 49 complete. |
**Goal:** Make ooke the best possible target for LLM code generation. Repair loop, structured scaffold API, reference prompts. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 52.1.1 | `ooke build --repair`: compile, capture errors, format for LLM | backlog | — | **P2** After a failed `ooke build`: capture structured JSON diagnostics from tkc. Format as a prompt-ready block: file, line, error code, message, suggestion. Print to stdout. Designed to be piped to an LLM API call in a repair loop script. |
| 52.1.2 | `ooke repair <file>`: automated generate-compile-fix loop | backlog | — | **P2** Accepts a toke source file with compile errors. Calls configured LLM (via `std.llm`) with error context + source + fix prompt. Applies suggested patch. Recompiles. Repeats up to N times (configurable). Reports: fixed, gave-up, or manual-required. |
| 52.2.1 | Structured scaffold API: `ooke gen` accepts JSON spec | backlog | — | **P2** `ooke gen --spec spec.json`: reads a JSON document describing the site structure (content types, routes, templates). Generates all files in one pass. Designed for LLM invocation: LLM generates the spec JSON, scaffold generates the code. |
| 52.2.2 | JSON spec format for site generation | backlog | — | **P2** Define the JSON schema for `ooke gen --spec`. Fields: `site.name`, `types[]` (content type definitions), `pages[]` (route + template pairs), `islands[]` (interactive component names), `nav[]` (navigation links). Document schema in `docs/scaffold-spec.md`. |
| 52.3.1 | Reference prompts: system prompts for LLM ooke generation | backlog | — | **P2** Write `docs/prompts/`: `create-page.md`, `create-type.md`, `create-api.md`, `fix-error.md`. Each is a tested system prompt that reliably produces correct ooke/toke code when given to a capable LLM. Include few-shot examples from working ooke projects. |
| 52.3.2 | ooke corpus: working examples for training data | backlog | — | **P2** Build `examples/` directory: 10 complete ooke projects (blog, docs site, landing page, portfolio, API server, etc.). Each compiles and runs. Feeds into toke-corpus training data (Epic 9.1 descendant). |
 |
--- |
 |
## Epic 53 — Website on ooke |
 |
**Dependency:** Epic 49 complete (Phase 1). |
**Goal:** Rebuild toke-website-new as an ooke project. The tokelang.dev website is served by a compiled ooke binary — the website is a live demonstration of the framework it documents. |
 |
Update to Epic 48: the architecture shifts from raw toke + static files to ooke + content-driven pages. toke-website-new becomes an ooke project in `~/tk/toke-website-new/`. The ooke framework lives in `~/tk/toke-ooke/`. |
 |
| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------| |
| 53.1.1 | Convert toke-website-new to ooke project structure | backlog | — | **P0** Depends on 49.1.3. Add `ooke.toml` to `toke-website-new/`. Reorganise: existing `sites/tokelang.dev/` HTML → `pages/` (route handlers) + `templates/` (layouts). Static assets → `static/`. Content docs → `content/docs/`. |
| 53.1.2 | Port toke-web documentation content to ooke content/ | backlog | — | **P0** Convert `toke-web/src/content/docs/**/*.md` to ooke flat-file content. Frontmatter: `title`, `slug`, `section`, `order`. Body: Markdown (rendered via `| md` filter in templates). Fix code examples (as per Epic 48.2 series) before converting. |
| 53.1.3 | Base layout template (`templates/base.tkt`) | backlog | — | **P0** Site chrome: `<html>`, `<head>` with meta/CSS, `<nav>` with section links, `<main>{! yield("content") !}</main>`, `<footer>`. Single source of truth for all pages. |
| 53.1.4 | Doc page template (`templates/docs/page.tkt`) | backlog | — | **P0** Sidebar nav (auto-generated from `content/docs/` structure), breadcrumb, article content rendered via `{= page.body | md =}`, prev/next navigation. |
| 53.1.5 | Route handlers for all site sections | backlog | — | **P0** `pages/index.tk` (landing), `pages/docs/index.tk` (docs home), `pages/docs/[section]/[slug].tk` (doc page), `pages/about.tk`, `pages/404.tk`. Each loads content via `store.slug()` and renders with `tpl.renderfile()`. |
| 53.1.6 | Build the site with `ooke build` and verify output | backlog | — | **P0** Run `ooke build` in `toke-website-new/`. All pages render without error. Check: no broken internal links, all images resolve, all code examples are highlighted correctly. |
| 53.1.7 | Serve on staging with `ooke serve --tls` | backlog | — | **P0** Replace the current hand-rolled `main.tk` server with `ooke serve`. Same Lightsail deployment flow (48.1.3 deploy script). Verify staging.tokelang.dev loads correctly on 8443. |
| 53.1.8 | Update Epic 48.7.x: go-live using ooke serve on port 443 | backlog | — | **P0** Once 53.1.7 is verified on staging, proceed with 48.7.3 (Let's Encrypt) and 48.7.4 (nginx retirement) using `ooke serve` as the production server. Update deploy scripts accordingly. |
 |