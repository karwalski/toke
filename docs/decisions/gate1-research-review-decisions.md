# Gate 1 Research Review — Decisions Required

**Source:** 8 research teams (T1–T8) + 2 specification reviews, April 2026
**Status:** All 20 decisions resolved
**Last updated:** 2026-04-04

---

## How to use this document

Each decision lists the conflicting recommendations, which teams raised it, and the options available. Decisions are grouped by urgency. Mark each as DECIDED with the chosen option and rationale when resolved.

---

## BLOCKING — Resolve before external review

### D1. Token efficiency metric definition and reconciliation

**Issue:** README/website claim "2.5–4x token density improvement" but Gate 1 measured 12.5% reduction. These are different metrics (cross-language comparison vs same-language custom-BPE improvement) but presented without disambiguation.

**Raised by:** T1 (all three sources), T6, T7, T8

**Options:**
- A) Define two separate metrics: "cross-language token density" (toke vs Python/C/Java via cl100k_base) and "same-language tokenizer improvement" (cl100k_base vs custom BPE on toke). Report both with clear labels.
- B) Drop cross-language comparison from headline claims; lead with 12.5% same-tokenizer improvement only.
- C) Redefine single "Token Efficiency Measurement Spec" (TEMSpec) as versioned artifact covering all measurement types.

**Recommendation:** Option A + C combined. Define TEMSpec, report both metrics clearly.

**Decision:** 12.5% reduction was for the legacy 80-char format. website also clearly mentions best practice and full program expansion. leave both as is, we will update when we have new numbers after default syntax work.... so A.

---

### D2. Integer overflow semantics

**Issue:** No explicit decision documented. Affects correctness, safety, and performance.

**Raised by:** T2 (all three sources), T4, T8

**Options:**
- A) Undefined behavior on overflow (like C) — emit `add nsw`. Fastest but dangerous for LLM-generated code.
- B) Wrapping semantics (like Rust release) — emit plain `add i64`. Safe, moderate performance.
- C) Checked arithmetic (like Rust debug) — emit `llvm.sadd.with.overflow.i64`, trap on overflow. ~11.8% SPEC overhead but eliminates ~43% of checks at -O3. Safest for LLM target.
- D) Hybrid — wrapping default with opt-in `@checked` annotation for safety-critical paths.
- E) Hybrid inverted — checked default with `@wrapping` annotation for performance paths. (T2 recommended for LLM target.)

**Recommendation:** Option E — checked by default is safest for LLM-generated code with escape hatch for performance.

**Decision:** E

---

### D3. Memory model and allocation strategy

**Issue:** Formal semantics document defers memory model. Arena-only allocation has documented failure modes (growing data structures, caches, connection pools, long-running loops). Without a published memory model, performance and optimization work is blocked.

**Raised by:** T2, T4, T8 (specification reviews)

**Options:**
- A) Arena/region-based with compile-time escape analysis (formalize and implement soundness checks).
- B) General heap with ownership conventions (manual malloc/free, documented).
- C) Stack-oriented for non-escaping temporaries + heap for escaped allocations.
- D) Hybrid: arena for request-processing patterns + explicit allocator API for long-lived data.
- E) Defer to Phase 3 and document supported workload patterns in the interim.

**Recommendation:** Option D — hybrid approach matching documented use cases. Explicitly state which patterns are supported and which are not.

**Decision:** D

---

### D4. Concurrency model: ship in v1 or defer?

**Issue:** Compiler already has spawn/await AST nodes and pthread stubs, but T2 strongly recommends deferring concurrency to post-v1 based on CONCUR benchmark evidence that LLMs fail at concurrent code generation. T4 wants std.thread for ecosystem completeness.

**Raised by:** T2 (strongly defer), T4 (wants std.thread), T8

