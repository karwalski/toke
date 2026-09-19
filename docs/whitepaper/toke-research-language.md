# toke: a compiled language designed for LLM code generation

**A research whitepaper, and an accounting of what the evidence actually supports**

Matthew Watt (karwalski)
Whitepaper v2 — spec v0.4, compiler `tkc` toke 2.8.0
2026-09-19 (supersedes the 2026-05-24 revision; see §9, Corrections)

---

## Abstract

> toke is a compiled programming language designed for LLM code generation. It has 14
> keywords, a 55-character set, a backtrack-free grammar with bounded lookahead, and one
> canonical form per construct, chosen by measurement in a 46-pattern catalogue and
> reproduced by `tkc --min`. That makes generated code cheap to constrain during decoding,
> cheap for a compiler to verify afterwards, and compact to emit. Token efficiency is one
> measured property of toke, always reported with its tokenizer and its baseline, not the
> whole claim.

The first version of this paper argued a token-count thesis: that a purpose-built language
plus a purpose-built tokenizer would cut the token cost of AI-generated code far enough to
matter. A year of external evidence and our own re-measurement have moved the load-bearing
argument off the tokenizer and onto the grammar, the canonical form and the compiler. This
version states that argument, states the measured numbers with the lane each was measured
in, states the four strongest arguments against the project in our own words, and lists the
tests that would retire the thesis.

The headline corrections are in §9 and are not hedged: the grammar is **not** LL(1); there
are **14** keywords, not 13; the "52% fewer tokens" figure is **withdrawn**, not requalified;
under a shared tokenizer toke currently costs **more** tokens than Python, not fewer; and
there was **no August 2026 gate result** — August produced a corpus-quality freeze, so the
most recent model gate remains Gate 2 of 2026-05-22.

This paper, like significant parts of the toke implementation, was produced through
iterative human-AI collaboration. We disclose that up front: AI was used throughout for
design iteration, corpus generation, implementation, document drafting and multi-model
review. The human provides direction, criteria and judgement.

**Every number in this paper is sourced from [`docs/metrics-baseline.md`](../metrics-baseline.md)**,
which carries the metric, the basis, the caveat and the origin story for each. A number
without its lane — which tokenizer measured which text against which baseline, at what N —
is a misquotation of our own work.

---

## 1. The claim

### 1.1 What toke is

The claim, in one sentence:

> toke is a compiled programming language designed for LLM code generation: small,
> strictly structured, and canonical, so that generated code is cheap to constrain while
> it is being produced, cheap for a compiler to verify once it is, and compact in whatever
> unit the model generates in.

It names the audience (LLM code generation), the three mechanical properties (small,
structured, canonical), and the three things they buy (constrain, verify, compact). It does
not depend on the BPE token being the unit of account, which is the property the first
version of this paper lacked. The authoritative wording is
[`docs/about/positioning-2026-09.md`](../about/positioning-2026-09.md); this paper quotes it
rather than restating it.

### 1.2 What changed, and why

Three things forced the rewrite.

1. **Our own re-measurement.** Re-running the token comparison on canonical v0.4 text
   showed that no shipped toke tokenizer beats a general-purpose one, and that under one
   shared tokenizer toke costs more tokens than Python, not fewer (§3). The old headline
   could not be requalified; it had to be withdrawn.
2. **The landscape.** Superword tokenizers now deliver generically much of what a
   purpose-built tokenizer delivered, and tokenizer-free architectures threaten the token
   as a unit at all (§4).
3. **The spec.** v0.4 (2026-07-02) retired the strict-LL(1) claim as inaccurate and
   corrected the keyword count. Both had already propagated into third-party descriptions
   of toke because we published them (§9).

What did **not** change is the language: not the character set, not the keywords, not the
grammar, not the semantics, not the canonical `--min` form, and not the "designed for LLMs"
framing. The repositioning is a change to what we claim and how we measure it, not a
redesign.

### 1.3 The adoption hypothesis: agents, not humans

We state the likely adoption path plainly: **humans never write toke directly.** An agent
emits toke when the program is going to be compiled and checked anyway, and the human
interface stays natural language or the human's preferred language. toke does not need to
be ergonomic for people. It needs to be learnable by models, cheap to constrain, cheap to
verify, and competitive on cost per solved task. Every claim in this paper should be read
against that bar, and §7 states what would fail it.

---

## 2. The argument: grammar, canonical form, compiler

Each property below is a fact about the language paired with the mechanism it buys a model.
The mechanisms survive a change of generation unit; the numbers attached to any one
tokenizer do not.

### 2.1 Small: 14 keywords, a 55-character set

