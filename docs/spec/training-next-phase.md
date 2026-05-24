# Training Next Phase — Specification and Options

**Status:** Planning (post-Gate 2)
**Date:** 2026-05-23 (updated with research review recommendations)
**Context:** Gate 2 passed with 100% compilation but ~8% functional correctness. Next phase targets functional correctness (correct I/O on benchmark tasks).
**Language version:** v0.3 syntax and spec are **locked**. All training targets v0.3. Language changes (if any) are deferred to a future v1.0 RFC process after community feedback.

---

## Design Decisions (locked for this phase)

These decisions are made and documented. They can be revisited for v1.0 but are not open for this training phase.

| Decision | Status | Rationale | Revisit trigger |
|----------|--------|-----------|-----------------|
| v0.3 syntax frozen | **LOCKED** | 100% compilation proves syntax works | Community feedback for v1.0 |
| No inline comments | **LOCKED** | Token efficiency; reasoning via `.tkc` companion files ([reasoning-channel.md](reasoning-channel.md)) | If functional Pass@1 stalls below 30% AND reasoning is identified as root cause |
| 55-char alphabet | **LOCKED** | BPE tokenizer trained on this; 52% reduction measured | v1.0 RFC only |
| 13 keywords | **LOCKED** | Compiler, spec, training all aligned | v1.0 RFC only |
| Purpose-built BPE | **LOCKED** | 16K vocab, 52% measured reduction | Retrain on expanded corpus (same approach) |
| Qwen base model | **OPEN** | Gate 2 used Qwen 2.5 Coder 7B; other bases viable | If Qwen3-Coder-Next or DeepSeek outperforms |
| GRPO/RLVR vs SFT-only | **OPEN** | Research strongly recommends GRPO for functional correctness | Adopt GRPO in next training run |
| Strict vs adaptive curriculum | **OPEN** | Research recommends adaptive over strict-phased | Try adaptive; keep strict as fallback |
| Python decompiler view | **PLANNED** | Research identifies as Day-1 requirement for enterprise | Ship in v0.4 as `toke --python-view` |

## Open Questions (decisions deferred)

These are explicitly not decided yet. Each has a decision deadline and fallback.

| Question | Options | Deadline | Fallback |
|----------|---------|----------|----------|
| Should we pivot to "Python-subset + custom BPE + linter"? | A: Continue toke, B: Pivot | Week 12 GO/NO-GO (mid-Aug) | If functional Pass@1 < 35% at week 12, publish honest post-mortem and evaluate pivot |
| Vocabulary extension vs replacement for base model? | A: Extend Qwen vocab, B: Replace entirely | Before next training run | Extend (lower risk, per arXiv 2402.01035) |
| Community contribution model? | A: Open corpus (HuggingFace), B: Curated submissions only | After first community feedback round | Start with B, move to A if quality holds |
| v1.0 syntax changes? | A: Keep v0.3 as-is, B: RFC process for changes | After Gate 3 | No changes before Gate 3 |

---

## Current State

| Asset | Status |
|-------|--------|
| Compiler (toke 0.3.1) | Production — 22 W1020 hints, --migrate for LLM patterns |
| BPE tokenizer (16,384 tokens) | Trained on normalised v0.3 code. 52% avg reduction vs cl100k |
| QLoRA adapter (Gate 2) | 100% compile rate, ~8% functional |
| Corpus | 25,953 records (canonical prompt) |
| loke codebase | 698 .tk files, 87,318 lines — production, tested |
| MCP server | Built, not deployed — can collect new training data |
| Benchmark | 500 hidden tasks with test I/O, 200 eval tasks |

## Key Insight

The model now writes perfect toke syntax. This is a **force multiplier**: we can use the trained model to generate candidate solutions, filter by compilation, and build a much larger corpus of correct programs — a self-improvement loop.

## Research-Informed Priorities (from 23-May review)

The following recommendations from external research review are adopted:

1. **Randomise input values per training record.** The 67% argv-hardcoding rate is a corpus problem. Generate 5–20 distinct `(stdin, expected_stdout)` pairs per problem template. Embed expected behaviour in the prompt, not the code.

2. **GRPO/RLVR is the default post-training method for code.** Binary pass/fail reward from `toke --check` + test execution. ACECoder achieved +25pp on HumanEval-plus with 48 H100-hours. Adopt this approach.

