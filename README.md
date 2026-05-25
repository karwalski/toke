# toke

toke is a compiled programming language designed to reduce the token cost of AI-generated code. A purpose-built BPE tokenizer trained on toke programs achieves **52% fewer tokens** on average compared to cl100k_base (GPT-4/Claude's tokenizer) across 42 benchmark programs, and a fine-tuned 7B model writes toke that compiles correctly **100% of the time**.

The token reduction comes from three reinforcing design choices:

1. **A purpose-built tokenizer** trained on real toke code, so common patterns like `f=main():i64{` and `i=j:std.json` merge into single tokens
2. **Structural choices that eliminate overhead** -- no comments in source (documentation lives in companion files), semicolons as the only separator, errors as values not exceptions
3. **A constrained character set and grammar** (55 characters, 13 keywords, LL(1)) that reduces the space of valid programs, making it easier for both the tokenizer and the model to learn

The character set and syntax are means to an end. The goal is measurable: fewer tokens per unit of functionality, validated by compilation and execution.

## Key Features

- **52% token reduction** -- measured with a 16K BPE tokenizer trained on 25,953 toke programs. [Try the live tokenizer](https://tokelang.dev/tokenizer/)
- **100% compilation Pass@1** -- a fine-tuned Qwen 2.5 Coder 7B produces valid toke on every attempt (Gate 2, May 2026)
- **Compiled to native code** via LLVM -- standalone binaries for x86-64 and ARM64, sub-second compile times for fast feedback loops
- **70+ structured diagnostic codes** -- machine-readable JSON errors with fix suggestions, designed for automated repair loops
- **38 standard library modules** with C runtime backing -- strings, JSON, HTTP server/client, database, crypto, ML, and more
- **Error handling with result types** -- no exceptions; errors are values handled explicitly with `mt` (match)

## Project Status

| Milestone | Date | Result |
|-----------|------|--------|
| Gate 1 | 2026-04-03 | 63.7% compilation Pass@1, 12.5% token reduction vs cl100k_base |
| Gate 2 | 2026-05-22 | **100% compilation Pass@1** on 700 tasks. **55.6% functional correctness** (corrected from 8% — stdlib bug; model masters both syntax and semantics) |
| Tokenizer | 2026-05-22 | 16K BPE trained on 25,953 programs. **52% avg token reduction** vs cl100k across 42 benchmarks |

**What works:** the model writes syntactically valid toke every time. The tokenizer compresses toke code significantly. The compiler provides 70+ structured diagnostic codes for automated repair.

**What doesn't yet:** functional correctness is 55.6% (corrected from 8% — the original figure was caused by a missing `io.readln()` C glue function that prevented programs from linking). The remaining gap is a mix of argv-hardcoding patterns and algorithmic errors. Next phase: execution-verified RLVR training with randomised inputs. See [training-next-phase.md](docs/spec/training-next-phase.md).

Three production codebases validate the language and standard library:

- **ooke** -- static site generator and web framework, built in toke, serving [tokelang.dev](https://tokelang.dev)
- **loke** -- privacy and AI platform: 698 files, 87,000 lines of toke across security, networking, and ML
- **moke** -- data analysis demo exercising privacy pipeline, governance, and LLM integration

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
./toke hello.tk -o hello
./hello
```

Or use the `toke` wrapper to compile and run in one step:

```bash
./toke hello.tk
```

## Install & Use

**Build from source** (current primary method):

```bash
git clone https://github.com/karwalski/toke.git
cd toke
make
./build/tkc --version
```

**Homebrew** (coming soon):

```bash
brew tap karwalski/toke && brew install tkc
```

**Run the model locally** via Ollama:

```bash
ollama run karwalski/toke
```

**Generate toke via API** (free tier, no credit card):

```bash
curl -X POST https://api.tokelang.dev/v1/generate \
  -H "X-Api-Key: YOUR_KEY" \
  -d '{"description": "Sum an array"}'
```

## Tooling & Integrations

| Platform | Package | Install |
|----------|---------|---------|
| VS Code | `tokelang.toke-language` | Extensions: search "Toke" |
| Open VSX | `tokelang.toke-language` | [open-vsx.org/extension/tokelang/toke-language](https://open-vsx.org/extension/tokelang/toke-language) |
| npm (MCP) | `@tokelang/mcp-server` | `npx @tokelang/mcp-server` |
| npm (LSP) | `@tokelang/lsp` | `npm install -g @tokelang/lsp` |
| PyPI | `toke-tokenizer` | `pip install toke-tokenizer` |
| Ollama | `karwalski/toke` | `ollama run karwalski/toke` |
| HuggingFace | Model | [huggingface.co/karwalski/toke](https://huggingface.co/karwalski/toke) |
| HuggingFace | Tokenizer | [huggingface.co/karwalski/toke-tokenizer](https://huggingface.co/karwalski/toke-tokenizer) |
| Docker | Self-host | `docker compose up` (see `toke-model/docker/`) |
| API | REST | [api.tokelang.dev](https://api.tokelang.dev) |
| Console | Web UI | [console.tokelang.dev](https://console.tokelang.dev) |

## Project Structure

| Directory | Contents |
|-----------|----------|
| `src/` | Reference compiler (`toke`) -- lexer, parser, type checker, LLVM backend |
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

- **Language specification:** [docs/spec/toke-spec-v0.3.md](docs/spec/toke-spec-v0.3.md)
- **Formal grammar:** [docs/spec/grammar.ebnf](docs/spec/grammar.ebnf)
- **Standard library reference:** each module has a `.md` doc in [docs/stdlib/](docs/stdlib/) (e.g., [docs/stdlib/str.md](docs/stdlib/str.md), [docs/stdlib/http.md](docs/stdlib/http.md))
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
make            # Build toke compiler
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
toke [flags] <source-files>

  --target <arch-os>    cross-compile (x86_64-linux, arm64-macos, etc.)
  --out <path>          output binary path
  --emit-interface      emit .tki interface files
  --check               type-check only, no code generation
  --legacy              legacy syntax: 80-character set
  --diag-json           structured JSON diagnostics (default)
  --diag-text           human-readable diagnostics
```

## Standard Library

toke ships with 38 standard library modules backed by C runtime implementations:

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
| [toke-ooke](https://github.com/karwalski/toke-ooke) | Static site generator and web framework, built in toke |
| [toke-website](https://github.com/karwalski/toke-website) | Project website (tokelang.dev) |
| [toke on HuggingFace](https://huggingface.co/karwalski/toke) | Gate 2 model, tokenizer, and model card |
| [Developer Console](https://console.tokelang.dev) | Free API access for testing toke code generation |

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
