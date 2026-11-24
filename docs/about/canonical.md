---
title: "Canonical facts: how toke is described"
slug: canonical
section: about
story: 132.1
date: 2026-09-19
order: 12
---

**What this file is.** The single source of truth for every fact and every sentence used
to describe toke in public. The home page, `/llms.txt`, the README of every toke repo, the
Hugging Face model card and the PyPI, npm, VS Code and Ollama descriptions copy the blocks
in §9 **word for word**. The machine-readable copy is
[`canonical.json`](https://github.com/karwalski/toke/blob/main/docs/about/canonical.json) beside this file, and `make check-canonical`
(`scripts/check_canonical.py`) fails CI when a copy has drifted.

**Four rules.**

1. Copy the blocks in §9 verbatim. Do not paraphrase, do not drop a qualifier, do not
   re-wrap a number.
2. No number appears without its tokenizer and its N, and numbers come from
   [`docs/metrics-baseline.md`](https://github.com/karwalski/toke/blob/main/docs/metrics-baseline.md) only.
3. The name is `toke`, lower case, always. "Token-Optimised Language" is a descriptor,
   never the name.
4. If a fact changes, change it **here and in `canonical.json` first**, then re-copy
   downstream and re-run `make check-canonical`.

Every fact below cites the file it comes from. Where the fact is a number, the tokenizer
and the sample size are part of the fact.

---

## 1. Name

**toke** — lower case in every heading, sentence, description, README and registry field,
including sentence-initial position. Never "Toke", never "TOKE".

"Token-Optimised Language" is a **descriptor** and may be used as one ("toke, a
token-optimised language for LLM code generation"). It is never the name, never an
expansion presented as an acronym, and never a title.

*Source: `docs/progress.md`, Epic 132 acceptance criterion 1.*

## 2. Type

A compiled, statically typed, general-purpose programming language with an LLVM backend,
designed as a code-generation target for large language models.

Not a DSL, not a template language, not a serialization format, not a prompt syntax.

*Source: `docs/about/positioning-2026-09.md` §1; `docs/spec/toke-spec-v0.4.md`.*

## 3. Purpose, in one sentence

> toke is a compiled programming language designed for LLM code generation: small,
> strictly structured, and canonical, so that generated code is cheap to constrain while
> it is being produced and cheap for a compiler to verify once it is.

*Verbatim from `docs/about/positioning-2026-09.md` §1, v2 (story 132.7; copied here by story
132.154). This is the sentence every downstream surface quotes. v1 (commit 33f72e8) ended
with a third benefit, compactness "in whatever unit the model generates in"; the owner
dropped it on 2026-11-15, and it must not be re-copied.*

## 4. Language facts

| Fact | Value | Source |
|---|---|---|
| Spec version | **v0.4** — `docs/spec/toke-spec-v0.4.md` is the authority; v0.3 is historical and superseded | `docs/spec/toke-spec-v0.4.md` |
| Keywords | **14**: `m i t f let if el lp br rt as mt sc mut` | `toke-spec-v0.4.md` §A |
| Character set | **59** — 26 lowercase letters, 10 digits, 23 symbols; no uppercase, no underscores | `src/lexer.c` (ground truth); `docs/metrics-baseline.md` § Project facts |
| Grammar | **backtrack-free with bounded lookahead of up to 3 tokens** on a small, enumerated set of productions (E1–E5) | `toke-spec-v0.4.md` §E; `docs/spec/grammar.ebnf` Appendix A |
| Canonical form | one canonical form per construct, chosen by measurement in a **47-entry** pattern catalogue and reproduced by `tkc --min` | `docs/spec/idiom-v0.4.md`; `docs/spec/patterns-protocol-v0.4.md`; `patterns/catalogue.json` |
| Machine-readable grammar | `docs/spec/grammar.ebnf`, `docs/spec/toke.gbnf` | `toke-spec-v0.4.md` |

**Three corrections that this file exists to hold in place (stories 132.12, 132.14).**

- **toke is not LL(1).** `toke-spec-v0.4.md` §E retired the v0.3 strict-LL(1) claim on
  2026-07-02: it "was **not accurate** for the real grammar". The verified property is that
  the parser never rescans input it has already consumed, and that a small, **enumerated**
  set of productions require **bounded lookahead of up to 3 tokens**, never more — listed
  normatively with their FIRST-sets in Appendix A of `grammar.ebnf`. An implementation that
  backtracks, or that requires unbounded lookahead at any production, is non-conforming.
  **The mechanical argument is unchanged:** a small backtrack-free grammar with bounded
  lookahead is still cheap to constrain during decoding and cheap to parse — the pushdown
  automaton behind a decoding mask stays small and the masks stay cheap. The argument never
  depended on the label.
- **The keyword count is 14, not 13.** §A fixes the set at `m i t f let if el lp br rt as
  mt sc mut`, verified against the lexer keyword table: both `mut` and `sc` are keywords.
  The logical operators are lexical, not keywords.
- **The character set is 59, not 55 and not 56 (story 132.14).** Two normative documents
  disagreed and neither matched the compiler. `src/lexer.c` is the ground truth: in
  `PROFILE_DEFAULT` it rejects exactly eight printable ASCII characters in structural
  position with E1003 — `` ' , ? [ \ ] _ ` `` — and accepts every other symbol, giving 26
  lowercase + 10 digits + **23** symbols. The RFC's 56 omitted `%` and `&`, which are live
  operators, and called `^` and `~` "reserved and unassigned" after story **114.8** had
  assigned them bitwise XOR and bitwise NOT; the 55 wording omitted all four. Reproduce
  with `python3 scripts/verify_project_facts.py --probe`. The design property was never the
  number — it is that the alphabet is small and **closed** — so prefer "a closed alphabet
  of 59 printable ASCII characters, lowercase only" to a bare figure. See
  `docs/metrics-baseline.md` § Project facts, which is the single source for this and every
  other project-scale count.

## 5. Compiler

tkc, the reference compiler: C99; links only libc, libm, zlib and pthreads; emits LLVM IR
and drives clang to build native binaries for x86-64 and ARM64; structured diagnostics with
stable error codes, machine-parseable spans and a fix field. Current version: **toke 3.2.0**
(`tkc --version`). Apache-2.0.

The first sentence is the `compiler` fact in `canonical.json`, and `make check-canonical`
compares every copy of it word for word. It replaced "single-pass C99, zero dependencies
beyond LLVM" (story 132.141), which was wrong on both counts: tkc runs separate lexer,
parser, name-resolution, type-checking and IR-lowering stages, and it does not link LLVM.
It writes textual LLVM IR and runs clang (the absolute path baked in at build time, or
`TKC_CC`; E9027 if neither is an executable), so clang is a run-time requirement for
building a binary, though not for `--check`.

*Source: `src/main.c` (`VERSION`, `resolve_cc`); `Makefile` (`LDLIBS = -lm -lz -lpthread`);
`otool -L ./tkc` / `ldd ./tkc`; `src/llvm.c` (target triples); `docs/about/repos.md`.*

## 6. Tokenizer

No toke tokenizer currently beats a general-purpose one. The shipped 8K SentencePiece
model needs **15.4% more** tokens than cl100k_base on canonical v0.4 `tkc --min` text
(N = 2,000 stratified corpus records, 2026-09-18); the v0.3 16,384-vocab tokenizer only
appears to win (0.545×) because a null `unk_token` silently deletes every backslash
(2,606 in that sample). No "purpose-built tokenizer beats cl100k" claim is supportable
until the v0.4 tokenizer is trained and locked against the Phase-3 anchor of
cl100k_base = 242,427 tokens on that exact sample (story 116.9).

*Source: `docs/metrics-baseline.md`, § 2026-09-18 v0.4 tokenizer baseline (story 131.20).*

## 7. Benchmark

**toke-eval.** The 60 Gate-1 JSON-CLI tasks were re-delivered on v0.4 on 2026-09-19:
**60/60** `tkc --check`, **60/60** hidden tests (120 cases each), lint 0/0, and **4,787**
cl100k_base tokens of `--min` text against **3,565** for equivalent Python (N = 60).

**Caveat, which travels with the number.** The programs are hand-written, not
model-generated — 27 ids are pure `--migrate` output and 33 were hand-repaired — so the set
measures what the *language* can express, not what a *model* produces. It may not be quoted
as a model result, as a Pass@1, or as a reduction against Python.

*Source: `docs/metrics-baseline.md` (2026-09 rows, story 133.4);
`docs/about/toke-eval-drift-decision.md`.*

## 8. Models

No v0.4-native model exists, and no model has been trained since April 2026. The most
recent model gate run is **Gate 2, 2026-05-22**: Qwen 2.5 Coder 7B + QLoRA on v0.3 syntax —
**100% compile Pass@1** and **55.6% functional** (272/489) on the curated 500-hidden +
200-eval set, whose 200-eval half may be corpus-derived. The PASS recorded on it was
**withdrawn on 2026-11-15** (owner decision D4): **Gate 2 has no standing verdict**, and
Gate 3 never ran and is void. The honest floor is the full-local re-audit of all 1,748
v0.3.9 corpus programs: **37.5% compile** (655/1,748) and **about 2.2% fully correct** (38
PASS).

Never quote the 100% without the curated set it was measured on, never quote the 55.6%
without the caveat that the 200-eval half may be corpus-derived, never call Gate 2 passed
(its PASS was withdrawn on 2026-11-15, owner decision D4), and never present the August
2026 corpus freeze as a model gate — August produced a training-data quality freeze, not a
gate result.

*Source: `docs/metrics-baseline.md` § Correctness and § 2026-08 (story 132.0(a));
`docs/decisions/gate2-decision.md` (owner decision D4, story 127.459).*

## 9. The blocks to copy

Everything in this section is reproduced **word for word** downstream. `check_canonical.py`
compares each copy against `canonical.json`.

### One-liner (19 words)

> toke: a compiled language designed for LLM code generation, with a small grammar, one
> canonical form and compiler verification.

*Verbatim from `docs/about/positioning-2026-09.md` §9 (story 132.7).*

### Paragraph (79 words)

> toke is a compiled programming language designed for LLM code generation. It has 14
> keywords, a 59-character set, a backtrack-free grammar with bounded lookahead, and one
> canonical form per construct, chosen by measurement in a 47-pattern catalogue and
> reproduced by `tkc --min`. That makes generated code cheap to constrain during decoding
> and cheap for a compiler to verify afterwards. Token efficiency is one measured property
> of toke, always reported with its tokenizer and its baseline, not the whole claim.

*Verbatim from `docs/about/positioning-2026-09.md` §9, v2 (story 132.7; copied here by story
132.154). v1's 82-word paragraph ended its third sentence "and compact to emit"; the owner
dropped that on 2026-11-15. Story 131.145 (2026-11-24) changed "46-pattern" to "47-pattern"
when str-char-read became the catalogue's 47th entry; the count is derived as
`patterns_catalogue_entries` in `docs/metrics-baseline.md` § Project facts and gated by rule 4
of `scripts/check_metrics_claims.py`.*

### Token efficiency, short form

> **Token efficiency, measured:** under one shared tokenizer (cl100k_base) toke costs
> **1.34× [1.22, 1.48]** the tokens of equivalent Python on the 60 Gate-1 tasks (N = 60,
> 2026-09-19) — more, not fewer. The v0.3-era "52% fewer tokens" figure was a
> *tokenizer-vs-tokenizer* measurement on identical toke text (Toke-16K v0.3 vs cl100k_base,
> N = 42) and is superseded: on canonical v0.4 text the shipped 8K tokenizer needs **15.4%
> more** tokens than cl100k_base (N = 2,000). See `docs/metrics-baseline.md`.

*Verbatim from `docs/metrics-baseline.md` § Canonical wording for token-efficiency claims,
short form (story 132.6). The long form, for documentation and site pages, lives in the
same section.*

### Disambiguation (one line)

> toke is a programming language. It is not the slang word for a draw on a cigarette, not
> the cannabis brands that use the name, not the TOKE crypto tokens, not Tokelau or its
> `.tk` country-code domain, and not tokelang.com, which is an unrelated third-party
> project. The language is at tokelang.dev and github.com/karwalski/toke.

*Source: `docs/progress.md`, Epic 132 acceptance criterion 5. "tokelang" appears on the
site once — in this line — and nowhere else in new content.*

### Disambiguation (structured data)

> toke is a compiled programming language. It is unrelated to the slang term for smoking,
> to cannabis brands using the name, to the TOKE cryptocurrency tokens, to Tokelau or its
> .tk country-code domain, and to tokelang.com, which is an unrelated third-party project.

*Source: toke-website commit 077064e (owner, 2026-09-22). The home page does not show the
one-line disambiguation; it carries it as schema.org JSON-LD in the page head, as the
`disambiguatingDescription` of a `SoftwareApplication` whose `sameAs` lists
https://github.com/karwalski/toke and https://tokelang.dev (the two locations the visible
line names). This is the only form a surface may use in place of the visible line, and
`make check-canonical` checks it word for word like any other block (story 132.126).*

### Sub-projects (one line)

> ooke, loke and moke are toke sub-projects, not separate products: ooke is toke's web
> framework and static site generator, and it builds tokelang.dev (a toke binary on
> std.http serves it); loke is toke's local
> intelligence layer; moke is loke's data-analysis demo. All three are written in toke.

*Source: `docs/progress.md`, Epic 132 acceptance criterion 4; `docs/about/ecosystem.md`.*

## 10. Source and author

**Source.** github.com/karwalski/toke — compiler, specification and standard library,
Apache-2.0, except spec/, stdlib/ and src/stdlib/, which are MIT. The project is thirteen
public repositories under github.com/karwalski, plus two private ones; the site is
tokelang.dev, built with ooke and served by a toke binary on std.http.

*Source: `docs/about/repos.md` (repository map and licence boundary); `LICENSING.md`
(story 135.15a); the public count is `repos_public` from
`scripts/verify_project_facts.py`, derived from the verified inventory in
`scripts/about/github_repo_descriptions.py` (story 132.121; toke-ocr added to it, story
132.134).*

**Author.** Matthew Watt (github.com/karwalski), sole maintainer.

## 11. What must never be published

| Never | Write instead | Source | Story |
|---|---|---|---|
| "LL(1)" | "backtrack-free", or "backtrack-free with bounded lookahead" | `toke-spec-v0.4.md` §E | 132.12 |
| "13 keywords" | "14 keywords" | `toke-spec-v0.4.md` §A | 132.12 |
| "52% fewer tokens" as a headline | the token-efficiency short form in §9, verbatim | `docs/metrics-baseline.md` | 132.6 |
| "42% reduction vs Python" (or 31% / 48%) | withdrawn outright: toke costs about 1.34× Python's cl100k_base tokens | `docs/metrics-baseline.md` § What is withdrawn outright | 132.13 |
| any percentage about tokens with no tokenizer and no N | the approved wording in `docs/metrics-baseline.md` | TEMSpec §6.3 | 132.6 |

`make check-canonical` enforces rows 1 and 2; `make check-metrics` enforces rows 3 to 5.
Both run in `make ci`.
