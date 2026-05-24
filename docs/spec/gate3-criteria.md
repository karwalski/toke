# Gate 3 Success Criteria — Pre-Registration

**Pre-registration date:** 2026-05-24
**Evaluator:** Automated pipeline (gate3_pipeline.py)
**Status:** PRE-REGISTERED — criteria are locked once training begins
**Context:** Gate 2 passed 2026-05-22 with 100% compilation, ~8% functional correctness. Gate 3 targets functional correctness.

---

## 1. Success Criteria

These thresholds are fixed. They cannot be adjusted after the first training run starts.

| # | Criterion | Threshold | Rationale |
|---|-----------|-----------|-----------|
| C1 | Functional Pass@1 | ≥ 35% on 500-task hidden benchmark | Proves the model reasons about algorithms, not just syntax |
| C2 | Compilation Pass@1 | ≥ 95% | Maintain Gate 2 level (100% achieved; 95% floor allows tokenizer/model changes) |
| C3 | argv-generalisation | ≥ 50% of solutions read from argv | Addresses the 67% hardcoding problem identified in Gate 2 |
| C4 | Multi-model coverage | ≥ 2 model families tested (Qwen + one other) | Proves toke is learnable, not an artefact of one architecture |
| C5 | Self-improvement | Iteration N+1 > Iteration N on held-out eval set | Validates the compile-filter-retrain loop described in training-next-phase.md |

All five criteria must be met for a GO decision.

---

## 2. Evaluation Protocol

| Parameter | Value |
|-----------|-------|
| Benchmark size | 500 hidden tasks (same set used in Gate 2) |
| Compilation check | `toke --check` on every generated solution |
| Execution check | Compiled binary executed against test I/O |
| Test cases per task | ≥ 10 (randomised inputs, deterministic expected outputs) |
| Sampling temperature | 0.2 |
| Samples per task | 1 (strict Pass@1 — no best-of-N) |
| Baseline comparison | Same model (Qwen base) generating Python, same benchmark, same temperature |
| argv detection | Automated: solution must reference `args` or `argv` and not contain hardcoded test values |
| Self-improvement measurement | Compare Pass@1 on 200-task held-out eval set across consecutive training iterations |

Solutions are scored binary: compile AND pass all test cases = 1, anything else = 0.

---

## 3. GO / NO-GO Decision

### GO — all five criteria met
- Advance to Gate 4 planning.
- Publish results (blog post, model card, benchmark scores).
- Release v0.4 with `toke --python-view` and `.tkc` companion format.
- Open community feedback round on v0.3 syntax.

### PARTIAL — some criteria met, functional Pass@1 ≥ 15%
- Document which criteria failed and root cause.
- One retry permitted: extend training by up to 4 weeks with targeted corpus fixes.
- Retry uses the same locked criteria — no threshold adjustments.

### FUNDAMENTAL FAILURE — functional Pass@1 < 15%
- Trigger pivot evaluation.
- Evaluate alternative: Python-subset language with custom BPE tokenizer and toke-style linter.
- Publish honest post-mortem preserving tokenizer and diagnostics work (the empirically strongest components).
- Pivot decision documented in `gate3-decision.md` with full data.

---

## 4. Timeline

| Milestone | Target Date |
|-----------|-------------|
| Pre-registration locked | 2026-05-24 |
| Training sprint begins | 2026-06-02 (week 1) |
| Corpus rebuild complete | 2026-06-20 (week 3) |
| SFT + GRPO training | 2026-06-23 – 2026-07-11 (weeks 4–6) |
| Evaluation + MCP dogfooding | 2026-07-14 – 2026-08-01 (weeks 7–9) |
| **GO/NO-GO decision** | **2026-08-17 (week 12, mid-August)** |
| Retry deadline (if partial) | 2026-09-14 (week 16) |

---

## 5. Amendments

This document is append-only after pre-registration. Any clarifications are logged below with date and rationale. Thresholds cannot be changed.

| Date | Amendment |
|------|-----------|
| — | — |
