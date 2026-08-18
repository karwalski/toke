# toke v0.4 Tokenizer — Program Plan

**Status:** Planned (2026-08-18)
**Owns:** story 116.9 (Workstream D); consolidates 81.2d
**Feeds:** Epic 128 (base-model decision), 118.4 (efficiency re-measure), 116.13-H4 (website token-viz), 81.4 (1B training)
**Supersedes:** `toke-tokenizer/docs/tokenizer-design.md` (v0.3-era; 32k vocab / 2.5–4x goal, both contradicted by measurement)

## 1. Why

Every shipped tokenizer is pre-v0.4:

- The SentencePiece 8k/32k models (Apr 2026) and the HF-format `tokenizer_v03.json` (May 2026) predate the `=`/`==` split, `!=`, and expression-`if`/`mt`. v0.4's `==` costs +1 token on all of them (116.3/A3a); new patterns fall to byte fallback exactly as `$`/`@(` did before the Epic 23 retrain (0/13 single-token, per `eval_syntax_tokens_11_4_6.json`).
- The v0.3 training text (`toke-corpus/data/tokenizer_training.txt`, 43,658 programs) is 100% v0.3: every line uses `=` for equality, 62% contain uppercase identifiers now rejected by tkc 2.8.0, and 19% is FUZZ material excluded from LM training but never from tokenizer training.
- The eval harness has known bugs (§6 Phase 0) — including a compression gate that cannot fail — and `tokenizer_v03.json` has **no committed training script** (provenance failure this plan must not repeat).
- The custom tokenizer was never wired into any training run: all 7B fine-tunes used Qwen's own tokenizer; `train_1b.py::load_tokenizer()` is `NotImplementedError`.

Meanwhile the input is nearly ready: `toke-corpus/corpus/regen_v04/` holds 23,382 accepted v0.4 records (98.1% shard acceptance, `syntax_version: v0.4-2.8.0`), with an audit/repair pass in flight.

## 2. Goal and non-goals

**Goal.** A locked, versioned, reproducible v0.4 BPE tokenizer that minimizes **tokens per program** on `tkc --min` canonical source (not characters — common multi-char patterns must merge to single tokens), measured honestly under TEMSpec, and consumable by all three downstream targets: 1B from-scratch training (HF `tokenizer.json`), the `toke_tokenizer` pip package, and the website token visualizations.

**Non-goals.**
- The two-model code+string split (stays 116.9/D4 research; nothing here precludes it — string masking already isolates strings).
- The fine-tune-vs-from-scratch decision (Epic 128). This plan only supplies inputs: a correct Qwen vocab-overlap analysis and the locked tokenizer.

## 3. Design decisions

