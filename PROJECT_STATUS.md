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
| 1.7 | Compiler Security and SAST | backlog |
| 1.8 | Corpus Pipeline Threat Model | backlog |
| 2.6 | Responsible Disclosure and CVE Process | backlog |
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

Start Epic 1.2 — Reference Compiler Frontend.
Stories 1.2.1 (lexer), 1.2.6 (diagnostic emitter), and 1.2.10 (conformance suite) can run in parallel.
See spec/grammar.ebnf, spec/character-set.md, spec/keywords.md, and docs/spec-review-m0.md for compiler input.
