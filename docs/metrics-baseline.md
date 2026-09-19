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

## Project facts (story 132.14)

**These are the only sanctioned project-scale counts.** Every one is derived from
the tree by `scripts/verify_project_facts.py`; the command in each row reproduces
it in one line. `make ci` runs `verify_project_facts.py --check`, which fails if
this table and the tree disagree, so the table cannot silently rot.

Measured on tkc **toke 2.8.0**, regenerate with
`python3 scripts/verify_project_facts.py` (add `--json` for the machine-readable
sheet, `--probe` to re-derive the character set against the built compiler).

<!-- PROJECT-FACTS BEGIN -->

| Fact | Value | Derived by |
|---|---:|---|
| `charset_total` | **59** | `python3 scripts/verify_project_facts.py --probe` — 26 lowercase + 10 digits + 23 symbols, from `src/lexer.c` |
| `charset_symbols` | **23** | `!"$%&()*+-./:;<=>@^{|}~` — the `case` arms of the symbol switch in `src/lexer.c` that do not emit E1003 in `PROFILE_DEFAULT`, plus `"` |
| `keywords` | **14** | `sed -n '/KEYWORDS_DEFAULT/,/};/p' src/lexer.c` — 10 reserved words (`if el lp br let mut as rt sc mt`) plus the 4 declaration heads `m= i= t= f=` |
| `grammar_productions` | **53** | `grep -cE '^[A-Za-z][A-Za-z0-9_]*[[:space:]]*=' docs/spec/grammar.ebnf` |
| `stdlib_modules` | **57** | `ls stdlib/*.tki \| wc -l` |
| `conformance_cases_yaml` | **222** | `find test -name '*.yaml' \| wc -l` (grammar 98, diagnostics 89, lexical 35) |
| `conformance_cases_shell` | **6** | `ls test/conform/*.sh \| wc -l` |
| `conformance_cases_total` | **228** | the two rows above; `make conform` runs both |
| `diagnostic_codes_documented` | **52** | `grep -cE '^### [EW][0-9]{4}' docs/reference/errors.md` |
| `diagnostic_codes_in_src` | **52** | `python3 scripts/check_error_codes.py --list` — codes `src/` defines or emits; the gate fails on any divergence from the documented set |
| `corpus_records_v04_frozen` | **23,382** | `jq .total ../toke-corpus/regen/freeze/freeze_129_summary.json` — freeze `129-freeze-2026-08-19`, reopened by Epic 131 |
| `corpus_records_v02_2026_04` | **46,754** | `jq .total_entries ../toke-corpus/corpus/manifest.json` — the **v0.2-era** corpus of 2026-04-01, historical |
| `epics` | **132** | `grep -oE '^#{2,3} Epic [0-9]+' docs/progress.md \| awk '{print $3}' \| sort -u \| wc -l` — *live, not gated: re-derive before citing* |
| `stories` | **2,374** | `grep -oE '^\| [0-9]+\.[0-9]+[a-z0-9.]* \|' docs/progress.md \| sort -u \| wc -l` — *live, not gated: moves with every tracker commit, re-derive before citing* |

<!-- PROJECT-FACTS END -->

### The character set: 59, not 55 and not 56

Two normative documents disagreed and **neither matched the compiler**. The RFC
(`spec/rfc/draft-karwalski-toke-lang-00.md` §5.1) said 56 — and its own table
listed 21 symbols while totalling 20, so 56 was wrong even on its own arithmetic.
`docs/about/positioning-2026-09.md` and `docs/glossary.md` said 55 (19 symbols),
which is the RFC's list minus `^` and `~`.

`src/lexer.c` settles it. In `PROFILE_DEFAULT` the lexer rejects exactly eight
printable ASCII characters in structural position with E1003 — `` ' , ? [ \ ] _ ` ``
— and accepts every other symbol. Three of the accepted ones are absent from both
documents:

- **`^` and `~` are not reserved.** Story **114.8** assigned them bitwise XOR and
  bitwise NOT. `let e=a^b; let g=~a;` compiles today. The RFC's "reserved and
  unassigned … MUST NOT be used in toke source" is simply out of date.
- **`%` and `&` are live operators** (modulo, bitwise-and, and `&&` short-circuit),
  yet the RFC's §5.1 exclusion list names both as excluded.

