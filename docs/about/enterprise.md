---
title: Enterprise Adoption
slug: enterprise
section: about
order: 6
---

## The Value Proposition

Every token an LLM generates costs compute time and API dollars. When your code generation pipeline produces Python, TypeScript, or Go, the model spends tokens on verbose keywords, ambiguous syntax, and whitespace conventions that were designed for human readability -- not machine efficiency.

toke eliminates this overhead at the language level. Its 55-character set and strict LL(1) grammar have exactly one valid interpretation at every syntactic position. Its mandatory typed interface files compress module contracts into minimal token sequences. toke source is comment-free by design -- there are no comments to add. Documentation lives in companion files (`.tkc`), not inline. The token cost of generated toke programs is fixed regardless of coding standards.

The result: **toke programs use 40--75% fewer tokens than equivalent Python, C, or Java.** A complete fibonacci program in toke uses ~23 tokens with the purpose-built tokenizer, compared to 41 in Python, 59 in C, and 62 in Java. With documentation best practices applied, the gap widens further: Python doubles to 82 tokens and Java grows to 102, while toke stays at 23 -- because toke source is comment-free by design and there are no comments to add. The purpose-built tokenizer is being finalised as part of Gate 2.

## Use Cases

### AI Coding Assistants

Coding assistants that generate toke produce correct programs in fewer tokens. The strict grammar eliminates entire classes of syntax errors, and the structured JSON diagnostic schema enables automated repair loops without parsing English prose.

### Automated Code Generation Pipelines

High-volume pipelines that generate, test, and deploy code benefit directly from token efficiency. Each generation pass costs less, and the deterministic grammar reduces the retry rate for invalid programs.

### Cost Optimization for High-Volume APIs

If your platform serves code generation at scale -- thousands or millions of requests per day -- switching the target language to toke reduces per-request inference cost. The savings compound at volume.

### Internal Tooling

When LLMs write and execute code for internal automation -- data transformations, report generation, workflow orchestration -- toke provides a safe, efficient target language with predictable compilation and execution behaviour.

## Integration Path

toke compiles to native code via LLVM. The compiler (`tkc`) produces a native binary directly, using clang/LLVM as its backend. It supports any target triple clang supports -- x86-64, ARM64, RISC-V, WebAssembly, and more.

```bash
# Compile a toke program to a native binary
tkc program.tk -o program

# Cross-compile for a different target
tkc --target aarch64-linux-gnu program.tk -o program
```

The compiler is a single self-contained binary with no runtime dependencies. It integrates into any CI/CD pipeline, container image, or build system that can invoke a command-line tool.

### Interface Files

toke's mandatory `.tki` interface files act as compressed task descriptions for LLMs. Feed the interface file to a model as context, and it can generate a conforming implementation with minimal prompt engineering.

```bash
# Generate the interface file
tkc --emit-interface module.tk

# The .tki file is now available as LLM context
```

### Diagnostic Integration

Compiler diagnostics are emitted as structured JSON to stderr by default. This enables automated repair loops: generate code, compile, parse the JSON diagnostics, feed errors back to the model, regenerate.

```bash
# JSON diagnostics (default) for machine consumption
tkc program.tk 2> diagnostics.json

# Human-readable diagnostics for debugging
tkc --diag-text program.tk
```

## Security

The toke compiler is built with security as a baseline requirement:

- **SAST-scanned.** Every commit is checked with cppcheck and clang-tidy. Zero error-level findings are permitted.
- **Fuzz-tested.** The compiler frontend is fuzzed to catch crashes, undefined behaviour, and memory safety issues.
- **SBOM and signed releases.** Release artifacts include a software bill of materials and cryptographic signatures.
- **No external dependencies.** The compiler frontend has zero external C library dependencies, minimising supply chain risk.
- **Responsible disclosure.** Security issues are handled through the process described in [SECURITY.md](https://github.com/karwalski/toke/blob/main/SECURITY.md).

## Licensing

toke is released under the **Apache 2.0 licence**. You can use, modify, and distribute the compiler and standard library in commercial products. You can embed the compiler in proprietary products, ship generated binaries to customers, and modify the source for internal use -- with the standard Apache 2.0 obligations (preserving the copyright notice and licence, stating changes).

## Support

- **[GitHub Issues](https://github.com/karwalski/toke/issues)** -- bug reports and feature requests. The maintainers triage and respond to issues regularly.
- **[GitHub Discussions](https://github.com/karwalski/toke/discussions)** -- questions about integration, architecture decisions, and general usage.
- **[Security reports](https://github.com/karwalski/toke/blob/main/SECURITY.md)** -- responsible disclosure for security vulnerabilities.

## Getting Started

1. **Clone and build the compiler.**

   ```bash
   git clone https://github.com/karwalski/toke
   cd toke
   make
   ```

2. **Run the conformance suite** to verify your build.

   ```bash
   make conform
   ```

3. **Compile a toke program** and verify the output.

   ```bash
   tkc src/hello.tk -o hello
   ./hello
   ```

4. **Integrate into your pipeline.** The `tkc` binary is a single static executable. Copy it into your container image or CI environment and invoke it as part of your code generation workflow.

5. **Feed `.tki` interface files to your LLM** as context for code generation tasks. Measure the token reduction against your current target language.