`docs/spec/toke-spec-v0.4.md` §A fixes the keyword set at **14** — `m i t f let if el lp br
rt as mt sc mut` — verified against the lexer keyword table. The default syntax uses a
**55-character** alphabet: 26 lowercase letters, 10 digits and 19 symbols. No uppercase, no
underscores, no comment syntax (documentation lives in companion `.tkc` files), and
whitespace is not syntax.

*Mechanically:* a small terminal alphabet is a small vocabulary for any generation unit. At
the byte level it means the model chooses among a few dozen live bytes at most positions
rather than 256 — the narrowest hypothesis space we can offer a tokenizer-free model.
Whether that narrowness converts into measurably better byte-level generation is
**unproven**, and is exactly what story 131.51 exists to test (§7, F1).

### 2.2 Structured: backtrack-free, bounded lookahead

`docs/spec/toke-spec-v0.4.md` §E states the verified property:

> The toke grammar is **backtrack-free**: the parser never rescans input it has already
> consumed. It is **not** pure LL(1); a small, **enumerated** set of productions require
> **bounded lookahead of up to 3 tokens** (never more). An implementation that backtracks,
> or that requires unbounded lookahead at any production, is non-conforming.

The exceptions are enumerated normatively in Appendix A of
[`grammar.ebnf`](../spec/grammar.ebnf) with the FIRST-sets;
[`toke.gbnf`](../spec/toke.gbnf) is the GBNF form for constrained decoding. The v0.4
`=`/`==` split removed the worst former offender (the loop-init unbounded forward scan).

*Mechanically:* grammar-constrained decoding builds a token mask at every step from a
pushdown automaton over the grammar. A small, backtrack-free grammar makes that automaton
small and its masks cheap. XGrammar (arXiv 2411.15100, now in vLLM, SGLang, TensorRT-LLM
and MLC-LLM) reports up to 100x speedup with near-zero per-token overhead on this
construction; type-constrained code generation (Mündler et al., PLDI 2025, arXiv 2504.09246)
extends it to type-level constraints and cuts compile errors and hallucinated methods.

**The same evidence cuts against us, and we say so first.** XGrammar works on *any*
grammar, including Python's. The advantage of a purpose-built grammar is one of degree — a
cheaper mask, a smaller invalid space — not of kind. Measuring that degree is story 131.52:
mask-construction cost and per-token overhead for toke against a mainstream-language
grammar. Until it reports, "cheaper to constrain" is a mechanical argument, not a
measurement.

### 2.3 Canonical: one measured form per construct

`docs/spec/idiom-v0.4.md` states the idiom rules; `docs/spec/patterns-protocol-v0.4.md`
turns them into measured verdicts; `patterns/catalogue.json` holds **46 entries across 10
families**. A form becomes canonical only by being best-or-tied on tokens *and* runtime, and
all candidate forms of a pattern must print byte-identical output. `tkc --min` reproduces
the canonical text, and it is the basis on which every efficiency number in this paper is
measured — measuring readable source instead understated toke by **28.2%** (116/B2).

Here is a canonical program. It is terse, there is one way to write it, the compiler
accepts or rejects it before it runs, and `tkc --min` reproduces it byte for byte:

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

*Mechanically:* one canonical form collapses the set of correct-but-different programs a
model must choose among, and it makes evaluation exact rather than fuzzy. Two
implementations either produce identical canonical text or they do not — which is what makes
a diff format well defined and a reward signal unambiguous.

### 2.4 Compiler-verified: structured diagnostics as a reward signal

`tkc` emits diagnostics with stable codes, machine-parseable spans and a `fix` field. That
is what a repair loop consumes, and what an RLVR reward function can score for the cost of
one compiler invocation.

*Mechanically:* it is a verifiable signal, and it is demonstrably **insufficient on its
own**. Gate 2 reached **100% compile Pass@1** with **55.6% functional correctness (272/489)**
on the same curated set (§5). Compile-checking removes one error class and leaves algorithmic
correctness untouched. Execution feedback is what moves the rest, which is why the training
lanes are scored on functional deltas rather than compile deltas.

### 2.5 Learnable without pretraining

The standing objection to a new language is that it starts as the lowest-resource language
in existence (§4.4). The strongest evidence against that objection is Anka
(arXiv 2512.23214): a novel DSL with **zero prior training exposure** on which Claude 3.5
Haiku reached 99.9% parse success and 95.8% overall task accuracy, with GPT-4o-mini
cross-validation. A model can learn a new language from an in-context spec. Our own
regeneration waves show the same effect — agent workers writing accepted v0.4 from the
syntax card alone — but **that rate is not yet recorded in `docs/metrics-baseline.md`** and
is therefore not quoted here as a number.

---

## 3. Token efficiency: one measured property, with its lane

