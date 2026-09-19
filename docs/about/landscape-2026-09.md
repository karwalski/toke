---
title: The Landscape and the Evidence Against Us
slug: landscape-2026-09
section: about
order: 11
---

# The landscape and the evidence against us (September 2026)

## 1. Why this page exists

toke is a research bet, and the honest way to run a research bet is to publish the evidence against it alongside the evidence for it. This page summarises a September 2026 review of everything else working on the same problem — the full review is archived beside it at [`reviews/landscape-2026-09-18.md`](reviews/landscape-2026-09-18.md) — and states, lane by lane, where toke stacks with the rest of the field, where it is simply substituted by something cheaper, and where it is threatened at the root.

Companion page: `positioning-2026-09.md` states what we claim after reading this. `competitive-matrix.md` is the per-project comparison; `metrics-baseline.md` is the only source for our own numbers.

---

## 2. The eight lanes

Everything attacking the cost of LLM code generation falls into eight lanes. For each: what it is, the strongest published result in it, and our verdict — **stacks** (adds to what toke does), **substitutes** (delivers the same benefit without toke), or **threatens** (invalidates a premise toke rests on).

### Lane 1 — Language level

Purpose-built or compacted languages that an LLM emits instead of Python: toke, KERN, Anka, memelang, sui-lang/Isu, Pel, Sigil, SimPy.

**Strongest result:** Anka (Saif Al Mazrouei, arXiv 2512.23214, December 2025) — a *deliberately verbose* DSL with one canonical form. Claude 3.5 Haiku reached 99.9% parse success and 95.8% overall task accuracy with **zero prior training exposure**, and beat Python by 40 percentage points on multi-step pipeline tasks (100% vs 60%); GPT-4o-mini cross-validation confirmed +26.7 points on multi-step tasks.

**Verdict: mixed — this is our lane, and the strongest result in it cuts both ways.** It *supports* the feasibility of a new language (a model can learn one from an in-context spec, so the bootstrap objection is weaker than it looks) and *threatens* the terseness half of our design (Anka won by being verbose and canonical, not short). The lane is also where our direct substitutes live: KERN gets much of the density with none of the cold-start cost by compressing Python's surface syntax and keeping Python's semantics.

### Lane 2 — Tokenizer level

Changing how text becomes model input: superword BPE, and tokenizer-free architectures.

**Strongest results:** SuperBPE (Liu et al., arXiv 2503.13423, ICML 2025) — at a fixed 200k vocabulary, up to **33% fewer tokens** than BPE (6.63 vs 4.45 bytes/token), with **+4.0% absolute average accuracy** across 30 downstream tasks (+8.2% MMLU) and 27% less inference compute at 8B. Separately, H-Net dynamic chunking (Hwang, Wang and Gu, arXiv 2507.07955, July 2025) and the Byte Latent Transformer (Pagnoni et al., Meta FAIR, ACL 2025) learn chunking from raw bytes, match or beat token-based models at 8B scale, and are reported strongest exactly where tokenisation is weakest — including code.

**Verdict: substituted, and threatened.** Superword BPE **substitutes** for our purpose-built tokenizer work: it delivers the bulk of that gain generically, inside ordinary model training, with an accuracy gain rather than an accuracy cost, and it needs no new language. Tokenizer-free models **threaten** the unit itself: if the model consumes bytes or learned chunks, "tokens per program" stops being a stable figure of merit and a terse ASCII syntax loses its tokenizer arbitrage. This is the single largest correction the review forced on us.

### Lane 3 — Output-format level

Making the model emit only the change: search/replace and unified-diff edit formats, `apply_patch`, `str_replace`, fast-apply.

**Strongest result:** Aider's unified-diff format (Paul Gauthier, aider.chat benchmark) raised GPT-4 Turbo from a 20% baseline to **61%**, and June GPT-4 from 26% to 59%, versus whole-file rewrites, while cutting latency and cost. Diff-XYZ (arXiv 2510.12487) isolates which representation is best for which operation on a 1,000-item benchmark.

