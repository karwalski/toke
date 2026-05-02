# Corpus Preparation Plan — 1B Purpose-Built Model

**Story:** 81.2
**Status:** planned
**Date:** 2026-05-01

---

## 1. Corpus Requirements

All programs in the training corpus must conform to the current toke default syntax:

- **55-char alphabet** — no characters outside the defined set
- **No comments** — toke is comment-free by design; no `(* ... *)` blocks
- **No underscores in identifiers** — concatenated lowercase only (e.g. `getvalue`, not `get_value`)
- **`mt expr {...}` match syntax** — not the legacy `|{` form
- **`&name` function references** — not bare names in reference position
- **`$` type sigils** — e.g. `$i64`, `$str`
- **`@` collection sigils** — e.g. `@()` for arrays and maps
- **All programs compile** — `tkc --check` returns exit code 0

---

## 2. Corpus Sources

### 2a. Existing Phase 1 corpus (~46K programs)

Mechanically transform from legacy to default syntax, then verify compilation. Expected ~60-70% survival rate after transformation. Losses come from programs that use constructs not cleanly mappable (nested underscores in identifiers that produce ambiguous concatenation, etc.).

### 2b. Existing Phase 2 corpus (partial)

Already partially transformed. Needs syntax updates for:
- `mt`/`&name` keyword changes
- Underscore removal from identifiers

### 2c. New generation

Use current LLMs (Claude, GPT) with the `spec-prompt.md` system prompt to generate fresh programs directly in default syntax. Higher quality starting point than mechanical transformation — no lossy conversion step.

### 2d. stdlib examples

Extract working code snippets from stdlib `.md` documentation. These are curated, human-reviewed examples that exercise real API surfaces.

### 2e. Test suite

Conformance tests and end-to-end tests are compilable programs. Include those that pass `tkc --check` in default syntax mode.

---

## 3. Transformation Pipeline

For sources 2a and 2b (existing corpora that need conversion):

| Step | Operation | Method |
|------|-----------|--------|
| 1 | Mechanical syntax transformation | Legacy to default: uppercase type names to `$` sigils, `[]` to `@()`, etc. |
| 2 | Underscore removal | sed-based, word-boundary safe (e.g. `get_value` → `getvalue`) |
| 3 | Match syntax update | `\|{` → `mt ... {` |
| 4 | Comment stripping | Remove all `(* ... *)` blocks |
| 5 | Compilation validation | `tkc --check`; discard failures |
| 6 | Deduplication | Exact match removal + near-duplicate detection (normalised edit distance < 0.1) |

Each step is idempotent. Programs that fail step 5 are logged with the diagnostic output for later analysis but excluded from the corpus.

---

## 4. Quality Filtering

After transformation and deduplication, apply the following filters:

- **Must compile** — `tkc --check` returns 0
- **Must be well-formed** — no empty function bodies, no trivial programs (fewer than 3 statements)
- **Diversity scoring** — category coverage across: string, math, IO, HTTP, JSON, error handling, type definitions, generics, concurrency
- **Length distribution target** — 80% of programs between 50-500 tokens (measured by the purpose-built tokenizer)

Programs outside the length distribution are not discarded but down-weighted during training.

---

## 5. Curriculum Ordering

Training proceeds in tiers of increasing complexity:

| Tier | Content | Purpose |
|------|---------|---------|
| 1 | Single-function programs | stdlib usage, basic control flow, literals, expressions |
| 2 | Multi-function programs | Function calls, type definitions, error handling with `mt` |
| 3 | Multi-module programs | Imports, module structure, interface files |
| 4 | Complex programs | HTTP servers, JSON APIs, database access, concurrency |

Within each tier, programs are shuffled. The model sees all of Tier 1 before Tier 2, etc. This mirrors how language learners progress from simple to compound constructions.

---

## 6. Target Corpus Size

| Metric | Minimum | Target |
|--------|---------|--------|
| Validated programs | 50K | 100K-200K |
| Estimated tokens | 5M | 5M-20M |

The target range is small by LLM standards, but toke's compact syntax means fewer tokens per semantic unit. Augmentation techniques to reach the target:

- **Name variation** — rename identifiers to produce semantically equivalent programs with different surface forms
- **Literal parameterisation** — vary numeric and string literals while preserving program structure

---

## 7. Tokenizer Alignment

The purpose-built BPE tokenizer must be trained on this corpus (not the old Phase 1 tokenizer):

| Parameter | Target |
|-----------|--------|
| Vocabulary size | 8K-16K tokens |
| Expected compression | Common patterns like `f=`, `:i64`, `<0;` become single tokens |
| Fertility | < 1.5 tokens per semantic unit |

The tokenizer is trained before the model. The corpus token counts in section 6 are measured with the new tokenizer, not the old one. If the tokenizer changes, recount.