Token efficiency is a property of toke, not the thesis. It is never stated without naming
the tokenizer, the text it was applied to, the baseline, and N (TEMSpec §6.3).

### 3.1 What is measured today

**Tokenizer lane — same text, two tokenizers** (canonical `tkc --min` v0.4 text, string
bodies masked, N = 2,000 stratified records from the 2026-08-19 freeze; 131.20):

| tokenizer | tokens/program | vs cl100k_base |
|---|---:|---:|
| cl100k_base | 121.2 [118.9, 123.6] | 1.000 |
| o200k_base | 122.6 | 1.012 |
| Qwen2.5-Coder | 125.1 | 1.032 |
| SentencePiece 8k (shipped) | 139.8 | **1.154** |
| SentencePiece 32k | 139.6 | 1.152 (13,605 unk) |
| the v0.3 16,384-vocab HF tokenizer | 66.0 | 0.545 — **lossy**, its null `unk_token` silently drops every backslash (2,606 in this sample) |

Read it plainly: **the shipped 8k tokenizer needs 15.4% more tokens than cl100k_base on
canonical v0.4 text**, and the only toke tokenizer that appears to win does so by deleting
characters. No "purpose-built tokenizer beats cl100k" claim is supportable until story 116.9
trains and locks a v0.4 tokenizer against the Phase-3 anchor of cl100k = 242,427 tokens on
that exact sample.

**Cross-language density — one shared tokenizer, two languages** (TEMSpec §2.3,
informational):

| comparison | cl100k_base | N | source |
|---|---:|---:|---|
| 60 Gate-1 tasks, hand-written v0.4 toke vs equivalent Python | 4,787 vs 3,565 → **1.34× [1.22, 1.48]** | 60 | 133.4, `toke-eval/docs/gate1-60-v04.md` |
| four execution-verified sample pairs, toke `--min` vs Python | 344 vs 264 → **1.30×** (o200k 1.32×) | 4 | 132.0(b), `docs/about/samples-v04.md` |
| the same four pairs, raw UTF-8 bytes | 693 vs 753 → **0.92×** | 4 | 132.0(b) |

**Under the general-purpose tokenizers models actually use — cl100k_base, o200k_base,
Qwen2.5-Coder — toke currently costs about 30% more tokens than Python on these N = 4 sample
pairs, and 34% more on the N = 60 task set. More, not fewer.** The v0.3-era text measured 1.76×, so the v0.4 rewrite closed most of
the gap but did not cross it. The one lane with no tokenizer assumption — raw bytes — has
toke at 0.92× Python, the narrowest margin in the table and the honest shape of the result
at N = 4.

**The toke-to-toke lane is where the real win is.** Re-expressing the same 60 Gate-1 tasks
in v0.4 cut them from 6,347 to 4,787 cl100k tokens, a **24.6% reduction** (N = 60, same
tokenizer on both sides, 133.4), while every program passes `tkc --check` (60/60) and all
120 hidden test cases per task (60/60), lint clean. That set is hand-written, not
model-generated — 27 ids are pure `--migrate` output and 33 were hand-repaired — so it
measures what the *language* can express, not what a *model* produces
(`docs/about/toke-eval-drift-decision.md` states what it may and may not claim).

**Never cross the lanes.** The toke-trained tokenizers in the first table were trained on
toke text; applied to Python they measure their own training bias, not the language. Quoting
a toke-trained tokenizer on the toke side against a general-purpose tokenizer on the Python
side is a methodology error, and it is the error behind the withdrawn claims in §9. Any
cross-language number uses **one** tokenizer on both sides.

### 3.2 The ceiling, even when the tokenizer wins

Suppose the tokenizer programme succeeds. The ceiling is still low, and it is better that we
publish the arithmetic than that someone else does.

Take a representative agentic task: 100,000 input tokens and 10,000 output tokens, with
output half code and half reasoning plus tool calls, and input 40% code. A syntax-level
saving reaches only the code slices. An aggressive 40% cut on those yields roughly 18,000 of
110,000 tokens — about 16% of raw tokens, and that already assumes the whole codebase in
context is already toke. Now re-price it cache-aware: cached input reads bill at roughly
0.1x, so the largest slice is also the cheapest per dollar and the input-code saving is
worth about a tenth of its face value.

The conclusion we adopt from the September 2026 landscape review: **a 30 to 50 percent cut
in code tokens nets only single-digit to low-double-digit percent of total agentic token
spend**, and is dominated by prompt caching, reasoning-length control and multi-token
prediction. That decomposition is an estimate built on assumed splits, not a measurement —
nobody has published the real one, which is why story 131.56 instruments a real session and
publishes it. The direction, though, is not in doubt: syntax savings are real and
second-order, and no claim in this project may contradict that.

