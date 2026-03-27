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
| 1.1.3 | Symbol disambiguation rules | backlog | — | Depends on 1.1.1 |
| 1.1.4 | Formal EBNF grammar | backlog | — | Depends on 1.1.1–1.1.3 |
| 1.1.5 | Phase 2 transformation rules | backlog | — | Depends on 1.1.4 |

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
| 1.2.10 | Conformance test suite (Phase 1) | backlog | — | Parallel with 1.2.1–1.2.9 |

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
| 1.6.1 | Held-out benchmark task set | done | feature/benchmark-held-out-tasks | Lock before corpus gen starts |
| 1.6.2 | Token efficiency measurement | backlog | — | Depends on 1.5 complete |
| 1.6.3 | Pass@1 measurement | backlog | — | Depends on 1.6.2 |
| 1.6.4 | Gate 1 decision document | backlog | — | Depends on 1.6.3 |

---

## Completed Stories

| ID | Story | Completed | Branch |
|----|-------|-----------|--------|
| 1.1.1 | Character set finalisation | 2026-03-27 | feature/spec-character-set (toke-spec) |
| 1.1.2 | Keyword table lock | 2026-03-27 | feature/spec-keyword-table (toke-spec) |
| 1.6.1 | Held-out benchmark task set | 2026-03-27 | feature/benchmark-held-out-tasks (toke-benchmark) |

---

## Active Blockers

None.
