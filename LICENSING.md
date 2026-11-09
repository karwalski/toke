# Toke Project Licensing

The toke project uses a dual-licence model across its repositories.
This document explains the split, the reasoning, and what it means for
users and contributors.

## Licence by repository

Repositories are named by their GitHub name; where the local checkout
directory differs it is given in brackets. The project's repository map is
[`docs/about/repos.md`](docs/about/repos.md).

| Repository | Licence | Purpose |
|---|---|---|
| `toke` | Apache 2.0 (compiler, docs) / MIT (`stdlib/`, `src/stdlib/`, `spec/`) | Compiler, standard library, documentation |
| `toke-spec` | MIT | Language specification archive, tree-sitter grammar |
| `toke-corpus` | Apache 2.0 | Corpus generation, audit and verification pipeline |
| `toke-models` (local `toke-model`) | Apache 2.0 | Fine-tuning, evaluation and packaging of models |
| `toke-tokenizer` | Apache 2.0 (including `python/`) | BPE tokenizer training and evaluation |
| `toke-eval` | Apache 2.0 | Benchmark tasks and evaluation harness |
| `toke-mcp` | MIT | MCP server, language server, VS Code extension |
| `ooke` (local `toke-ooke`) | MIT | Web framework and static site generator, written in toke |
| `loke` | MIT | Local intelligence layer, written in toke |
| `toke-web` (local `toke-website`) | MIT | Website source |
| `toke-ocr` | MIT | Optical character recognition, written in toke |
| `toke-test-programs` | Apache 2.0 | Test program corpus |
| `homebrew-toke` | MIT | Homebrew tap (`tkc`, `ooke` formulae) |
| `toke-cloud`, `toke-console` | Proprietary (private) | Hosted services |

### Where the MIT boundary is inside `toke`

The `toke` repository's top-level `LICENSE` is Apache 2.0 and covers
everything in it **except**:

- `stdlib/` — the standard library interfaces (`.tki`), toke sources and
  documentation. MIT, by `stdlib/LICENSE`.
- `src/stdlib/` — the C implementation of the standard library and the
  runtime. MIT; every source file carries `SPDX-License-Identifier: MIT`.
- `spec/` — MIT, by `spec/LICENSE`.

The standard library is MIT because `tkc` links it into every program it
compiles: its terms travel into the user's binary, and MIT carries no NOTICE
obligation. Vendored third-party code under `stdlib/vendor/` keeps its
upstream licence (cmark BSD-2-Clause, tomlc99 MIT, miniz MIT); see
`stdlib/vendor/README.md`.

## Why dual licensing

**MIT (spec, stdlib, libraries written in toke)** -- The language
specification, standard library, and the toke-written libraries use MIT to
minimise friction for adoption. Anyone building a toke compiler, editor
plugin, or teaching material can do so with the simplest possible terms,
and a program compiled by `tkc` carries no obligation beyond MIT's notice.

**Apache 2.0 (compiler, corpus, training pipeline, models, benchmarks)** --
The compiler, corpus pipeline, tokenizer, model training code, and
benchmarks use Apache 2.0 for one key reason: **patent protection**.
Apache 2.0 includes an explicit patent grant (Section 3) that protects
both contributors and users from patent claims related to the covered
work. This matters for the compiler (which implements novel
optimisations) and for the ML pipeline (where model architectures and
training techniques may touch patentable territory).

In short: MIT where simplicity helps adoption, Apache 2.0 where patent
clarity is needed.

## Contributor guidelines

### DCO sign-off

All commits must include a `Signed-off-by` line (Developer Certificate
of Origin). Use `git commit -s` or add the line manually:

```
Signed-off-by: Your Name <your@email.com>
```

This certifies you have the right to submit the contribution under the
repository's licence.

### Licence headers

**Apache 2.0 repos** -- Every new source file must include the standard
Apache 2.0 header comment:

```
// Copyright 2026 Matthew Watt
// SPDX-License-Identifier: Apache-2.0
```

**MIT repos** -- No per-file header is required. The repository-level
`LICENSE` file applies.

**MIT parts of `toke`** -- A new C source file under `src/stdlib/` starts
with:

```
/* SPDX-License-Identifier: MIT */
```

Files under `stdlib/` are covered by `stdlib/LICENSE`; `.tki` files are
JSON and carry no header.

### Which licence applies to your contribution

Your contribution is licensed under whichever licence covers the
repository you are contributing to. You do not need to choose.

## FAQ

**Can I use toke commercially?**
Yes. Both MIT and Apache 2.0 permit commercial use with no royalties or
fees. You must include the relevant licence notice in distributions.

**Does a program compiled with `tkc` carry the compiler's licence?**
No. The compiler's Apache 2.0 terms cover `tkc` itself. What `tkc` links
into your program is the standard library and runtime, which are MIT
(`stdlib/LICENSE`, `src/stdlib/`); keep the MIT notice when you
redistribute the binary.

**Can I fork the compiler?**
Yes. `tkc` is Apache 2.0 -- you can fork, modify, and redistribute it
commercially. You must retain the copyright and licence notices, state
any changes you made, and include the NOTICE file if one exists.

**What about the trained model weights?**
Model weights produced by the `toke-models` pipeline are covered by the
Apache 2.0 licence of that repository. You can use, redistribute, and
fine-tune them commercially. The Apache 2.0 patent grant applies.

**Can I write my own toke compiler from the spec?**
Yes. The spec (`toke/spec/`) is MIT-licensed. You can implement a
conforming compiler under any licence you choose, including proprietary.

**Do I need a CLA to contribute?**
No. The project uses the DCO (sign-off) model instead of a Contributor
Licence Agreement. Your sign-off line is sufficient.
