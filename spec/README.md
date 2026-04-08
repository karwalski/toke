# toke specification

The normative language specification for toke (tk) — a compiled,
statically typed programming language designed for LLM code generation.

## What is toke?

toke is a programming language built for machines, not humans. Its syntax
is designed to minimise token usage in LLM generation while eliminating
ambiguity and providing structured, machine-readable compiler diagnostics.

- **Legacy profile:** 80-character set, compatible with existing LLM tokenizers (used for bootstrapping)
- **Default syntax:** 56-character set, for use with the purpose-built toke tokenizer

Programs compile to native machine code via LLVM. No runtime. No GC.

## Repository contents

- `rfc/` — the toke RFC document
- `spec/grammar.ebnf` — the normative formal grammar
- `spec/semantics.md` — type rules and memory model
- `spec/errors.md` — error code registry
- `examples/` — example programs in legacy and default syntax

## Related

| Location | Description |
|----------|-------------|
| [../src/](../src/) | Reference compiler (tkc) |
| [../stdlib/](../stdlib/) | Standard library |
| [toke-model](https://github.com/karwalski/toke-model) | Corpus generation, tokenizer, model training |
| [toke-eval](https://github.com/karwalski/toke-eval) | Benchmarks and evaluation |

## Licence

MIT. See [LICENSE](LICENSE).
