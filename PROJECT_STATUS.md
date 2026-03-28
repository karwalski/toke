# PROJECT_STATUS.md
## toke — Live Project Status

**Last updated:** see git log  
**Current phase:** Phase 1 — Falsification  
**Current milestone:** M1 — Reference compiler

---

## Gate Status

| Gate | Month | Criterion | Status |
|------|-------|-----------|--------|
| Gate 1 | 8 | >10% token reduction AND Pass@1 ≥ 60% | not reached |
| Gate 2 | 14 | Extended features retain efficiency AND 7B model beats baseline | not reached |
| Gate 3 | 26 | Two+ model families ≥70% Pass@1 AND self-improvement loop running | not reached |
| Gate 4 | 32 | All benchmarks met, spec complete, consortium proposal ready | not reached |

---

## Phase 1 Epic Status

| Epic | Title | Status |
|------|-------|--------|
| 1.1 | Language Specification Lock | done |
| 1.2 | Reference Compiler Frontend | in_progress |
| 1.3 | Standard Library Core | backlog |
| 1.4 | Mac Studio Setup and Local Pipeline | backlog |
| 1.5 | Phase A Corpus Generation | backlog |
| 1.6 | Gate 1 Benchmark | backlog |

---

## Active Blockers

None.

---

## Known Technical Debt

None yet.

---

## Next Action

Epic 1.1 complete. Start Epic 1.2 — Reference Compiler Frontend.
Stories 1.2.1 (lexer), 1.2.6 (diagnostic emitter), and 1.2.10 (conformance suite) can run in parallel.
See `docs/epics_and_stories.md` for acceptance criteria.
