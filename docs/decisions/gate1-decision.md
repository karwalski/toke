---
title: Gate 1 Decision Document
slug: gate1-decision
section: decisions
order: 4
---

**Date:** 2026-04-03
**Status:** recorded PASS 2026-04-03 — **verdict RE-OPENED 2026-09-19** (story 128.19)
**Deciders:** Matt Watt
**Story:** 1.6.4

---

## Gate 1 Criteria (from toke-spec-v02.md, Month 8)

| # | Criterion | Threshold | Result | Verdict |
|---|-----------|-----------|--------|---------|
| 1 | Token reduction on held-out D2C tasks using legacy character set | >= 10% | 12.5% (8K vocab) / 13.1% (32K vocab) | **PASS** |
| 2 | Functional Pass@1 on held-out tasks | >= 60% | **58.8%** (588/1,000) | **NOT MET — open** |

> **Pass@1 corrected 2026-09-19 (story 128.19): 58.8%, not 63.7%.**
> 1,000 solutions were generated, 923 compiled, 588 passed every hidden test. The
> figure published as 63.7% was 588/**923**: `load_toke_solutions()` dropped the 77
> solutions that failed to compile out of the denominator, so it measured Pass@1
> *given that the solution compiled* — a different and strictly more generous
> quantity. A solution that fails to compile is a failed attempt, not an absent
> one, so the denominator is the 1,000 generated: 588/1,000 = **58.8%**. No re-run
> was needed; the correction is arithmetic over `toke-eval/benchmark/solutions/*.toke`.
> **58.8% is below the declared `pass_at_1_minimum: 0.60`, so the Gate 1 verdict is
> re-opened and has not been re-decided here.** Derivation:
> `toke-eval/docs/suspect-numbers-128-1c.md` §1.

(The criterion was also mislabelled "first-pass compile success". 58.8% is the
*functional* Pass@1 — solutions passing every hidden test. The compile rate was
92.3%, 923/1,000.)

As published until 2026-09-19 this row read `63.7% (588/923 tasks)` with a verdict
of **PASS**. That is the withdrawn figure and it is kept here, not erased.

**Failure consequence (not triggered):** Halt language development and pivot to typed-IR approach only.

**Decision as recorded 2026-04-03: Gate 1 passes. Phase 1 (Falsification) is complete. The project proceeds to Phase 2.**

**Re-opened 2026-09-19 (story 128.19).** That decision rested on a Pass@1 of
63.7%, which was computed on the wrong denominator. The corrected figure, 58.8%,
is below the gate's own >= 60% threshold. Whether Gate 1 passes on the corrected
number is the owner's decision and is **not** made here. The original decision is
left above as the record of what was concluded at the time.

---

## Token Efficiency (Criterion 1)

Measured with a purpose-built BPE tokenizer trained on 46,754 validated toke programs, compared against OpenAI's cl100k_base tokenizer on the same corpus.

| Metric | 8K Vocabulary | 32K Vocabulary |
|--------|--------------|----------------|
| Token reduction vs cl100k_base | **12.5%** | **13.1%** |
| Mean tokens (toke BPE) | 172.9 | 171.8 |
| Mean tokens (cl100k baseline) | 197.6 | 197.6 |
| Compression ratio | 0.875 | 0.869 |
| Vocabulary utilisation | 70.2% | 23.5% |
| Fertility | 0.377 | 0.374 |

Cross-language comparison (cl100k_base, complete programs):

| Language | Mean tokens | vs toke |
|----------|------------|---------|
| **toke** | **52** | baseline |
| Python | 156 | 3.0x more |
| C | 168 | 3.2x more |
| Java | 127 | 2.4x more |

---

## Pass@1 (Criterion 2)

### Final Benchmark: gate1_v5_1000

| Metric | Value |
|--------|-------|
| Solutions generated | 1,000 |
| Solutions compiled | 923 (92.3%) |
| Solutions passing every hidden test | 588 |
| Pass@1 | **588/1,000 = 58.8%** |
| Mean Pass@1 | **0.588** |
| Pass@1 as published until 2026-09-19 | 588/923 = 63.7% — **withdrawn, wrong denominator** |
| Inference time | 41.7 minutes (1000 tasks) |
| Model | Qwen 2.5 Coder 7B + LoRA adapter |
| Platform | Mac Studio M4 Max (local) |

### Benchmark Progression

| Run | Date | Tasks | Compiled | Pass@1 | Key Change |
|-----|------|-------|----------|--------|------------|
| v1 | 2026-04-03 | 500 | 183 (37%) | 153 (31%) | Baseline |
| v2 | 2026-04-03 | 500 | 293 (59%) | 217 (43%) | String globals + loop SSA + ptr tracking |
| v3 | 2026-04-03 | 500 | 435 (87%) | 312 (62%) | Bool print + nested JSON + i1 coercion |
| v5 (final) | 2026-04-03 | 1000 | 923 (92%) | 588 (**59%**) | 500 new diverse tasks, full re-inference |

The v5 row read `588 (64%)` until 2026-09-19. Note that every other row in this
table computes its Pass@1 percentage against the **generated** count in the Tasks
column — 153/500 = 31%, 217/500 = 43%, 312/500 = 62% — and only the v5 row was
computed against the *compiled* count (588/923 = 64%). The corrected v5 figure,
588/1000 = 59%, is the one consistent with the rest of its own table. (The same
anomaly is visible in `toke-spec/docs/gate1-decision.md`, independently.)

### Codegen Fixes Applied (Epic 2.8)

Seven compiler codegen bugs were identified and fixed during benchmark iteration:

1. **String globals hoisting** — `@.str.N` constants emitted inside function bodies; moved to module scope via buffered flush
2. **Loop variable SSA scoping** — re-declared variables in loops collided with outer scope; added NameAlias system
3. **Array ptr tracking through function returns** — return type not tracked as ptr for array-returning functions; added LocalType registry
4. **Bool JSON printing** — `tk_json_print` received i1 instead of i64; added type-aware dispatch to `tk_json_print_bool`
5. **Nested/heterogeneous JSON array parsing** — runtime only handled flat integer arrays; extended to handle nested arrays, strings, booleans
6. **String .len and [i] on parsed strings** — runtime functions for string length and char-at added
7. **i1/i64 boundary coercion** — branch conditions and cast expressions didn't handle type mismatches; added `icmp ne` coercion and source-type-aware casting

All fixes validated with zero regressions across 90 conformance tests and 9 e2e tests.

### Remaining Failures (41.2%)

(This heading read 36.3% until 2026-09-19 — the complement of the withdrawn
63.7%. The complement of the corrected 58.8% is 41.2%. The ~335 failures counted
below exclude the 77 solutions that never compiled; those are failures too.)

| Category | ~Count | Nature |
|----------|--------|--------|
| Model logic errors | 200 | Incorrect algorithm, off-by-one, wrong output |
| Model syntax errors | 15 | void return pattern, parse errors, charset violations |
| String type tracking | 20 | Codegen doesn't track str type through all paths |
| Runtime computation | 100 | Compiles but produces wrong result |

These are predominantly model quality issues, not compiler bugs. Further training data, prompt refinement, and model scaling will address them in Phase 2.

---

## What Was Built in Project Phase 1

### Reference Compiler (tkc)

| Component | Lines | Stories |
|-----------|-------|---------|
| Lexer | 296 | 1.2.1 |
| Parser | 394 | 1.2.2 |
| Import resolver | 179 | 1.2.3 |
| Name resolver | — | 1.2.4 |
| Type checker | 346 | 1.2.5 |
| Diagnostic emitter | 180 | 1.2.6 |
| Interface emitter | 190 | 1.2.7 |
| LLVM IR backend | 396+ | 1.2.8, 2.8.1, 2.8.2 |
| CLI | 186 | 1.2.9 |
| **Total** | **~1800** | **10 stories** |

Conformance: 90/90 tests passing. 9/9 e2e tests passing.
Targets: x86-64 Linux (ELF), ARM64 Linux (ELF), ARM64 macOS (Mach-O).

### Standard Library (14 modules)

str, json, toon, yaml, i18n, http, db, file, env, process, crypto, time, log, test.

All with C runtime backing, `.tki` interface files, and unit tests.
TOON-first serialization strategy documented in ADR-0003.

### Training Corpus

46,754 validated, deduplicated, compiler-checked programs across 4 stages:
- Stage A: 26,978 core algorithms
- Stage B: 9,776 multi-function programs
- Stage C: 5,000 boundary conditions
- Stage D: 5,000 application-level programs

Multi-model generation pipeline (Claude Haiku 4.5, GPT-4.1-mini, Grok-3-mini) with 3-language differential testing.

### Benchmark Harness

1,000 held-out tasks with 120 test inputs each.
Python/C/Java reference baselines.
Automated compile, run, and score pipeline.

### Security

SAST (cppcheck + clang-tidy), libFuzzer fuzzing, secret scanning, dependency scanning, signed commits with DCO enforcement.

---

## Next Steps

With Gate 1 passed, the project proceeds to:

1. **Default syntax implementation** (Epic 2.1) — 56-character set, type sigils, array literals
2. **Corpus default syntax transformation** (Epic 2.14) — transform 46K programs to default syntax
3. **Model scaling** — larger base models, expanded training data, improved prompts
4. **Hugging Face publication** (Epic 6.1) — model card, weights, benchmark results
5. **Research review** (Story 2.17.2) — update placeholders with final Gate 1 results

---

## References

- Language specification: [toke/spec/](https://github.com/karwalski/toke/tree/main/spec)
- Reference compiler: [toke/src/](https://github.com/karwalski/toke/tree/main/src)
- Benchmark results: `toke-eval/benchmark/results/gate1_v5_1000.json`
- Tokenizer evaluation: `toke/spec/docs/research-review-request.md` Section 5.1
- Serialization strategy: `toke/spec/docs/architecture/ADR-0003.md`
- Story tracker: `toke/docs/progress.md`
