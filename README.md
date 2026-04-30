# toke

toke is a compiled programming language designed so that AI can write better code. It uses a 59-character alphabet -- lowercase letters, digits, and a small set of punctuation -- and just 12 keywords, making programs shorter and more predictable for language models to generate. The result is a language where AI-generated code is more likely to compile correctly on the first try, while remaining readable and maintainable by humans.

## Key Features

- **Compiled to native code** via LLVM -- produces standalone binaries for x86-64 and ARM64
- **59-character alphabet** -- programs use only lowercase letters, digits, and a small set of punctuation, eliminating an entire class of tokenisation ambiguity
- **12 keywords** -- the entire language control flow fits in a short list: `m`, `f`, `t`, `i`, `if`, `el`, `lp`, `br`, `let`, `mut`, `as`, `rt`
- **LL(1) grammar** -- every parse decision is determined by a single token of lookahead, making the language simple for both humans and machines to parse
- **Error handling with result types** -- no exceptions; errors are values returned from functions and handled explicitly with `match`
- **38 standard library modules** with C runtime backing -- from strings and JSON to HTTP servers, database access, cryptography, and machine learning

## Quick Start

```bash
# Build the compiler
git clone https://github.com/karwalski/toke.git
cd toke
make

# Write a program
cat > hello.tk << 'EOF'
m=hello;
i=io:std.io;
f=main():i64{
  io.println("hello world");
  <0
};
EOF

# Compile and run
./tkc hello.tk -o hello
./hello
```

Or use the `toke` wrapper to compile and run in one step:

```bash
./toke hello.tk
```

## Project Structure

| Directory | Contents |
|-----------|----------|
| `src/` | Reference compiler (`tkc`) -- lexer, parser, type checker, LLVM backend |
| `src/stdlib/` | C runtime implementations for standard library modules |
| `spec/` | Language specification, formal grammar (EBNF), and semantics |
| `stdlib/` | Standard library interface files (`.tki`) and documentation |
| `test/` | Conformance tests, end-to-end tests, stdlib unit tests, fuzz tests |
| `docs/` | Architecture docs, project tracking, security policies |
| `examples/` | Complete example programs (CLI tools, web apps, REST APIs) |
| `bench/` | Compiler benchmark programs and performance scripts |
| `tree-sitter-toke/` | Tree-sitter grammar for editor syntax highlighting |
| `scripts/` | Build and deployment helper scripts |
| `wasm/` | WebAssembly playground (experimental) |

## Documentation

- **Language specification:** [spec/spec/toke-spec-v02.md](spec/spec/toke-spec-v02.md)
- **Formal grammar:** [spec/spec/grammar.ebnf](spec/spec/grammar.ebnf)
- **Semantics:** [spec/spec/semantics.md](spec/spec/semantics.md)
- **Standard library reference:** each module has a `.md` doc in [stdlib/](stdlib/) (e.g., [stdlib/str.md](stdlib/str.md), [stdlib/http.md](stdlib/http.md))
- **Example programs:** [examples/](examples/)
- **Architecture decisions:** [docs/architecture/](docs/architecture/)
- **Conventions:** [docs/conventions.md](docs/conventions.md)

## Building

**Prerequisites:**
- C99 compiler (GCC or Clang)
- LLVM (for the code generation backend)
- Make
- zlib (`-lz`)

**Build commands:**

```bash
make            # Build tkc compiler
make clean      # Remove build artifacts
make lint       # Run static analysis (cppcheck + clang-tidy)
```

## Testing

```bash
make conform    # Run the full conformance suite (must pass at 100%)
make test-e2e   # Run end-to-end integration tests
make test-stdlib # Run standard library unit tests
make fuzz       # Run the fuzzer
make bench      # Run compiler benchmarks
```

## Supported Targets

- x86-64 Linux (ELF)
- ARM64 Linux (ELF)
- ARM64 macOS (Mach-O)

## Usage

```
tkc [flags] <source-files>

  --target <arch-os>    cross-compile (x86_64-linux, arm64-macos, etc.)
  --out <path>          output binary path
  --emit-interface      emit .tki interface files
  --check               type-check only, no code generation
  --profile1            legacy profile: 80-character set
  --profile2            default syntax: 59-character set (default)
  --diag-json           structured JSON diagnostics (default)
  --diag-text           human-readable diagnostics
```

## Standard Library

tkc ships with 38 standard library modules backed by C runtime implementations:

| Module | Description |
|--------|-------------|
| `std.str` | String operations (len, concat, slice, split, case, encoding) |
| `std.json` | JSON encoding, decoding, and typed field extraction |
| `std.toon` | TOON (Token-Oriented Object Notation) -- default serialisation format |
| `std.yaml` | YAML encoding, decoding, and typed field extraction |
| `std.toml` | TOML configuration file parsing |
| `std.csv` | CSV reading, writing, and streaming |
| `std.http` | HTTP server and client with routing, TLS, cookies, multipart |
| `std.router` | URL routing with path parameters and middleware |
| `std.ws` | WebSocket client and server |
| `std.sse` | Server-Sent Events |
| `std.db` | Database queries (SQLite3 backend) |
| `std.file` | File I/O (read, write, append, list, delete) |
| `std.env` | Environment variable access |
| `std.process` | Subprocess spawning and control |
| `std.crypto` | SHA-256, HMAC-SHA-256, hex encoding |
| `std.encrypt` | AES-256 encryption and decryption |
| `std.auth` | Authentication (JWT, sessions, OAuth2) |
| `std.encoding` | Base64, hex, URL encoding/decoding |
| `std.time` | Time operations (now, format, since, duration) |
| `std.log` | Structured logging with rotation |
| `std.test` | Test assertions and runner |
| `std.math` | Mathematical functions |
| `std.i18n` | Internationalisation -- locale-aware string bundles |
| `std.template` | HTML/text template engine |
| `std.html` | HTML document generation |
| `std.svg` | SVG graphics generation |
| `std.canvas` | 2D canvas drawing |
| `std.image` | Image encoding and manipulation |
| `std.chart` | Chart and graph generation |
| `std.dashboard` | Dashboard layout composition |
| `std.dataframe` | Tabular data operations |
| `std.analytics` | Data analytics and aggregation |
| `std.ml` | Machine learning (regression, classification) |
| `std.llm` | LLM API client (tool calling, streaming) |
| `std.args` | Command-line argument parsing |

## Related Repositories

| Repository | Description |
|-----------|-------------|
| [toke-model](https://github.com/karwalski/toke-model) | Corpus generation, BPE tokeniser, and model fine-tuning pipeline |
| [toke-eval](https://github.com/karwalski/toke-eval) | Benchmark tasks and evaluation harness |
| [toke-mcp](https://github.com/karwalski/toke-mcp) | Model Context Protocol server for toke |
| [toke-ooke](https://github.com/karwalski/toke-ooke) | toke-on-ooke: interactive web playground |
| [toke-website](https://github.com/karwalski/toke-website) | Project website (tokelang.dev) |

## Licence

Apache 2.0. See [LICENSE](LICENSE) for the full text and [LICENSING.md](LICENSING.md) for the project's licensing rationale.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to contribute, including branch naming, commit format, DCO sign-off requirements, and the pull request checklist.

All contributions require a `Signed-off-by` trailer in every commit (`git commit -s`).

## Code of Conduct

This project follows the [Contributor Covenant v2.1](CODE_OF_CONDUCT.md).

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

To report a security issue, email security@tokelang.dev or use GitHub's private vulnerability reporting.

## Reporting Issues

Please use [GitHub Issues](https://github.com/karwalski/toke/issues) to report bugs, request features, or ask questions.
