# toke

toke is a compiled programming language designed for LLM code generation. It has 14
keywords, a 59-character set, a backtrack-free grammar with bounded lookahead, and one
canonical form per construct, chosen by measurement in a 46-pattern catalogue and
reproduced by `tkc --min`. That makes generated code cheap to constrain during decoding,
cheap for a compiler to verify afterwards, and compact to emit. Token efficiency is one
measured property of toke, always reported with its tokenizer and its baseline, not the
whole claim.

*This paragraph is the canonical description. It is reproduced word for word from
[`docs/about/canonical.md`](docs/about/canonical.md); every number in this README comes
from [`docs/metrics-baseline.md`](docs/metrics-baseline.md) and nowhere else.*

## Why the grammar comes first

The argument for toke is mechanical, and it is about the grammar and the compiler, not
about a tokenizer.

1. **Small.** `docs/spec/toke-spec-v0.4.md` §A fixes the keyword set at 14 (`m i t f let
   if el lp br rt as mt sc mut`), over a closed alphabet of 59 printable ASCII
   characters — 26 lowercase letters, 10 digits, 23 symbols — lowercase only, with no
   underscores. The design property is not the number but that the alphabet is small and
   closed: a small terminal alphabet is a small vocabulary for whatever unit a model
   generates in. The count is derived from `src/lexer.c` by
   `scripts/verify_project_facts.py`; earlier published figures of 55 and 56 were both
   wrong (see [`docs/metrics-baseline.md`](docs/metrics-baseline.md) § Project facts).
2. **Structured.** §E: the parser never rescans input it has already consumed, and a
   small, enumerated set of productions require bounded lookahead of up to 3 tokens, never
   more. An implementation that backtracks, or that needs unbounded lookahead at any
   production, is non-conforming. Grammar-constrained decoding builds a token mask at
   every step from a pushdown automaton over the grammar; a small, backtrack-free grammar
   keeps that automaton small and its masks cheap. Machine-readable artefacts ship with
   the compiler: [`docs/spec/grammar.ebnf`](docs/spec/grammar.ebnf) and
   [`docs/spec/toke.gbnf`](docs/spec/toke.gbnf).
3. **Canonical.** One measured form per construct. `patterns/catalogue.json` holds 46
   entries across 10 families; a form becomes canonical only by being best-or-tied on
   tokens *and* runtime, and all candidate forms of a pattern must print byte-identical
   output. `tkc --min` reproduces the canonical text, which makes comparison exact rather
   than fuzzy.
4. **Compiler-verified.** `tkc` emits structured diagnostics with stable error codes,
   machine-parseable spans and a fix field — the input a repair loop or a verifiable
   reward function consumes. This is necessary and demonstrably not sufficient on its
   own: see the correctness numbers below.

The honest caveat on point 2, stated first: grammar-constrained decoding works on *any*
grammar, including Python's. The advantage a purpose-built grammar has is one of degree —
a cheaper mask, a smaller invalid space — not of kind, and measuring that degree is open
work. The full argument, the counter-evidence and the falsification tests are in
[`docs/about/positioning-2026-09.md`](docs/about/positioning-2026-09.md).

## Token efficiency

**Token efficiency, measured:** under one shared tokenizer (cl100k_base) toke costs
**1.34× [1.22, 1.48]** the tokens of equivalent Python on the 60 Gate-1 tasks (N = 60,
2026-09-19) — more, not fewer. The v0.3-era "52% fewer tokens" figure was a
*tokenizer-vs-tokenizer* measurement on identical toke text (Toke-16K v0.3 vs cl100k_base,
N = 42) and is superseded: on canonical v0.4 text the shipped 8K tokenizer needs **15.4%
more** tokens than cl100k_base (N = 2,000). See `docs/metrics-baseline.md`.

## Key features

- **Compiled to native code** via LLVM — standalone binaries for x86-64 and ARM64,
  single-pass C99 compiler with no dependencies beyond LLVM
- **Structured diagnostics** — stable error codes, machine-parseable spans and a fix
  field, emitted as JSON by default, designed for automated repair loops
  ([`docs/reference/errors.md`](docs/reference/errors.md))
- **One canonical form** per construct, reproduced by `tkc --min`, so two implementations
  either produce identical canonical text or they do not
