# tkc — the toke reference compiler

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

tkc is the reference compiler for [toke](https://github.com/karwalski/toke-spec),
a machine-native programming language designed for LLM code generation.

## Status

Default Syntax Implementation. Gate 1 passed 2026-04-03 (12.5% token reduction, 63.7% Pass@1).

## DCO

All contributions require a `Signed-off-by` trailer in every commit
(`git commit -s`). See [CONTRIBUTING.md](CONTRIBUTING.md) for details.

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
    --profile1            legacy profile: 80-character set
    --profile2            default syntax: 56-character set (default)
    --diag-json           structured JSON diagnostics (default)
    --diag-text           human-readable diagnostics

## Running the conformance suite

    make conform

## Standard Library

tkc includes a growing standard library with C runtime backing:

| Module | Description |
|--------|-------------|
| `std.str` | String operations (len, concat, slice, split, case, encoding) |
| `std.json` | JSON encoding, decoding, and typed field extraction |
| `std.toon` | TOON (Token-Oriented Object Notation) -- default serialization format, 30-60% fewer tokens than JSON |
| `std.yaml` | YAML encoding, decoding, and typed field extraction |
| `std.i18n` | Internationalisation -- locale-aware string bundles with placeholder substitution |
| `std.http` | HTTP request/response handling and routing |
| `std.db` | Database queries (SQLite3 backend) |
| `std.file` | File I/O (read, write, append, list, delete) |
| `std.env` | Environment variable access |
| `std.process` | Subprocess spawning and control |
| `std.crypto` | SHA-256, HMAC-SHA-256, hex encoding |
| `std.time` | Time operations (now, format, since) |
| `std.log` | Structured logging |
| `std.test` | Test assertions |

toke uses a **TOON-first serialization strategy**: TOON for tabular data, YAML and JSON as secondary formats. See [ADR-0003](https://github.com/karwalski/toke-spec/blob/main/docs/architecture/ADR-0003.md) for the design rationale.

## Supported targets

- x86-64 Linux (ELF)
- ARM64 Linux (ELF)
- ARM64 macOS (Mach-O)

## Language specification

See [toke-spec](https://github.com/karwalski/toke-spec) for the normative
language specification and formal grammar.

## Licence

Apache 2.0. See [LICENSE](LICENSE).
