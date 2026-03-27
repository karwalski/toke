# tkc — the toke reference compiler

tkc is the reference compiler for [toke](https://github.com/karwalski/toke-spec),
a machine-native programming language designed for LLM code generation.

## Status

Phase 1 — Falsification. Under active development.

## Building

Requirements: C99 compiler, LLVM (for the backend), Make.

    git clone https://github.com/karwalski/tkc
    cd tkc
    make

Produces `./tkc` binary.

## Usage

    tkc [flags] <source-files>

    --target <arch-os>    e.g. x86_64-linux, arm64-macos
    --out <path>          output binary path
    --emit-interface      emit .tki interface files
    --check               type-check only, no code generation
    --phase1              80-character set (default)
    --phase2              56-character set
    --diag-json           structured JSON diagnostics (default)
    --diag-text           human-readable diagnostics

## Running the conformance suite

    make conform

## Supported targets

- x86-64 Linux (ELF)
- ARM64 Linux (ELF)
- ARM64 macOS (Mach-O)

## Language specification

See [toke-spec](https://github.com/karwalski/toke-spec) for the normative
language specification and formal grammar.

## Licence

Apache 2.0. See [LICENSE](LICENSE).