That gives 26 lowercase + 10 digits + 23 symbols = **59**. Uppercase `A-Z` is
excluded because the parser rejects an uppercase identifier (E2002), and `_` is
excluded by the no-underscore rule (113.2a); `#` is accepted with W1020 only as
Python-comment *recovery* and is not a member of the alphabet; `\` is legal inside
string literals but not in structural position.

59 is also the number this project already ratified once: story **75.1.1**
(2026-04-30) decided "recount to 59, retire the 56 branding", and the count
regressed to 55/56 during the v0.3 documentation rewrite. The design property was
never the number — it is that the set is small and closed — so prefer "a closed
alphabet of 59 printable ASCII characters, lowercase only" to a bare figure.

### Claims deleted rather than corrected (132.14)

These were published, are not derivable from any artefact in any repo, and have
been removed rather than re-stated: loke's "698 `.tk` files / 87,318 lines" (the
loke tree is not in this workspace and no manifest records it), "3 months of daily
development", "84% compilation via production API" and its gap analysis (a 21/25
sample from 71.5.4, published as a project-scale capability figure), and the "67%
of Gate 2 functional failures hardcode their inputs" rate (no baseline row and no
script reproduces it). A claim nobody can reproduce is worse than no claim.

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

## Canonical wording for token-efficiency claims (story 132.6)

**These two blocks are the only approved public wording for the "52% token reduction"
and "42% reduction vs Python" claims.** Stories 132.1, 132.8, 132.9, 132.10 and 132.13
copy them **verbatim**; do not paraphrase, do not drop a qualifier, do not quote a bare
percentage. Both carry the four TEMSpec §6.3 reporting fields — metric type, tokenizer(s),
baseline, and N.

### Long form (documentation, whitepaper, site pages)

> **Token efficiency, stated per TEMSpec §6.3 (metric type · tokenizer · baseline · N).**
>
> - **Tokenizer-level reduction — same text, two tokenizers.** The v0.3 Toke-16K BPE
>   (16,384 vocab, trained on 25,953 v0.3-syntax programs) encoded toke source in **~52%
>   fewer tokens than cl100k_base encoded the same toke source** — N = 42 v0.3 benchmark
>   programs, 2026-05-22. It compares two tokenizers on one text; it was never a comparison
>   with Python. **It is superseded and must not be republished as a headline:** on
>   canonical v0.4 `--min` text (N = 2,000 stratified corpus records, 2026-09-18) every
>   shipped toke tokenizer is *worse* than cl100k_base — the 8K SentencePiece needs **15.4%
>   more** tokens (ratio 1.154 [1.147, 1.160]) — and `tokenizer_v03.json` only appears to
>   win (ratio 0.545) because its null `unk_token` silently drops every backslash (2,606 in
>   that sample). No "purpose-built tokenizer beats cl100k" claim is supportable until the
>   v0.4 tokenizer is trained and locked (116.9).
> - **Cross-language density — one shared tokenizer, two languages (TEMSpec §2.3,
>   informational).** On the 60 Gate-1 tasks, hand-written v0.4 toke costs **1.34×
>   [1.22, 1.48]** the cl100k_base tokens of equivalent Python (4,787 vs 3,565; N = 60,
>   2026-09-19); on the four execution-verified v0.4 sample pairs it is **1.30×** under
>   cl100k_base and 1.32× under o200k (N = 4). **toke currently costs about 30% more tokens
>   than Python under the tokenizers models actually use**, not fewer. The v0.3-era text
>   measured 1.76×.
> - **Never cross the lanes.** `proxy8k` and `tokenizer_v03` are trained on toke text;
>   applied to Python they measure their own training bias, not the language. Quoting a
>   toke-trained tokenizer on the toke side against cl100k on the Python side is the
>   methodology error behind the withdrawn "42% reduction vs Python" (story 132.13). Any
>   cross-language number uses **one** tokenizer on both sides.
>
> Sources: `docs/metrics-baseline.md`, `docs/about/samples-v04.md`,
> `toke-eval/docs/gate1-60-v04.md`, `toke-tokenizer/docs/baseline_v04_pre131.md`.
> Methodology: TEMSpec §2.1, §2.3, §6.3.

### Short form (READMEs, registry descriptions, cards, one-liners)

> **Token efficiency, measured:** under one shared tokenizer (cl100k_base) toke costs
> **1.34× [1.22, 1.48]** the tokens of equivalent Python on the 60 Gate-1 tasks (N = 60,
> 2026-09-19) — more, not fewer. The v0.3-era "52% fewer tokens" figure was a
> *tokenizer-vs-tokenizer* measurement on identical toke text (Toke-16K v0.3 vs cl100k_base,
> N = 42) and is superseded: on canonical v0.4 text the shipped 8K tokenizer needs **15.4%
> more** tokens than cl100k_base (N = 2,000). See `docs/metrics-baseline.md`.

### What is withdrawn outright

| Claim | Why it cannot be requalified | Replacement |
|---|---|---|
| "42% reduction vs Python" (and "56% vs Java") | Applies a **toke-trained** tokenizer to the toke side and cl100k to the Python/Java side — it measures tokenizer training bias, not the language | The §2.3 density ratios above (1.30×/1.34×), stated as toke costing *more* |
| "*N* toke BPE tokens vs *M* for Python on cl100k" (fibonacci 14 vs 27, 24 vs 41, …) | Same two-tokenizer error in per-example form | `docs/about/samples-v04.md`, which reports every lane side by side |
| "52% average token reduction vs cl100k" with no metric type / tokenizer / N | Reads as "52% fewer than Python", which is false by a factor of ~2 | The long or short form above, verbatim |

**Provenance check on the 52% itself (2026-09-19, story 132.6).** The number could not be
traced to a primary artefact in any repo. The only published N = 42 dataset behind the
headline is `docs/reference/token-comparison.md`, and re-aggregating it gives **61.6%**
(sum-ratio) / **62.6%** (per-task mean) / 63.8% (median) for Toke-16K vs cl100k_base on the
same toke text — not 52%. That table *does* reproduce the two withdrawn cross-tokenizer
headlines exactly (Toke-16K-on-toke vs cl100k-on-Python = 31.1%; vs Go = 48.2%), which is
how those got published. So "52%" is a v0.3-era headline with no reproducible basis **and**
a superseded one. It may be described as a withdrawn past claim; it must not be restated as
a measurement.

The guard `scripts/check_metrics_claims.py` (`make check-metrics`, wired into `make ci`)
fails CI on any percentage stated next to "token" without a tokenizer name and an N, and on
any toke-trained tokenizer named alongside a non-toke baseline.

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
