---
title: Changelog
slug: changelog
section: about
order: 1
---

A reverse-chronological record of major project milestones.

## April 2026

- **(2026-04-15)** **Gate 2 ON HOLD**: Training corpus quality insufficient for reliable model training. Automated toke code generation produces too many syntax and semantic errors for the corpus to be useful without significant manual curation. Gate 2 evaluation paused until a higher-accuracy training corpus can be built, likely requiring local compute hardware (Mac Mini/Studio) for iterative retraining.
- **(2026-04-15)** **Documentation consolidation**: All documentation merged into `~/tk/docs/` as single source of truth. Research and planning documents moved to `~/tk/research/`.
- **(2026-04-12)** **Corpus overhaul complete**: 188,830 deduplicated records, 18,890 train + 994 eval rows in chat format with quality gates. Phase 1 data archived.
- **(2026-04-05)** **Syntax frozen** at `v0.2-syntax-lock`. 55-char profile is the default ("toke"). 80-char profile available under `--legacy`.
- **(2026-04-03)** **Gate 1 PASS**: 12.5% token reduction (8K purpose-built BPE vs cl100k_base on the **same toke source** -- mean 172.9 vs 197.6 tokens/program over 46,754 validated toke programs; a tokenizer-lane figure, not a comparison with Python, and superseded on v0.4 text -- `docs/metrics-baseline.md`); 63.7% Pass@1 (Qwen 2.5 Coder 7B + LoRA, 1,000 held-out tasks). *(The original entry read "vs. Python/Go/Rust equivalents" -- corrected 2026-09-19, story 132.6: the figure never compared toke with another language.)* Both thresholds exceeded (spec requires >= 10% reduction, >= 60% Pass@1).

## Q1 2026

- **Repo consolidation**: 10 repositories reduced to 6.
- **30+ standard library modules** implemented in C with full `.tki` interface contracts.
- **Build system, test hardening, and integration test suites** completed.
- **8K BPE tokenizer trained**: 15.2% token reduction compared to `cl100k_base`.
- **Website and specification updates** for default syntax.

## Q4 2025

- **Phase A--D corpus**: 46,730 validated programs covering pure language, stdlib, and advanced patterns.
- **Compiler bootstrap**: `tkc` compiler producing valid executables with conformance tests.
- **toke-spec formalized**: Specification document locked for Gate 1 evaluation.

## Project Gates (from spec Section 21.5)

| Gate | Criteria | Status |
|------|----------|--------|
| Gate 1 (Month 8) | Token reduction >= 10%, Pass@1 >= 60% | **PASS** (Apr 2026) |
| Gate 2 (Month 14) | 7B model >= 65% Pass@1, extended features maintain efficiency | **ON HOLD** |
| Gate 3 (Month 26) | Two+ LLM families >= 70% Pass@1, self-improvement loop running | Pending |
| Gate 4 (Month 32) | All benchmarks met, spec complete, consortium proposal ready | Pending |