**Options:**
- A) Ship spawn/await as-is with pthread backend. Accept LLM generation quality risk.
- B) Remove spawn/await from language; provide concurrency only via C FFI (extern pthreads). Add std.thread/std.sync modules later.
- C) Keep spawn/await in spec but mark as "experimental/unstable". Ship FFI-based concurrency as primary path.
- D) Structured task runtime with work-stealing scheduler (T2 preference if concurrency added).

**Recommendation:** Option C — keep in spec, mark experimental, primary path via FFI for now.

**Decision:** B

---

### D5. Corpus data governance and provenance

**Issue:** Corpus pipeline uses outputs from proprietary models (Claude, GPT-4.1-mini, Grok-3-mini). Provider terms may restrict using outputs to train competing models. Corpus is stored locally, not in repo, preventing independent verification.

**Raised by:** T3, T7 (legal risk), T6 (reproducibility), T8

**Options:**
- A) Establish strict separation: training corpus generated only by open-weight models and/or humans. Proprietary model outputs used only for evaluation/critique, stored separately, excluded from training data.
- B) Accept legal risk; document provenance but don't restrict sources.
- C) Regenerate corpus entirely using open-weight models (Qwen, Llama) to create clean-room training path.
- D) Hybrid: keep existing corpus for internal development; create separate "clean" corpus for public release.

**Recommendation:** Option A for new corpus; Option D for transition period.

**Decision:** Option A for new corpus; Option D for transition period.

---

### D6. Diagnostic format: custom JSON vs SARIF vs hybrid

**Issue:** Current custom JSON diagnostics lack normative schema, versioning, and structured edit suggestions. T8/specification reviews recommend adopting SARIF (OASIS standard used by GCC 13+, Clang 15+).

**Raised by:** T2, T4, T5, T8

**Options:**
- A) Adopt SARIF as primary format with Toke-specific extensions.
- B) Keep custom format but publish normative JSON Schema/JTD, add versioning semantics and compatibility contract.
- C) Hybrid — emit both Toke-native (for repair loop) AND SARIF (for CI/tooling integration).

**Recommendation:** Option C — repair loop benefits from custom format, CI/tooling benefits from SARIF.

**Decision:** Option C

---

### D7. Base-64 / 6-bit storage encoding proposal

**Issue:** Specification proposes 6-bit storage encoding for ~25% file-size savings. T1, T7, T8 strongly recommend against it: introduces debugging friction, breaks git/diff/editors, no precedent in production use, and no reversible fallback.

**Raised by:** T1, T7, T8

**Options:**
- A) Deprecate/remove from specification entirely.
- B) Keep as optional, non-default encoding with mandatory text-mode fallback.
- C) Defer to Phase 4+ as research item only.

**Recommendation:** Option A — remove from specification. File-size savings are marginal compared to tooling cost.

**Decision:** C. Potentially used just for shipping compressed packages etc.

---

## HIGH PRIORITY — Resolve during Phase 2

### D8. Interface file extension: .tki vs .tokei

**Issue:** Spec describes `.tki`, compiler README advertises `.tokei`, stdlib ships `.tki` files. Must choose one canonical extension.

**Raised by:** T4 (all sources), T6

**Options:**
- A) Standardize on `.tki` (shorter, consistent with current shipped files).
- B) Standardize on `.tokei` (more descriptive).
- C) Support both with transitional period and defined deprecation date.

**Recommendation:** Option A — `.tki` is already shipped and shorter (token-efficient).

**Decision:** A. Update all references

---

### D9. Document venue: IETF RFC format vs alternative

**Issue:** Programming languages are standardized via ECMA, ISO/IEC, W3C, or language-specific governance — not IETF RFCs. RFC 2119 keywords are designed for protocol interoperability, not language semantics. Using RFC format may reduce credibility.

**Raised by:** T8 (Grok)

**Options:**
- A) Keep RFC format for internal governance but don't submit to IETF. Rename to "Toke Language Report" or "Toke Specification".
- B) Keep RFC format as-is; acknowledge non-standard venue in document.
- C) Migrate to Haskell/R5RS-style standalone language report.
- D) Adopt Rust-style community RFC process for language changes.

