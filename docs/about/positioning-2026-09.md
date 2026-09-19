---
title: "Repositioning brief: the durable claim (September 2026)"
slug: positioning-2026-09
section: about
story: 132.7
date: 2026-09-19
status: draft (owner ratification pending)
---

# Repositioning brief: the durable claim

**What this file is.** The single source for how toke is described. Stories 132.1 through
132.4 and 132.8 through 132.11 copy from this file plus the 132.1 canonical facts block,
word for word. Nothing in this file changes the language.

**Evidence base.** `docs/about/reviews/landscape-2026-09-18.md` (the September 2026
landscape review), `docs/about/reviews/kern-2026-08.md` (the KERN review, story 133.1),
`docs/metrics-baseline.md` (the only source for toke numbers),
`docs/spec/toke-spec-v0.4.md`, `docs/spec/idiom-v0.4.md`,
`docs/spec/patterns-protocol-v0.4.md` and `patterns/catalogue.json`.

**Two corrections this brief applies to its own sources.**

1. **Not LL(1).** `docs/spec/toke-spec-v0.4.md` §E retired the strict LL(1) claim on
   2026-07-02: the grammar is **backtrack-free with bounded lookahead of up to 3 tokens**,
   with the exceptions enumerated in the spec. The landscape review, the story text, the
   README and `oke-namespace.md` all still say "LL(1)" because they inherited it from the
   v0.3 spec. Every downstream surface must use the backtrack-free wording. The mechanical
   argument is unaffected: what makes a constrained-decoding mask cheap is a small grammar
   that never rescans input, not the literal lookahead constant.
2. **Gate 2 functional correctness is 55.6%, not 8%.** The landscape review argues from
   "about 8% functional correctness". `docs/metrics-baseline.md` records that the 8% was
   an `io.readln` stdlib linking bug, corrected on 2026-05-25 to **55.6% (272/489)**. The
   review's conclusion (a compile guarantee removes one error class and leaves algorithmic
   correctness open) survives the correction; its force does not. The harsher honest number
   is the full-local re-audit: **37.5% compile (655/1,748), about 2.2% fully correct (38
   PASS)** on all 1,748 v0.3.9 corpus programs.

---

## 1. The claim in one sentence

**The claim:**

> toke is a compiled programming language designed for LLM code generation: small,
> strictly structured, and canonical, so that generated code is cheap to constrain while
> it is being produced, cheap for a compiler to verify once it is, and compact in whatever
> unit the model generates in.

This is the sentence every downstream surface quotes. It names the audience (LLM code
generation), the three mechanical properties (small, structured, canonical), the three
things they buy (constrain, verify, compact), and it does not depend on the BPE token
being the unit of account.

**Candidates considered and rejected.**

| Rejected candidate | Why it was rejected |
|---|---|
| "The most token-efficient programming language for LLMs." | The current framing. It dies the moment the unit of account moves, and it is not supportable on our own numbers today: the shipped 8k tokenizer costs **15.4% more** tokens than cl100k on canonical v0.4 text (`docs/metrics-baseline.md`, 2026-09-18, N = 2,000). |
| "A minimal, LL(1), compiler-verified language designed for LLM code generation: constrained, checkable, and compact." | The landscape review's own proposed wording. Rejected on two counts: LL(1) is factually retired by `toke-spec-v0.4.md` §E, and "minimal" invites the Anka counter-evidence (a deliberately verbose DSL beat Python by 40 points on multi-step pipelines) without answering it. |
| "toke is a minimal, compiler-verified language designed for constrained and byte-level generation." | Bets the headline on an outcome we have not measured. Whether a small grammar helps byte-level generation is exactly the open question in story 131.51; claiming it before the spike reports would be the same error we are correcting. |
| "A language LLMs can be forced to write correctly." | Overclaims. Constrained decoding guarantees syntactic validity, not correctness. Our own Gate 2 is the counterexample: 100% compile Pass@1 on the curated set and 55.6% functional. |
| "toke makes AI-generated code cheaper." | Marketing register, no mechanism, and second-order on the evidence: a 30 to 52% cut in code tokens nets only single-digit to low-double-digit percent of total agentic spend (see §3). |
| "toke is a DSL for agents." | Wrong category. toke is a general-purpose compiled language with an LLVM backend and three production codebases (ooke, loke, moke). Calling it a DSL concedes ground the language does not need to concede. |
| "toke is a compiled language designed to be written by language models: 14 keywords, a 59-character set, a backtrack-free grammar and one canonical form." | Accurate, and it is the boilerplate paragraph's opening (§9). Rejected as *the* sentence because it lists the properties without saying what they buy, which is the part that has to survive an architecture shift. |

