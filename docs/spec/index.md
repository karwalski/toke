---
title: Language Specification
slug: index
section: spec
order: 0
---

Normative specification documents for the toke language, including grammar, semantics, and standard library signatures.

## Documents

- [Error Code Registry](/docs/spec/errors/) -- canonical error codes and their meanings
- [Formal Semantics](/docs/spec/semantics/) -- type rules and evaluation semantics
- [Spec-Implementation Delta](/docs/spec/spec-implementation-delta/) -- known differences between spec and compiler
- [Standard Library Signatures](/docs/spec/stdlib-signatures/) -- normative function signatures for `std.*`
- [Language Specification v0.4 (Amendment)](/docs/spec/toke-spec-v0.4/) -- **current**; breaking v0.4 changes (`==` equality, expression-`if`, `&&`/`||`, backtrack-free grammar, `str.fields`)
- [Language Specification v0.3](/docs/spec/toke-spec-v0.3/) -- the full base specification (partially superseded by v0.4)
- [Grammar (EBNF)](/docs/spec/grammar/) -- machine-readable grammar of record, with Appendix A (FIRST-sets, bounded lookahead)
- [Idiom Standard v0.4](/docs/spec/idiom-v0.4/) -- normative minimal-code idiom rules
- [Memory Model Specification](/docs/spec/memory-model/) -- formal arena allocation, lifetime, concurrency, and FFI memory rules
- [Capability Model](/docs/spec/capabilities/) -- deny-by-default fs/net/env/process authority, grant channels, CAP001 (ADR-0010)