**Recommendation:** Option A — keep format for internal structure, rename to avoid IETF legitimacy confusion.

**Decision:** A. Toke Specification. also rename folder.

---

### D10. QLoRA vs full fine-tuning vs DoRA

**Issue:** T3/T8 literature shows QLoRA may be insufficient for encoding entirely new languages (LoRA peaked at 0.407 vs 0.497 full fine-tuning on code tasks). DoRA consistently outperforms LoRA by +3.7 to +4.4 points. Full fine-tuning on 7B feasible on single A100.

**Raised by:** T3 (Claude review), T8

**Options:**
- A) Continue QLoRA as primary approach (current plan). Accept 10-20% quality gap.
- B) Switch to DoRA (weight-decomposed low-rank adaptation). Similar cost, better quality.
- C) Move to full fine-tuning for 7B (requires A100/H100 but eliminates quantization loss).
- D) Benchmark all three (QLoRA, DoRA, full) on same corpus and decide based on Pass@1 delta.

**Recommendation:** Option D — benchmark before deciding. If budget constrained, try DoRA first.

**Decision:** B

---

### D11. Embedding layer training for custom tokenizer

**Issue:** Custom BPE tokenizer introduces tokens absent from Qwen vocabulary. Standard QLoRA trains adapter matrices only — cannot update embedding weights for unknown tokens. T3 states this is "not optional" — without training `lm_head` and `embed_tokens`, model produces random outputs for Toke-specific tokens.

**Raised by:** T3 (Claude review — marked as critical)

**Options:**
- A) Add `lm_head` and `embed_tokens` to `modules_to_save` in QLoRA config. Initialize new token embeddings by averaging sub-word embeddings.
- B) Use Qwen's existing tokenizer (no custom tokens) and accept higher token counts.
- C) Full fine-tuning (bypasses issue entirely).

**Recommendation:** Option A if continuing QLoRA/DoRA. This is a hard requirement.

**Decision:** A

---

### D12. Constrained decoding: competitor or complement?

**Issue:** Grammar-constrained decoding (XGrammar, Outlines) achieves near-zero overhead syntax enforcement in existing languages. T7 asks: why build a new language when you can constrain generation in Python? Toke must articulate differentiation.

**Raised by:** T7 (all sources), T8

**Options:**
- A) Position as complementary: Toke + constrained decoding = best of both worlds. LL(1) grammar makes constrained decoding maximally efficient.
- B) Position as alternative: Toke provides what constrained decoding cannot — token efficiency, compiled performance, structured diagnostics.
- C) Run ablation study: Toke + constrained decoding vs Python + constrained decoding on same tasks. Let data answer.

**Recommendation:** Options A + C — position as complement AND prove it with data.

**Decision:** C

---

### D13. Corpus publication strategy

**Issue:** 46,754-program corpus is stored locally, not in repo. Multiple teams require at least partial publication for reproducibility. Full publication risks benchmark contamination.

**Raised by:** T1, T3, T6, T7, T8

**Options:**
- A) Publish full corpus in toke-corpus repo.
- B) Publish corpus statistics + SHA hashes + representative samples (100-500 programs). Keep full corpus private.
- C) Publish curriculum/templates for corpus regeneration (not the 46K programs themselves).
- D) Publish "clean" subset generated only by open-weight models. Keep proprietary-model outputs private.
- E) Combination: publish statistics + regeneration scripts + clean subset.

**Recommendation:** Option E — maximum transparency while preserving benchmark integrity.

**Decision:** E

---

### D14. Gate 2 success criteria specificity

**Issue:** Gate 2 criterion "Extended features + 7B model beats baseline" is under-specified. No concrete baselines, evaluation protocols, or statistical confidence levels defined. T8 provides detailed proposed criteria.

**Raised by:** T8 (all sources)

