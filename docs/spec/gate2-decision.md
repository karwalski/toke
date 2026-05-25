# Gate 2 Decision Document

**Date:** 2026-05-22
**Verdict:** PASS
**Evaluator:** Automated pipeline (gate2_pipeline.py)

---

## Results

| Metric | Gate 1 (2026-04-03) | Gate 2 (2026-05-22) | Target |
|--------|---------------------|---------------------|--------|
| Pass@1 (compile) | 63.7% | **100%** | >70% |
| Compilation rate | 92.3% | **100%** | — |
| Tasks evaluated | 1,000 | 700 (500 hidden + 200 eval) | — |
| Token reduction | 12.5% (8K vocab) | 8,192 BPE trained | — |
| Functional correctness | Not measured | **55.6%** (272/489) — corrected 2026-05-25; originally reported ~8% before stdlib fix | Not a gate criterion |

## Gate 2 Criteria (from Epic 10.10)

> 7B fine-tuned model outperforms baseline on toke code generation using default (55-char) syntax.

**Baseline (Gate 1):** 63.7% Pass@1 on legacy 80-char syntax
**Gate 2 result:** 100% Pass@1 on default 55-char syntax

Gate 2: **PASS** — model significantly outperforms baseline.

---

## What Changed Between Gate 1 and Gate 2

### Corpus
- Gate 1: 73,000 records (synthetic, multi-format, legacy syntax)
- Gate 2: 25,953 records (canonical prompt, v0.3 syntax, includes 6,069 loke production records)
- Loke production code used because it represents real, thoroughly-tested toke programs (172 modules, 87K lines, all compiling)

### System Prompt
- Gate 1: 3 inconsistent prompt variants (524-5103 chars), contained wrong syntax (`mut x=0`, `|{Ok:v}` match, `$void`)
- Gate 2: Single canonical prompt (3,433 chars), every example verified to compile, structured by category (STRUCTURE, CHARACTERS, KEYWORDS, TYPES, BINDINGS, FUNCTIONS, CONTROL FLOW, etc.)

### Compiler
- 22 new W1020 foreign keyword hints (Go/Rust/JS/C + near-miss toke keywords)
- 6 new --migrate transforms for LLM patterns (fn→f=, :void→:i64, null→0, ->→:)
- W1020 false positive fix (module-qualified calls like j.print excluded)
- Parser hint for Rust-style `let mut x` pattern

### Eval Pipeline
- All scripts updated: tkc→toke binary name
- 1000 benchmark solutions converted from v0.1 to v0.3
- Type signatures updated from [i64] to @i64
- grammar.ebnf rewritten to match spec Section 10

### Infrastructure
- Cloud training on AWS EC2 A10G (24GB VRAM) instead of local compute
- 37 hours training time (QLoRA rank 64, alpha 128, batch 1, seq 1024, 3 epochs)
- BPE tokenizer trained (8,192 tokens) on v0.3 corpus

---

## Functional Correctness Analysis

While Gate 2 measured compilation (the defined criterion), we also tested functional correctness:

### Original Measurement (before stdlib fix)

| Category | Count | % |
|----------|-------|---|
| Solutions that compile | 500/500 | 100% |
| Solutions that read from argv | 167/500 | 33% |
| Solutions that hardcode values | 333/500 | 67% |
| Compiled to binary (of testable) | 50/167 | 30% |
| Functionally correct (of compiled) | 4/50 | 8% |

### 2026-05-25 Correction

**Original measurement:** 4/50 (8%) — caused by missing `io.readln()` C glue.

`io.readln()` was declared in the `.tki` interface file but had no implementation in `io_glue.c`. Programs that used standard input (the majority of benchmark tasks) compiled correctly but could not link to a working binary — they couldn't be tested for functional correctness. The 8% figure reflected infrastructure failure, not model failure.

**Re-evaluation with fix:** 272/489 (55.6%) functional Pass@1.

This means the model learned both syntax AND semantics from Gate 2 training. The "algorithmic reasoning gap" identified in the original analysis was largely an artefact of the missing stdlib glue function.

### Updated Interpretation

The model writes syntactically valid toke 100% of the time AND produces functionally correct output 55.6% of the time. The remaining 44.4% failure includes genuine algorithmic errors, argv-hardcoding patterns, and edge cases — but the model demonstrably reasons about program semantics, not just syntax.

**This was not the goal of Gate 2** (which validated compilation), but it significantly changes the narrative for Gate 3 planning: the corrected baseline of 55.6% already exceeds the Gate 3 C1 threshold of 35%.

---

## Next Steps (Gate 3 Planning)

With the corrected 55.6% baseline, Gate 3 C1 (functional Pass@1 >= 35%) is likely already met. However, formal Gate 3 evaluation requires the exact Gate 3 eval harness with all five criteria (C1-C5). The remaining goals:
1. Multi-model coverage (C4) — test on a second model family
2. Self-improvement loop demonstration (C5)
3. argv-generalisation >= 50% (C3)
4. MCP-based data collection from real usage

See `docs/spec/training-next-phase.md` for the full specification.

---

## Decision

Gate 2 is **PASS**. The project advances to Gate 3 planning.