### D1 — HuggingFace `tokenizers`, byte-level BPE, trained natively
Not SentencePiece + convert. Every consumer reads HF JSON (`train_1b.py` expects `tokenizer.json`; the pip package's pure-Python runtime parses HF format; the website viz bakes in HF token IDs), and SP→HF conversion is lossy around `▁`/user-defined-symbol semantics. Configuration: ByteLevel pre-tokenizer with `add_prefix_space=False` and **no GPT-2 regex split** — the default regex forbids cross-category merges like `):i64{`, which are precisely the fragments a deterministic grammar makes safe and profitable. The byte-level base vocab replaces SP `byte_fallback` for arbitrary inference-time strings. The committed training script emits the shipped JSON directly.

### D2 — Strings: mask literal contents to `"_"` in training data
Standardize on `"_"` (the 116.9 convention, matching `tokenizer_v03.json`); retire the competing `"<STR>"` placeholder. Masking is escape-aware and must preserve `\(...)` interpolation interiors — those are code. Real strings at inference degrade to ~1 byte/token for uncommon content; that is honest and acceptable — report unmasked tokens/char as an informational metric so the headline number isn't accused of masking inflation. No vocabulary budget for common string literals: the corpus's string distribution is a prompt-generation artifact, not real usage.

### D3 — Vocab size: sweep {2k, 4k, 8k, 12k, 16k}; select empirically
Expected winner: 8k. Evidence: v0.3's 32k bought +0.6pp compression over 8k at 23.5% utilization, and the v0.4 corpus is ~55% of the v0.3 volume (8.04 MB chars pre-`--min`). Selection = smallest vocab where:
1. the next size up gains **<1% absolute reduction** on holdout;
2. holdout **vocab utilization ≥ ~60%**;
3. all syntax gates (§3 D4, §6 Phase 3) pass;
tiebreak: rarest retained merge has **≥ ~100 training occurrences**. Purge the stale 32k docs and artifacts (116.9's "confirm 16K" is superseded by this sweep — 16k is a candidate, not the default).

### D4 — Forced tokens: small closed-class syntax list; everything else data-driven
Forced (as `AddedToken`s or seeded merges): type sigils `$i64 $f64 $str $bool $u64 $byte`; collection open `@(`; declaration heads `m= f= t= i=`; operators `== != && || <= >=`; interpolation open `\(`; `lp(`; `mt`. Rationale: BPE demonstrably left rare-in-corpus operators (`&&`) as two byte-fallback tokens; forcing closed-class syntax is cheap insurance.

Not forced: `.get(`, stdlib identifiers, whole import statements (`i=io:std.io;`) — open-class surface that evolves; the gate suite verifies they merge naturally instead. Fragment merges like `):i64{` are a **feature**: over a deterministic canonical form the grammar guarantees they generalize; the anti-overfit guard is the task-family holdout, not merge suppression. N-gram mining is used as *verification* (assert every code n-gram above a frequency threshold merged), never as forced input.

Known wart: `AddedToken` matches anywhere, so `xi=5` → `x`+`i=`+`5` (lossless, but a compression wart, and `=`-binding is more common in v0.4). Phase 0 measures the frequency of identifiers ending in {m,f,i,t} before `=`; if material, the four declaration heads move from AddedTokens to seeded merges.

### D5 — Training corpus: regen_v04 + library programs, frozen first
Include:
- `corpus/regen_v04/` accepted records (`judge.accepted` + `compiler_exit_code==0`), **including** the 877 `test_fail` records (they compile; a tokenizer cares about surface statistics), **excluding** the 158 `build_fail` records (may not survive `--min`);
- the 1,583 idiom-pass library programs and the 1,460 dual-verified Epic-126 library programs (dedup overlap after `--min`) — they carry library-usage idioms the regen corpus underrepresents;
- a lightly-repeated synthetic doc of the stdlib **public surface** (module names, function names/signatures) — just enough repetitions that each identifier clears the merge-support threshold; not full stdlib internals at heavy weight.

No category reweighting — the natural A/D mix stands; per-category reduction is reported at eval instead. Entry gate: the in-flight `corpus/regen_v04/audit/replaced/` repair pass lands first; then freeze a **corpus manifest** (record SHAs, tkc version, counts). Training reproduces byte-for-byte from the manifest.

### D6 — Canonical form: `tkc --min` output
One program per line, single-line by construction (28.2% fewer tokens than readable source; 116.7/B2 designates `--min` as "the training target + tokenizer input"). This replaces both `scripts/canonicalise_tk_source.py` and `curate_tokenizer_set.py`'s `\n`→`\`+`n` escaping — the latter taught the v0.3 tokenizer a two-character artifact of the file format that collides with genuine `\n` escapes inside strings. The vocab is thereby **coupled to tkc's minifier**: pin the tkc version in the manifest and keep a canary test that re-`--min`s a sample and diffs, so a future tkc change cannot silently degrade the locked vocab.

### D7 — Evaluation: TEMSpec-governed, baseline-first
- **Holdout** split by **base task family** (not record — the 22,914 unique sources contain near-variants; record-level splits leak structure), stratified by category × difficulty. Zero overlap with `data/holdout_task_ids.txt` benchmark IDs (verified clean 2026-08-18; re-verify at freeze).
- **Gate target set from measurement, not aspiration.** v0.3's 12.5% same-tokenizer reduction is not a valid prior: `--min` also helps cl100k_base (indentation was a large share of the old advantage). Phase 0 runs v0.3-toke, cl100k_base, qwen2.5, and llama3 tokenizers over an identical `--min`+masked v0.4 sample; the Phase 3 gate is that measured baseline minus a margin. Both sides of every reduction metric see identical normalized text.
- TEMSpec rules stand: same-tokenizer reduction is the only pass/fail metric; cross-language density is informational (§6.2). TEMSpec gets a version bump in Phase 3 (define `--min`+mask normalization; qwen/llama required at Gate 2+; tokenizer ID updated to the sweep winner).

## 4. Phases

### Phase 0 — Harness repair & measured baseline (toke-tokenizer)
1. Fix metric bugs: `char_to_token_ratio` returns tokens/char but is gated `<= 1.8` as chars/token (gate can never fail); `tokens_per_line` actually measures tokens/program; `eval.py` "fertility" is tokens/char, inverted from convention — align names/orientations with TEMSpec §2.
2. Fix `scripts/tokenizer_alignment.py`: hard-fail when `transformers` is absent (the recorded 9.8.2 verdict `vocab_extension_prototype` came from `qwen_vocab_size: 0`, `partial_analysis: true`); normalize `▁`/`Ġ` piece representations before set comparison. Re-run properly → deliverable for Epic 128.
3. Update `scripts/eval_syntax_tokens.py`: v0.4 `KEY_PATTERNS` (`==`, `!=`, expr-`if`, `mt`, `\(`, current sigils), add a CLI (currently hardcoded paths, no args). Run against the shipped G2/G3 models to quantify the v0.4 gap — the "before" number.
4. HF-format spike: train a toy byte-level tokenizer; verify all three consumers load it (train_1b loader path, pip package runtime, a viz decode check — byte-level tokens need a decode mapping or base-256 range renders as garbage).
5. Measure the D4 AddedToken substring wart frequency; decide AddedTokens vs seeded merges for `m= f= t= i=`.
6. Baseline run per D7 → sets the Phase 3 gate number.

**Exit:** metric bugs fixed with tests; v0.4-gap and baseline numbers recorded; spike passes on all three consumers.

### Phase 1 — Corpus curation & freeze (toke-corpus)
**Entry:** `audit/replaced/` repair pass landed.
1. Assemble the set per D5; `tkc --min` everything (build_fail excluded up front; any record failing `--min` is dropped and logged); mask strings per D2; SHA-256 exact dedup post-min.
2. Carve the holdout per D7; document it.
3. Rewrite `scripts/curate_tokenizer_set.py`: source `regen_v04` + library sets; v0.4 charset validation (no uppercase/`_`/`,` outside string literals); drop the MUT-/DOC-dir logic and the `\n` escaping.
4. Regenerate `data/tokenizer_training.txt` + `docs/tokenizer_data_report.md` (both currently v0.3).
5. Freeze the corpus manifest (SHAs, tkc version, counts, holdout spec).

**Exit:** frozen manifest; data report shows v0.4-clean charset; holdout documented and benchmark-disjoint.

### Phase 2 — Training & vocab sweep (toke-tokenizer)
1. New `train_v04.py` (HF `tokenizers`): D1 config, D4 forced tokens, trains all sweep sizes from the frozen manifest.
2. Sweep report: reduction / utilization / fertility / merge-support curves per size; select winner per D3 with recorded evidence.
3. N-gram verification pass (D4); 100% roundtrip fidelity (`decode(encode(x)) == x`) on holdout — a new obligation under byte-level.

**Exit:** winner selected with evidence; committed script reproduces the artifact byte-for-byte from the manifest.

### Phase 3 — Evaluation, gates, TEMSpec bump, lock (toke-tokenizer, toke-spec)
1. Full TEMSpec eval on holdout: same-tokenizer reduction vs cl100k_base + qwen2.5 + llama3 (o200k informational), bootstrap CIs, per-category stratification, gate card with artifact SHA-256 and pinned library versions.
2. Acceptance gates: all v0.4 syntax patterns single-token; **zero byte-fallback tokens on masked holdout code**; top-N stdlib identifiers ≤2 tokens; 100% roundtrip; reduction ≥ the Phase 0 baseline-derived target.
3. TEMSpec v1.1: `--min`+mask normalization defined; qwen/llama baselines required; tokenizer ID updated (keep `toke-bpe-8k` only if 8k wins).
4. Freeze canonical-program regression budgets (e.g. fibonacci ≤ N tokens) **at lock time** as regression tests — not pre-lock aspirations.

**Exit:** gate card published; vocab locked and versioned (`toke-bpe-v04-<size>`, SHA recorded).

### Phase 4 — Integration & handoffs (toke-model, toke-tokenizer, toke)
1. Implement `train_1b.py::load_tokenizer()`; create `toke-model/tokenizer/vocab/` with the locked `tokenizer.json` (the path `train/config.py` already expects).
2. Ship `toke_tokenizer` 0.2.0: new JSON; resolve the dual-pyproject license conflict (root Apache-2.0 wins over the package's MIT); real tests (package test dir is currently empty); fix the README's nonexistent CLI flags and the unsupported "52% reduction" claim.
3. Handoffs (inputs, not blockers): Qwen alignment result → Epic 128; locked tokenizer → 118.4 (efficiency re-measure) and 116.13-H4 (regenerate website token-viz — never hand-edit the baked v0.3 token IDs).
4. Hygiene: tkc-version canary test in CI; retire `models/32k/`, `.old` models, stale README/design-doc sections.

**Exit:** 1B loader loads the artifact; package published; handoff notes filed on 128 / 118.4 / H4. Phase 4 does **not** block on Epic 128's decision.

## 5. Risks

| Risk | Mitigation |
|---|---|
| `--min` drift: vocab coupled to tkc's canonical form | Pin tkc in manifest; canary test re-`--min`s a sample and diffs |
| AddedToken substring wart (`xi=` → `x`+`i=`) | Measured in Phase 0; seeded merges fallback for decl heads |
| Train/deploy skew: model trains on masked strings, serves real ones | Flagged to Epic 128; informational unmasked metric tracked |
| Corpus volume (8.04 MB pre-min) too small for large vocabs | The sweep decides, with utilization + merge-support floors; expectation is 8k |
| Provenance: shipped artifact without a committed generator (the tokenizer_v03.json failure) | Training script emits the shipped JSON directly; byte-for-byte reproduction from the frozen manifest is a Phase 2 exit criterion |
| `--min`+mask helps baselines too; headline shrinks vs old claims | By design — Phase 0 measures it; targets are set from that measurement; TEMSpec forbids aspirational public numbers |

## 6. Story mapping

- **116.9 (Workstream D)** — owning story; this doc is its plan. Sequencing unchanged: cannot precede 116.8/C (corpus freeze = Phase 1 entry gate here).
- **81.2d** — superseded, consolidated into 116.9 (row updated 2026-08-18). Its dependents (81.4 1B training) now trace through 116.9 → this plan's Phase 4.
- Prior art absorbed: Epic 23 (forced-merge mechanism, stdlib regression tests), Epic 9.8 (alignment analysis — rerun with fixes), stories 11.4.6/11.6.2 (syntax-token gate harness), 116.7/B2 (`--min` canonical form), TEMSpec 10.1.1 (metric authority).
