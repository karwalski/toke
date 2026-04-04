# toke specification

The normative language specification for toke (tk) — a compiled,
statically typed programming language designed for LLM code generation.

## What is toke?

toke is a programming language built for machines, not humans. Its syntax
is designed to minimise token usage in LLM generation while eliminating
ambiguity and providing structured, machine-readable compiler diagnostics.

- **Phase 1:** 80-character set, compatible with existing LLM tokenizers
- **Phase 2:** 56-character set, for use with the purpose-built toke tokenizer

Programs compile to native machine code via LLVM. No runtime. No GC.

## Repository contents

- `rfc/` — the toke RFC document
- `spec/grammar.ebnf` — the normative formal grammar
- `spec/semantics.md` — type rules and memory model
- `spec/errors.md` — error code registry
- `examples/` — example programs in Phase 1 and Phase 2 syntax

## Related repositories

| Repository | Description |
|------------|-------------|
| [tkc](https://github.com/karwalski/tkc) | Reference compiler |
| [toke-stdlib](https://github.com/karwalski/toke-stdlib) | Standard library |
| [toke-corpus](https://github.com/karwalski/toke-corpus) | Corpus generation |
| [toke-benchmark](https://github.com/karwalski/toke-benchmark) | Benchmarks |

## Licence

MIT. See [LICENSE](LICENSE).
