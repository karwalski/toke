# toke — Honest Metrics Baseline

**Purpose.** A single, honest reference for every quantitative claim about toke —
token efficiency, compile rate, functional correctness. External, academic, and
marketing claims MUST cite these numbers and this methodology, **not** headline
figures stripped of context (e.g. "Gate 2 PASS 100%"). This exists because the
project's thesis is *falsifiable research* (risk R001) and its credibility depends
on numbers that survive peer review (R008, R015).

**Last updated:** 2026-09-19 (story 132.0(a)). Numbers are sourced from `docs/progress.md`
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

---

## 2026-08 — **no model gate ran.** August produced a corpus-quality freeze, not a gate result

**This is the entry Epic 132 criterion 6 must cite for "the August gate result", and the
honest answer is that there is no August gate number to cite.** The M3 milestone line in
`PROJECT_STATUS.md` ("week-12 GO/NO-GO mid-August") is a *plan*, not a result. Nothing was
trained, fine-tuned or evaluated as a model in August 2026: every Epic 128 training story
(128.1–128.15) is still `planned` and compute-gated, and the last — and only — trained-model
evaluation on record remains **Gate 2, 2026-05-22** (the v0.3 QLoRA Qwen 2.5 Coder 7B, rows
above). **Do not manufacture an August gate number, and do not present the August work as a
model result.**

What August actually delivered, and what may be claimed for it:

| Date | Deliverable | Measured on | Number | Caveat |
|---|---|---|---|---|
| 2026-08-19 | **Training-data freeze** (Epic 129.6; record of source `toke-corpus/regen/AUDIT_129.md`) | v0.4 regen corpus, 23,382 records, under tkc **toke 2.8.0** | **14,727 pass all tightened gates**; 8,531 not-executable (no harness); 124 driver-limit; **test_fail 0, build_fail 0** | Data quality, not model quality. "Pass" = compiles + builds + exact test match + exit 0 + no extra output + idiom ≥ 0.6 + structure within rubric. It says nothing about what a model can generate. |
| 2026-08-19 | Library re-verification (129.3) | 1,583 library programs | **1,583/1,583 PASS** all gates (incl. gazeta 123) | Hand-written/curated library, not model output. 597 orphan solutions outside the manifests: **239 compile-fail (40%)** — excluded from training. |
| 2026-08-19 | A-category test coverage (129.7) | 631 A-category base tasks | 3–5 **execution-verified** test cases each; 2,894 A-side records executed for the first time | Exposed 1,100 failures that were then repaired (129.5). Before this, all 10,116 A-category specs had **zero** test cases — they had never been verified at all. |
| 2026-08-19 | Repair/compaction waves (129.4/129.5) | audit-flagged records | **2,639 records repaired or rewritten**; idiom-below-floor 0; fn-bytes p99 622 → 501 | Every replacement independently re-audited before banking; originals archived. |
| 2026-08-12→19 | Repo cleanup (Epic 130) | 15 repos | 33G → ~12G; every repo committed | Infrastructure, no metric content. |

**The August freeze has since been reopened.** Epic 131 reopened it on 2026-09-18; the frozen
state is preserved byte-for-byte (`freeze-129-20260819`) and the successor freeze will be
`AUDIT_131.md`. So even the corpus numbers above are a *superseded* snapshot, and the
September findings below show why.

**One-sentence form for downstream copy (132.2/132.3):** *"August 2026 produced a
training-data quality freeze — 23,382 audited v0.4 corpus records, 14,727 passing every
execution gate, and 1,583/1,583 library programs — not a model gate; the most recent model
gate remains Gate 2 of 2026-05-22 (100% compile-Pass@1, 55.6% functional, on a v0.3-trained
model), and no v0.4-native model has been trained."*

---

## 2026-09 — post-freeze findings (Epic 131 / 133)

These are the numbers now available to Epic 132. All are **compiler-and-corpus**
measurements under tkc **toke 2.8.0**; none is a model evaluation.

