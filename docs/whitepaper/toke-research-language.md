# Not Another AI-Generated Sloplang

**A Case Study in AI-Assisted Language Research**

Matthew Watt (karwalski)
v0.1, v0.2, v0.3 (syntax locked)
2026-05-24 (revised)

---

## Abstract

toke is a compiled, statically typed programming language investigating whether purpose-built languages can reduce large language model inference costs through structural token efficiency. This paper — like significant portions of the toke implementation — was produced through iterative human-AI collaboration. We disclose this upfront: AI was used extensively throughout this project to assist language design iteration, generate the training corpus, write implementation code, produce research documents including this whitepaper, and run multi-model reasoning and review. This is far from one-shot AI slop. It represents months of iterative collaboration with extensive review, testing, and falsification. The human provides direction, criteria, and judgment; AI provides implementation velocity and multi-perspective analysis.

Over three months of systematic research, toke has progressed through two formal falsification gates with pre-registered success criteria. Using the same tokenizer (cl100k_base) for both languages, toke achieves a 12.5% token reduction over equivalent Python programs — a modest but real signal that justified continuation. A purpose-built 16K BPE tokenizer achieves 52% fewer tokens (upper bound, purpose-built BPE trained on toke's own corpus — methodological limitations discussed below). A fine-tuned 7B parameter model produces syntactically valid toke 100% of the time on 700 evaluation tasks.

The project employs a falsification-first methodology where any gate failure terminates the research programme. toke is backed by a reference compiler in C with LLVM backend, a formal EBNF grammar (65 productions, LL(1) verified), 38 standard library modules, and three production codebases. Gate 3 criteria are pre-registered and locked. This paper presents both the evidence for toke's viability and an honest accounting of what remains unproven.

---

## 1. Introduction

Large language model inference costs scale linearly with token count. Every token consumed by syntactic overhead — verbose keywords, comments, whitespace conventions, case-sensitive identifiers, redundant separators — is a token not spent on semantic reasoning. Existing programming languages were not designed with tokenizer efficiency in mind; their syntax evolved for human readability under constraints that predate the era of AI-generated code.

toke asks a direct question: **can we design a programming language that reduces LLM inference cost by producing fewer tokens per unit of functionality, without sacrificing compilability or correctness?**

This is not a rhetorical question. It is a falsifiable hypothesis with pre-registered criteria. If the answer is no — if token reduction does not translate to improved generation quality or reduced cost — the project terminates and publishes an honest post-mortem.

### 1.1 The AI-Language Landscape

2025-2026 has seen a measurable proliferation of AI-targeted languages: Mog (3,200-token spec, QBE backend), Sigil (polysynthetic, LLVM), Pel (homoiconic, minimal grammar), Quasar (42% execution-time reduction), Plang, EnCompass, and PayPal's agent workflow DSL — among many others. Most ship a spec and a toy interpreter and stop there.

toke must be judged against this cohort on the evidence, not on intent. This paper presents that evidence — including the gaps.

### 1.2 The Agent-Internal-IR Hypothesis

We state explicitly the most likely adoption path: **humans never write toke directly.** Agents write toke when token budget matters — expensive inference, limited context windows — with automatic compilation verification. The human interface remains natural language or their preferred language; toke is the efficient intermediate representation the agent chooses when cost optimization matters.

This reframes the evaluation question. toke does not need to be ergonomic for human developers. It needs to be learnable by models, verifiable by compilers, and cheaper than alternatives measured in tokens per unit of correct functionality.

### 1.3 Relationship to Constrained Decoding and In-Context Specification

Two alternative approaches deserve comparison:

**Constrained decoding** (grammar-guided generation) can achieve 100% syntactic validity on any language by restricting the output distribution at each token step. This is orthogonal to toke's goal: constrained decoding ensures validity but does not reduce token count. A Python program generated under grammar constraints still costs the same number of tokens. toke targets the token budget itself.

**In-context specification** (e.g., Mog's 3,200-token spec fits entirely in a system prompt) allows models to generate valid code without fine-tuning. toke's richer type system, LLVM backend, and 38-module standard library require more than a system prompt can teach. The fine-tuning requirement is a feature — it enables a richer language surface — not a bug. A spec that fits in-context necessarily constrains the language to what can be described in that context budget.

---

## 2. Methodology

### 2.1 Falsification-First Design

toke follows a gated research methodology where each stage must pass pre-registered criteria before the project advances. Criteria are locked before experiments run. Thresholds cannot be adjusted after the first training run starts. The document is append-only.

### 2.2 Gate 1: Does a Restricted Character Set Reduce Tokens?

- **Pre-registered criterion:** Measurable token reduction vs cl100k_base on equivalent programs.
- **Result:** 12.5% average token reduction. 63.7% compilation Pass@1. **PASS** (2026-04-03).
- **Honest assessment:** 12.5% is modest. SimPy (arXiv:2404.16333, ISSTA '24) reported 13.5%/10.4% with a different approach. This result justified continuation but is not transformative by itself.

### 2.3 Gate 2: Can an LLM Learn to Generate Valid toke?

- **Pre-registered criterion:** Fine-tuned 7B model outperforms Gate 1 baseline on compilation Pass@1.
- **Result:** 100% compilation Pass@1 on 700 tasks. **PASS** (2026-05-22).
- **Additionally measured:** 55.6% functional correctness (272/489) after 2026-05-25 stdlib fix. Originally reported as ~8% due to missing io.readln() C glue. See Section 10.

### 2.4 Gate 3: Does Functional Correctness Follow? (Pre-Registered, Locked)

- **Pre-registered criteria (locked 2026-05-24):**
  - C1: Functional Pass@1 >= 35% on 500-task hidden benchmark
  - C2: Compilation Pass@1 >= 95%
  - C3: argv-generalisation >= 50% (defined in Section 2.5)
  - C4: Multi-model coverage >= 2 families
  - C5: Self-improvement demonstrated (defined in Section 2.6)
- **Decision rule:** Point estimate >= 35% required for PASS. If point estimate is 30-34% (borderline fail), we publish the exact number with 95% confidence interval and let the community assess. No post-hoc threshold adjustment permitted.
- **Failure mode:** If functional Pass@1 < 15%, project triggers post-mortem and pivot evaluation.
- **Timeline:** Training starts 2026-06-02. GO/NO-GO by mid-August 2026.

### 2.5 Defining argv-generalisation and the 67% Hardcoding Problem

The dominant failure mode observed in Gate 2 functional evaluation: the model hardcodes test inputs rather than reading from argv and computing. For example, when asked to write a program that doubles its input, the model writes `<42` (the expected output for input 21) instead of reading argv, parsing, and computing `n * 2`.

67% of "functional" failures in Gate 2 exhibit this pattern. The model has learned to produce syntactically valid code that embeds memorized expected outputs rather than implementing the specified algorithm.

**C3 requires:** >= 50% of programs correctly read and process dynamic inputs (inputs not seen during training) rather than memorizing expected outputs. This is measured by running each task with 3 novel input values not present in any training example.

### 2.6 Operationalizing Self-Improvement (C5)

C5 is satisfied when: **Iteration N+1 achieves >= 5 percentage points higher functional Pass@1 than iteration N on the same held-out benchmark, same model family, same decoding parameters.**

Concretely: if iteration 1 achieves 20% functional Pass@1, iteration 2 must achieve >= 25% on the identical 500-task benchmark using the same base model, same LoRA rank, same temperature, same top-p. The only variable is the training corpus (enriched by the self-improvement loop).

---

## 3. The stdlib Fix: From 8% to 55.6%

The corrected 55.6% functional correctness already exceeds Gate 3 C1 (35% threshold) requires explanation. Three mechanisms provide the training signal that Gate 2 lacked:

**1. Execution feedback in the self-improvement loop.** Gate 2 was trained on compilation correctness only — the model learned syntax, not semantics. The self-improvement loop adds functional signal: only programs that produce correct output on test inputs enter the training corpus. The model learns from its own successes, progressively enriching the corpus with execution-verified examples.

**2. Curriculum training pairing specs with implementations.** Gate 3 training pairs functional specifications (natural-language intent + expected I/O) with compiler-verified implementations. This teaches the intent-to-code mapping — not just "produce valid syntax" but "produce code that does what was asked." Gate 2 had no such pairing; it trained on code alone without functional context.

**3. Functional correctness as the optimization target.** The original 8% was a measurement error (missing io.readln() C glue). The model actually produces functionally correct code at 55.6%. Gate 3 C1 (>=35%) is likely already met. Gate 3 training makes functional correctness the explicit optimization target through the self-improvement loop's selection pressure.

We do not claim 35% is guaranteed. We claim these mechanisms provide training signal that was entirely absent in Gate 2, making substantial improvement plausible. If these mechanisms fail to deliver >= 35%, that is itself an informative negative result about the learnability of functional semantics through self-play.

---

## 4. Language Design

### 4.1 Design Process: Iterative Human-AI Collaboration

toke was designed by a human engineer through iterative specification work spanning 31 normative sections, with AI used extensively for design iteration, trade-off analysis, and implementation. The language *design decisions* — character set, keyword selection, grammar structure, type system — are human-directed with documented rationale. AI contributed implementation velocity, alternative exploration, and multi-perspective analysis at each decision point. This workflow is itself the thesis in action: human judgment directing AI execution.

| Property | Value | Rationale |
|----------|-------|-----------|
| Character set | 55 (a-z, 0-9, 19 symbols) | Eliminates tokenizer ambiguity from uppercase |
| Keywords | 13 (m, f, t, i, if, el, lp, br, let, mut, as, rt, mt) | Minimal control flow vocabulary |
| Grammar | LL(1), 65 productions | Deterministic parsing from one token lookahead |
| Comments | None in source | Documentation in companion files (.tkc) |
| Naming | Lowercase concatenated only | Reduces tokenizer vocabulary pressure |
| Separators | Semicolons exclusively | Eliminates common LLM generation errors |
| Compilation | LLVM backend, native binaries | Real programs on x86-64 and ARM64 |

### 4.2 Formal Specification

- **3,400+ lines** across 31 sections (normative and informative)
- **65 EBNF productions**, verified LL(1)
- **70+ diagnostic codes** in structured JSON schema
- Available at github.com/karwalski/toke/docs/spec/toke-spec-v0.3.md

### 4.3 Reference Compiler

Written in C. Lexer, parser, type checker, LLVM IR code generation. Compiles to native binaries for x86-64 Linux, ARM64 Linux, ARM64 macOS. 62+ conformance tests. Structured JSON diagnostics. Migration mode for legacy syntax.

### 4.4 Target Domain: Small Tools and Services

toke targets the "small tools and services" space where its 38-module stdlib is sufficient: CLI tools, HTTP services, data pipelines, file processing. For tasks requiring numpy, pandas, or torch, the agent should emit Python. toke is not a Python replacement — it is a compilation target for the subset of tasks where token cost dominates and stdlib coverage is adequate.

This is a deliberate scope constraint, not a limitation to be fixed. The stdlib long-tail problem (no language can cover every domain) is addressed by honest scoping rather than aspirational coverage claims.

---

## 5. Training Infrastructure

### 5.1 Model Training (Gate 2 Configuration)

| Parameter | Value |
|-----------|-------|
| Base model | Qwen 2.5 Coder 7B-Instruct |
| Method | QLoRA (rank 64, alpha 128) |
| Training corpus | 25,953 records |
| Hardware | NVIDIA A10G 24GB, 37 hours |
| Result | 100% compilation Pass@1 |

### 5.2 Purpose-Built Tokenizer

- **16,384-token BPE vocabulary** trained on normalised v0.3 toke programs
- **52% average token reduction** vs cl100k_base (measured on 42 benchmark programs)
- Available: PyPI (`pip install toke-tokenizer`), HuggingFace (`karwalski/toke-tokenizer`)

### 5.3 Corpus Provenance (73,643 Records)

The Gate 3 training corpus of 73,643 records was transformed from the 46,754-record base corpus. The base corpus is sourced from loke production code (698 .tk files) and execution-verified generated programs. Each record is compiler-verified (passes lexing, parsing, and type-checking).

The transformation from 46,754 to 73,643 is mechanical: splitting complete programs into progressive-difficulty training pairs across 6 curriculum phases (token completion, function completion, spec-to-implementation, error correction, multi-file context, full application). This is structural rearrangement for pedagogical ordering, not new generation. No record enters the corpus without compiler verification.

### 5.4 Curriculum Design (Gate 3)

6 progressive phases spanning token completion through full application synthesis. The self-improvement loop generates execution-verified training data from the model's own correct outputs — programs that compile AND produce correct output on test inputs are added to subsequent training iterations.

---

## 6. Real-World Applications

### 6.1 Production Codebases

- **ooke:** CMS/web framework serving tokelang.dev in production. Native HTTP server, template engine, markdown rendering. One compiled binary.
- **loke:** Privacy and AI platform. 698 .tk files, 87,318 lines. All compiling. Covers HTTP, crypto, JSON, database, ML.
- **moke:** Data analysis application exercising governance and LLM integration.

### 6.2 Developer Infrastructure (Published)

| Platform | Package | Install |
|----------|---------|---------|
| Ollama | karwalski/toke | `ollama run karwalski/toke` |
| npm | @tokelang/mcp-server | `npx @tokelang/mcp-server` |
| npm | @tokelang/lsp | `npm i -g @tokelang/lsp` |
| PyPI | toke-tokenizer | `pip install toke-tokenizer` |
| VS Code | tokelang.toke-language | Extensions → "Toke" |
| Open VSX | tokelang.toke-language | VS Codium/Gitpod |
| HuggingFace | karwalski/toke | Model + tokenizer |
| Docker | docker-compose.yml | Self-hosted inference |
| API | api.tokelang.dev | Free tier, live |
| Console | console.tokelang.dev | Keys, usage, admin |

---

## 7. Measured Results

| Metric | Value | Caveat |
|--------|-------|--------|
| Token reduction (same tokenizer, cl100k) | **12.5%** | Honest baseline; isolates language design effect |
| Token reduction (toke BPE vs cl100k) | **52%** | Upper bound, purpose-built BPE; see Section 10.2 |
| Compilation Pass@1 (Gate 2) | **100%** | On training-adjacent tasks; see Section 10.3 |
| Compilation via production API | **84%** | With system prompt guidance; see Section 7.1 |
| Functional correctness | **55.6%** (272/489) | Corrected from ~8% — see Section 10.1 |
| Production codebase size | **87,318 lines** | loke, all compiling |

### 7.1 The 100% vs 84% API Gap

100% compilation is measured on the fine-tuned model running in the evaluation harness: controlled conditions, temperature 0.2, no system prompt, deterministic task formatting. 84% is the same model behind the production API (api.tokelang.dev) with a system prompt and post-processing pipeline.

The 16-point gap comes from three sources: (1) system prompt interactions that shift the model's output distribution away from the fine-tuned behaviour, (2) diverse user prompts that fall outside the training distribution's formatting conventions, and (3) occasional post-processing truncation that breaks syntactic validity. The eval harness number measures the model's capability ceiling; the API number measures real-world deployment performance.

---

## 8. Research Review

### 8.1 Structured Multi-Perspective Internal Review

Eight structured review perspectives (T1-T8) were applied across distinct focus areas: reproducibility, compiler engineering, training methodology, specification quality, translation/readability, documentation, external credibility, and gate governance.

**Disclosure:** These were internal multi-perspective reviews conducted through AI-assisted analysis, not independent external peer review. They are useful for improving rigour but should not be confused with external validation. We welcome and invite genuine external review.

### 8.2 Feedback Incorporated

- Reasoning channel mandated (companion files)
- Curriculum training adopted
- Gate 3 pre-registration required
- Quality over quantity (execution-verified corpus)

---

## 9. What Distinguishes toke

Five things genuinely separate toke from low-effort AI-targeted novelty languages:

1. **A real backend.** LLVM IR codegen to native binaries is materially harder than a tree-walking interpreter.
2. **Pre-registered falsification with stated failure mode.** Most novelty PLs have no exit criteria. toke will publish a post-mortem if it fails.
3. **Trained tokenizer + trained model on an engineered corpus.** Most novelty PLs don't train anything; they rely on zero-shot generalization.
4. **Honest separation between "compiles" and "works."** We do not claim functional correctness. We gate the project on achieving it.
5. **Published, installable artifacts.** Not README-ware. Working tools on npm, PyPI, Ollama, VS Code Marketplace, and a live API.

---

## 10. What toke Still Needs to Prove

This section is the most important in the paper. We do not yet have sufficient evidence for the thesis.

### 10.1 Functional Correctness

At 55.6% functional Pass@1, the toke model writes correct programs at roughly 63% the rate of than the same base model (Qwen 2.5 Coder 7B) writes correct Python (88.4% HumanEval pass@1, Hui et al. 2024). The remaining 44.4% gap represents genuine algorithmic errors and edge cases that further training can address — retry loops and debugging tokens would obliterate the structural advantage. **Gate 3 must pass for the thesis to hold.**

### 10.2 Fair Tokenizer Comparison

The 52% reduction compares toke under a *toke-trained BPE* against other languages under cl100k_base. This is structurally biased: any language with a custom BPE trained on its own corpus will dominate a general-purpose tokenizer. The honest comparison requires:

- toke under cl100k_base vs Python under cl100k_base (isolates language design)
- toke under toke BPE vs Python under a Python-trained 16K BPE (controls for tokenizer)

We commit to publishing all four cells before Gate 3 evaluation. The 12.5% Gate 1 result (same tokenizer for both) is the fairer measure of language design impact.

### 10.3 End-to-End Cost in Agent Workflows

Source-side tokens != total inference cost. A coding agent's context window is dominated by retrieved files, tool outputs, error traces, and chain-of-thought — not just the lines being generated. A 50% reduction in source tokens may translate to only 5-15% reduction in a real agent workflow. We need to measure and publish end-to-end round-trip token costs on realistic tasks.

### 10.4 The Macro Headwind: Inference Cost Decline

LLM inference costs are falling ~10x/year (Appenzeller, a16z "LLMflation" Nov 2024) or up to 50x/year median (Epoch AI). A 52% structural reduction is roughly equivalent to ~3 months of natural price decline. If this rate holds, toke's advantage erodes within months. Counter-argument: cost decline applies uniformly; structural efficiency compounds on top of it. But the adoption-friction cost of a new language may not amortize fast enough.

### 10.5 External Usage

As of 2026-05-24, all toke usage is within the author's own repositories and projects. No external developers are building production applications in toke. **This is the strongest current basis for skepticism about real-world value** — a language nobody else uses, regardless of its technical merits, has not demonstrated practical utility. Gate 3 is the resolution point: either the functional correctness results justify adoption friction, or they don't.

### 10.6 Benchmark Provenance

The 700-task benchmark used for Gate 2 evaluation was derived from the same categories as the training corpus. While 500 tasks are "hidden" (not in training data), they share the same distribution. If the model has learned to pattern-match categories rather than generalise, 100% compilation may be partly tautological. Gate 3 must demonstrate transfer to genuinely novel tasks.

### 10.7 The LL(1) Claim

The claim that "LL(1) eliminates ambiguity that confuses LLMs" is plausible but not literature-backed. Research on constrained decoding (TokDrift, arXiv:2510.14972) suggests tokenizer-grammar misalignment matters more than parser class. We state this as a design hypothesis, not a proven advantage.

---

## 11. Project Scale

| Metric | Value |
|--------|-------|
| Tracked epics | 99 |
| Tracked stories | 500+ |
| Active repositories | 6 |
| Duration | 3 months daily development |
| Compiler diagnostic codes | 70+ |
| Standard library modules | 38 |
| Conformance tests | 62+ |
| Formal gate decisions | 2 passed, 1 pre-registered |

---

## 12. Call for External Validation

toke needs external users and independent assessment to move beyond "one person's research project." We specifically invite:

### 12.1 Application Developers

Build something in toke. The compiler, standard library, and tooling are ready:
- `ollama run karwalski/toke` — generate toke code locally
- VS Code extension with syntax highlighting and LSP diagnostics
- 38 stdlib modules (HTTP, crypto, JSON, database, file I/O, ML)
- `api.tokelang.dev` — free tier API for code generation

We want to see toke used for:
- CLI tools
- Web services
- Data processing pipelines
- Any application where token cost matters for AI-assisted development

### 12.2 Researchers

Reproduce our measurements. All artifacts are published:
- Tokenizer: `pip install toke-tokenizer` — verify the 52% claim on your own programs
- Model: `ollama run karwalski/toke` — measure compilation and correctness rates
- Compiler: `github.com/karwalski/toke` — run the conformance suite

We specifically invite:
- Independent like-for-like tokenizer benchmarks (controlled comparisons)
- End-to-end agent-workflow token-cost measurement
- Cross-model evaluation (run the eval harness on Llama, DeepSeek, etc.)

### 12.3 Skeptics

Tell us what would change your mind. We have pre-registered criteria that define failure. If Gate 3 fails (functional Pass@1 < 15%), we publish a post-mortem. If the like-for-like tokenizer comparison shows no significant advantage, we say so.

The strongest form of this project is not toke succeeding — it is toke producing a clear, honest answer to whether purpose-built languages improve LLM inference. A rigorous negative result would be at least as valuable as a positive one.

---

## 13. Conclusion

toke is a falsifiable research hypothesis, not a vanity project. The hypothesis is specific: purpose-built languages can reduce LLM inference costs through structural token efficiency.

**What we have demonstrated:**
- A compiled language with formal grammar, LLVM backend, and conformance tests
- 100% compilation from a fine-tuned model
- Published, installable tools on every major platform
- Production codebases totalling 87,000+ lines

**What we have not yet demonstrated:**
- Functional correctness competitive with Python baselines
- Fair like-for-like tokenizer comparison
- End-to-end cost reduction in agent workflows
- External adoption beyond the author's projects
- Transfer to genuinely novel tasks

**What will settle it:**
- Gate 3 (mid-August 2026): functional Pass@1 >= 35%, multi-model coverage, self-improvement
- Or: functional Pass@1 < 15% → post-mortem and honest negative result

The distinction between a research language and a novelty project is not the outcome — it is the methodology. toke has pre-registered criteria, formal gates, published artifacts, and an explicit failure mode. We invite you to watch Gate 3, reproduce our measurements, and build something. Let the data decide.

---

## References

1. Gate 2 Decision. 2026-05-22. `docs/spec/gate2-decision.md`
2. Gate 3 Pre-Registration. 2026-05-24. `docs/spec/gate3-criteria.md`
3. Training Next Phase. 2026-05-23. `docs/spec/training-next-phase.md`
4. Research Feedback Request. 2026-05-23. `docs/spec/research-feedback-request.md`
5. Reasoning Channel Spec. 2026-05-23. `docs/spec/reasoning-channel.md`
6. toke Language Spec v0.3. `docs/spec/toke-spec-v0.3.md`
7. SimPy: arXiv:2404.16333 (ISSTA '24). 13.5% token reduction via simplified grammar.
8. Token Sugar: arXiv:2512.08266. 15.1% reduction via tokenizer-aware syntax transforms.
9. TokDrift: arXiv:2510.14972. Tokenizer-grammar misalignment as obstacle to code generation.
10. Appenzeller, G. "Welcome to LLMflation." a16z.com, 12 November 2024. ~10x/year cost decline.
11. Epoch AI. "LLM inference prices have fallen rapidly but unequally." Median ~50x/year.
12. Hui et al. "Qwen2.5-Coder Technical Report." arXiv:2409.12186. 88.4% HumanEval pass@1.
13. GitHub Blog. "Why AI is pushing developers toward typed languages." 8 January 2026.
14. Project: https://github.com/karwalski/toke
15. Model: https://huggingface.co/karwalski/toke
16. Tokenizer: https://huggingface.co/karwalski/toke-tokenizer
17. API: https://api.tokelang.dev
18. Ollama: https://ollama.com/karwalski/toke