- **Machine-readable grammar** — EBNF and GBNF artefacts for constrained decoding
- **<!--fact:stdlib_modules-->65<!--/fact--> standard library modules** (`stdlib/*.tki`) with C runtime backing — strings,
  JSON, TOON, HTTP server/client, database, crypto, ML, and more
- **Error handling with result types** — no exceptions; errors are values handled
  explicitly with `mt` (match)

## Project status

Current compiler: **toke 3.0.0** (`tkc --version`). Spec: **v0.4** —
[`docs/spec/toke-spec-v0.4.md`](docs/spec/toke-spec-v0.4.md) is the authority; v0.3 is
historical.

**3.0.0 is the first compiler version that names v0.4.** `VERSION` read `2.8.0`
from 2026-06-21 through the whole v0.4 break, so `2.x` means pre-v0.4 and will
not compile v0.4 source. Downstreams should require `>= 3.0.0`; the break and
its per-change commits are listed in [`CHANGELOG.md`](CHANGELOG.md).

**No v0.4-native model exists, and every model number below is from a v0.3-syntax model.**
The corpus, tokenizer and model must be refreshed before any of these carry forward.

| Evaluation | Date | Result |
|---|---|---|
| Gate 1 | 2026-04-03 | **58.8% functional Pass@1** (588 of 1,000 generated), fine-tuned 7B on v0.3 syntax; compile rate 92.3% (923/1,000). Published as "63.7% compile Pass@1" until 2026-09-19 — wrong on both counts: the number was 588/**923**, which drops non-compiling solutions from the denominator, and the label was wrong (the figure is functional, not compile). 58.8% is below Gate 1's declared 60% minimum, so **the Gate 1 verdict is re-opened and has not been re-decided** (story 128.19). |
| Gate 2 (curated set) | 2026-05-22 | **100% compile Pass@1** and **55.6% functional** (272/489) on the curated 500-hidden + 200-eval set; Qwen 2.5 Coder 7B + QLoRA, v0.3 syntax |
| Full-local re-audit (the honest floor) | 2026-05-28 | **37.5% compile** (655/1,748) and **about 2.2% fully correct** (38 PASS) across all 1,748 v0.3.9 corpus programs |
| Gate-1 60 re-delivered on v0.4 | 2026-09-19 | 60/60 `tkc --check`, 60/60 hidden tests (120 cases each), lint 0/0 |

Never quote the 100% without the curated set it was measured on. The 2026-09-19
re-delivery is **hand-written, not model-generated** — 27 ids are pure `--migrate` output
and 33 were hand-repaired — so it measures what the *language* can express, not what a
*model* produces, and it may not be quoted as a model result or as a Pass@1.

**What works:** the compiler is stable at 3.0.0, the conformance suite passes, and
compile-checking removes an entire error class cheaply. **What does not yet:** functional
correctness is the open weakness on every honest number in
[`docs/metrics-baseline.md`](docs/metrics-baseline.md), and no toke tokenizer currently
beats a general-purpose one. Next: execution-verified training and the falsification tests
listed in the positioning brief.

## Sub-projects

ooke, loke and moke are toke sub-projects, not separate products: ooke is toke's web
framework and static site generator, and it serves tokelang.dev; loke is toke's local
intelligence layer; moke is loke's data-analysis demo. All three are written in toke.

## Not to be confused with

toke is a programming language. It is not the slang word for a draw on a cigarette, not
the cannabis brands that use the name, not the TOKE crypto tokens, not Tokelau or its
`.tk` country-code domain, and not tokelang.com, which is an unrelated third-party
project. The language is at tokelang.dev and github.com/karwalski/toke.

## Quick start

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

## Install & use

**Build from source** (current primary method):

```bash
git clone https://github.com/karwalski/toke.git
cd toke
make
./build/tkc --version
```

**Homebrew** — *not published yet.* The tap `github.com/karwalski/homebrew-toke`
does not exist, so `brew tap karwalski/toke` returns 404 today. The formula is
written and waiting on a publish step (story 132.18); build from source until then.

**Run the model locally** via Ollama:

```bash
ollama run karwalski/toke
```

Verified 2026-09-20: the manifest resolves and the model layer is 4.7 GB. The
published model is the Gate 2 v0.3-syntax model described above; it does not write
v0.4.

**Hosted API — withdrawn, 2026-09-20.** This section used to publish a working
`curl` against `https://api.tokelang.dev/v1/generate` with a free-tier signup at
`console.tokelang.dev`. **Neither is available.** The inference endpoint behind the
API (the SageMaker endpoint `toke-cloud-qwen-endpoint`) was deleted on 2026-09-18,
so a caller holding a valid key gets *"Endpoint not found. The SageMaker endpoint
has not been deployed."*; the gateway still answers, which makes the failure look
like a key problem rather than a withdrawn service. `console.tokelang.dev` resolves
but its origin does not answer at all.

The example is **withdrawn rather than deleted**, because deleting it silently
would leave the same impression the broken example did — that a hosted channel
exists. It does not, and there is no date for one. To run the model today, use
Ollama above, or the weights on
[HuggingFace](https://huggingface.co/karwalski/toke).

## Tooling & integrations

| Platform | Package | Install |
|----------|---------|---------|
| VS Code | `tokelang.toke-language` | Extensions: search "toke" |
| Open VSX | `tokelang.toke-language` | [open-vsx.org/extension/tokelang/toke-language](https://open-vsx.org/extension/tokelang/toke-language) |
| npm (MCP) | `@tokelang/mcp-server` | `npx @tokelang/mcp-server` |
| npm (LSP) | `@tokelang/lsp` | `npm install -g @tokelang/lsp` |
| PyPI | `toke-tokenizer` | `pip install toke-tokenizer` |
| Ollama | `karwalski/toke` | `ollama run karwalski/toke` |
| HuggingFace | Model | [huggingface.co/karwalski/toke](https://huggingface.co/karwalski/toke) |
| HuggingFace | Tokenizer | [huggingface.co/karwalski/toke-tokenizer](https://huggingface.co/karwalski/toke-tokenizer) |
| Docker | Self-host | `docker compose up` (see `toke-model/docker/`) |
| ~~API~~ | ~~REST~~ | **withdrawn 2026-09-20** — the inference endpoint was deleted 2026-09-18 |
| ~~Console~~ | ~~Web UI~~ | **withdrawn 2026-09-20** — the origin does not answer |

Every row above except the two struck ones was re-checked on 2026-09-20 and resolves.

## Project structure

| Directory | Contents |
|-----------|----------|
| `src/` | Reference compiler (`tkc`) -- lexer, parser, type checker, LLVM backend |
| `src/stdlib/` | C runtime implementations for standard library modules |
| `spec/` | Language specification, formal grammar (EBNF), and semantics |
| `stdlib/` | Standard library interface files (`.tki`) and documentation |
| `patterns/` | The pattern catalogue that decides the canonical form of each construct |
| `test/` | Conformance tests, end-to-end tests, stdlib unit tests, fuzz tests |
| `docs/` | Architecture docs, project tracking, security policies |
| `examples/` | Complete example programs (CLI tools, web apps, REST APIs) |
| `bench/` | Compiler benchmark programs and performance scripts |
| `tree-sitter-toke/` | Tree-sitter grammar for editor syntax highlighting |
| `scripts/` | Build, deployment and claim-guard helper scripts |
| `wasm/` | WebAssembly playground (experimental) |

## Documentation

- **Language specification:** [docs/spec/toke-spec-v0.4.md](docs/spec/toke-spec-v0.4.md)
  (v0.4 is normative; [v0.3](docs/spec/toke-spec-v0.3.md) is historical)
- **Formal grammar:** [docs/spec/grammar.ebnf](docs/spec/grammar.ebnf),
  [docs/spec/toke.gbnf](docs/spec/toke.gbnf)
- **Canonical description of toke:** [docs/about/canonical.md](docs/about/canonical.md)
- **Every published number:** [docs/metrics-baseline.md](docs/metrics-baseline.md)
- **Positioning and falsification tests:** [docs/about/positioning-2026-09.md](docs/about/positioning-2026-09.md)
- **Standard library reference:** each module has a `.md` doc in [docs/stdlib/](docs/stdlib/)
  (e.g., [docs/stdlib/str.md](docs/stdlib/str.md), [docs/stdlib/http.md](docs/stdlib/http.md))
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
make            # Build the toke compiler
make clean      # Remove build artifacts
make lint       # Run static analysis (cppcheck + clang-tidy)
```

## Testing

```bash
make conform         # Run the full conformance suite (must pass at 100%)
make test-e2e        # Run end-to-end integration tests
make test-stdlib     # Run standard library unit tests
make fuzz            # Run the fuzzer
make bench           # Run compiler benchmarks
make check-canonical # Fail if a published copy of the canonical block has drifted
make check-metrics   # Fail if a number is published without its tokenizer and its N
make check-facts     # Fail if a countable project number disagrees with the tree
make check-claims-all # The two guards above, across every sibling repository
```

Those four are the claim guards. All four run in `make ci` and, since story 132.41,
in the GitHub workflow. Any new number published in this repository must appear in
`docs/metrics-baseline.md` first.

`check-claims-all` is the one that reaches outside this repository: it runs the same
two rule sets over every sibling repo checked out beside this one — the website, the
spec, the model and tokenizer repos, the MCP server, the console and the cloud repo —
and reports any it cannot find by name rather than passing over them. Until story
132.42 the website was exempt from it, so the site could publish a number this repo
would have rejected.

Documentation is gated separately, by `make check-docs`: every full-program ```toke
block in `docs/` is compiled **and linked**, so a documented call to a function that
exists nowhere fails. It runs its own negative control first (`--self-test`), which
builds a deliberately broken program and fails unless the gate rejects it.

## Supported targets

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
  --min                 emit the canonical minimal form
  --legacy              legacy syntax: 86-character set
  --diag-json           structured JSON diagnostics (default)
  --diag-text           human-readable diagnostics
```

## Standard library

toke ships <!--fact:stdlib_modules-->65<!--/fact--> standard library modules (`stdlib/*.tki`) backed by C runtime
implementations. The most used:

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

The full list is `stdlib/*.tki`; see [docs/stdlib/](docs/stdlib/) for per-module reference.

## Related repositories

The GitHub name is the only correct one for a link or a clone URL; several of these
are checked out locally under a different name. The full map, including the private
and archived repositories, is [docs/about/repos.md](docs/about/repos.md).

| Repository | Description |
|-----------|-------------|
| [toke-spec](https://github.com/karwalski/toke-spec) | Specification, RFC draft and measurement specs |
| [toke-corpus](https://github.com/karwalski/toke-corpus) | Training corpus: generation, audit and freeze pipeline |
| [toke-models](https://github.com/karwalski/toke-models) | Model fine-tuning and evaluation pipeline |
| [toke-tokenizer](https://github.com/karwalski/toke-tokenizer) | Tokenizer training and the token-efficiency baselines |
| [toke-eval](https://github.com/karwalski/toke-eval) | Benchmark tasks and evaluation harness |
| [toke-test-programs](https://github.com/karwalski/toke-test-programs) | Hand-written programs exercising the language and stdlib |
| [toke-mcp](https://github.com/karwalski/toke-mcp) | Model Context Protocol server, language server and VS Code extension |
| [ooke](https://github.com/karwalski/ooke) | ooke -- web framework and static site generator, written in toke |
| [loke](https://github.com/karwalski/loke) | loke -- toke's local intelligence layer, written in toke |
| [toke-web](https://github.com/karwalski/toke-web) | tokelang.dev, built and served by ooke |
| [toke on HuggingFace](https://huggingface.co/karwalski/toke) | Gate 2 model, tokenizer, and model card |

The Homebrew tap (`brew tap karwalski/toke`) is not published yet; see
[docs/about/repos.md](docs/about/repos.md) § Not published.

## Licence

Apache 2.0. See [LICENSE](LICENSE) for the full text and [LICENSING.md](LICENSING.md) for the project's licensing rationale.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to contribute, including branch naming, commit format, DCO sign-off requirements, and the pull request checklist.

All contributions require a `Signed-off-by` trailer in every commit (`git commit -s`).

## Code of conduct

This project follows the [Contributor Covenant v2.1](CODE_OF_CONDUCT.md).

## Security

See [SECURITY.md](SECURITY.md) for the vulnerability disclosure policy.

To report a security issue, email security@tokelang.dev or use GitHub's private vulnerability reporting.

## Reporting issues

Please use [GitHub Issues](https://github.com/karwalski/toke/issues) to report bugs, request features, or ask questions.