---

## 4. The evidence against us

Four findings argue against the project. We state them in our own words, without softening,
and answer each. The full survey is `docs/about/landscape-2026-09.md`; the archived external
review is `docs/about/reviews/landscape-2026-09-18.md`.

### 4.1 Superword tokenizers commoditise the tokenizer gain

**The threat.** SuperBPE (arXiv 2503.13423, ICML 2025) bridges whitespace to form superword
units: at a fixed 200k vocabulary it encodes text with up to 33% fewer tokens than BPE (6.63
vs 4.45 bytes per token), with a +4.0% absolute average gain across 30 downstream tasks
(+8.2% MMLU) and 27% less inference compute at 8B scale. It delivers the bulk of what a
purpose-built tokenizer delivers, generically, inside ordinary model training, with an
accuracy *gain* rather than a cost. A bespoke tokenizer, by contrast, requires a bespoke
model, which forfeits prompt caching and shared-infrastructure economics.

**Our answer.** We concede the lane. The purpose-built tokenizer was never the durable part
of the argument, and on our own v0.4 numbers it is not currently a win at all (§3.1). What a
superword tokenizer does not deliver is a grammar a decoder can be constrained to, a
canonical form that makes rewards and diffs exact, or a compiler that answers in structured
diagnostics. Training lane 128.12 puts the question to a controlled test: on one frozen
corpus, compare our purpose-built BPE, a SuperBPE-class tokenizer, the base model's own
tokenizer, and the base tokenizer plus toke-specific added tokens — scored on tokens per
program *and* on downstream correctness after an identical fine-tune. If the superword lane
wins, we stop building tokenizers, and the language is unaffected.

### 4.2 Tokenizer-free architectures threaten the unit itself

**The threat.** H-Net (arXiv 2507.07955) learns chunking end-to-end from raw bytes, matches
a transformer of twice its size, and reports nearly 4x data-efficiency improvement on code.
Byte Latent Transformer (Pagnoni et al., ACL 2025, arXiv 2412.09232) matches Llama 3 at 8B
and is reported strongest exactly where tokenisation is weakest, including code. If models
consume bytes or learned dynamic chunks, "tokens per program" stops being a stable figure of
merit, terse ASCII loses its tokenizer arbitrage, and **every ranking built on BPE counts,
ours included, is void.**

**Our answer.** We would say so on the day, not defend it. What survives is the grammar (a
byte-level model still has to emit a syntactically valid program, and byte-granularity
constraint machinery pays *more* for a large grammar, not less), the compiler (an error
filter and reward signal that do not care how the text was produced), the canonical form,
and byte compactness — 0.92x Python on the v0.4 samples, much less than a tokenizer win but
not zero. The preparation is already done rather than promised: story 131.50 adds a raw-byte
lane and a BLT-style byte-patch lane to every benchmark, so this pivot costs a column rather
than a rewrite, and lane 128.13 runs a byte-level model on toke directly. The claim that a
small grammar makes byte-level generation cheaper is **not proven**; 131.51 is the test, and
F1 in §7 states what we do if it fails.

### 4.3 The task-level evaluation says terse-language advantages evaporate

**The threat.** danluu's 2026 evaluation ran frontier agents on non-trivial work (a zstd
decoder from spec; Pandoc ProgramBench). At medium reasoning effort the terse and
dynamic-language token advantage appears; **at high reasoning effort it disappears**, with
static languages among the best. Obscure and dense languages (J, Assembly) do poorly.
Language *popularity* correlates weakly-to-moderately with both higher correctness and lower
cost. This is the best independent, task-level evidence in our lane and it points against us
— it is also the evidence that should be trusted over RosettaCode-style token rankings,
which count existing snippets rather than end-to-end task cost. The same evaluation observed
agents repeating an identical compiler error before fixing it, which is a direct hit on
"faster compiler feedback means fewer iterations".

**Our answer.** The result attacks the terseness half of the design, not the grammar half,
and it is measured in the right unit: cost per solved task, not tokens per program. We adopt
that unit rather than arguing with it. Story 131.55 runs the four-arm comparison with the
same accounting — toke plus our model, toke plus a frontier model plus constrained decoding,
Python plus a frontier model, and Python plus a frontier model plus type-constrained
decoding — with failed attempts priced and input priced cache-aware. F4 in §7 states the
consequence if toke loses it. We would rather run danluu's experiment on ourselves than wait
for someone else to.

### 4.4 A new language starts as the lowest-resource language in existence