---

## 2. Why it holds

Each property below is a fact about the language, paired with the mechanism it buys a
model. The mechanisms are what survive a change of generation unit; the numbers attached
to any one tokenizer do not.

### Small: 14 keywords, a 59-character set

`toke-spec-v0.4.md` §A fixes the keyword set at **14** (`m i t f let if el lp br rt as mt
sc mut`); the default syntax uses a **59-character** alphabet (26 lowercase, 10 digits, 23
symbols), no uppercase and no underscores. (The brief originally said 55; story 132.14
resolved the count against `src/lexer.c` -- the 55 and the RFC's 56 both omitted live
operators. See `docs/metrics-baseline.md` § Project facts.)

*Mechanically:* a small terminal alphabet is a small vocabulary for any generation unit. At
the byte level it means the model is choosing among a few dozen live bytes at most
positions rather than the full 256, which is the narrowest hypothesis space we can offer a
tokenizer-free model. Whether that narrowness converts into measurably better byte-level
generation is **unproven** and is the subject of story 131.51.

### Structured: backtrack-free, bounded lookahead

`toke-spec-v0.4.md` §E: the parser never rescans consumed input, and an enumerated set of
productions require at most 3 tokens of lookahead. An implementation that backtracks or
needs unbounded lookahead is non-conforming. Machine-readable artefacts exist:
`docs/spec/grammar.ebnf` and `docs/spec/toke.gbnf`.

*Mechanically:* grammar-constrained decoding builds a token mask at every step from a
pushdown automaton over the grammar. A small, backtrack-free grammar makes that automaton
small and its masks cheap. XGrammar (arXiv 2411.15100, now in vLLM, SGLang, TensorRT-LLM
and MLC-LLM) reports up to 100x speedup with near-zero overhead on this construction;
type-constrained code generation (Mündler et al., PLDI 2025, arXiv 2504.09246) extends the same
idea to type-level constraints and reduces compile errors and hallucinated methods.

The same evidence cuts both ways and we say so first: XGrammar works on *any* grammar,
including Python's. The advantage a purpose-built grammar has is one of degree (a cheaper
mask, a smaller invalid space), not of kind. Measuring that degree is story 131.52: mask
construction cost and per-token overhead for toke against a mainstream-language grammar.

### Canonical: one measured form per construct

`docs/spec/idiom-v0.4.md` states the idiom rules; `docs/spec/patterns-protocol-v0.4.md`
turns them into measured verdicts, and `patterns/catalogue.json` currently holds **46
entries** across 10 families (41 provisional, 5 blocked; 11 carry a distinct `hot_path`
form). A form becomes canonical only by being best-or-tied on tokens *and* runtime (§6 of
the protocol), and all candidate forms of a pattern must print byte-identical output (§5.1).
`tkc --min` reproduces the canonical text.

*Mechanically:* one canonical form collapses the set of correct-but-different programs the
model must choose among. It also makes evaluation exact rather than fuzzy: two
implementations either produce identical canonical text or they do not, which is what makes
a diff format well defined (131.57) and a reward signal unambiguous.

### Compiler-verified: structured diagnostics, exit codes

The compiler emits diagnostics with stable codes and machine-parseable spans and a fix
field, which is what a repair loop and an RLVR reward function consume.

*Mechanically:* this is a verifiable reward signal that costs one compiler invocation. It
is necessary and demonstrably insufficient on its own. Gate 2 reached **100% compile
Pass@1** on the curated 500-hidden + 200-eval set with **55.6% functional** (272/489),
Qwen 2.5 Coder 7B + QLoRA, v0.3 syntax (`docs/metrics-baseline.md`). The full-local
re-audit over all 1,748 v0.3.9 programs gives the honest floor: **37.5% compile,
about 2.2% fully correct**. Compile-checking removes one error class. Execution feedback is
what moves the rest.

### Learnable without pretraining

