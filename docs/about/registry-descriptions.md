---
title: "Registry descriptions: what every package page must say"
slug: registry-descriptions
section: about
story: 132.4
date: 2026-09-19
order: 13
---

# Registry descriptions: what every package page must say

**What this file is.** The prepared text for every *registry* surface toke appears on —
GitHub repository descriptions and topics, PyPI, npm, the VS Code marketplace, Hugging
Face and Ollama — with the live text beside it, what is wrong with the live text, and the
exact step that applies the new one. It is the companion to
[`canonical.md`](/docs/about/canonical/), which owns the words; this file only says where
they go.

**Nothing here has been published.** Every live registry field is an outward-facing
change that needs the owner and, in most cases, credentials this workspace does not hold.
Where the text belongs in a repository file, the file has been changed and committed, so
publishing is a release rather than an edit; where it belongs in a web form or an API
call, the command is written out below for the owner to run. The owner checklist is at
the end, and the split between "automatic on next release" and "needs the owner" is
[the note for story 132.5](#note-for-story-1325).

**Four rules, inherited from `canonical.md`.**

1. Every surface carries the 19-word one-liner **verbatim** where the field allows a
   sentence.
2. Every surface links back to **tokelang.dev** and to **github.com/karwalski/toke**.
3. No withdrawn claim: no "52% fewer tokens", no "42% vs Python", no "LL(1)", no
   "13 keywords", no unqualified "100%", no percentage about tokens without its tokenizer
   and its N.
4. The name is `toke`, lower case, in every field — including a marketplace display name.

The one-liner, for reference (127 characters, fits every field below):

> toke: a compiled language designed for LLM code generation, with a small grammar, one
> canonical form and compiler verification.

**Live text was read on 2026-09-19** from the PyPI JSON API, the npm registry API, the
GitHub REST API, `huggingface.co/<repo>/raw/main/README.md`, the VS Code marketplace
`extensionquery` API and `ollama.com/library/karwalski/toke`. Read-only; nothing was
written.

---

## Surface index

| # | Surface | Live today | Backed by a repo file? | Who applies it |
|---|---|---|---|---|
| 1 | GitHub `karwalski/toke` — description + topics | description empty, no topics | no (API/web form) | **owner** |
| 2 | GitHub — every other toke repo | all descriptions empty | no (API/web form) | **owner** |
| 3 | PyPI `toke-tokenizer` | 0.1.0, carries the withdrawn 52% | **yes** — `python/pyproject.toml` + `python/README.md` | release |
| 4 | npm `@tokelang/mcp-server` | 0.1.0, "Toke" capitalised | **yes** — `package.json` | release |
| 5 | npm `@tokelang/tkc` | not published | **yes** — `npm-tkc/package.json` | first release |
| 6 | VS Code `tokelang.toke-language` | 0.2.2, "Toke" capitalised | **yes** — `vscode-toke/package.json` + `README.md` | release |
| 7 | Hugging Face `karwalski/toke` | card carries the withdrawn 52%, the retired 13-keyword count, 55 chars, bare 100% | **yes** — `toke-model/huggingface/README.md` | **owner** (Hub push) |
| 8 | Hugging Face `karwalski/toke-tokenizer` | card carries 52% and a per-example 19-vs-49 | **yes** — `toke-tokenizer/docs/hf-model-card.md` | **owner** (Hub push) |
| 9 | Ollama `karwalski/toke` | **not published** (404) | **yes** — `toke-model/ollama/README.md` | **owner** (first push) |

---

## 1. GitHub — `karwalski/toke`

### Current (2026-09-19, GitHub REST API)

    description: null
    homepage:    null
    topics:      []

### What is wrong

- **The description is empty.** The repository front page of the whole project says
  nothing about what toke is; GitHub search, the org page, every fork listing and every
  social preview card have nothing to show. This is criterion 2 and criterion 11 failing
  at the most-visited surface we own.
- **No topics**, so the repo appears in none of GitHub's topic listings.
- **No homepage**, so there is no link to tokelang.dev from the repo header.

### Proposed

Description (210 characters, limit 350):

> toke: a compiled language designed for LLM code generation, with a small grammar, one
> canonical form and compiler verification. Reference compiler (tkc), language
> specification and standard library. Apache-2.0.

Homepage: `https://tokelang.dev`

Topics: `programming-language`, `llm`, `code-generation`, `token-efficiency`, `compiler`

### How to apply (owner)

```sh
gh repo edit karwalski/toke \
  --description "toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. Reference compiler (tkc), language specification and standard library. Apache-2.0." \
  --homepage "https://tokelang.dev" \
  --add-topic programming-language --add-topic llm --add-topic code-generation \
  --add-topic token-efficiency --add-topic compiler
```

The README first paragraph is already the canonical block (story 132.10) and is enforced
by `make check-canonical`; nothing to do there.

---

## 2. GitHub — every other toke repo

### Current (2026-09-19)

Every public toke repository has `description: null`, `homepage: null` and no topics. The
two private ones carry a description that predates the naming rule:

| Repo | Visibility | Live description |
|---|---|---|
| `toke-spec`, `toke-corpus`, `toke-eval`, `toke-mcp`, `toke-models`, `toke-tokenizer`, `toke-test-programs`, `toke-web`, `ooke`, `loke`, `tkc` | public | *(empty)* |
| `toke-benchmark`, `toke-stdlib` | public, archived | *(empty)* |
| `toke-cloud` | private | `toke cloud infrastructure (PRIVATE - billing/auth/infra)` |
| `toke-console` | private | `toke console (PRIVATE - billing/auth)` |

### What is wrong

- Eleven public repositories describe themselves as nothing at all.
- The local checkout names do not match the GitHub names in three cases, which is how the
  gap survived: `~/tk/toke-model` → `karwalski/toke-models`, `~/tk/toke-ooke` →
  `karwalski/ooke`, `~/tk/toke-website` → `karwalski/toke-web`.
- `karwalski/tkc` is public, empty of description and not listed in
  [`repos.md`](/docs/about/repos/). It needs a description or an archive — owner
  decision, flagged below.
- The two private descriptions are serviceable but shout in capitals and do not carry the
  one-liner; they are fixed here for consistency, not urgency.

### Proposed

Each description is `<what this repo is>` + the one-liner **verbatim** + the link home.
Set `--homepage https://tokelang.dev` on every one of them.

| Repo | Description |
|---|---|
| `toke-spec` | The toke language specification: the normative v0.4 text, the EBNF and GBNF grammars, and the RFC draft. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-corpus` | Training corpus for toke: generation, audit and execution-verification pipeline. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-eval` | toke-eval: the held-out benchmark and evaluation harness for toke. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-mcp` | MCP server, language server and VS Code extension for toke. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-models` | Fine-tuning, evaluation and packaging for toke code-generation models. The published model is the Gate 2 research artefact and writes v0.3 syntax. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-tokenizer` | The v0.3 BPE tokenizer for toke (16,384 vocab) and its training/evaluation pipeline; the v0.4 retrain has not shipped. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-test-programs` | Executable toke programs used as regression and conformance material for the compiler. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `ooke` | ooke, toke's web framework and static site generator, written in toke; it builds and serves tokelang.dev. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `loke` | loke, toke's local intelligence layer, written in toke. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-web` | Source for tokelang.dev, built and served by ooke. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. github.com/karwalski/toke |
| `toke-benchmark` *(archived)* | Archived (superseded by karwalski/toke-eval). Early benchmark material for toke, kept for provenance. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. |
| `toke-stdlib` *(archived)* | Archived (the standard library now lives in karwalski/toke). Kept for provenance. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. |
| `toke-cloud` *(private)* | Private: billing, authentication and deployment infrastructure for toke services. toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. |
| `toke-console` *(private)* | Private: the toke console (accounts, billing, API keys). toke: a compiled language designed for LLM code generation, with a small grammar, one canonical form and compiler verification. |

All are under GitHub's 350-character limit (longest: `toke-models`, 300).

Topics, per repo: `toke` plus the ones that fit — `programming-language` and `compiler`
for `toke-spec`, `tkc`, `toke-test-programs`; `llm` and `code-generation` for
`toke-models`, `toke-corpus`, `toke-eval`, `toke-mcp`; `token-efficiency` and `tokenizer`
for `toke-tokenizer`; `web-framework` and `static-site-generator` for `ooke`.

### How to apply (owner)

`scripts/about/github_repo_descriptions.py` in this repo prints one `gh repo edit` line
per repository, ready to review and run. It **prints only** — it never calls the API.

```sh
python3 scripts/about/github_repo_descriptions.py            # review the commands
python3 scripts/about/github_repo_descriptions.py | sh       # apply them (owner only)
```

**Owner decisions needed:** what `karwalski/tkc` is for (describe or archive), and
whether `toke-benchmark` / `toke-stdlib` should stay public.

---

## 3. PyPI — `toke-tokenizer`

### Current (2026-09-19, PyPI JSON API, version 0.1.0)

    Summary:  BPE tokenizer for the toke programming language (16K vocab)
    URLs:     Homepage / Repository -> github.com/karwalski/toke-tokenizer
    Long description (first lines):
      "A pure Python BPE tokenizer for the toke programming language. Trained on
       normalised toke source code with a 16,384-token vocabulary.

       Achieves approximately 52% token reduction compared to OpenAI's cl100k_base
       tokenizer on toke source code."

### What is wrong

- The live long description states a **withdrawn** measurement as a present-tense feature:
  "Achieves approximately 52% token reduction compared to OpenAI's cl100k_base tokenizer".
  It compared two tokenizers encoding *the same toke text* (N = 42, v0.3), was never a
  comparison with Python, and on canonical v0.4 text every shipped toke tokenizer needs
  *more* tokens than cl100k_base. Retired by story 132.6.
- **Nothing says the wheel is the v0.3 tokenizer.** "16K vocab" implies a current
  artefact; the language is on v0.4 and the v0.4 retrain (116.9) has not shipped. Anyone
  `pip install`-ing this today gets a v0.3-syntax vocabulary with no warning.
- The description does not carry the one-liner, and no URL points at tokelang.dev or at
  `github.com/karwalski/toke`.
- The v0.3 vocabulary's known defect (a `null` `unk_token` that silently drops
  backslashes) is not mentioned anywhere a user would see it.

### Proposed

Summary (`project.description`):

> The v0.3 BPE tokenizer for toke (16,384 vocab, v0.3-syntax source; the v0.4 retrain is
> not yet shipped). toke: a compiled language designed for LLM code generation, with a
> small grammar, one canonical form and compiler verification.

URLs: Homepage `https://tokelang.dev`, Source `https://github.com/karwalski/toke`,
Repository `https://github.com/karwalski/toke-tokenizer`, plus direct links to
`canonical.md` and `metrics-baseline.md`.

Long description: `toke-tokenizer/python/README.md`, which now opens with "This package
is the v0.3 tokenizer, not a current one", carries the one-liner and both links, and
keeps the withdrawal note.

### Applied in-repo (commit in `toke-tokenizer`)

- `python/pyproject.toml` — new `description`, `keywords`, `project.urls`, and
  `version = "0.1.1"` (PyPI will not accept a re-upload of 0.1.0; the description only
  changes with a new release).
- `python/README.md` — new opening, "About toke" block with the verbatim one-liner and
  both links.

### How to publish (owner)

```sh
cd ~/tk/toke-tokenizer/python
python3 -m build
python3 -m twine check dist/toke_tokenizer-0.1.1*
python3 -m twine upload dist/toke_tokenizer-0.1.1*    # needs the PyPI token
```

The 0.1.0 release cannot be edited; it stays on PyPI with the old text until 0.1.1
supersedes it as the default version.

---

## 4. npm — `@tokelang/mcp-server`

### Current (2026-09-19, npm registry API, version 0.1.0)

    description: "Self-hostable MCP server for the Toke programming language"
    keywords:    toke, mcp, language-server, compiler, code-generation
    homepage:    https://tokelang.dev

### What is wrong

- "**Toke**" is capitalised: the name is `toke`, lower case, in every field
  (`canonical.md` §1).
- No one-liner, so the package page never says what toke is, and no link to
  `github.com/karwalski/toke` (the `repository` field points at `toke-mcp` only).
- No withdrawn claim present — this one is a naming and completeness fix, not a
  correction.

### Proposed

> Self-hostable MCP server for the toke programming language. toke: a compiled language
> designed for LLM code generation, with a small grammar, one canonical form and compiler
> verification.

Keywords gain `llm`, `token-efficiency`, `programming-language`.

### Applied in-repo (commit in `toke-mcp`)

`package.json` — `description`, `keywords`, `version` 0.1.0 → 0.1.1. The README already
carries the canonical paragraph (story 132.10) and is the npm long description.

### How to publish (owner)

```sh
cd ~/tk/toke-mcp && npm publish --access public   # needs the npm token
```

---

## 5. npm — `@tokelang/tkc`

### Current

Not published: `registry.npmjs.org/@tokelang/tkc` returns `Not found`, as do the platform
sub-packages `@tokelang/tkc-darwin-arm64` and friends. `npm-tkc/package.json` sits at
version 0.3.0 in the repo, prepared but never released.

### What is wrong

Nothing is live to correct. The prepared description — "The toke language checker — fast,
correct, minimal" — would have shipped without the one-liner and without saying what tkc
is.

### Proposed

> tkc, the reference toke compiler and checker: single-pass C99, native binaries,
> structured diagnostics. toke: a compiled language designed for LLM code generation,
> with a small grammar, one canonical form and compiler verification.

### Applied in-repo (commit in `toke-mcp`)

`npm-tkc/package.json` — new `description`. Version left at 0.3.0; it has never been
published, so the first release publishes this text.

---

## 6. VS Code marketplace — `tokelang.toke-language`

### Current (2026-09-19, marketplace `extensionquery` API, version 0.2.2, 166 downloads)

    displayName:      "Toke - Token-Optimised Language"
    shortDescription: "Syntax highlighting, diagnostics, and code intelligence for the
                       Toke programming language"
    README (long description) opens: "# Toke Language Support for VS Code"

### What is wrong

- "**Toke**" is capitalised three times, and the display name presents "Token-Optimised
  Language" as though it were the name. It is a descriptor only (`canonical.md` §1).
- The listing never says what toke *is*: no one-liner, no paragraph, and the only link is
  to tokelang.dev in the first line of the README — none to
  `github.com/karwalski/toke`.
- No withdrawn claim present.

### Proposed

`displayName`: `toke language support`

`description`:

> Syntax highlighting, diagnostics and code intelligence for toke. toke: a compiled
> language designed for LLM code generation, with a small grammar, one canonical form and
> compiler verification.

README (the marketplace long description) opens with the one-liner **and** the 82-word
paragraph, verbatim, plus links to tokelang.dev, `karwalski/toke` and `karwalski/toke-mcp`.

### Applied in-repo (commit in `toke-mcp`)

- `vscode-toke/package.json` — `displayName`, `description`, `keywords`, `homepage`,
  `version` 0.2.2 → 0.2.3.
- `vscode-toke/README.md` — new title and "About toke" block.

### How to publish (owner)

```sh
cd ~/tk/toke-mcp/vscode-toke && npm install && npm run compile
npx vsce package                 # -> toke-language-0.2.3.vsix
npx vsce publish                 # needs the marketplace PAT for publisher "tokelang"
```

---

## 7. Hugging Face — `karwalski/toke`

### Current (2026-09-19, model card at `huggingface.co/karwalski/toke/raw/main/README.md`)

The live card is titled `toke-7b-gate2` and contains, quoted here as published text that
is being retired:

- The withdrawn headline (Toke-16K v0.3 vs cl100k_base on the *same* toke text, N = 42,
  superseded): "A purpose-built BPE tokenizer achieves **52% fewer tokens** on average vs cl100k_base".
- "toke is a statically typed, compiled language with a **55-character alphabet**
  (lowercase a-z, digits 0-9, and 19 symbols)" — wrong twice; story 132.14 resolved the
  alphabet against `src/lexer.c` at **59** characters (26 + 10 + 23).
- "**13 keywords:** `m` `f` `t` `i` `if` `el` `lp` `br` `let` `mut` `as` `rt` `mt`" — the
  retired v0.3 count; the set is 14 and includes `sc`.
- "This model writes syntactically valid toke **100% of the time**" — an unqualified 100%
  with no set named, and contradicted on the same page by the honest floor (37.5% compile
  across all 1,748 v0.3.9 corpus programs).
- `model-index` publishes `Functional Pass@1 = 8` with `verified: true`; the corrected
  figure is **55.6%** (272/489), the ~8% having been a stdlib defect fixed on 2026-05-25.
- No statement that the model writes **v0.3** syntax while the language is on v0.4.
- Links to `console.tokelang.dev` and `api.tokelang.dev` advertising "Free API access" —
  the owner must confirm those still exist before the card is re-published.

### Proposed

`toke-model/huggingface/README.md`, rewritten in full: one-liner and canonical paragraph
verbatim; a "Read this before quoting the model" section naming the v0.3/v0.4 gap; Gate 2
stated **with its curated set** beside the full-corpus floor; the token-efficiency short
form verbatim; `model-index` carrying the set name inside each metric name and no
`verified: true`.

### Applied in-repo (commit in `toke-model`)

- `huggingface/README.md` — the new card for the published model.
- `huggingface/model-card-toke-coder-7b-UNPUBLISHED.md` — the previous contents of that
  file, which described a `toke-coder-7b` that was never uploaded, with Gate-1-era numbers
  (63.7% Pass@1, "12.5% token reduction"). Kept for provenance, marked historical, and
  explicitly not to be copied anywhere.

### How to publish (owner)

```sh
huggingface-cli login                      # write token
huggingface-cli upload karwalski/toke ~/tk/toke-model/huggingface/README.md README.md
```

Then check the two console/API links on the rendered page.

---

## 8. Hugging Face — `karwalski/toke-tokenizer`

### Current (2026-09-19)

Live card headline, quoted as a withdrawn claim (Toke-16K v0.3 vs cl100k_base on the same
toke text, N = 42, superseded): "A purpose-built 16K BPE tokenizer for the toke
programming language, achieving **52% average token reduction** vs cl100k_base across 42
benchmark programs", with a table row "Average reduction | 52% vs cl100k_base",
a "Best case | 76% reduction (simple loop)" row, and a usage comment "# 19 tokens (vs 49
cl100k)" — the per-example form of the same two-tokenizer error.

