# Request for Research Feedback

**Project:** toke — A Programming Language Designed to Reduce Token Cost of AI-Generated Code
**Date:** 2026-05-23 (updated with research review responses)
**Author:** Matthew Watt (karwalski)
**Status:** Post-Gate 2, planning 12-week functional correctness sprint. v0.3 syntax locked.
**Reasoning channel:** Out-of-band reasoning mandated via `.tkc` companion files — see [reasoning-channel.md](reasoning-channel.md)

---

## Executive Summary

toke is a statically typed, compiled programming language designed from first principles so that large language models can write better code. After 18 months of development, we have achieved:

- **Gate 1 (2026-04-03):** 63.7% Pass@1 compilation rate with legacy syntax
- **Gate 2 (2026-05-22):** 100% Pass@1 compilation rate with default syntax (7B model)
- **Functional correctness:** 55.6% (corrected from ~8% after stdlib fix — see gate2-decision.md)

We are seeking feedback from researchers in code generation, curriculum learning, program synthesis, and AI safety on our next training phase design.

---

## The toke Language

### Design Thesis

LLMs waste tokens on syntactic overhead that provides no semantic value. Comments, docstrings, whitespace conventions, camelCase/snake_case naming, and verbose keywords all consume tokens without helping the model reason about computation. toke eliminates this overhead by design.

### Key Properties

| Property | Value | Rationale |
|----------|-------|-----------|
| Character set | 55 (a-z, 0-9, 19 symbols) | Eliminates tokenizer ambiguity from uppercase, underscore, most special chars |
| Keywords | 13 (m, f, t, i, if, el, lp, br, let, mut, as, rt, mt) | Minimal control flow vocabulary |
| Grammar | LL(1), deterministic | Every parse decision from one token of lookahead |
| Comments | None | Documentation lives in companion files (.tkc), never in source |
| Naming | Lowercase concatenated only | No case conventions, no underscores |
| Type system | Static, structural, with sum types | Errors are values, not exceptions |
| Separator | Semicolons exclusively | No commas anywhere — eliminates a common LLM error |
| Compilation | LLVM backend → native binaries | Real programs, not interpreted |

### Language Specification

The full normative specification is available at:
- **Spec:** [toke-spec-v0.3.md](toke-spec-v0.3.md) (142KB, 31 sections)
- **Grammar:** [grammar.ebnf](grammar.ebnf) (LL(1) EBNF, 90 productions)
- **Gate 1 decision:** [gate1-decision.md](../docs/gate1-decision.md)
- **Gate 2 decision:** [gate2-decision.md](gate2-decision.md)

---

## Training Results

### Gate 1 (Legacy 80-character syntax)

| Metric | Result |
|--------|--------|
| Base model | Qwen 2.5 Coder 7B-Instruct |
| Method | QLoRA (rank 64, alpha 128) |
| Corpus | 73,000 records (synthetic, multi-format) |
| Compilation Pass@1 | 63.7% (588/923 compilable tasks) |
| Token reduction | 12.5% vs cl100k_base (8K vocab BPE) |

### Gate 2 (Default 55-character syntax)

| Metric | Result |
|--------|--------|
| Base model | Qwen 2.5 Coder 7B-Instruct |
| Method | QLoRA (rank 64, alpha 128, 3 epochs) |
| Corpus | 25,953 records (canonical prompt, includes 6,069 from production codebase) |
| Compilation Pass@1 | **100%** (700/700 tasks) |
| Functional Pass@1 | 55.6% (272/489) — corrected 2026-05-25 from ~8% after io.readln() fix |
| BPE tokenizer | 8,192 tokens, trained on v0.3 corpus |
| Training time | 37 hours on NVIDIA A10G 24GB |

### Key Observation

The model achieved **perfect syntax** with a relatively small corpus (25K) and modest hardware. This validates the language design thesis: a constrained, unambiguous grammar is dramatically easier for LLMs to learn than general-purpose languages.

However, **functional correctness** (producing correct algorithms) requires capabilities beyond syntax mastery.

---

## Proposed Next Phase: Curriculum Learning

We propose a 6-phase progressive training curriculum:

| Phase | Objective | Corpus Size | Success Metric |
|-------|-----------|-------------|----------------|
| 1. Token completion | Complete partial lines | 10K | >95% valid |
| 2. Single statement | Write one statement given context | 20K | >90% valid, >60% semantic |
| 3. Single function | Complete function from signature + description | 30K | >85% compile, >50% functional |
| 4. Multi-function | Write programs with multiple cooperating functions | 15K | >80% compile, >40% functional |
| 5. Multi-module | Write systems spanning multiple files with imports | 5K | >70% compile, >30% functional |
| 6. Full application | Design complete applications from requirements | 1K | Compiles as unit |

Each phase uses the previous phase's checkpoint as starting point. Evaluation gates progression.

---

## Questions for Researchers

### Curriculum Design

