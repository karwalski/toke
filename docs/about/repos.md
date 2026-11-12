---
title: Project Repositories
slug: repos
section: about
order: 9
---

The toke project is eighteen repositories under the
[karwalski](https://github.com/karwalski) GitHub account: thirteen public and active,
one public but undescribed pending an owner decision, two private, and two archived.

Several repositories are checked out locally under a different name from their
GitHub name. The **GitHub name in the table below is the only correct one** for a
link, a clone URL or a registry field; the local name is listed only so the two can
be told apart.

## Public repositories

| Repository | Role | Licence | Copyright | Local directory |
|---|---|---|---|---|
| [toke](https://github.com/karwalski/toke) | Reference compiler (`tkc`), standard library, documentation, and the cross-repo story tracker. Root of trust. | Apache-2.0, except `spec/`, `stdlib/` and `src/stdlib/`, which are MIT (see below) | Matthew Watt | `toke` |
| [toke-spec](https://github.com/karwalski/toke-spec) | The language specification: the normative v0.4 text, the EBNF and GBNF grammars, the tree-sitter grammar, and the RFC draft. | MIT | Matthew Watt | `toke-spec` |
| [toke-corpus](https://github.com/karwalski/toke-corpus) | Training corpus: generation, audit and execution-verification pipeline, regen harness and ledgers. | Apache-2.0 | Matthew Watt | `toke-corpus` |
| [toke-models](https://github.com/karwalski/toke-models) | Fine-tuning, evaluation and packaging for toke code-generation models (QLoRA, MLX). | Apache-2.0 | Matthew Watt | `toke-model` (singular) |
| [toke-tokenizer](https://github.com/karwalski/toke-tokenizer) | BPE tokenizer training and evaluation, and the token-efficiency baselines. | Apache-2.0 | Matthew Watt | `toke-tokenizer` |
| [toke-eval](https://github.com/karwalski/toke-eval) | Held-out benchmark and evaluation harness. Owns `benchmark/hidden_tests/`. | Apache-2.0 | Matthew Watt | `toke-eval` |
| [toke-test-programs](https://github.com/karwalski/toke-test-programs) | Executable toke programs used as regression and conformance material for the compiler. | Apache-2.0 | Matthew Watt | `toke-test-programs` |
| [toke-mcp](https://github.com/karwalski/toke-mcp) | MCP server, language server and VS Code extension. | MIT | Matthew Watt | `toke-mcp` |
| [ooke](https://github.com/karwalski/ooke) | ooke, toke's web framework and static site generator, written in toke; it builds tokelang.dev; a toke binary on std.http serves it. | MIT | Matthew Watt | `toke-ooke` |
| [loke](https://github.com/karwalski/loke) | loke, toke's local intelligence layer, written in toke. | MIT | Matthew Watt | `loke` |
| [toke-web](https://github.com/karwalski/toke-web) | Source for tokelang.dev, built with ooke and served by a toke binary on std.http. | MIT | Matthew Watt | `toke-website` |
| [homebrew-toke](https://github.com/karwalski/homebrew-toke) | Homebrew tap for toke: the `tkc` and `ooke` formulae. Not installable yet: the formulae carry placeholder checksums until the release artefacts they download are published. | MIT | Matthew Watt | `homebrew-toke` |
| [toke-ocr](https://github.com/karwalski/toke-ocr) | Optical character recognition for clean machine-printed documents, written in toke (Epic 139). Separate from `std.ocr`, the platform-engine binding in the standard library. | MIT | Matthew Watt | `toke-ocr` |

### Licence boundary inside toke

The `toke` repository is Apache-2.0 (`LICENSE`) with one boundary, recorded in
[`LICENSING.md`](https://github.com/karwalski/toke/blob/main/LICENSING.md): the
language specification (`spec/`) and the standard library (`stdlib/` and its C
implementation in `src/stdlib/`) are MIT, so that anyone can implement the language
or ship programs that statically link the standard library on the simplest terms.
`spec/LICENSE` carries the MIT text for the specification and `stdlib/LICENSE`
for the standard library; every `src/stdlib/` source file carries an
`SPDX-License-Identifier: MIT` line (story 135.15a).

Every repository in the table publishes a `LICENSE`, and every one is copyright
Matthew Watt. The split between the two licences is the dual model recorded in
[`LICENSING.md`](https://github.com/karwalski/toke/blob/main/LICENSING.md): MIT where
simple terms help adoption (the specification, the standard library, and the
libraries, tools and site written in toke), Apache-2.0 where its patent grant matters
(the compiler, the corpus pipeline, the tokenizer, model training and the benchmarks).

ooke and loke are toke sub-projects, not separate products — see
[`docs/about/ecosystem.md`](/docs/about/ecosystem/).

## Private repositories

Not public, and never to be made public.

| Repository | Role | Licence | Copyright |
|---|---|---|---|
| `karwalski/toke-cloud` | Billing, authentication, tiered rate limiting, and deployment infrastructure for toke services. | Proprietary | Matthew Watt |
| `karwalski/toke-console` | The toke console: accounts, billing, API keys. | Proprietary | Matthew Watt |

## Archived repositories

Kept public for provenance; no longer developed.

| Repository | Superseded by |
|---|---|
| [toke-benchmark](https://github.com/karwalski/toke-benchmark) | [toke-eval](https://github.com/karwalski/toke-eval) |
| [toke-stdlib](https://github.com/karwalski/toke-stdlib) | `stdlib/` in [toke](https://github.com/karwalski/toke) |

## Undescribed

| Repository | State |
|---|---|
| [tkc](https://github.com/karwalski/tkc) | Public, with no description, no homepage, and no role in the map above. It holds an early copy of the compiler tree. It must be given a description or archived; until then it is not part of the project's published surface. |

---

## Dependency Order

When a change in one repository affects another, the downstream repository must be
updated. The critical dependency chains:

| If you change... | Then update... |
|---|---|
| `toke-spec` grammar or normative text | `toke` compiler and conformance tests |
| `toke` stdlib signatures (`.tki`) | `toke` stdlib C implementations |
| `toke` compiler diagnostics | `toke-corpus` generation and audit pipeline |
| `toke-corpus` corpus schema | `toke-models` training data preparation |
| `toke-tokenizer` vocabulary | `toke-models` training and `toke-eval` token counts |
| `toke-eval` benchmark tasks | `toke-models` evaluation scripts |
| `toke` language surface | `toke-mcp` server, LSP and VS Code extension |

The general rule: open the downstream PR first, merge the downstream PR last.

---

This map is derived from the verified repository inventory in
`scripts/about/github_repo_descriptions.py` and is checked against it by
`make check-facts`: the repository names, and the Licence and Copyright columns
(story 132.132).
