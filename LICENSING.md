# Toke Project Licensing

The toke project uses a dual-licence model across its repositories.
This document explains the split, the reasoning, and what it means for
users and contributors.

## Licence by repository

| Repository | Licence | Purpose |
|---|---|---|
| `toke` | MIT | Meta landing repo, project overview |
| `toke-spec` | MIT | Language specification, RFC, grammar |
| `toke-stdlib` | MIT | Standard library (.toke source) |
| `tkc` | Apache 2.0 | Reference compiler (C) |
| `toke-corpus` | Apache 2.0 | Corpus generation pipeline |
| `toke-tokenizer` | Apache 2.0 | BPE tokenizer (default syntax) |
| `toke-models` | Apache 2.0 | QLoRA fine-tuning and evaluation |
| `toke-benchmark` | Apache 2.0 | Benchmark tasks and harness |

## Why dual licensing

**MIT (spec, stdlib, landing repo)** -- The language specification,
standard library, and overview repo use MIT to minimise friction for
adoption. Anyone building a toke compiler, editor plugin, or teaching
material can do so with the simplest possible terms.

**Apache 2.0 (compiler, training pipeline, models, benchmarks)** --
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
// Copyright 2026 Matt Watt
// SPDX-License-Identifier: Apache-2.0
```

**MIT repos** -- No per-file header is required. The repository-level
`LICENSE` file applies.

### Which licence applies to your contribution

Your contribution is licensed under whichever licence covers the
repository you are contributing to. You do not need to choose.

## FAQ

**Can I use toke commercially?**
Yes. Both MIT and Apache 2.0 permit commercial use with no royalties or
fees. You must include the relevant licence notice in distributions.

**Can I fork the compiler?**
Yes. `tkc` is Apache 2.0 -- you can fork, modify, and redistribute it
commercially. You must retain the copyright and licence notices, state
any changes you made, and include the NOTICE file if one exists.

**What about the trained model weights?**
Model weights produced by the `toke-models` pipeline are covered by the
Apache 2.0 licence of that repository. You can use, redistribute, and
fine-tune them commercially. The Apache 2.0 patent grant applies.

**Can I write my own toke compiler from the spec?**
Yes. The spec (`toke-spec`) is MIT-licensed. You can implement a
conforming compiler under any licence you choose, including proprietary.

**Do I need a CLA to contribute?**
No. The project uses the DCO (sign-off) model instead of a Contributor
Licence Agreement. Your sign-off line is sufficient.