**Options:**
- A) Adopt T8's full Gate 2 criteria (reproducibility, spec completeness, tooling, corpus 120K+, benchmark alignment, stdlib std.thread/std.bigint, governance, cost tracking).
- B) Define minimal Gate 2 criteria (Pass@1 ≥ 75%, token reduction ≥ 15%, corpus ≥ 100K, std.thread shipped).
- C) Define two-tier: "Gate 1.5" (reproducibility package within 30 days) + "Gate 2" (full criteria from T8).

**Recommendation:** Option C — immediate reproducibility gate, then full Gate 2.

**Decision:** A

---

### D15. Repository consolidation

**Issue:** 8+ repos creates coordination overhead. T6 recommends consolidation to 3-4 repos. Current structure fragments documentation and creates consistency issues.

**Raised by:** T6

**Options:**
- A) Keep 10-repo structure. Add automated cross-repo consistency checks.
- B) Consolidate to 4 repos: toke (spec + compiler + stdlib), toke-model (corpus + tokenizer + models), toke-eval (benchmark + eval), toke-web.
- C) Consolidate to 5 repos: core, ml, benchmark, eval, web.
- D) Keep current structure but add toke-audit repo with weekly coherence checks.

**Recommendation:** Option D for now — consolidation is high-effort and disruptive. Add consistency automation instead.

**Decision:** B. toke, toke-model, toke-eval, toke-web (DONE — consolidated April 2026)

---

### D16. "No comments" design philosophy vs audit requirements

**Issue:** Spec excludes comments for machine-first design. T5 notes this creates audit burden (96% of developers don't trust AI-generated code). Expansion/explanation tooling must fill the gap. T8/specification reviews flag this as adoption risk.

**Raised by:** T5, T8

**Options:**
- A) Maintain no-comments design. Invest heavily in expander/explanation tooling.
- B) Allow optional comments (stripped by tokenizer, preserved in source). Best of both worlds but adds complexity.
- C) External annotation files (sidecar .tki-doc files) with structured metadata.
- D) Compiler emits "explanation" artifacts as separate output (like source maps).

**Recommendation:** Option A + D — maintain design principle, invest in tooling to bridge gap.

**Decision:** A + hashing + large number of verifiable resusable components (dont keep reinventing new code doing same thing)

---

## MEDIUM PRIORITY — Phase 2 roadmap decisions

### D17. Package management timeline

**Issue:** All teams agree package management is critical for ecosystem growth. T4 recommends minimal `toke.toml` + lockfile for Phase 2. T8 estimates 12-18 person-months for MVP.

**Decision:** dont define timelines with persons/hours/months. continue to use markup documents throughout explaining for humans to read, not toml

### D18. Formatter design: opinionated (gofmt) vs configurable (rustfmt)

**Issue:** T5 strongly recommends opinionated canonical formatter shipped before expander tooling.

**Decision:** opinionated

### D19. Training infrastructure: MLX-only vs hybrid MLX+CUDA

**Issue:** MLX is 3-5x slower than CUDA. T8 recommends hybrid approach for Phase 2-3 scaling.

**Decision:** hybrid

### D20. Stdlib expansion priority order

**Issue:** Multiple teams want different modules first. T2 wants std.thread for benchmarks. T4 wants std.bigint. T8 wants 11 core modules to >80% spec coverage.

**Decision:** all core modules recommended

---

## INFORMATION ONLY — No decision needed

### Resolved by existing work
- Interface file format schema versioning: already using `schema_version` in .tki files
- Error code immutability: already policy ("never change error code meaning or number")
- Reproducible builds: already implemented (make repro-check)
- SBOM generation: already in tkc release workflow
- DCO enforcement: already implemented

### Noted but not actionable yet
- LSP server (12-60 person-months depending on scope) <- dont mention months
- Benchmarks Game submissions (need concurrency first)
- Self-hosting the compiler (long-term)
- HuggingFace publication (backlog stories 6.1.x exist)