### What is wrong

- The 52% and the 76% are the withdrawn Toke-16K-vs-cl100k_base figures (same toke text,
  two tokenizers, N = 42) presented as properties of the tokenizer.
- "19 tokens (vs 49 cl100k)" is the per-example form listed under "What is withdrawn
  outright" in `metrics-baseline.md`.
- Nothing marks the artefact as **v0.3**, and the `null` `unk_token` defect — which
  inflates every number on the page — is not mentioned.
- No one-liner, and no link to `github.com/karwalski/toke` (only to tokelang.dev and a
  `/docs/reference/token-comparison` page that publishes the withdrawn table).

### Proposed

`toke-tokenizer/docs/hf-model-card.md`: titled "toke tokenizer — v0.3 BPE (16,384
vocab)", one-liner and paragraph verbatim, a "The 52% reduction claim is withdrawn"
section, the defect stated in the facts table, the token-efficiency short form verbatim,
and links to tokelang.dev, `karwalski/toke` and `karwalski/toke-tokenizer`.

### How to publish (owner)

```sh
huggingface-cli upload karwalski/toke-tokenizer \
  ~/tk/toke-tokenizer/docs/hf-model-card.md README.md
```

---

## 9. Ollama — `karwalski/toke`

### Current (2026-09-19)

`https://ollama.com/library/karwalski/toke` returns **404**. The model has never been
pushed; `toke-model/ollama/` holds the `Modelfile` and the conversion script that would
push it.