3. **Adaptive curriculum, not strict-phased.** Self-Evolving Curriculum and RECRL show adaptive mixed-sampling outperforms hard phase gates. The 6-phase ladder in this doc is a starting framework; implementation should use adaptive sampling across difficulty levels.

4. **Cap multi-turn repair at 3 rounds.** Evidence is consistent: +6pp / +1.7pp / +1pp per round (arXiv 2511.03898). Budget for 3, not more.

5. **Quality >> quantity for SFT.** LeetCodeDataset shows "SFT with only 2.6K model-generated solutions achieves performance comparable to 110K-sample counterparts." Focus on execution-verified records, not volume.

6. **Out-of-band reasoning channel is mandated.** See [reasoning-channel.md](reasoning-channel.md). The model reasons in `(* *)` blocks or `.tkc` companion files; reasoning tokens don't inflate source.

7. **Ship `toke.python_view` decompiler.** Without human-readable translation, toke fails enterprise procurement. Planned for v0.4.

8. **DeepSeek V3.2 at $0.42/Mtok for bulk corpus generation.** 60x cheaper than Opus. Default to DeepSeek for volume; reserve frontier models for hard curriculum tiers and LLM-judge work.

9. **The strongest parts of the project are the tokenizer and structured diagnostics.** Lead with measurable token reduction (52%) and the 70+ diagnostic codes. The syntax is instrumental — a means to achieve token reduction — not the core thesis.

10. **Pre-register success criteria before training.** Functional Pass@1 ≥ 35% and argv-generalisation ≥ 50% as the week-12 GO/NO-GO call.

---

## Training Strategy Options

### Option A: Enterprise Scale (Unlimited Resources)

**Scenario:** An AI company wants the best possible toke model on top-end hardware.

| Parameter | Value |
|-----------|-------|
| Base model | Llama 3.1 70B or Qwen 2.5 Coder 32B |
| Training hardware | 8x H100 80GB (DGX pod), ~$25/hr |
| Training method | Full fine-tune (not LoRA) → distillation to 7B |
| Corpus target | 500K–2M records |
| Training time | 72–168 hours |
| Estimated cost | $1,800–4,200 (compute only) |

**Corpus generation strategy:**
1. **Seed:** Current 25,953 records + loke (87K lines) + moke patterns
2. **Self-play:** Use Gate 2 model to generate 100K candidates, filter by compile+test
3. **Multi-model:** Generate with Claude/GPT-4/Llama, verify with toke --check
4. **Execution-verified:** Run each solution against test cases, keep only functionally correct
5. **Curriculum:** Grade difficulty A→D, train in stages
6. **Repair loops:** Generate → compile → diagnose → fix → verify (3 iterations)

**Training phases:**
1. Pre-train adaptation (70B): 500K records, full context (4096 tokens)
2. Instruction tuning: 50K curated I/O-correct records with diverse algorithms
3. RLCF (Reinforcement Learning from Compiler Feedback): reward = compiles + passes tests
4. Distillation: 70B teacher → 7B student on 200K records
5. DPO (Direct Preference Optimisation): correct vs incorrect solutions

**Expected outcome:** >90% functional Pass@1 on 7B distilled model

---

### Option B: Cloud Training (Mid-Range)

**Scenario:** Solo developer or small team, cloud GPU budget $200–500.

| Parameter | Value |
|-----------|-------|
| Base model | Qwen 2.5 Coder 7B-Instruct |
| Training hardware | 1x A100 40GB ($1.50/hr) or A10G 24GB ($0.80/hr) |
| Training method | QLoRA (rank 64–128) |
| Corpus target | 100K–250K records |
| Training time | 48–120 hours |
| Estimated cost | $100–400 |

**Corpus generation strategy:**
1. **Self-improvement loop:** Use Gate 2 model to generate 50K candidates, compile-filter
2. **Execution filter:** Run compiled solutions against test I/O, keep correct ones
3. **loke mining:** Extract more patterns from the 698-file codebase with context
4. **Synthetic expansion:** Vary existing correct solutions (rename vars, reorder, refactor)
5. **MCP collection:** Deploy model via MCP, collect user-verified code over time

**Training phases:**
1. QLoRA on 100K execution-verified records (3 epochs, ~72 hours on A10G)
2. Second pass: train on repair pairs (broken → fixed) for self-correction
3. Temperature sweep evaluation at each checkpoint