The standing objection to a new language is that it is the lowest-resource language in
existence. The strongest evidence against that objection is Anka (arXiv 2512.23214): a
novel DSL with **zero prior training exposure** on which Claude 3.5 Haiku achieved **99.9%
parse success and 95.8% overall task accuracy**, with GPT-4o-mini cross-validation. A
model can learn a new language from an in-context spec. Our own regeneration waves show the
same effect, with agent workers writing accepted v0.4 from the syntax card alone; that rate
is **not yet recorded in `docs/metrics-baseline.md`** and must be before it is quoted
anywhere.

---

## 3. Token efficiency, honestly

Token efficiency is one measured property of toke. It is never stated without its lane:
which tokenizer measured which text against which baseline, and at what N.

**What we can state today, from `docs/metrics-baseline.md` only.**

| Statement | Lane | Source |
|---|---|---|
| The shipped 8k SentencePiece tokenizer needs **15.4% more** tokens than cl100k (279,672 vs 242,427; 139.8 vs 121.2 tokens per program) | canonical `tkc --min` v0.4 text, strings masked to `"_"`, N = 2,000 stratified records from the 2026-08-19 freeze | metrics-baseline, 2026-09-18 (131.20) |
| o200k is 1.012x cl100k; Qwen2.5-Coder 1.032x; SentencePiece 32k 1.152x (13,605 unk) | same sample | metrics-baseline, 2026-09-18 |
| The v0.3 HF 16,384-vocab tokenizer measures 0.545x cl100k, but it is **lossy**: a null `unk_token` silently deletes 2,606 backslash characters | same sample | metrics-baseline, 2026-09-18 |
| Gate 1 token reduction was **12.5%** | 8K purpose-built BPE vocab against its baseline, 2026-04-03 | metrics-baseline |
| Measuring readable source instead of `--min` understated toke by **28.2%** | any cross-language comparison must use the same basis on both sides | metrics-baseline (116/B2) |
| Under a shared cl100k tokenizer, toke costs **about 1.76x** Python on the 60 Gate-1 JSON-CLI tasks | cross-language density, informational | metrics-baseline, citing `reviews/kern-2026-08.md` §6.1 |

**What we must stop saying.** "52% average token reduction vs cl100k" is a same-tokenizer
number (Toke-16K on toke text against cl100k on the *same* toke text, N = 42, v0.3), it
rests on the lossy v0.3 tokenizer, and it invites the misreading "52% fewer tokens than
Python", which is the opposite of true. No "purpose-built tokenizer beats cl100k" claim is
supportable until story 116.9 trains and locks the v0.4 tokenizer against the Phase-3 gate
anchor of cl100k = 242,427 on that exact sample. Story 132.6 requalifies every existing
instance.

**The ceiling, even when the tokenizer wins.** The landscape review derives it from a
representative agentic task (100,000 input tokens, 10,000 output; half of output code, 40%
of input code). An aggressive 40% cut on the code slice saves about 16.4% of raw tokens.
Then two adjustments apply: syntax-only savings under an off-the-shelf tokenizer are closer
to 20 to 40% than to 52%, and prompt caching already prices repeated input at about 0.1x,
so the largest slice is also the cheapest per dollar. The review's conclusion, which we
adopt: **a 30 to 52% cut in code tokens nets only single-digit to low-double-digit percent
of total agentic token spend**, and is dominated by prompt caching, multi-token prediction
and reasoning-length control. Story 131.56 instruments a real session and publishes the
decomposition, because nobody has. Every Epic 132 claim must respect the result.

---

## 4. What is commoditised, what is at risk

Stated plainly, before anyone else states it for us.

**Commoditised: the tokenizer half.** SuperBPE (arXiv 2503.13423, ICML 2025) is a superword
tokenizer that bridges whitespace: at a fixed 200k vocabulary it encodes text with up to
**33% fewer tokens** than BPE (6.63 vs 4.45 bytes per token), with a **+4.0% absolute
average gain across 30 downstream tasks** (+8.2% MMLU) and **27% less inference compute** at
8B scale. It delivers most of what a purpose-built tokenizer delivers, generically, inside
ordinary model training, with an accuracy gain rather than a cost. Faster superword
training (arXiv 2604.05192) removes the last practical obstacle. A bespoke tokenizer also
requires a bespoke model, which forfeits prompt caching and shared-infrastructure
economics.