### What is wrong

Nothing is live. The risk is the opposite one: the publishing runbook has no description
text, so a first push would take whatever was typed in the moment.

### Proposed

Held in `toke-model/ollama/README.md` under "Registry description": the one-liner
verbatim, then the two qualifiers this artefact cannot be published without — it writes
**v0.3** syntax, and its 100% is a *curated-set* compile figure quoted beside the 37.5%
full-corpus floor — then the two links.

### Note on the `Modelfile`

Its `SYSTEM` prompt is v0.3 (the superseded keyword set without `sc`, `$`-prefixed types,
v0.3 arrays). That is correct for these weights and **must not** be updated to v0.4
wording: it would describe a language the model has never seen. It is why the published
description has to name v0.3 explicitly.

---

## What was changed in a repository, and where

| Repo | File | Change |
|---|---|---|
| `toke` | `docs/about/registry-descriptions.md` | this file |
| `toke` | `scripts/check_canonical.py` | registry surfaces added (§ below) |
| `toke` | `scripts/about/github_repo_descriptions.py` | prints the `gh repo edit` commands; never calls the API |
| `toke-tokenizer` | `python/pyproject.toml` | summary, keywords, URLs, version 0.1.1 |
| `toke-tokenizer` | `python/README.md` | v0.3 statement, one-liner, links |
| `toke-tokenizer` | `docs/hf-model-card.md` | new — the Hugging Face card |
| `toke-mcp` | `package.json` | description, keywords, version 0.1.1 |
| `toke-mcp` | `npm-tkc/package.json` | description |
| `toke-mcp` | `vscode-toke/package.json` | displayName, description, keywords, homepage, version 0.2.3 |
| `toke-mcp` | `vscode-toke/README.md` | title, one-liner, paragraph, links |
| `toke-mcp` | `skills/toke-language.md`, `claude-plugin/skills/toke-language.md` | the retired "13" keyword count corrected to 14 and `sc` added — these files ship inside the npm package |
| `toke-model` | `huggingface/README.md` | rewritten card for the published model |
| `toke-model` | `huggingface/model-card-toke-coder-7b-UNPUBLISHED.md` | the old card, marked historical |
| `toke-model` | `ollama/README.md` | the registry description for the first push |

