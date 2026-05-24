# Out-of-Band Reasoning Channel

**Status:** Normative design decision (v0.3.2)
**Date:** 2026-05-23
**Context:** Research feedback (23-May gate2 review) identified that removing comments from source is "self-defeating unless an out-of-band reasoning channel is mandated."

---

## Decision

**toke mandates companion files (`.tkc`) as the out-of-band reasoning channel.** This is not optional — it is a first-class language mechanism specified in §24.11 of toke-spec-v0.3.

The reasoning channel serves three purposes:

1. **LLM chain-of-thought during generation** — the model reasons about the problem in the `.tkc` file before or during code generation in the `.tk` file
2. **Human documentation** — API docs, design rationale, usage examples live in `.tkc`, not inline
3. **Training signal** — the reasoning trace in `.tkc` can be used as supervised training data for the model's planning capability

## How It Works

```
main.tk              ← token-minimal source (what the compiler sees)
main.tkc.md          ← reasoning, docs, rationale (freeform Markdown)
main.tkc.yaml        ← line-targeted annotations (future, v0.4)
main.tkc.json        ← machine metadata (future, v0.4)
```

The compiler never reads `.tkc.*` files. The IDE, MCP server, and training pipeline do.

The `.tkc.<format>` extension convention is forward-compatible — `.tkc.md` for prose, `.tkc.yaml` for structured per-function/per-line comments, `.tkc.json` for machine metadata. See spec §24.11.

### For LLM code generation

The MCP server's `toke.compile` and `toke.repair` tools accept an optional `reasoning` field. The model can:
1. Plan its approach in natural language (reasoning tokens — these are "free" because they don't appear in the compiled output)
2. Write the toke source
3. The reasoning is saved to `.tkc` for future reference

This directly addresses the Reflexion finding (+11% on HumanEval from verbal self-reflection): the model gets its reasoning tokens, but they don't inflate the source token count.

### For human developers

```
# main.tkc — companion file for main.tk
#
# This module implements the HTTP API server.
# Routes: GET /health, GET /users/:id, POST /users
#
# Design decision: using Result types for all handlers
# so errors propagate cleanly to the HTTP response layer.
# See architecture.md for the error handling strategy.
```

### For training

The `.tkc` content becomes part of the training record:
```json
{
  "text": "<|im_start|>system\n...<|im_end|>\n<|im_start|>user\nWrite a toke program that...<|im_end|>\n<|im_start|>assistant\n(* reasoning: I need to read JSON input, iterate the array, accumulate a sum, and print the result. I'll use std.json for parsing and std.str for argv. *)\nm=sum;i=j:std.json;i=s:std.str;f=main():i64{...};<|im_end|>",
  "reasoning": "I need to read JSON input, iterate the array, accumulate a sum..."
}
```

The `(* ... *)` comment syntax is tolerated by the lexer (silently discarded) specifically for this purpose — the model can emit reasoning in `(* *)` blocks during generation, and the compiler ignores them. The reasoning is captured by the training pipeline.

## Interaction with Token Efficiency

| Channel | Tokens counted? | Purpose |
|---------|-----------------|---------|
| `.tk` source | Yes (this is what we measure) | Compilable code — token-minimal |
| `.tkc` companion | No (not compiled) | Documentation, rationale, API docs |
| `(* *)` in source | Discarded by lexer | LLM reasoning during generation |
| MCP `reasoning` field | Separate from source | Chain-of-thought for repair loops |

The token reduction claim (52% vs cl100k) applies to the `.tk` source only. The reasoning channel is unlimited — use as many reasoning tokens as needed.

## v0.4 Roadmap

The `.tkc.*` extension family is established in v0.3. v0.4 will formalise:

- **`.tkc.md`** (available now): Freeform Markdown prose. Convention from loke (699 files): first sentence = what it does, mention exports and dependencies. No mandatory structure.
- **`.tkc.yaml`** (v0.4): Structured per-function documentation with line-targeted annotations. Enables "comments" on specific lines without touching source.
- **`.tkc.json`** (v0.4): Machine-readable metadata for IDE hover, training pipeline labels, diagnostic annotations.
- `toke --companion` generates `.tkc.md` skeletons from function signatures
- IDE hover displays companion content; MCP `toke.search_stdlib` includes it in search results

## Relationship to Research Feedback

The research review states: *"Chain-of-thought research is unambiguous: intermediate reasoning tokens improve correctness. Reflexion reports +11% on HumanEval. Banning comments is self-defeating unless an out-of-band reasoning channel is mandated."*

This is correct. toke's response:
1. Comments **are** banned from compiled source (by design — this is the token efficiency win)
2. Reasoning **is** mandated via `.tkc` companion files and `(* *)` lexer tolerance
3. The MCP server provides explicit reasoning fields in the repair loop protocol
4. Training records can include reasoning traces without inflating source token counts

The key insight: **reasoning tokens and source tokens serve different purposes.** Mixing them (as comments in Python/Java) wastes inference budget. Separating them (as toke does) lets the model reason freely while keeping the compiled output minimal.
