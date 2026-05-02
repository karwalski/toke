---
title: Project Repositories
slug: repos
section: about
order: 9
---

The toke project is organised across six repositories under the [karwalski](https://github.com/karwalski) GitHub account.

## toke

**Compiler, specification, and standard library.** The reference toke compiler (C99), language specification (EBNF grammar, RFC draft), and 30+ standard library modules with C runtime implementations. Includes 600+ conformance tests and full documentation.

- **Status:** Active — Phase 1 complete, Phase 2 on hold (waiting for local compute)
- **License:** Apache 2.0
- **Link:** [github.com/karwalski/toke](https://github.com/karwalski/toke)
- **Contains:** compiler, spec, stdlib

## toke-model

**Corpus, tokenizer, and model training.** The training corpus (46,754 verified programs), BPE tokenizer (8K/32K vocabularies), training scripts (QLoRA/DoRA), and model weights. Covers the full pipeline from data generation to fine-tuned model.

- **Status:** Active — corpus complete, tokenizer trained, model training round 2 pending
- **License:** Apache 2.0
- **Link:** [github.com/karwalski/toke-model](https://github.com/karwalski/toke-model)
- **Contains:** corpus, tokenizer, models

## toke-eval

**Benchmark and evaluation.** Held-out benchmark suite (1,000 tasks) and evaluation scripts for measuring Pass@1 rates, token efficiency, and gate criteria.

- **Status:** Active — Gate 1 complete (63.7% Pass@1, 12.5% token reduction)
- **License:** Apache 2.0
- **Link:** [github.com/karwalski/toke-eval](https://github.com/karwalski/toke-eval)
- **Contains:** benchmarks, evaluation pipeline

## toke-mcp

**MCP server for IDE integration.** Public, self-hostable MCP server providing toke tools (check, compile, explain, spec, stdlib, generate, bench) for use with Claude, VS Code, and other MCP-compatible clients.

- **Status:** Active
- **License:** MIT
- **Link:** [github.com/karwalski/toke-mcp](https://github.com/karwalski/toke-mcp)

## toke-web

**This website.** Public-facing documentation, learning course, API reference, and development timeline. Built with Astro Starlight.

- **Status:** Active
- **License:** MIT
- **Link:** [github.com/karwalski/toke-web](https://github.com/karwalski/toke-web)

## toke-cloud

**Private infrastructure.** Billing, authentication, tiered rate limiting, and deployment infrastructure. Not public.

- **Status:** Active (private)

---

## Dependency Order

When a change in one repository affects another, the downstream repository must be updated. The critical dependency chains:

| If you change... | Then update... |
|---|---|
| `toke` spec/grammar | `toke` conformance tests |
| `toke` stdlib signatures (.tki) | `toke` stdlib C implementations |
| `toke` compiler diagnostics | `toke-model` corpus pipeline |
| `toke-model` corpus schema | `toke-model` training data prep |
| `toke-eval` benchmark tasks | `toke-model` evaluation scripts |

The general rule: open the downstream PR first, merge the downstream PR last.