**At risk: the unit itself.** Tokenizer-free architectures consume bytes or learned dynamic
chunks. H-Net (arXiv 2507.07955) learns chunking end to end from raw bytes, matches a
transformer of twice its size, and reports nearly 4x data-efficiency improvement on code.
Byte Latent Transformer (ACL 2025, arXiv 2412.09232) matches Llama 3 at 8B and is reported
strongest exactly where tokenisation is weakest, including code; Fast BLT (arXiv 2605.08044)
and HoloByte (arXiv 2603.16917) follow. If these land, "tokens per program" stops being a
stable unit, terse ASCII loses its tokenizer arbitrage, and every ranking built on BPE
counts, ours included, is void.

**At risk: the task-level premise.** danluu's 2026 evaluation ran frontier agents on
non-trivial tasks (a zstd decoder from spec, Pandoc ProgramBench). At medium effort the
terse and dynamic-language advantage appears; at high effort it **disappears**, with static
languages among the best. Obscure and dense languages (J, Assembly) do poorly. Language
popularity shows a weak-to-moderate positive correlation with both correctness and lower
cost. This is the best independent, task-level evidence in the language lane and it points
against us.

**At risk: the cold start.** The low-resource programming-language literature (MultiPL-E,
MultiPL-T arXiv 2308.09895, Giagnorio et al. January 2025) puts Pass@1 for R, Racket, Perl,
Swift and Go at or below 30% against 50 to 75% for Python, JavaScript and Java. toke starts
below all of them: no pretraining corpus, no RL environments, no Stack Overflow. Anka
partly rebuts this for *syntax* (99.9% parse success from an in-context spec) but not for
*reasoning* in the language.

**At risk: terseness itself.** Anka is a deliberately **verbose** DSL, chosen for
reliability, and it beat Python by **40 percentage points** on multi-step pipeline tasks
(100% vs 60%), with GPT-4o-mini confirming +26.7 points. toke bets on terse *and* one
canonical form. Anka's result suggests the canonical-form half may be doing the work and
the terse half may be costing us. Story 131.54 tests this on our own 46-entry catalogue,
which already holds measured terse and verbose forms of identical behaviour.

**Our own contradicting evidence.** The KERN review (133.1) reproduced, exactly, that Kern
Compact uses 3,012 cl100k tokens against toke's 6,347 on 60 Gate-1 JSON-CLI pairs, and that
29 of those 60 stale toke sources no longer compile under tkc 2.8.0. On migrated text the
equal-vocabulary comparison narrows to parity within the confidence interval (Kern-16K /
Toke-16K = 0.971, 95% CI [0.759, 1.155]), which is the honest correction, but the
shared-tokenizer picture does not move: toke is about 1.76x Python on cl100k for these
programs. Publishing this is the point. The research review panel will find it either way.

---

## 5. The hedge, stated plainly

This is the central section. The question is what is left of toke if the BPE token stops
being the unit of account.

**What does not survive.** Tokens per program as a figure of merit. The
"52% versus cl100k" headline. Any ranking of languages by tokenizer output, ours and other
people's. The economic argument that rests on counting tokens. If H-Net-class or BLT-class
models become how code is generated, all of that is simply void, and we should say so on
the day rather than defend it.

**What survives, and is arguably strengthened.**

1. **The grammar.** A byte-level or dynamic-chunk model still has to emit a syntactically
   valid program. A small, backtrack-free grammar is *more* valuable there, not less,
   because constraint machinery at the byte level has to run at a finer granularity and
   therefore pays more for a large grammar. This is the claim story 131.51 exists to test,
   and it is the one the review names as our best surviving claim.
2. **The compiler.** An error filter and a verifiable reward signal are architecture
   independent. Whatever the model emits, `tkc` either accepts it or returns a structured
   diagnostic with a stable code and a span. Execution feedback does not care how the text
   was produced.
3. **One canonical form.** Exact comparison, well-defined diffs, unambiguous rewards. None
   of that depends on tokenization.
4. **Byte compactness.** Fewer bytes still costs less for a byte-level model. Much less
   than fewer BPE tokens bought under a tokenizer, but not zero, and it is directly
   measurable in the `byte256` lane we already carry.

**What we would do in that world.** Nothing to the language. The changes are all in
measurement and training:

- The headline metric moves from tokens per program to **bytes and byte-level patches per
  solved task**. Story 131.50 already adds a raw-byte lane and a BLT-style byte-patch lane
  to every benchmark alongside proxy8k, v03, Qwen2.5-Coder, cl100k and o200k, precisely so
  that this pivot costs us a column, not a rewrite.