**Expected outcome:** 50–70% functional Pass@1

---

### Option C: Local Hardware (Mac Mini/Studio Cluster)

**Scenario:** Developer with Apple Silicon hardware, no cloud costs.

| Parameter | Value |
|-----------|-------|
| Base model | Qwen 2.5 Coder 7B (MLX format, 4-bit) |
| Training hardware | Mac Studio M4 Max 128GB or 3x Mac Mini M4 Pro |
| Training method | MLX LoRA (rank 32–64) |
| Corpus target | 50K–100K records |
| Training time | 120–240 hours (slower than GPU) |
| Estimated cost | $0 (hardware owned) |

**Advantages:**
- Zero recurring cost
- Data never leaves local network
- Can run continuously
- MLX native inference for deployment

**Limitations:**
- ~3–5x slower than A100 for training
- Max ~7B parameters (14B possible on 128GB but slow)
- No multi-node training without custom work

**Corpus generation strategy:**
- Same as Option B but generation is also local (Ollama/MLX inference)
- Slower iteration cycles but completely private

**Expected outcome:** 40–60% functional Pass@1

---

### Option D: Community-Driven (Open Source)

**Scenario:** Build the corpus collaboratively through community usage.

| Parameter | Value |
|-----------|-------|
| Base model | Multiple (Qwen 7B, Llama 8B, Mistral 7B) |
| Training hardware | Community-contributed (spot instances, donated compute) |
| Training method | LoRA adapters shared via HuggingFace |
| Corpus target | Unbounded (grows with community) |
| Training time | Continuous |
| Estimated cost | Distributed |

**Data collection mechanisms:**
1. **MCP server:** Deployed toke-mcp records all code that passes `toke --check`
2. **GitHub integration:** Public repos using toke contribute to corpus (opt-in)
3. **Editor telemetry:** VS Code extension records accepted completions (opt-in, anonymised)
4. **Playground:** tokelang.dev playground logs successful programs
5. **Bounty system:** Community solves benchmark tasks, verified solutions enter corpus

**Training infrastructure:**
- Public corpus on HuggingFace (versioned, deduplicated)
- Monthly model retraining by project maintainers
- Community can train their own adapters and publish
- Leaderboard for adapter quality (Pass@1 on held-out set)

**Expected outcome:** Improves over time; 60–80% functional within 6 months of active community

---

## Curriculum Learning (Phased Training)

The model should learn toke progressively, mirroring how a human learns a language. Each phase builds on the previous, with the model only advancing when it demonstrates mastery at the current level.

### Phase 1: Token Completion
**Objective:** Complete a partial line of toke code.
**Format:** `<prefix>___` → fill the blank
**Examples:**
- `let x=mut.` → `0;`
- `lp(let i=0;i<` → `n;i=i+1){`
- `f=add(a:i64;b:` → `i64):i64{<a+b};`

**Corpus size:** 10,000 records (extract from existing corpus by splitting at random points)
**Success metric:** >95% valid completion (compiles when reassembled)

### Phase 2: Single Statement
**Objective:** Write one complete statement given context.
**Format:** Function signature + preceding statements → next statement
**Examples:**
- Given `f=sum(arr:@i64):i64{let acc=mut.0;` → `lp(let i=0;i<arr.len;i=i+1){acc=acc+arr.get(i)};`
- Given `if(n<0){` → `<0-n`

**Corpus size:** 20,000 records
**Success metric:** >90% syntactically valid, >60% semantically appropriate

### Phase 3: Single Function
**Objective:** Write a complete function body given its signature and description.
**Format:** Signature + docstring → complete function
**Examples:**
- `f=abs(n:i64):i64` + "Return absolute value" → `{if(n<0){<0-n}el{<n}};`
- `f=max(a:i64;b:i64):i64` + "Return larger value" → `{if(a>b){<a}el{<b}};`

**Corpus size:** 30,000 records (existing corpus is primarily this format)
**Success metric:** >85% compile, >50% functionally correct

### Phase 4: Multi-Function Program
**Objective:** Write a complete program with multiple functions that work together.
**Format:** Task description + I/O examples → complete module
**Examples:**
- "Sort an array using quicksort" → module with partition + quicksort + main
- "Fibonacci with memoisation" → module with cache struct + fib + main