| Date | Evaluation | Measured on | Result | Caveat |
|---|---|---|---|---|
| 2026-09-19 | **Pattern/conformance sweep** (131.13) | all 23,382 regen records + 1,583 library programs, 18 tests, `regen/pattern_sweep.py` | regen buckets **EXEMPT 1,678 · AUTO 643 · AGENT 1,197 · LEAVE 14,215 · REGEN 5,649**; library **AUTO 53 · AGENT 1,272 · LEAVE 258**. Top savings by Σ estimate: `str-interp-vs-join` 7,896, `iter-map` 2,748, `str-build-loop` 2,342 (proxy8k tokens). AGENT saving p50/p90 = **4/16 tokens**. | Savings are **proxy8k** estimates on `--min`+masked text, not cl100k and not a published reduction. "AGENT/AUTO" is a rewrite backlog, not a defect rate: 14,215 records needed no change. tkc sha `936ce131` on every row. |
| 2026-09-19 | **A-ERR error-union gaming scan** (131.42) | 576 A-ERR / error-union single_function records, `regen/err_union_check.py`, verdicts from the driver run | **429 records across 69 bases are gamed** (175 more than a text search found): 389 `target_returns_str`, 40 `wrapper_returns_str`; 424 build the harness's `{'err': …}` marker as a literal, **5 construct it at run time from string operations**. Correct: **147**. `wrong_return_type`: **0**. | The generation model learned to defeat a literal-matching test. This is the clearest evidence in the project that **passing a test is not evidence of correctness when the test can be gamed** — it directly qualifies any "% pass" figure taken from the corpus. All 429 routed to the 131.15 rewrite wave; a hard `return_type` gate now blocks the shape. |
| 2026-09-19 | **a_tests re-authoring recheck** (131.47) | 336 records of the 54 re-authored packed-arity bases | **pass 249 · fail 68 · driver_fail 19**; 40 previously-failing records **resolved**, 0 regressed | The 68 failures are the finding: records that only ever "passed" against wrong-arity or gamed tests and are **genuinely wrong** against correct ones (→ 131.49). Same lesson as 131.42: the test, not the record, was the weak link. The 19 driver_fails are a pre-existing driver limit (map inputs), not record faults. |
| 2026-09-19 | **Gate-1 60 v0.4 re-delivery** (133.4 Part A) | the 60 KERN-benchmarked task ids, `toke-eval/docs/gate1-60-v04.md` | **60/60 `tkc --check`, 60/60 hidden tests** (120 cases each), lint 0/0. Tokens (cl100k of `--min`, 60 programs): **4,787** (o200k 4,778; 11,537 bytes) vs Kern 3,012 / Python 3,565 / old toke 6,347. **toke/Python 1.34× [1.22, 1.48]** (was 1.76×); Kern/toke 0.63 (was 0.48). | **Hand-written, not model-generated** — 27 ids are pure `--migrate` output, 33 were hand-repaired. It measures what the *language* can express, not what a *model* produces; see `docs/about/toke-eval-drift-decision.md` for what this set may and may not claim. toke/Python is a cross-language density ratio (TEMSpec §2.3), not a reduction. |
| 2026-09-19 | **v0.4 sample side-by-side** (132.0(b)) | 4 execution-verified toke/Python pairs, all lanes | `docs/about/samples-v04.md` + `.json` — cl100k: toke 344 vs Python 264 (**1.30×**); bytes 693 vs 753 (0.92×); proxy8k on the toke side 178 | N = 4, illustrative only. `proxy8k`/`tokenizer_v03` are toke-trained and must **never** be applied to the Python side for a comparison. Replaces the v0.3-era "42% reduction vs Python" site copy, which compared two different tokenizers. |

**What is still NOT measured (unchanged):** no v0.4-native model exists; no model has been
trained since April 2026; every functional-correctness number on this page is v0.3-era.
