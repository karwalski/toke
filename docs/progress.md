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
| 1.2.2 | Parser implementation | backlog | — | Depends on 1.2.1 |
| 1.2.3 | Import resolver | backlog | — | Depends on 1.2.2 |
| 1.2.4 | Name resolver | backlog | — | Depends on 1.2.3 |
| 1.2.5 | Type checker | backlog | — | Depends on 1.2.4 |
| 1.2.6 | Structured diagnostic emitter | backlog | — | Can parallel 1.2.5 |
| 1.2.7 | Interface file emitter | backlog | — | Depends on 1.2.5 |
| 1.2.8 | LLVM IR backend | backlog | — | Depends on 1.2.5 |
| 1.2.9 | CLI interface | backlog | — | Depends on 1.2.8 |
| 1.2.10 | Conformance test suite (Phase 1) | in_progress | test/compiler-conformance-suite | L-series (25 tests), G-series (25 tests), D-series (12 tests) written; N/T/C stubs with READMEs; runner at test/run_conform.sh; `make conform` wired |

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