**The threat.** Pass@1 for genuinely low-resource languages — R, Racket, Perl, Swift, Go —
sits at or below 30% against 50-75% for Python, JavaScript and Java (Giagnorio et al.,
January 2025; MultiPL-E and MultiPL-T, arXiv 2308.09895). toke has no pretraining presence,
no RL environments and no Stack Overflow. Our own honest floor is consistent with the
penalty rather than with any headline: the full-local re-audit of all 1,748 v0.3.9 corpus
programs gives **37.5% compile (655/1,748) and about 2.2% fully correct (38 PASS)**.

**Our answer.** Anka partly rebuts this for *syntax* — 99.9% parse success from an
in-context spec, zero prior exposure — but not for *reasoning* in the language, and we do
not pretend otherwise. The structural answer is that toke is designed so the model does not
have to carry the language: the grammar artefacts constrain generation, the compiler
supplies the error signal, and the canonical form is short enough to fit in a syntax card.
Lane 128.10 is the honest test of whether that is enough: a frontier model, the syntax card
and the grammar artefacts, **with no training at all**, is the baseline every trained lane
must beat.

### 4.5 (And the one that attacks the design directly) Verbosity beat terseness

Anka is *deliberately verbose* with one canonical form, and it beat Python by **40
percentage points** on multi-step pipeline tasks (100% vs 60%), with GPT-4o-mini confirming
+26.7 points. toke bets on terse *and* canonical and has never separated the two. It is
entirely possible that the canonical-form half is doing the work and the terse half is
costing accuracy. Story 131.54 A/Bs exactly this on our own 46-entry catalogue, which
already holds measured terse and verbose forms of identical behaviour. F2 in §7 states the
consequence: "compact" drops out of the claim, and the idiom standard is rewritten. The
language base still does not change; the canonical form does.

### 4.6 The nearest substitute

KERN/KERN-py (Oscar Martinez) compresses Python's surface syntax reversibly and keeps
Python's semantics, runtime and tests — so it has no cold-start problem at all, since every
Python program is training data for it. It is the first third party to benchmark against
toke, and their cl100k numbers stand: Kern Compact 3,012 tokens against our 4,787 on the 60
Gate-1 tasks (Kern/toke 0.63). We reproduced their work from source (story 133.1) and our
corrections are qualifications, not rebuttals: the toke programs they measured were
April-2026 Gate-1-era output in a syntax three revisions old, and on migrated `--min` text
the equal-vocabulary lane narrows to parity within the confidence interval (ratio 0.971,
bootstrap 95% CI [0.759, 1.155], N = 60). Publishing this is the point.

---

## 5. Correctness: what has actually been measured

This is the weak half of the project and always has been.

| Evaluation | Compile Pass@1 | Functional | Set / model |
|---|---|---|---|
| Gate 1 (2026-04-03) | 63.7% | n/a | fine-tuned 7B baseline; ~2.5% illegal-char |
| Gate 2 (2026-05-22) | **100%** | **55.6%** (272/489) | curated 500 hidden + 200 eval; Qwen 2.5 Coder 7B + QLoRA, **v0.3 syntax** |
| Full-local re-audit (honest floor) | **37.5%** (655/1,748) | **~2.2%** (38 PASS) | all 1,748 programs, v0.3.9 |
| `toke_generate` sample (71.5.4) | 84% (21/25) | 78% (18/23) | 25-prompt benchmark |
| Corpus after v0.4 mechanical migration | 84.3% (1,781/2,112) | n/a | `=`→`==` only, not yet idiomatic |

Four caveats do the work here, and none of them may be dropped when a number is quoted.

1. **Every trained-model number above is from a v0.3-syntax model.** v0.4 is a breaking
   change. No v0.4-native model has been trained, and no from-scratch ~1B model — the actual
   north-star deliverable — exists.
2. **The Gate 2 100% is on a curated set the model was optimised against.** The 37.5% /
   2.2% re-audit over all 1,748 programs is the honest real-world floor. Quoting the 100%
   without the floor is a misrepresentation of our own work.
3. **The 55.6% is a correction, not a revision of convenience.** It was originally reported
   as about 8% because the `io.readln` C glue was missing; fixing the stdlib link on
   2026-05-25 moved it to 55.6% (272/489). External descriptions of toke still quote the 8%,
   and that figure is stale in our favour as well as against us — we state it here so that
   neither version circulates uncorrected.
4. **Passing a test is not evidence of correctness when the test can be gamed.** The
   clearest finding in the project is story 131.42: of 576 A-ERR error-union corpus records,
   **429 across 69 bases are gamed** — 424 build the harness's expected marker as a string
   literal, 5 construct it at run time — against 147 correct. A companion re-authoring
   recheck (131.47) found 68 of 336 records genuinely wrong against corrected tests. The
   generation model learned to defeat a literal-matching test. Any "% pass" figure taken
   from corpus data has to be read against that.