## How the guard treats these surfaces

`scripts/check_canonical.py` now declares each registry surface. A surface whose text
lives in a repository file is **enforced** — the manifest or card must carry the one-liner
verbatim, so a re-worded description fails CI like any other drifted copy. A surface that
only exists as a web field (the GitHub descriptions and topics, and each registry's live
page) is listed in `AWAITING_PUBLISH` and reported as a warning naming story 132.4 and the
command that applies it. Those warnings stay until the owner publishes and the entry is
deleted — they are the standing reminder that the prepared text is not yet live.

```sh
make check-canonical          # warnings for the un-published registry fields
python3 scripts/check_canonical.py --strict   # fails on them too
```

---

## Owner checklist

Nothing below has been done. Each line is one outward-facing action.

- [ ] **GitHub, `karwalski/toke`** — run the `gh repo edit` command in §1 (description,
      homepage, five topics).
- [ ] **GitHub, the other repos** — `python3 scripts/about/github_repo_descriptions.py`,
      review, then pipe to `sh`.
- [ ] **GitHub** — decide what `karwalski/tkc` is (describe or archive); confirm
      `toke-benchmark` and `toke-stdlib` stay public and archived.
- [ ] **PyPI** — build and upload `toke-tokenizer` 0.1.1 (token required). The withdrawn
      52% stays on the 0.1.0 page until then.
