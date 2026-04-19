# PROJECT_STATUS.md
## toke — Live Project Status

**Last updated:** see git log  
**Current phase:** Default Syntax Implementation (Gate 1 passed)
**Current milestone:** M1 — Reference compiler complete; M2 — Standard Library Core

---

## Gate Status

| Gate | Month | Criterion | Status |
|------|-------|-----------|--------|
| Gate 1 | 8 | >10% token reduction AND Pass@1 ≥ 60% | PASS (2026-04-03: 12.5% reduction, 63.7% Pass@1) |
| Gate 2 | 14 | Extended features retain efficiency AND 7B model beats baseline | ON HOLD (waiting for local compute) |
| Gate 3 | 26 | Two+ model families ≥70% Pass@1 AND self-improvement loop running | not reached |
| Gate 4 | 32 | All benchmarks met, spec complete, consortium proposal ready | not reached |

---

## Epic Status

| Epic | Title | Status |
|------|-------|--------|
| 1.1 | Language Specification Lock | done |
| 1.2 | Reference Compiler Frontend | done |
| 1.3 | Standard Library Core | done |
| 1.4 | Mac Studio Setup and Local Pipeline | backlog |
| 1.5 | Phase A Corpus Generation | backlog |
| 1.6 | Gate 1 Benchmark | backlog |
| 1.7 | Compiler Security and SAST | backlog |
| 1.8 | Corpus Pipeline Threat Model | backlog |
| 1.9 | Remote Monitoring Console | done |
| 1.10 | Phase 1 Integration and Conformance Review | backlog |
| 2.6 | Responsible Disclosure and CVE Process | done |
| 3.7 | Supply Chain Security and Release Signing | backlog |
| 4.6 | Security Audit and Hosted Service Readiness | backlog |

---

## Active Blockers

None.

---

## Known Technical Debt

- Arena escape detection (E5001) is conservative: flags any assignment to a module-scope variable inside an arena block. Needs `arena_depth` field added to `Decl` struct in names.h to distinguish pre-arena vs. in-arena declarations. (story 1.2.5)
- LLVM backend: `as` cast emits identity stub (full sitofp/fptosi/trunc/zext requires TypeEnv wiring); struct field GEP always uses offset 0 (correct only for first field); nested break requires a loop-label stack. (story 1.2.8)

---

## Next Action

Epic 1.1 complete including spec review gate (story 1.1.6, branch feature/spec-review-m0).
Spec review resolved 7 blocking issues; 4 warnings and 4 notes remain as tracked risks.
Two pre-conditions must be met before 1.2.1 starts:
  1. Document BOOL_LIT vs IDENT token-class strategy
  2. Document string interpolation lexer strategy
Both are ~1 day each and can run in parallel with sprint planning.

Epics 1.2, 1.3, 1.7, 1.8 complete.

**Current blocker:** Epic 1.4 (Mac Studio configuration) requires physical hardware setup.
All of 1.4.x, 1.5.x, and 1.6.2–1.6.4 are blocked until 1.4.1 is done.

Epic 2.6 (Responsible Disclosure and CVE Process) complete (feature/security-disclosure).
GitHub UI actions still required: enable private vulnerability reporting on karwalski/toke, karwalski/toke-model; establish Advisory Database point of contact.

Epics 2.7 (Standard Library Expansion) and 3.8 (Standard Library Production Hardening) added to backlog.

Epic 1.9 (Remote Monitoring Console) started: stories 1.9.1 (job manager daemon) and 1.9.2 (web dashboard) implemented at toke-model/corpus/monitor/. Stories 1.9.3 (log streaming) and 1.9.4 (auth/HTTPS) remain.