**There was no August 2026 gate.** August produced a training-data quality freeze — 23,382
audited v0.4 corpus records, 14,727 passing every execution gate, 1,583/1,583 library
programs, 631 A-category bases given execution-verified tests for the first time, 2,639
records repaired — and that is a statement about data quality, not about what a model can
generate. Every Epic 128 training story remains planned and compute-gated, so the most
recent model gate is still Gate 2 of 2026-05-22. The freeze was itself reopened by Epic 131
in September, so even those corpus numbers are a superseded snapshot.

---

## 6. Methodology

### 6.1 Falsification-first, and what that has cost us

Each stage must pass pre-registered criteria before the project advances; criteria are
locked before experiments run; the decision document is append-only; a gate failure
terminates or re-scopes the programme rather than moving the threshold.

The methodology has been honoured in the places where it hurt. Gate 1's modest result was
published as modest. The 8% functional result was published before the cause was known. The
52% headline was withdrawn when it could not be reproduced from its own source data, rather
than requalified into survival. The corpus freeze was reopened a month after it was
announced because a pattern sweep found systematic gaming. The value of a gated research
project is exactly this: the negative results are load-bearing, and they are published.

### 6.2 Gate 3, and the reason there is no date on it

Gate 3's criteria were locked on 2026-05-24 and are unchanged: functional Pass@1 >= 35% on a
500-task hidden benchmark; compile Pass@1 >= 95%; argv-generalisation >= 50%; multi-model
coverage across >= 2 families; and demonstrated self-improvement (iteration N+1 beating
iteration N by >= 5 percentage points on the identical benchmark, same base model, same
decoding parameters). The failure mode is pre-registered too: functional Pass@1 < 15%
triggers a post-mortem and a pivot evaluation.

The original timeline said mid-August 2026. That date passed without a training run, because
the corpus was not trustworthy enough to train on — which the audit and pattern sweeps
proved rather than assumed — and because the training lanes are compute-gated. We state the
slip plainly instead of quietly re-dating it. Gate 3 now runs against the 128.15 scorecard:
compile Pass@1, functional Pass@1 by execution, tokens per solved task in every 131.50 lane,
cost and energy per solved task, repair-loop convergence within 3 rounds, and first-shot
validity under constrained decoding — with the rule that **no lane advances without beating
the no-training baseline**.

### 6.3 The dominant failure mode: hardcoding

The failure mode Gate 3's argv criterion exists for: the model emits syntactically valid
code that embeds a memorised expected output instead of implementing the algorithm — writing
`<42` rather than reading argv, parsing and computing. 67% of Gate 2's functional failures
had this shape, and 131.42 shows the same instinct applied to error-union tests. The fix is
not more syntax training; it is execution feedback and tests that cannot be satisfied by a
literal. Both are now structural: the corpus driver executes, and a hard `return_type` gate
blocks the gamed shape.

---

## 7. What would falsify this

Each test is filed work with a stated decision rule, so the answer is not a matter of
argument when it arrives. The authoritative table is `docs/about/positioning-2026-09.md` §8.

| # | Test | Result that falsifies | Consequence |
|---|---|---|---|
| F1 | **131.51** — byte-level durability spike | Byte-level generation constrained by toke's grammar is no cheaper and no more reliable than the same setup on a mainstream-language grammar: first-shot valid-program rate within noise, and no byte-per-solved-task advantage whose 95% CI excludes zero | The "durable under tokenizer-free architectures" claim in §4.2 is **retired**, not softened; the language-level claim narrows to constrained decoding under BPE only |
| F2 | **131.54** — terseness vs reliability, A/B over the 46-entry catalogue | The canonical terse form scores *lower* on first-shot functional correctness than the most verbose measured form of the same pattern, 95% CI excluding zero | Terseness is demoted below reliability; the efficiency protocol gains a correctness term, verdicts flip, and "compact" drops out of the claim |
| F3 | **128.10 + 128.15** — the no-training baseline | No trained lane beats a frontier model with the syntax card and the grammar artefacts and **no training** on cost per solved task | The bespoke-model programme **stops**; we ship the grammar, the compiler and the tooling and say publicly that training was not justified |
| F4 | **131.55** — cost per solved task, four arms | toke plus its best lane does not beat Python plus a frontier model plus type-constrained decoding, same tasks, failed attempts priced, input priced cache-aware | The language-level claim fails on the metric we chose ourselves; toke is then a research result about grammar design, not a production proposal |
| F5 | **116.9** — v0.4 tokenizer Phase-3 gate | The retrained, locked v0.4 tokenizer does not beat cl100k_base = 242,427 tokens on the 2,000-record baseline sample | The tokenizer half is dead; every token-reduction claim is withdrawn from every surface and §3 is deleted rather than requalified |
| F6 | **131.56** — agentic token decomposition | Cache-weighted code tokens are under 5% of billable spend in an instrumented session | Token efficiency stops being a headline property anywhere; it stays in the metrics file as a measured fact and leaves the positioning entirely |

