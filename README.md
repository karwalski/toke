# toke

toke is a statically typed, compiled programming language designed for high-quality LLM code generation.

The thesis: a language whose character set, syntax, and type system are co-designed with the tokeniser used during model training will produce shorter, more correct programs from the same model capacity than a general-purpose language.

---

## What toke is for

LLMs generate code token by token. Every token boundary that falls mid-identifier, mid-keyword, or mid-operator is wasted capacity. toke fixes this at the language level:

- An 80-character set aligned to cl100k_base token boundaries — no wasted sub-token splits
- A mandatory typed interface file (`.tokei`) per module — the compiler's output is the model's compressed task description
- A strict, LL(1) grammar — every syntactic position has exactly one valid interpretation
- A structured JSON diagnostic schema — repair loops consume errors without parsing English prose

If toke does not show a measurable token-efficiency advantage over Python, Go, or TypeScript at Gate 1, the project stops. The language exists to be falsified.

---

## Current status

| Phase | Gate | Criterion | Status |
|-------|------|-----------|--------|
| 1 — Falsification | Gate 1 (month 8) | >10% token reduction AND Pass@1 ≥ 60% | not reached |
| 2 — Extension | Gate 2 (month 14) | Extended features retain efficiency AND 7B model beats baseline | not reached |
| 3 — Scale | Gate 3 (month 26) | Two+ model families ≥70% Pass@1 AND self-improvement loop running | not reached |
| 4 — Release | Gate 4 (month 32) | All benchmarks met, spec complete, consortium proposal ready | not reached |

**Phase 1 milestone M1 complete.** The reference compiler, standard library core, and conformance suite are built. Corpus generation (Epics 1.4–1.5) is blocked on Mac Studio hardware provisioning.

---

## Repositories

| Repository | Role |
|------------|------|
| [karwalski/toke](https://github.com/karwalski/toke) | This meta-repo: project landing page, issue tracker for cross-repo work |
| [karwalski/tkc](https://github.com/karwalski/tkc) | Reference compiler (C), standard library, conformance test suite, project docs |
| [karwalski/toke-spec](https://github.com/karwalski/toke-spec) | Language specification: grammar.ebnf, character set, keyword table, Phase 2 transform rules |
| [karwalski/toke-corpus](https://github.com/karwalski/toke-corpus) | Corpus generation pipeline, monitoring console, sandbox execution harness |
| [karwalski/toke-model](https://github.com/karwalski/toke-model) | Fine-tuning scripts and model evaluation harness |
| [karwalski/toke-benchmark](https://github.com/karwalski/toke-benchmark) | Held-out benchmark task set (500 tasks, gitignored output), gate measurement scripts |
| [karwalski/toke-eval](https://github.com/karwalski/toke-eval) | Pass@1 and token-efficiency evaluation pipeline |

---

## Getting started

### Build the compiler

Requirements: clang (any recent version), make.

```
git clone https://github.com/karwalski/tkc
cd tkc
make
```

The `tkc` binary is placed in the repo root. It is a self-contained native binary with no runtime dependencies.

### Run the conformance suite

```
make conform
```

This runs 62 tests across the lexer, grammar, and diagnostic series. All tests must pass.

### Compile a toke program

```
tkc src/hello.toke
```

Output is LLVM IR on stdout. To produce a native binary:

```
tkc src/hello.toke | clang -x ir - -o hello
```

### Compiler flags

| Flag | Effect |
|------|--------|
| `--diag-text` | Human-readable diagnostics (default: JSON to stderr) |
| `--emit-interface` | Write a `.tokei` interface file alongside compilation |
| `--target <triple>` | LLVM target triple (default: host) |

---

## Contributing

See [CONTRIBUTING.md](https://github.com/karwalski/tkc/blob/main/CONTRIBUTING.md) in the tkc repository. All commits must be signed off (DCO). Security issues: see [SECURITY.md](https://github.com/karwalski/tkc/blob/main/SECURITY.md).

---

## Licence

MIT. See [LICENSE](LICENSE).