- **Cost per solved task** (131.55) becomes the reported number. It is unit-agnostic by
  construction: it prices failed attempts, repair rounds and cached input, whatever the
  model consumes internally.
- The **grammar artefacts become the product** (131.52, and lane 128.13). `toke.gbnf` and
  `grammar.ebnf` plus the MCP tooling are what a byte-level model integrates against.
- The **tokenizer programme is descoped, not the language**. 116.9 exists to produce a
  correct v0.4 tokenizer and to settle a number. If the number stops mattering, that work
  stops; the compiler, the grammar and the catalogue do not.
- If the no-training baseline (128.10: frontier model, syntax card v2, grammar artefacts)
  beats every trained lane on cost per solved task, the honest outcome is to ship the
  grammar and the tooling and not train a model. That rule is already written into 128.15.

**The framing stays "designed for LLMs".** It was never "designed for a tokenizer". The
supportive evidence the review itself cites (Anka's in-context learnability, XGrammar,
PLDI 2025 type-constrained generation) endorses exactly the grammar-and-verifiability
argument. What changes is which half of the sentence carries the weight.

---

## 6. What we borrow

Three things the landscape does better than we do, taken directly.

1. **Constrained decoding as the delivery mechanism.** XGrammar-class grammar-constrained
   decoding and PLDI 2025 type-constrained generation, wired to our own grammar artefacts:
   validate `toke.gbnf` and `grammar.ebnf` against the real grammar, benchmark mask
   construction cost and per-token overhead against a mainstream-language grammar, measure
   first-shot syntactic validity with and without constraints, and break the build on
   grammar drift. Story 131.52. Caveat carried from the review: constraints applied naively
   can reduce reasoning ability, and validity is not correctness.
2. **Execution-feedback and RLVR training.** The compile gate is necessary and not
   sufficient (§2). Test and execution feedback is what moves functional correctness, which
   is our open weakness on every honest number we have. Lane 128.11, and the reasoning
   channel A/B in 131.53: the review recommends dropping the reasoning-light corpus stance,
   and efficient-reasoning results (TokenSkip, EMNLP 2025: 40% fewer reasoning tokens for
   under 0.4% accuracy loss) show reasoning tokens materially aid correctness. The language
   does not change; comments stay out of band. The **record shape** changes if the A/B says
   so.
3. **Diff and patch output format.** Edit formats capture much of the output-token saving
   on any language: aider's unified-diff format raised GPT-4 Turbo from 20% to 61% on its
   own benchmark, and `apply_patch` and `str_replace` are trained into frontier models.
   Define the canonical toke edit format, decide which dialect survives `--min`
   canonicalisation, map `tkc --fix` and `--migrate` spans to patches, and measure apply and
   anti-apply accuracy. Story 131.57.

---

## 7. What explicitly does not change

**The language base does not move.** Not the 59-character set, not the 14 keywords, not the
grammar, not the semantics, not the type system, not the error-union model, not the
canonical `--min` form, not the "designed for LLMs" framing. This brief changes no `.tk`
file, no production in `grammar.ebnf`, and nothing in `src/`.

Anyone reading a repositioning as a redesign has misread it. The language is doing the job
it was designed for, and the evidence the review assembles about grammar, constrained
decoding and in-context learnability is evidence *for* the design, not against it.

**What expands is everything around it:**

- **Testing and trials.** More lanes measured, not different code measured. Story 131.50
  requires every benchmark to report superword, raw-byte and byte-patch columns beside the
  existing ones, and makes an unlaned claim unpublishable.
- **Training lanes.** Seven of them, scored against each other rather than argued about:
  prompt-only (128.10), RLVR (128.11), tokenizer variants (128.12), byte-level (128.13),
  continued pretrain or adapter (128.14), fine-tune (128.5), from scratch (128.6).
- **Validation.** Execution-verified correctness rather than compile-checked validity,
  because the gap between those two is the largest honest weakness in
  `docs/metrics-baseline.md`.
- **How many things each stage is benchmarked against.** One pre-registered scorecard
  (128.15) applied at every stage: compile Pass@1, functional Pass@1 by execution, tokens
  per solved task in every 131.50 lane, cost and energy per solved task (131.55),
  repair-loop convergence within 3 rounds, and first-shot validity under constrained
  decoding (131.52). **No lane advances without beating the no-training baseline.**