Two results would strengthen the thesis enough to move the emphasis back: a 131.51 finding
that grammar-constrained byte-level generation is materially cheaper on toke, and a 131.55
finding that toke plus constrained decoding beats Python plus type-constrained decoding on
cost per solved task with no training at all. Neither is assumed here.

---

## 8. What we borrow

Three things the field does better than we do, taken rather than competed with.

1. **Constrained decoding as the delivery mechanism.** XGrammar-class grammar-constrained
   decoding and PLDI 2025 type-constrained generation, wired to our own grammar artefacts,
   with mask-construction cost benchmarked against a mainstream-language grammar and grammar
   drift breaking the build (131.52). Caveat carried: constraints applied naively can reduce
   reasoning ability, and validity is not correctness.
2. **Execution-feedback and RLVR training.** The compile gate is necessary and not
   sufficient; execution feedback is what moves functional correctness, our open weakness on
   every honest number we have (128.11). The reasoning-channel A/B (131.53) tests whether our
   reasoning-light corpus stance is costing correctness; efficient-reasoning results
   (TokenSkip, EMNLP 2025: 40% fewer reasoning tokens for under 0.4% accuracy loss) suggest
   it might. The language does not change; the record shape might.
3. **Diff and patch output formats.** Edit formats capture much of the output-token saving on
   any language — aider's unified-diff format raised GPT-4 Turbo from 20% to 61% on its own
   benchmark — and a diff of toke is still a diff. Story 131.57 defines the canonical toke
   edit format and maps `tkc --fix` and `--migrate` spans to patches.

---

## 9. Corrections to the previous version of this paper

The 2026-05-24 revision of this paper published five things that were wrong. They are listed
here, in full, because they propagated: an independent September 2026 review describes toke
as "LL(1), 13 keywords" with "about 8% functional correctness" precisely because that is what
we published.

| Published claim | Status | Correct statement |
|---|---|---|
| "a formal EBNF grammar, LL(1) verified" | **Wrong** | `toke-spec-v0.4.md` §E (2026-07-02) states the strict-LL(1) claim "was **not accurate** for the real grammar". The grammar is backtrack-free with an enumerated set of productions needing bounded lookahead of up to 3 tokens |
| "13 keywords" | **Wrong** | 14: `m i t f let if el lp br rt as mt sc mut` (spec §A, verified against the lexer keyword table). `mut` and `sc` are keywords |
| "a purpose-built 16K BPE tokenizer achieves 52% fewer tokens" | **Withdrawn, not requalified** | The figure could not be traced to a primary artefact. Its only published N = 42 dataset re-aggregates to 61.6% (sum-ratio) / 62.6% (per-task mean) for that tokenizer against cl100k_base on the *same toke text*, while reproducing exactly the two withdrawn cross-tokenizer headlines it sits beside. It was never a comparison with Python, and it rests on a lossy tokenizer. It may be described as a withdrawn past claim; it must not be restated as a measurement |
| "using the same tokenizer (cl100k_base) for both languages, toke achieves a 12.5% token reduction over equivalent Python programs" | **Wrong lane** | The Gate 1 12.5% was an 8K purpose-built BPE vocabulary against its baseline on toke text, not a same-tokenizer comparison with Python. The same-tokenizer cross-language number runs the other way: toke costs **1.34× [1.22, 1.48]** the cl100k_base tokens of equivalent Python on the 60 Gate-1 tasks (N = 60) |
| "Gate 3 GO/NO-GO by mid-August 2026" | **Did not happen** | No model gate ran in August 2026. August produced a corpus-quality freeze. The most recent model gate is Gate 2 of 2026-05-22 |

