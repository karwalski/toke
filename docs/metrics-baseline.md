# toke — Honest Metrics Baseline

**Purpose.** A single, honest reference for every quantitative claim about toke —
token efficiency, compile rate, functional correctness. External, academic, and
marketing claims MUST cite these numbers and this methodology, **not** headline
figures stripped of context (e.g. "Gate 2 PASS 100%"). This exists because the
project's thesis is *falsifiable research* (risk R001) and its credibility depends
on numbers that survive peer review (R008, R015).

**Last updated:** 2026-07-03 (Epic 123.3). Numbers are sourced from `docs/progress.md`
stories; each row cites its origin.

---

## The load-bearing caveats (read first)

1. **All trained-model numbers below are from a v0.3-syntax model.** Gate 2 was a
   fine-tuned Qwen 2.5 Coder 7B (QLoRA), trained on the v0.3 corpus. Epic 116 then
   shipped the **breaking v0.4** change (`=`/`==`, expression-`if`, `&&`/`||`).
   The corpus, tokenizer, and model must be refreshed before any Gate-2 number
   carries forward to v0.4. **No from-scratch ~1B model (the actual north-star
   deliverable) has been trained yet.**
2. **"Gate 2 100% compile-Pass@1" is on a *curated* hidden+eval set** the model was
   optimised against. A **full-local re-audit of all 1,748 corpus programs
   (101.R1, v0.3.9)** is the harsher, honest real-world floor: **37.5% compile,
   ~2.2% fully-correct.** Never cite the 100% without this context.
3. **Efficiency must be measured on the `--min` canonical form**, not readable
   source. Measuring readable source understated toke by **28.2%** (116/B2). Any
   cross-language token comparison must use the same tokenizer basis on both sides.
4. **Functional correctness — not compile rate — is the open weakness.** Compile
   Pass@1 is high on curated sets; functional correctness is ~56% at best.

---

## Efficiency (token reduction)

| Metric | Value | Basis / caveat | Source |
|---|---|---|---|
| Gate 1 token reduction vs baseline | **12.5%** | 8K purpose-built BPE vocab | Gate 1 (2026-04-03) |
| `--min` vs readable-source understatement | **28.2%** | published library used readable source; canonical basis is `--min` | 116/B2 |
| Target (1B model, north star) | illegal-char <0.1% | — | `architecture/1b-model-design.md` |

**Rule:** re-measure the public library on the `--min` form with the toke tokenizer
(vs cl100k for Python, etc.) before republishing — tracked as **118.4** (compute-gated).

---

## Correctness (compile + functional Pass@1)

| Evaluation | Compile Pass@1 | Functional | Set / model | Source |
|---|---|---|---|---|
| Gate 2 (curated) | **100%** | **55.6%** (272/489) | 500 hidden + 200 eval; Qwen 2.5 Coder 7B + QLoRA (v0.3) | 2.5.1 (2026-05-22); functional corrected 2026-05-25 from ~8% after an `io.readln` stdlib fix |
| **Full-local re-audit (honest floor)** | **37.5%** (655/1748) | **~2.2%** (38 PASS) | all 1,748 programs, v0.3.9 | 101.R1 / 102.17 (2026-05-28): 38 PASS, 141 WRONG_OUTPUT, 64 RUN_FAIL, 9 BUILD_FAIL, 1093 COMPILE_FAIL |
| `toke_generate` sample | 84% (21/25) | 78% (18/23) | 25-prompt benchmark, new model | 71.5.4 (2026-05-24) |
| Corpus after v0.4 mechanical migration | **84.3%** (1781/2112) | n/a | `=`→`==` only, not yet idiomatic | 118.2 (2026-07-02) |

Baseline comparison (Gate 1): the fine-tuned-7B baseline was **63.7% Pass@1**,
~2.5% illegal-char, ~15 tok/s on an M4.

---

## What is NOT yet measured (gaps that block honest claims)

- **v0.4-native model numbers** — none exist; everything above is v0.3.
- **`--min`-basis efficiency for the public library** — 118.4 (compute-gated).
- **Runtime/functional correctness under runtime verification** — CI checks compile
  (`--check`) only; runtime miscodegen (e.g. anonymous functions, the `$none` arm —
  see `known-limitations.md` #3/#5, Epic 123.5) is invisible to `--check`. A runtime
  (exit-code) correctness gate is needed (123.6/123.8) before functional numbers are
  trustworthy.

---

## How to use this file

- Cite the **specific row + caveat**, not a headline number.
- When a new evaluation runs, add a row here **and** update `PROJECT_STATUS.md`; never
  publish a number that isn't reflected here first.
- The `--min` measurement machinery is `tkc --min` (`src/fmt.c`); the idiom/efficiency
  gate is `qwen_judge.py` (116/B3).

## 2026-09-18 — v0.4 tokenizer baseline (Epic 131.20, PRE-rewrite)

Measured on 2,000 stratified records from the 2026-08-19 freeze (ids + SHAs in
`toke-tokenizer/data/baseline_sample_ids_v04.txt`), canonical `tkc --min` text with
string bodies masked to `"_"`, per TEMSpec. Report: `toke-tokenizer/docs/baseline_v04_pre131.md`.

| tokenizer | total tokens | tokens/program | vs cl100k |
|---|---:|---:|---:|
| cl100k_base | 242,427 | 121.2 [118.9, 123.6] | 1.000 |
| o200k_base | 245,217 | 122.6 | 1.012 |
| Qwen2.5-Coder | 250,287 | 125.1 | 1.032 |
| SentencePiece 8k (shipped) | 279,672 | 139.8 | **1.154** |
| SentencePiece 32k | 279,143 | 139.6 | 1.152 (13,605 unk) |
| tokenizer_v03 (16,384) | 131,998 | 66.0 | 0.545 — **lossy: drops 2,606 `\` chars** |

**Reading:** the shipped 8k tokenizer needs 15.4% *more* tokens than cl100k on v0.4 text, and the
v0.3 HF tokenizer only appears to win because its null `unk_token` silently deletes every backslash.
No "purpose-built tokenizer beats cl100k" claim is supportable until 116.9 trains and locks the v0.4
tokenizer; the Phase-3 gate anchors on cl100k = 242,427 on this exact sample. Cross-language density
(toke vs Python under cl100k) is a separate, informational number — see the KERN review
(`docs/about/reviews/kern-2026-08.md`): on the 60 Gate-1 tasks toke/Python ≈ 1.76 under cl100k.
