# docs/progress.md
## toke — Story Progress Tracker

**Project phase:** Phase 1 — Falsification  
**Active milestone:** M0  
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
| 1.1.5 | Phase 2 transformation rules | done | feature/spec-phase2-transform | [x] Type sigil rule (TYPE_IDENT→$name) [x] Array literal rule ([]→@()) [x] Array index rule (a[n]→a.get(n)) [x] Determinism proof [x] Round-trip spec [x] 20+ example pairs [x] Frozen M0 2026-03-28 |
| 1.1.6 | Spec review and alignment | done | feature/spec-review-m0 | [x] Cross-document review complete [x] 7 blocking issues resolved [x] grammar.ebnf fully populated (was stub) [x] character-set.md created [x] keywords.md created [x] symbol-disambiguation.md created [x] phase2-transform.md created [x] CastExpr production added [x] LoopStmt semicolon ambiguity fixed [x] spec-review-m0.md written [x] 4 warnings, 4 notes in risk register [x] Frozen M0 2026-03-27 |

### Epic 1.2 — Reference Compiler Frontend

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.2.1 | Lexer implementation | backlog | — | Depends on 1.1 complete |
| 1.2.2 | Parser implementation | done | feature/compiler-parser | [x] All 47 non-terminals [x] LL(1) [x] AST with source positions [x] E2001–E2004 diagnostics [x] 394 lines |
| 1.2.3 | Import resolver | done | feature/compiler-import-resolver | [x] .tki loading [x] E2030 unresolved [x] E2031 circular [x] std.* prefix [x] SymbolTable [x] 179 lines |
| 1.2.4 | Name resolver | done | feature/compiler-name-resolver | [x] Scope chain [x] Predefined ids [x] Import aliases [x] E3011 E3012 [x] All scope levels |
| 1.2.5 | Type checker | done | feature/compiler-type-checker | [x] 10/10 rules [x] E4031 E4010 E4011 E4025 E5001 E3020 [x] 346 lines (arena escape: conservative — Decl needs depth stamp) |
| 1.2.6 | Structured diagnostic emitter | backlog | — | Can parallel 1.2.5 |
| 1.2.7 | Interface file emitter | done | feature/compiler-interface-emitter | [x] JSON .tki [x] module/func/type/const [x] interface_hash [x] E9001 [x] 190 lines |
| 1.2.8 | LLVM IR backend | in_progress | feature/compiler-llvm-backend | Depends on 1.2.5 |
| 1.2.9 | CLI interface | backlog | — | Depends on 1.2.8 |
| 1.2.10 | Conformance test suite (Phase 1) | backlog | — | Parallel with 1.2.1–1.2.9 |

### Epic 1.7 — Compiler Security and SAST

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.7.1 | SAST on compiler C code | backlog | — | Can parallel 1.2.x |
| 1.7.2 | Compiler input fuzzing | backlog | — | Depends on 1.2.2 |
| 1.7.3 | Secret scanning in corpus pipeline | backlog | — | Can parallel 1.4.x |
| 1.7.4 | Dependency vulnerability scanning | backlog | — | Can parallel 1.4.x |
| 1.7.5 | Signed commits and DCO enforcement | backlog | — | Can parallel 1.2.x |

### Epic 1.8 — Corpus Pipeline Threat Model

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.8.1 | Corpus pipeline threat model document | backlog | — | Before 1.5 starts |
| 1.8.2 | Sandboxed execution of generated programs | backlog | — | Before 1.5 starts |

### Epic 1.3 — Standard Library Core

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.3.1 | std.str | backlog | — | Depends on 1.2.9 |
| 1.3.2 | std.http | backlog | — | Depends on 1.3.1 |
| 1.3.3 | std.db | backlog | — | Depends on 1.3.1 |
| 1.3.4 | std.json | backlog | — | Depends on 1.3.1 |
| 1.3.5 | std.file | backlog | — | Depends on 1.3.1 |

### Epic 1.4 — Mac Studio Setup and Local Pipeline

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.4.1 | Mac Studio configuration | backlog | — | Hardware prerequisite |
| 1.4.2 | Local inference pipeline | backlog | — | Depends on 1.4.1 |
| 1.4.3 | Corpus storage schema | backlog | — | Depends on 1.4.2 |

### Epic 1.5 — Phase A Corpus Generation

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.5.1 | Task curriculum generator | backlog | — | Depends on 1.4 complete |
| 1.5.2 | Four-language parallel generation | backlog | — | Depends on 1.5.1 |
| 1.5.3 | Differential test harness | backlog | — | Depends on 1.5.2 |
| 1.5.4 | Corpus quality metrics | backlog | — | Depends on 1.5.3 |

### Epic 1.6 — Gate 1 Benchmark

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 1.6.1 | Held-out benchmark task set | done | feature/benchmark-held-out-tasks | [x] 500 tasks [x] 120 test inputs each [x] Gitignored [x] Generation script committed |
| 1.6.2 | Token efficiency measurement | backlog | — | Depends on 1.5 complete |
| 1.6.3 | Pass@1 measurement | backlog | — | Depends on 1.6.2 |
| 1.6.4 | Gate 1 decision document | backlog | — | Depends on 1.6.3 |

### Epic 2.6 — Responsible Disclosure and CVE Process

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 2.6.1 | Security policy and disclosure process | backlog | — | Before first public release |
| 2.6.2 | CVE coordination process | backlog | — | Depends on 2.6.1 |

### Epic 3.7 — Supply Chain Security and Release Signing

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 3.7.1 | SBOM generation for compiler releases | backlog | — | Before Phase 3 release |
| 3.7.2 | Release binary signing | backlog | — | Before Phase 3 release |
| 3.7.3 | Reproducible builds for the compiler | backlog | — | Depends on 3.1 complete |
| 3.7.4 | Model release safety evaluation | backlog | — | Before model release |

### Epic 4.6 — Security Audit and Hosted Service Readiness

| ID | Story | Status | Branch | Notes |
|----|-------|--------|--------|-------|
| 4.6.1 | Third-party security audit of the compiler | backlog | — | Before v1.0 release |
| 4.6.2 | SOC 2 readiness assessment (conditional on hosted service) | backlog | — | Conditional: only if hosted service launches |

---

## Completed Stories

| ID | Story | Completed | Branch |
|----|-------|-----------|--------|
| 1.1.1 | Character set finalisation | 2026-03-27 | feature/spec-character-set (toke-spec) |
| 1.1.2 | Keyword table lock | 2026-03-27 | feature/spec-keyword-table (toke-spec) |
| 1.1.3 | Symbol disambiguation rules | 2026-03-27 | feature/spec-symbol-disambiguation (toke-spec) |
| 1.1.4 | Formal EBNF grammar | 2026-03-27 | feature/spec-ebnf-grammar (toke-spec) |
| 1.1.5 | Phase 2 transformation rules | 2026-03-28 | feature/spec-phase2-transform (toke-spec) |
| 1.1.6 | Spec review and alignment | 2026-03-27 | feature/spec-review-m0 (toke-spec) |
| 1.6.1 | Held-out benchmark task set | 2026-03-27 | feature/benchmark-held-out-tasks (toke-benchmark) |

---

## Active Blockers

None.