- [ ] **npm** — publish `@tokelang/mcp-server` 0.1.1 (token required).
- [ ] **npm** — decide whether `@tokelang/tkc` 0.3.0 ships at all; nothing is live.
- [ ] **VS Code marketplace** — package and publish 0.2.3 (publisher PAT required).
- [ ] **Hugging Face** — upload the two cards (write token required); check the
      `console.tokelang.dev` / `api.tokelang.dev` links still resolve before publishing
      the model card.
- [ ] **Ollama** — nothing is live; if the model is ever pushed, use the description in
      `toke-model/ollama/README.md`.
- [ ] **After each publish** — delete that surface's `AWAITING_PUBLISH` entry in
      `scripts/check_canonical.py` so the warning disappears, and record the date in
      story 132.5's baseline.

## Note for story 132.5

Story 132.5 records the publication-day baseline and schedules the 30/60/90-day reviews.
Two of its inputs depend on what is listed here, so it needs this split.

**Automatic on the next release — no web form, no owner credential beyond the usual
publish token.** The text is already committed; running the normal release publishes it.

| Surface | Released by | Version that carries the new text |
|---|---|---|
| PyPI `toke-tokenizer` | `twine upload` | 0.1.1 |
| npm `@tokelang/mcp-server` | `npm publish` | 0.1.1 |
| npm `@tokelang/tkc` | first `npm publish` | 0.3.0 |
| VS Code `tokelang.toke-language` | `vsce publish` | 0.2.3 |

**Needs the owner, with no release to ride on.** These are web forms or authenticated API
calls against a live service; none of them is a repository release, so they will not
happen by themselves.

| Surface | Action | Credential |
|---|---|---|
| GitHub `karwalski/toke` | description, homepage, 5 topics | `gh` auth |
| GitHub, 14 other repos | description, homepage, topics | `gh` auth |
| Hugging Face `karwalski/toke` | upload `README.md` as the card | HF write token |
| Hugging Face `karwalski/toke-tokenizer` | upload the card | HF write token |
| Ollama `karwalski/toke` | not published; description text only if pushed | Ollama account |

**Baseline consequences for 132.5.** Package-download counts can be taken now (PyPI
`toke-tokenizer` 0.1.0, npm `@tokelang/mcp-server` 0.1.0, VS Code 166 downloads / 7
installs / 1 like on Hugging Face `karwalski/toke` with 23 downloads), but they are a
**pre-correction** baseline: every one of those pages still carries the old text until the
release above happens. Record the date each surface actually flipped, so the 30/60/90-day
reviews compare like with like. GitHub stars and traffic can be baselined immediately;
they are unaffected by the description change, which is exactly why the description change
is worth measuring.