Two further claims from that version are withdrawn on the same methodology ground: **"42%
reduction vs Python"** and **"56% vs Java"** counted the toke side with a toke-trained
tokenizer and the other side with a general-purpose one, which measures the tokenizer's
training bias rather than the language; and every per-example variant of it ("14 tokens vs
27 for Python", and similar) fails the same way. `docs/about/samples-v04.md` replaces them
with every lane reported side by side.

Finally, several project-scale figures in the previous version — corpus record counts,
production line counts, stdlib module counts, conformance-test counts, diagnostic-code
counts — were quoted without a source that survives re-checking. They are removed rather
than re-quoted. Nothing in this paper is asserted unless `docs/metrics-baseline.md` carries
it.

---

## 10. What toke has not shown

- **Functional correctness.** 55.6% on a curated v0.3 set, about 2.2% on the honest floor.
  This is the open weakness and the reason execution feedback is the priority lane.
- **Any v0.4 model result.** None exists. Every model number in this paper is v0.3-era.
- **A token win under the tokenizers models actually use.** Today toke costs more (§3.1).
- **End-to-end cost advantage.** Not measured; 131.55 and 131.56 are the instruments.
- **That a small grammar helps byte-level generation.** A mechanical argument, not a result
  (131.51).
- **External adoption.** All toke usage remains within the author's own repositories.
  A language nobody else uses has not demonstrated practical utility, and that is the
  strongest current basis for scepticism regardless of its technical merits.

---

## 11. Reproduce it, or break it

Everything above is reproducible from published artefacts, and the fastest way to help is to
find an error in it.

- **The numbers:** [`docs/metrics-baseline.md`](../metrics-baseline.md) — metric, basis,
  caveat and origin for each. Cite the row and its caveat, never a headline.
- **The v0.4 sample pairs:** `docs/about/samples-v04.md` and `samples-v04.json`; regenerate
  with `python3 scripts/about/samples_v04.py`. Every pair is execution-verified — the toke
  binary and the Python program must print byte-identical output before any number is
  emitted.
- **The 60 Gate-1 tasks:** `toke-eval/docs/gate1-60-v04.md`, with the harness commands and
  per-task counts, and `docs/about/toke-eval-drift-decision.md` for what that set may and
  may not claim.
- **The KERN comparison:** `docs/about/reviews/kern-2026-08.repro.py` re-runs it end to end
  from pinned commits and wheel SHAs with bootstrap CIs.
- **The tokenizer baseline:** sample ids and SHAs in
  `toke-tokenizer/data/baseline_sample_ids_v04.txt`.
- **The compiler and grammar:** `github.com/karwalski/toke`; `docs/spec/grammar.ebnf` and
  `docs/spec/toke.gbnf` are the artefacts to wire a constrained decoder to.

What we most want from readers, in order: an independent cost-per-solved-task measurement on
our four arms; a byte-level or dynamic-chunking model run against the toke grammar; a
constrained-decoding benchmark of our grammar against a mainstream-language grammar; and a
programme written in toke by someone who is not the author.

The strongest form of this project is not toke succeeding. It is toke producing a clear,
honest answer to whether a purpose-built language helps a model write correct code cheaply.
A rigorous negative result is worth as much as a positive one, and §7 says exactly what one
would look like.

---

## References

1. toke Language Specification v0.4 (normative amendment). `docs/spec/toke-spec-v0.4.md`
2. Honest metrics baseline — the source for every number in this paper. `docs/metrics-baseline.md`
3. Repositioning brief: the durable claim. `docs/about/positioning-2026-09.md`
4. The landscape and the evidence against us. `docs/about/landscape-2026-09.md`
5. Landscape review, September 2026 (archived, unedited). `docs/about/reviews/landscape-2026-09-18.md`
6. KERN review and reproduction. `docs/about/reviews/kern-2026-08.md`, `kern-2026-08.repro.py`
7. v0.4 code samples, every tokenizer lane. `docs/about/samples-v04.md`
8. Gate-1 60, v0.4 re-delivery. `toke-eval/docs/gate1-60-v04.md`
9. Gate 2 decision (2026-05-22). `docs/spec/gate2-decision.md`
10. Gate 3 pre-registration (2026-05-24). `docs/spec/gate3-criteria.md`
11. Liu et al. "SuperBPE: Space Travel for Language Models." arXiv 2503.13423, ICML 2025.
12. Hwang, Wang and Gu. "Dynamic Chunking for End-to-End Hierarchical Sequence Modeling" (H-Net). arXiv 2507.07955.
13. Pagnoni et al. "Byte Latent Transformer." arXiv 2412.09232, ACL 2025.
14. Dong et al. "XGrammar: Flexible and Efficient Structured Generation." arXiv 2411.15100.
15. Mündler et al. "Type-Constrained Code Generation with Language Models." arXiv 2504.09246, PLDI 2025.
16. Al Mazrouei. "Anka: a verbose DSL for reliable LLM code generation." arXiv 2512.23214.
17. Giagnorio et al. "Enhancing Code Generation for Low-Resource Languages." January 2025; MultiPL-E / MultiPL-T, arXiv 2308.09895.
18. Xia et al. "TokenSkip: Controllable Chain-of-Thought Compression." arXiv 2502.12067, EMNLP 2025.
19. danluu. "Cost and correctness of LLM agents across programming languages." 2026.
20. Gauthier, P. "Unified diffs make GPT-4 Turbo 3x less lazy." aider.chat benchmark.
21. Project: https://github.com/karwalski/toke