1. **Phase boundaries:** Are our 6 phases the right granularity? Should statement→function have intermediate steps (e.g., "write a loop body", "write a conditional")?

2. **Curriculum scheduling:** Should we use strict gating (must pass threshold before advancing) or mixed sampling (gradually increase difficulty while still training on earlier phases)?

3. **Negative examples:** Should we include "broken → fixed" pairs at each phase? The compiler provides structured diagnostics — is this a useful training signal?

### Self-Improvement Loop

4. **Bootstrapping:** We can generate 50K candidates at 100% compile rate and 55.6% functional rate. Is rejection sampling (keep only correct) the best use of this, or should we also train on the incorrect-but-compilable examples with appropriate labels?

5. **Diversity:** When generating training data via self-play, how do we avoid mode collapse (model generating the same solution patterns repeatedly)?

6. **Verification oracle:** Our test I/O is limited to 10–31 cases per task. Is this sufficient to trust functional correctness, or do we need property-based testing / formal verification for higher confidence?

### Model Architecture

7. **Model size:** Given perfect syntax at 7B, should we invest in:
   - (a) More data for the same 7B model
   - (b) A larger base model (32B/70B) for better reasoning
   - (c) A mixture: 70B generates data for 7B distillation

8. **Tokenizer integration:** We have a purpose-built 8K BPE tokenizer for toke. Should we:
   - (a) Replace the base model's tokenizer entirely
   - (b) Add toke tokens to the existing vocab (vocabulary extension)
   - (c) Keep the base tokenizer and let the model learn the mapping

9. **Multi-language transfer:** The model was pre-trained on Python/JS/Go/Rust. Is there measurable transfer from these languages to toke algorithmic reasoning, or does the syntax difference break transfer?

### Data Collection

10. **MCP telemetry:** We plan to collect compile-verified code from developer usage via MCP (IDE integration). What privacy/bias concerns should we address? How do we handle the distribution shift between "code developers actually write" vs "benchmark tasks"?

11. **Community contributions:** We're considering open-sourcing the corpus and accepting community-contributed solutions. What quality control mechanisms would you recommend?

### Evaluation

12. **Beyond Pass@1:** What metrics beyond compilation and functional correctness should we track? Suggestions: token efficiency of generated code, code diversity, robustness to prompt variation, compositional generalisation.

13. **Difficulty calibration:** Our benchmark tasks range from "return absolute value" to "implement quicksort". How should we weight difficulty in our metrics? Is a single Pass@1 number meaningful, or should we report per-difficulty-band?

---

## Reproducibility

All code, data, and training configurations are available:

| Repository | Contents |
|-----------|----------|
| [toke](https://github.com/karwalski/toke) | Compiler, spec, grammar, stdlib (38 modules) |
| [toke-model](https://github.com/karwalski/toke-model) | Corpus, tokenizer, training configs |
| [toke-eval](https://github.com/karwalski/toke-eval) | Benchmark tasks, evaluation harness |
| [toke-mcp](https://github.com/karwalski/toke-mcp) | MCP server, VS Code extension, LSP |

Gate 2 training configuration:
```yaml
base_model: Qwen/Qwen2.5-Coder-7B-Instruct
method: QLoRA
rank: 64
alpha: 128
epochs: 3
batch_size: 1
gradient_accumulation: 16
max_seq_length: 1024
learning_rate: 2e-4
scheduler: cosine
warmup_ratio: 0.03
hardware: NVIDIA A10G 24GB
training_time: 37 hours
```

---

## How to Provide Feedback

We welcome feedback in any form:

1. **GitHub Issues:** [github.com/karwalski/toke/issues](https://github.com/karwalski/toke/issues) — tag with `research-feedback`
2. **Email:** research@tokelang.dev
3. **Pull requests:** Concrete improvements to training configs, curriculum design, or evaluation methodology
4. **Papers:** If you cite toke in your research, we'd love to know

---

## Research Teams Previously Consulted

| Team | Focus | Feedback Received |
|------|-------|-------------------|
| T1 | Reproducibility | Evaluation methodology |
| T2 | Compiler engineering | Performance benchmarking |
| T3 | Model training | Data curation, curriculum design |
| T4 | Specification quality | Formal methods, type system |
| T5 | Translation/readability | Cross-language transfer |
| T6 | Repository coherence | Documentation standards |
| T7 | External credibility | Benchmarking methodology |
| T8 | Gate governance | Evaluation criteria |

---

## Timeline

| Date | Milestone |
|------|-----------|
| 2026-04-03 | Gate 1 PASS (63.7% compile) |
| 2026-05-22 | Gate 2 PASS (100% compile) |
| 2026-06 (target) | Self-improvement loop: 30K execution-verified records |
| 2026-07 (target) | Gate 3 attempt (>50% functional) |
| 2026-Q3 (target) | Community corpus program launch |
| 2026-Q4 (target) | Gate 4 (multi-model, self-improvement demonstrated) |