**Corpus size:** 15,000 records
**Success metric:** >80% compile, >40% functionally correct

### Phase 5: Multi-Module System
**Objective:** Write a system spanning multiple modules with imports.
**Format:** Architecture description → multiple .tk files with correct cross-module references
**Examples:**
- "REST API with user CRUD" → main.tk + routes.tk + db.tk + models.tk
- "CLI tool with config and logging" → main.tk + config.tk + logger.tk

**Corpus size:** 5,000 records (extracted from loke's multi-module patterns)
**Success metric:** >70% compile (including cross-module resolution), >30% functionally correct

### Phase 6: Full Application
**Objective:** Design and implement a complete application given high-level requirements.
**Format:** Requirements document → project scaffold with all files
**Examples:**
- "Build a bookmark manager with HTTP API and SQLite storage"
- "Build a log aggregator that tails multiple files and serves a dashboard"

**Corpus size:** 1,000 records (hand-curated from ooke/loke/moke examples)
**Success metric:** Compiles as a unit, serves/runs without crash

### Training Schedule

| Phase | Duration | Builds On |
|-------|----------|-----------|
| 1 (completion) | 8 hours | Base model |
| 2 (statement) | 16 hours | Phase 1 adapter |
| 3 (function) | 24 hours | Phase 2 adapter |
| 4 (multi-function) | 24 hours | Phase 3 adapter |
| 5 (multi-module) | 16 hours | Phase 4 adapter |
| 6 (application) | 8 hours | Phase 5 adapter |
| **Total** | **~96 hours** | |

Each phase uses the previous phase's adapter as starting point. Evaluation at each stage gates progression — if the model doesn't meet the success metric, extend training before advancing.

---

## Recommended Approach (updated 2026-05-23)

Dual-path: cloud-first validation now, local M5 Studio hybrid from October.

**Weeks 1–3: Corpus rebuild**
- Audit existing 25,953 records for argv-hardcoding; purge or rewrite
- Generate 5–20 randomised `(input, expected_output)` pairs per task template via DeepSeek V3.2 (~$80–160)
- Run self-improvement loop: Gate 2 model generates 50K+ candidates → compile + test filter → execution-verified corpus
- Target: 150K–200K execution-verified records
- Include reasoning traces in `(* *)` blocks for chain-of-thought training

**Weeks 4–6: SFT + GRPO**
- QLoRA SFT on execution-verified corpus (cloud spot, ~$150–300)
- GRPO/RLVR with binary reward: `compile_ok × 0.2 + tests_pass_fraction × 0.8`
- Adaptive curriculum sampling across difficulty levels (not strict-phased)
- Cap repair rounds at 3

**Weeks 7–9: Evaluation + MCP**
- Deploy `toke-mcp` server with `compile/test/repair/search_stdlib` tools
- Integrate with Claude Code / Cursor for dogfooding
- Evaluate: functional Pass@1, argv-generalisation rate, comparison vs Python baseline

**Week 12: GO/NO-GO decision (mid-August)**
- **GO criteria:** functional Pass@1 ≥ 35%, argv-generalisation ≥ 50%
- **If GO:** publish v0.4 (with `toke --python-view`), blog post, open governance discussion, begin community feedback round
- **If NO-GO:** publish honest post-mortem, evaluate pivot options (see Open Questions above)

**Weeks 13–28 (Sep–Dec):**
- If GO: community feedback, monthly retraining, MCP telemetry collection
- M5 Studio arrives October → persistent local inference + overnight QLoRA iteration
- Gate 3 attempt: ≥50% functional Pass@1 on 2+ model families

**Budget allocation (cloud, $10K total toke-attributable):**

| Bucket | Amount |
|--------|--------|
| Cloud GPU (SFT + GRPO sprints) | $6,000 |
| Synthetic data APIs (DeepSeek bulk + targeted Opus) | $2,300 |
| Eval / inference | $1,000 |
| Orchestration, storage | $500 |
| Reserve | $200 |

---

## Self-Improvement Loop (Key Innovation)

Because Gate 2 achieved 100% compilation, we can now:

```
┌─────────────────────────────────────────────────┐
│  1. Generate: Gate 2 model produces solutions   │
│  2. Compile:  toke --check filters syntax       │ 100% should pass
│  3. Execute:  Run against test I/O              │ Filter correct
│  4. Verify:   Keep only functionally correct    │ ~8% initially
│  5. Train:    Add to corpus, retrain            │ Model improves
│  6. Repeat:   Improved model → more correct     │ Positive feedback
└─────────────────────────────────────────────────┘
```

Each iteration should improve the functional rate. With 500 tasks × 20 samples × 5 temperatures = 50,000 attempts, even at 8% success rate that's ~4,000 verified solutions per iteration.

---

## MCP Data Collection Architecture

```
Developer using toke
       │
       ▼
┌─────────────────┐
│  toke-mcp       │  ← VS Code / Claude Code / IDE
│  (local server) │
└────────┬────────┘
         │ code passes toke --check
         ▼
┌─────────────────┐
│  Telemetry      │  ← Opt-in, anonymised, no PII
│  (local buffer) │
└────────┬────────┘
         │ batch upload (daily/weekly)
         ▼
┌─────────────────┐
│  Corpus Server  │  ← HuggingFace dataset / S3
│  (dedup, score) │
└────────┬────────┘
         │ monthly retrain trigger
         ▼
┌─────────────────┐
│  Training       │  ← Cloud or community compute
│  Pipeline       │
└─────────────────┘
```

**Privacy guarantees:**
- All code stripped of comments, strings anonymised if containing emails/keys
- No file paths, no project structure, no metadata beyond pattern category
- Users can review/delete their contributions
- Corpus is public (Apache 2.0) — no proprietary code

---

## Corpus Growth Projections

| Source | Records (Year 1) | Quality |
|--------|-------------------|---------|
| Self-improvement loop | 30,000–100,000 | Execution-verified |
| MCP collection (10 active devs) | 5,000–20,000 | Compile-verified |
| GitHub public repos | 1,000–5,000 | Context-rich |
| Community bounties | 500–2,000 | High quality |
| Synthetic variation | 50,000–200,000 | Varied but repetitive |
| **Total (conservative)** | **86,500** | |
| **Total (optimistic)** | **327,000** | |

---

## Decisions Required

1. **Immediate:** Deploy MCP with telemetry? (Unblocks data collection)
2. **Week 1:** Run self-improvement loop on existing infrastructure?
3. **Month 1:** Cloud budget allocation for next training run?
4. **Month 2:** Open-source the corpus to HuggingFace?
5. **Month 3:** Community contribution program design?

---

## Gate 3 Criteria (Proposed)

| Criterion | Threshold |
|-----------|-----------|
| Functional Pass@1 (hidden tasks) | >50% |
| Compilation rate | >95% (maintain) |
| I/O pattern adherence | >80% solutions read from argv |
| Model families tested | ≥2 (Qwen + one other) |
| Self-improvement demonstrated | Iteration N+1 > Iteration N on held-out set |
| Python decompiler view | Shipped (`toke --python-view`) |
| Reasoning channel | `.tkc` companion files documented and used in training |

## Version Roadmap

| Version | Scope | Status |
|---------|-------|--------|
| v0.3 | Syntax, spec, compiler, 38 stdlib modules | **LOCKED** — no changes |
| v0.3.x | Bug fixes, new stdlib modules, compiler improvements | Active |
| v0.4 | `.tkc` companion file format, `toke --python-view`, MCP v1 | Planned (post-Gate 3) |
| v1.0 | Community RFC process for syntax changes, formal governance | After community feedback |

**v0.3 is the stable target for all training, tooling, and community engagement.** Syntax experiments and breaking changes are deferred to a v1.0 RFC process that will incorporate community feedback, research findings, and the results of Gates 2–4.

## Branching and Rollback Strategy

All decisions in this document are reversible:

- **Corpus:** versioned in `toke-model/training-data-v03/`. Previous versions archived. Any training run can be reproduced from a specific corpus version.
- **Adapters:** each training run produces a named adapter directory with full config. Previous adapters are kept for A/B comparison.
- **Tokenizer:** `tokenizer_v03.json` is versioned. Retraining produces a new file; the old one is preserved.
- **Spec:** git-versioned. Any spec change goes through a PR with rationale.
- **Pivot option:** if the GO/NO-GO at week 12 is NO-GO, the pivot path ("Python-subset + custom BPE + linter") is documented and preserves all tokenizer and diagnostics work. This is not failure — it's a valid alternative that captures the empirically strongest components.
