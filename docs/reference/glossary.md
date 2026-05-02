---
title: toke Glossary — Terminology Reference
slug: glossary
section: reference
order: 5
---

**Status:** Normative
**Frozen:** 2026-03-28 (updated 2026-04-04)
**Authority:** toke-spec-v02.md, epics_and_stories.md, this file

---

## Four Distinct Meanings of "Phase"

This project uses the word "phase" in four distinct senses. To eliminate ambiguity, each sense has a canonical term.

---

### 1. PROJECT_PHASE

**Canonical term:** Project Phase (numbered 1–4)

**Definition:** The numbered execution timeline of the toke research project. There are four project phases with explicit go/no-go gates.

| Phase | Name | Duration | Gate |
|-------|------|----------|------|
| 1 | Falsification | Months 1–6 | Gate 1 (Month 8) |
| 2 | Language Viability | Months 6–14 | Gate 2 (Month 18) |
| 3 | Ecosystem Proof | Months 14–26 | Gate 3 (Month 26) |
| 4 | Standard Pathway | Months 26–32 | Gate 4 (Month 32) |

**Usage in docs:** "project Phase 1", "project Phase 2", "Phase 1 go/no-go gate". Never abbreviated to P1/P2.

**Do not rename.** These terms are part of the project planning framework and appear in cost models, milestone charts, and deliverable lists.

---

### 2. LANGUAGE_PROFILE → Default Syntax / Legacy Syntax

**Canonical term:** Default syntax (toke syntax) and Legacy syntax (legacy profile)

**Definition:** The two character-set profiles of the toke source language.

| Profile | Characters | Tokenizer | Status |
|---------|-----------|-----------|--------|
| Legacy (80 characters) | 26 lower + 26 upper + 10 digits + 18 symbols | Compatible with cl100k_base and existing LLM tokenizers | Historical — used for bootstrapping |
| Default (55 characters) | 26 lower + 10 digits + 19 symbols, including `$` and `@` | Purpose-built BPE tokenizer trained on legacy corpus | Current — the normative toke syntax |

The default syntax eliminates all 26 uppercase letters. Type names use a `$`-prefixed lowercase form. Array literal syntax `[...]` becomes `@(...)`.

**CLI flags:** `--profile1` (legacy), `--profile2` (default). The deprecated aliases `--phase1` and `--phase2` are accepted with a warning.

**Authoritative spec files (in toke repo, spec/ directory):**
- `spec/spec/character-set.md`
- `spec/spec/phase2-transform.md`
- `spec/spec/grammar.ebnf`

---

### 3. CORPUS_PHASE

**Canonical term:** Corpus Phase (lettered A–E)

**Definition:** The training curriculum tiers for corpus generation. Each tier introduces progressively more complex toke programs.

| Tier | Name | Description |
|------|------|-------------|
| A | Primitives | Basic arithmetic, boolean logic, simple functions |
| B | Data Structures | Structs, arrays, sum types, pattern matching |
| C | System Interaction | File I/O, HTTP, database access, error handling |
| D | Applications | Multi-function modules, concurrency, FFI |
| E | Complex Systems | Full applications, self-improvement tasks |

**Usage in docs:** "Phase A corpus", "Phase B tasks", "corpus phase A–C". The `"phase"` field in corpus schema.json and task YAML files contains these letter values ("A", "B", "C", etc.).

**Do not rename.** The schema field name `"phase"` in corpus and benchmark JSON/YAML is a CORPUS_PHASE field and is NOT the same as the compiler diagnostic `"stage"` field.

---

### 4. COMPILER_STAGE

**Canonical term:** Compiler stage (e.g., lex stage, parse stage)

**Definition:** A named step in the tkc compiler pipeline. Stages are sequential and produce diagnostics tagged with the stage name.

| Stage | Description |
|-------|-------------|
| `lex` | Lexical analysis (tokenisation) |
| `parse` | Syntactic analysis (AST construction) |
| `import_resolution` | Module import resolution |
| `name_resolution` | Identifier binding and scope resolution |
| `type_check` | Type checking and inference |
| `arena_check` | Arena memory discipline validation |
| `ir_lower` | Intermediate representation lowering |
| `codegen` | Machine code generation via LLVM |

**In JSON diagnostics:** The `"stage"` field in the diagnostic schema contains these values. The field was formerly named `"phase"` — the renamed field is load-bearing and used by automated repair loops.

**Authoritative spec:** toke-spec-v02.md §18.1, Appendix B (JSON schema).

---

## Quick Reference

| Term you see | Canonical meaning | Action |
|-------------|------------------|--------|
| "Phase 1 character set" | Legacy profile | → "legacy character set" |
| "Phase 2 sigil syntax" | Default syntax | → "default syntax (sigil encoding)" |
| "Phase A corpus" | Corpus Phase A | Leave unchanged |
| "project Phase 1" | Project Phase 1 | Leave unchanged |
| `"phase": "type_check"` in JSON | Compiler stage | → `"stage": "type_check"` |
| "lex phase" / "parse phase" | Compiler stage | → "lex stage" / "parse stage" |
| "Profile 1" / "P1" | Legacy profile | → "legacy profile" or "legacy syntax" |
| "Profile 2" / "P2" | Default syntax | → "default syntax" or "toke syntax" |

---

*Frozen 2026-03-28 (terminology updated 2026-04-04). Changes require a spec amendment.*