**Verdict: stacks.** Edit formats are orthogonal to language choice — a diff of toke is still a diff — and capture a large share of the same output-token saving on any language. We should adopt the format, not compete with it. (One caveat worth stating: OpenAI's Predicted Outputs bills rejected tokens as normal completion tokens, so speculative edits buy latency, not token cost.)

### Lane 4 — Context level

Attacking input tokens: LLMLingua-family prompt compression, code-specific context compression, agent compaction, and prompt caching.

**Strongest result:** prompt caching economics. Cached input reads price at roughly **0.1x** the uncached rate across the major vendors. Meanwhile average prompt length is up nearly fourfold since early 2024 and completion tokens roughly threefold (OpenRouter 100-trillion-token study, arXiv 2601.10088), with agentic workflows now more than half of output tokens. LongLLMLingua reports 4x fewer tokens with a +17.1% performance gain in long-context QA.

**Verdict: stacks technically, but shrinks the slice we can reach.** Caching is orthogonal to language — it applies equally to toke — yet it prices the input-code slice, the one our arithmetic leans on hardest, at about a tenth. Anything we claim about token spend has to be priced cache-aware or it is wrong.

### Lane 5 — Inference level

Efficient reasoning, reasoning budgets, multi-token prediction, speculative decoding, diffusion code models.

**Strongest result:** TokenSkip (arXiv 2502.12067, EMNLP 2025) cut reasoning tokens by **40%** on Qwen2.5-14B-Instruct (313 to 181 on GSM8K) with under a 0.4% performance drop; S-GRPO reports 35–61% output-length reductions. Multi-token prediction in DeepSeek-V3 reports 80–90% acceptance for the second token and about 1.8x throughput.

**Verdict: attacks a larger slice than syntax can reach.** Reasoning tokens are the fastest-growing part of output and a language cannot touch them; MTP and speculative decoding cut cost *per token* across every token, including ours. These stack with toke arithmetically, but they **substitute** for toke on the cost axis: if the goal is a cheaper session, they deliver more, sooner, with no adoption cost.

### Lane 6 — Verification level

Making invalid output impossible: grammar-constrained decoding, type-constrained generation, compiler and execution feedback.

**Strongest results:** XGrammar (Dong et al., arXiv 2411.15100) — grammar-constrained decoding with up to **100x speedup** and near-zero per-token overhead, now shipped inside vLLM, SGLang, TensorRT-LLM and MLC-LLM. Type-constrained code generation (Mündler et al., PLDI 2025, arXiv 2504.09246) cuts compile errors and hallucinated methods on an existing typed language.

**Verdict: stacks — and substitutes for the argument, not the artefact.** A small backtrack-free grammar makes the decoding mask cheaper, so constrained decoding is the cheapest thing we can borrow and it works better on toke than on a large grammar. But it also reproduces "syntactically valid by construction" on *any* language without a new language, which removes the compile-guarantee argument for adopting one. Our own Gate 2 is the evidence for why that matters less than it sounds: 100% compile Pass@1 with 55.6% functional correctness (272/489; curated 500 hidden + 200 eval set, Qwen 2.5 Coder 7B + QLoRA, v0.3 syntax — see `metrics-baseline.md`). A compile guarantee removes one error class and leaves algorithmic correctness untouched.

### Lane 7 — Economics and energy

What the savings are worth: token volume, coding's share of it, energy per prompt, and cost per *solved* task.

**Strongest results:** Google's median text prompt measured at **0.24 Wh**, 0.03 gCO2e and 0.26 mL water, with a 33x per-prompt energy and 44x carbon improvement in twelve months (arXiv 2508.15734). Cost-of-Pass (arXiv 2504.13359) formalises cost per solved task, pricing failed attempts rather than tokens. Vendor disclosures put Google's monthly token throughput above 3.2 quadrillion (I/O 2026, roughly 7x year on year) — unaudited, and mixing modalities.

**Verdict: threatens the figure of merit.** Per-prompt efficiency is improving by more than an order of magnitude a year from the inference side while volume grows about 7x, so a syntax-level saving is second-order against both trends. The correct unit is cost per solved task, not tokens per program — which is why we are building our own harness for it (131.55) rather than quoting token counts.

### Lane 8 — Contradicting evidence

Results that argue directly against a purpose-built language.

**Strongest result:** the low-resource-programming-language penalty. Pass@1 for R, Racket, Perl, Swift and Go sits at or below **30%** against 50–75% for Python, JavaScript and Java (Giagnorio et al., January 2025; MultiPL-E and MultiPL-T, arXiv 2308.09895). A brand-new language starts as the lowest-resource language in existence: no pretraining corpus, no RL environments, no Stack Overflow.

**Verdict: threatens.** Anka's in-context result partially rebuts this for *syntax* (a model can parse a novel DSL from a spec) but not for *reasoning* — rare DSLs remain harder to reason in than a mainstream language plus a domain description. See section 4.

---

## 3. Adjacent projects

| Project | What it is | Maturity | Evidence quality | Relationship to toke |
|---|---|---|---|---|
| **KERN / KERN-py** (Oscar Martinez, `OscarCode9/kern`, page dated 2026-08-01) | A compact, reversible surface syntax for Python. Python → Kern → Python round-trips deterministically; an optional compact profile alpha-renames locals. Semantics, runtime and tests are Python's. | Single-author research repo, grammar v0.4, ~39 commits. No PyPI package, no releases, **no licence file** — pin by commit; we cannot vendor it. | **Strong, and we reproduced it.** Pinned commits, pinned wheel SHAs, full denominators, published per-task CSVs. Our own re-run (story 133.1) reproduces their toke numbers to the token. | The first third party to benchmark against toke, and the closest substitute: no cold-start problem, since every Python program is Kern training data. Their cl100k numbers stand. Two qualifications, both ours to fix: the 60 toke programs they measured are April-2026 Gate-1-era output in a syntax three revisions old (29/60 accepted by tkc 2.8.0 — compiler drift on a stale public artefact, which their report says explicitly), and **"Toke-16K" is our own published v0.3 tokenizer** (`toke-tokenizer==0.1.0`), scored on text of a syntax it was not trained on. On migrated `--min` text the equal-vocab lane is 2,788 vs 2,872 (N = 60; ratio 0.971, bootstrap 95% CI [0.759, 1.155] — includes parity), not the 28.6% gap the legacy text produced. Their 25,953-program training set is **a deliberate count-match on CodeSearchNet Python functions**, stated in their own manifest as "matching Toke's published program count" — no toke data is involved. Full review: [`reviews/kern-2026-08.md`](reviews/kern-2026-08.md). |
| **Anka** (Saif Al Mazrouei, arXiv 2512.23214) | A deliberately verbose DSL for reliable data-transformation code generation, with a 100-task benchmark. | Preprint with released implementation. | Author's own suite, but rigorous and reported in full. | The sharpest counter-evidence to our terseness instinct, and the strongest support for our adoption case: 99.9% parse success and 95.8% task accuracy with zero prior exposure, +40 points over Python on multi-step pipelines. We test the conflict directly in 131.54. |
| **memelang** (Bri Holt, arXiv 2512.17967) | An "axial grammar" query DSL an LLM emits, compiling to parameterised PostgreSQL. | Preprint plus a working reference implementation. | Example-level (a 22-token query against 50 tokens of SQL), not a benchmark suite. | Complementary. Confirms the thesis at DSL scale, where the target language is verbose and the win is concrete; says nothing about general-purpose code. |
| **sui-lang / Isu** (Takato Honda, GitHub, ~October 2025) | LLM-optimised line-based language transpiling to Python/WASM, since pivoting to "Isu" pseudocode parsed into a closed-vocabulary IR. | Open-source prototype (MIT). | Weak: aspirational "100% accuracy" claims, no independent benchmark. The review could not confirm the primary repository within its search budget. | The nearest competitor *concept* to toke, and unproven — as we are. Listed for completeness, not as evidence either way. |
| **Pel** (Behnam Mohammadi, arXiv 2505.13453) | A homoiconic language for orchestrating AI agents, with a minimal grammar suited to constrained generation and syntax-level capability control. | Design/position paper. | No token benchmarks. | Complements the half of our thesis that survives lane 2: a small grammar aids constrained decoding. Orthogonal to token counting. |
| **Mirror** (Austin Z. Henley, CMU, late 2024) | Programming-by-example: give a signature plus input/output examples, an LLM generates JavaScript. | Proof of concept (blog and playground). | None quantitative. | Orthogonal. Not a token-reduction language; included because it is routinely grouped with them. |

The wider comparator set that KERN benchmarks (Sigil, KARN, NERD, Vyxal, SimPy, Token Sugar) is transcribed in `~/tk/toke-spec/docs/prior-art.md` §6.9 as *they* report it. We have not independently verified those rows.

---

## 4. What the evidence says against us

Four findings argue against toke's original thesis. We state them in our own words, without softening.

**1. The terse-language advantage does not survive realistic tasks.** danluu's 2026 evaluation ran agents on non-trivial work (a zstd decoder from spec; Pandoc ProgramBench). At medium reasoning effort the dynamic-language token advantage appears; at ultra effort it disappears, with static languages among the best. Obscure and dense languages (J, Assembly) do poorly. Language *popularity* correlates weakly-to-moderately with both higher correctness and lower cost. This is the best independent, task-level evidence in the lane, and it points the opposite way from the RosettaCode-style token rankings that make terse languages look good — those count existing snippets, not end-to-end task cost, and their own author calls them "not a scientific study". danluu also observed agents repeating an identical compiler error before fixing it, which is a direct hit on "faster compiler feedback means fewer iterations".

**2. A new language starts as the lowest-resource language in existence.** Pass@1 for genuinely low-resource languages sits at or below 30% against 50–75% for Python, JavaScript and Java. toke has no pretraining presence, no RL environments and no community corpus. Our own honest floor is consistent with that penalty rather than with our headline: a full local re-audit of all 1,748 v0.3.9 corpus programs gives **37.5% compile (655/1,748) and ~2.2% fully correct (38 PASS)** — not the curated-set 100%/55.6%.

**3. Verbosity beat terseness on the one controlled comparison that exists.** Anka is *deliberately verbose* with one canonical form, and it reached 99.9% parse success and 95.8% task accuracy with zero prior exposure, beating Python by 40 points on multi-step pipelines. toke bets on terse *and* one-canonical-form, and has never separated the two. It is entirely possible that the canonical-form half is doing the work and the terseness half is costing us accuracy.

**4. The arithmetic says syntax savings are second-order.** Take a representative agentic task: 100,000 input tokens, 10,000 output, output half code and half reasoning plus tool calls, input 40% code. A syntax-level saving reaches only the code slices. An aggressive 40% cut on those yields about 18,000 of 110,000 tokens, roughly 16% of raw tokens — and that already assumes the whole codebase in context is already toke. Re-price it cache-aware, with cached input reads at about 0.1x, and the input-code saving is worth about a tenth of its face value; the economically weighted saving lands on a much smaller effective base. On the same task, prompt caching cuts the dominant input slice by about 90%, reasoning-length control cuts a slice syntax cannot touch at all (TokenSkip: 40%), and MTP cuts cost per token across everything. This decomposition is an estimate built on assumed splits, not a measurement — nobody has published the real one, which is precisely why we are instrumenting it (131.56). But the direction is not in doubt: **syntax savings are real and second-order.**

We also have to publish this against ourselves: the "52% average token reduction vs cl100k" figure that appears in our own materials is a *tokenizer-lane* number — Toke-16K (the 16,384-vocab v0.3 BPE) on toke `--min` text versus cl100k on the same toke text, N = 42 benchmark programs. It is not a comparison against Python. On the same 60-pair set KERN measured, toke costs **about 1.76x Python's cl100k tokens** (N = 60; sum-ratio 1.757, bootstrap 95% CI [1.421, 2.345]). And on the 2026-09-18 v0.4 tokenizer baseline (N = 2,000 stratified records from the 2026-08-19 freeze, canonical `tkc --min`, string bodies masked), the shipped 8k SentencePiece tokenizer needs **15.4% more** tokens than cl100k, and the v0.3 16K tokenizer's apparent 45% advantage is not creditable because its null `unk_token` silently deletes every backslash (2,606 of them in that sample). No "purpose-built tokenizer beats cl100k" claim is supportable until 116.9 trains and locks a v0.4 tokenizer.

Toke's design is not free of cost, either. Here is the canonical form the terseness argument rests on — compact, one way to write it, parsed by a backtrack-free grammar, and checkable by the compiler before it is ever run:

```toke
m=main;
i=io:std.io;
f=sumpos(ns:@i64):i64{
  let t=mut.0;
  lp(let i=0;i<ns.len();i=i+1){
    let v=ns.get(i);
    if(v>0){t=t+v}
  };
  <t
};
f=main():i64{ io.print("\(sumpos(@(1;-2;3)))"); <0 };
```

Whether *that* — terse, canonical, grammar-constrained, compiler-checked — produces more solved tasks per dollar than Python plus constrained decoding is an open empirical question. It is the question the next section is about.

---

## 5. What we are doing about it

Each threat maps to a story that tests it. We are stating the tests, not the outcomes.

| Threat | Story | Test |
|---|---|---|
| Tokenizer-free models moot the token-count unit | **131.51** | Byte-level durability spike: measure toke vs Python in bytes and byte-level patches, and whether a 14-keyword backtrack-free grammar makes byte-level generation cheaper and more reliable than a large grammar. Outputs an ADR-grade finding that either supports or retires the "durable under tokenizer-free architectures" claim. |
| Constrained decoding reproduces the compile guarantee without a new language | **131.52** | Ship and benchmark v0.4 grammar artefacts (XGrammar / GBNF / Lark): mask-construction cost and per-token overhead for toke against a mainstream-language grammar, plus first-shot syntactic validity with and without constraints. Wire the artefacts into CI so grammar drift breaks the build. |
| Reasoning tokens aid correctness; our corpus is reasoning-light | **131.53** | A/B corpus records with and without a companion reasoning channel, scored on first-shot functional correctness at a fixed token budget. The language does not change; the record shape might. |
| Anka: verbosity may beat terseness | **131.54** | A/B a frontier model writing each of the 46 catalogue patterns in the canonical terse form, in the most verbose measured candidate, and free choice — correctness against tokens. If terseness costs accuracy, the efficiency protocol gains a correctness term. |
| Tokens per program is the wrong unit | **131.55** | Cost-per-solved-task harness using Cost-of-Pass accounting over the 128.1 held-out set: failed attempts and repair rounds priced, input priced cache-aware, four arms (toke + our model; toke + frontier + constrained decoding; Python + frontier; Python + frontier + type-constrained decoding). Methodology published so anyone can re-run it. |
| The arithmetic in section 4 rests on assumed splits | **131.56** | Instrument an equivalent toke session and Python session on the same tasks with the same agent, and publish the real decomposition: code, reasoning, tool schemas, logs, diffs; input vs output; cached vs uncached. |
| Constrained decoding plus an existing model may need no training at all | **128.10** | Lane A, the baseline every trained lane must beat: frontier model + syntax card + the 131.52 grammar artefacts on the 128.1 held-out set, scored on compile and functional Pass@1, tokens in every lane, and cost per solved task. |
| A compile guarantee does not buy functional correctness | **128.11** | Lane B: GRPO/RLVR execution-feedback training on compile × tests × length over the frozen corpus, measuring the **functional** delta, not the compile delta. |
| Superword BPE commoditises the tokenizer gain | **128.12** | Lane C: on one frozen corpus, compare our purpose-built BPE, a superword/SuperBPE-class tokenizer, the base model's own tokenizer, and the base tokenizer plus toke-specific added tokens — scored on tokens per program *and* on downstream correctness after an identical fine-tune. |
| If the BPE token stops being the unit | **128.13** | Lane D: run a small byte-level or dynamic-chunking model (H-Net/BLT-class) on toke and measure bytes per task and validity against a BPE baseline. Explicitly a research lane, not a product commitment. |

---

## 6. How to check our numbers

- **Every quantitative claim about toke** is sourced in [`metrics-baseline.md`](../metrics-baseline.md), which carries the metric, the value, the basis and the caveat for each row, plus the load-bearing caveats at the top (all trained-model numbers are from a v0.3-syntax model; the curated 100% is not the 37.5% honest floor; efficiency must be measured on the `--min` canonical form). Cite the row and its caveat, never a headline.
- **Tokenizer lane and N** are mandatory on every number we publish, per TEMSpec §6.3 (metric type, tokenizer(s), baseline, N). A figure without its lane is a misquotation of our own work.
- **The v0.4 tokenizer baseline** (N = 2,000) names its sample ids and SHAs in `toke-tokenizer/data/baseline_sample_ids_v04.txt`, drawn from the 2026-08-19 corpus freeze; the freeze manifests and the audit that froze it are in `toke-corpus/regen/`.
- **The KERN comparison reproduces from source.** [`reviews/kern-2026-08.repro.py`](reviews/kern-2026-08.repro.py) re-runs the whole thing — pinned toke-eval and KERN commits, pinned wheel SHAs, 10,000-resample bootstrap CIs, seed 0 — and writes `kern-2026-08.repro.json` with per-task rows. The archived source page and its sha256 are beside them.
- **The review behind this page** is archived unedited at [`reviews/landscape-2026-09-18.md`](reviews/landscape-2026-09-18.md), including its own caveats: several items in the language lane are blogs or single-developer repositories, every purpose-built-language token-reduction figure (ours included) is self-reported by its creator across different tokenizers with no neutral benchmark in existence, and some 2026 identifiers and model names come from forward-dated sources and are attributed to those sources rather than asserted as fact.

If you find an error in any of the above, it is a bug and we want it filed.