That is the whole change: the same language, measured against more things, more honestly,
at every stage.

---

## 8. Falsification

What result retires or re-scopes this thesis. Each is tied to work already scheduled, with
a stated decision rule, so that the answer is not a matter of argument when it arrives.

| # | Test | Result that falsifies | Consequence |
|---|---|---|---|
| F1 | **131.51**, byte-level durability spike | Byte-level generation constrained by toke's grammar is not cheaper and not more reliable than the same setup on a mainstream-language grammar: first-shot valid-program rate within noise (overlapping 95% CIs) and no byte-per-solved-task advantage whose 95% CI excludes zero | The "durable under tokenizer-free architectures" claim in §5 is **retired**, not softened. The hedge becomes "the compiler and the canonical form survive", and the language-level claim narrows to constrained decoding under BPE only. |
| F2 | **131.54**, terseness vs reliability A/B on the 46-entry catalogue | The canonical terse form scores *lower* on first-shot functional correctness than the most verbose measured form of the same pattern, 95% CI excluding zero | Terseness is demoted below reliability. Protocol §6 gains a correctness term, verdicts flip, the idiom standard is rewritten, and "compact" drops out of the headline claim. The language base still does not change; the canonical form does. |
| F3 | **128.10 + 128.15**, no-training baseline | No trained lane beats Lane A (frontier model, syntax card v2, 131.52 grammar artefacts, no training) on cost per solved task on the 128.1 held-out set | The bespoke-model programme **stops**. We ship the grammar, the compiler and the MCP tooling, and say publicly that training was not justified. This rule is already written into 128.15. |
| F4 | **131.55**, cost per solved task, four arms | toke plus its best lane does not beat Python plus a frontier model plus type-constrained decoding on cost per solved task, on the same tasks, with failed attempts and cache-aware input pricing included | The language-level claim fails on the metric we chose ourselves. toke is then a research result about grammar design, not a production proposal, and the READMEs say so. |
| F5 | **116.9**, v0.4 tokenizer Phase-3 gate | The retrained and locked v0.4 tokenizer does not beat cl100k = 242,427 tokens on the 2,000-record baseline sample (`toke-tokenizer/data/baseline_sample_ids_v04.txt`) | The tokenizer half is dead. Every token-reduction claim is withdrawn from every surface, and §3 of this brief is deleted rather than requalified. |
| F6 | **131.56**, agentic token decomposition | Cache-weighted code tokens are under 5% of billable spend in an instrumented agentic session | Token efficiency stops being a headline property anywhere, including in §3. It stays in the metrics file as a measured fact about the language and leaves the positioning entirely. |

Two results would *strengthen* the thesis enough to change the emphasis back: a 131.51
finding that grammar-constrained byte-level generation is materially cheaper on toke, and a
131.55 finding that toke plus constrained decoding beats Python plus type-constrained
decoding on cost per solved task with no training at all. Neither is assumed here.

---

## 9. Boilerplate block

Reproduced word for word by the 132.1 canonical facts block, every toke repo README, the
home page, and every registry description. Do not paraphrase; if it needs to change, change
it here first.

### Paragraph (82 words)

> toke is a compiled programming language designed for LLM code generation. It has 14
> keywords, a 59-character set, a backtrack-free grammar with bounded lookahead, and one
> canonical form per construct, chosen by measurement in a 46-pattern catalogue and
> reproduced by `tkc --min`. That makes generated code cheap to constrain during decoding,
> cheap for a compiler to verify afterwards, and compact to emit. Token efficiency is one
> measured property of toke, always reported with its tokenizer and its baseline, not the
> whole claim.

### One-liner (19 words)

> toke: a compiled language designed for LLM code generation, with a small grammar, one
> canonical form and compiler verification.

### Rules for reproducing these

1. Never quote a toke number without its lane (tokenizer, baseline, N) and never from
   anywhere but `docs/metrics-baseline.md`.
2. Never write "LL(1)". Write "backtrack-free" or "backtrack-free with bounded lookahead".
3. Never write "52% fewer tokens" without the same-tokenizer qualifier required by story
   132.6, and not at all once 116.9 reports.
4. Where the evidence is against us, state it in our own words before anyone else does.
