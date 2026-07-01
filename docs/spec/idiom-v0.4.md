---
title: toke v0.4 — Minimal-code idiom standard
slug: idiom-v0.4
section: spec
---

**Status:** normative (Epic 116 / Workstream B). **Audience:** the generation
prompt (Workstream C), the idiom judge (`toke-model/corpus/judge`), the `--min`
canonical formatter, and human authors.

## Why

toke's value is token efficiency for LLM code generation. The honest program
library (114.27) showed the corpus was *verbose* — the median program used more
tokens than Python — driven by "one mutation per line" mut-flag soup, deep
`if/el` ladders, and hand-rolled character parsers. Most of that was the
**language forcing verbosity** (no expression-`if`, no `&&`/`||`) plus a
generation prompt that never mentioned brevity. The v0.4 language work (Workstream
A) removed the structural drivers; this document is the **one right way** to
write each construct for minimum tokens on first shot. Token counts below are
measured with the toke BPE v03 tokenizer.

The single biggest lever is **use the stdlib instead of hand-rolling** — 507
corpus files hand-parse JSON while 11 use `json.dec` (44% fewer tokens, and
correct on the first shot). The per-construct wins are individually modest
(8–18%) but compound across a program, and the real prize is *first-shot
correctness*: idiomatic code is shorter *and* more likely to compile and pass.

## Rules

### 1. Expression-`if`, never a mut-flag
An `if` yields a value (A1). Bind or return it directly.
```
BAD   let g=mut.0; if(x>0){g=1}el{g=2};      (* 11 tok *)
GOOD  let g=if(x>0){1}el{2};                 (*  9 tok, −18% *)
```
Use `el if` for chains; never build a value with `let v=mut.…` followed by an
`if` that assigns to it.

### 2. `&&` / `||`, never a flag
Boolean logic short-circuits (A2). Never simulate `||` with two `if`s setting a flag.
```
BAD   let ok=mut.0; if(a>0){ok=1}; if(b>0){ok=1};   (* 12 tok *)
GOOD  let ok=if(a>0||b>0){1}el{0};                  (* 11 tok, and 1 stmt *)
```

### 3. `==` for equality, `=` for assignment
v0.4 (A3): `==` compares, `=` binds/assigns. `if(x==0)`, not `if(x=0)`.

### 4. Use the stdlib — never hand-roll a parser
This is the largest single win. The stdlib has typed, working parsers.
```
BAD   a per-char lp scan with s.charat/s.slice to read JSON   (* ~16+ tok, brittle *)
GOOD  let d=mt j.dec(src){$ok:v v;$err:e 0};                  (*  9 tok, −44%, correct *)
```
- JSON → `json.dec` + typed accessors `json.str`/`i64`/`u64`/`f64`/`bool`/`arr`.
- CSV → `csv.parse` / `csv.reader`+`csv.next`.
- Split on a delimiter → `str.split(s;sep)`; split on **whitespace runs** →
  `str.fields(s)` (A6 — drops empty fields; do not hand-roll a tokeniser).
- Search/slice → `str.indexof` / `str.contains` / `str.slice` /
  `str.startswith` / `str.endswith` / `str.replace` — not a manual scan.
- Parse a number → `str.toint` / `str.tofloat` (handle the error arm).

### 5. String building — interpolation or `str.join`, not nested `str.concat`
```
BAD   s.concat(s.concat(s.concat("x=";a);", y=");b)   (* 18 tok, unreadable *)
GOOD  s.interpolate(...) / a single s.join(sep;parts) (* fewer tok, flat *)
```
Never nest `str.concat` more than once; build a `[str]` and `str.join`, or
interpolate. (`+` is numeric-only, ADR-0004 — it is not string concat.)

### 6. Chain postfix calls
`s.split(line;",").get(0)` — one expression (A4 made chained `.get` clean). Do
not bind an intermediate `let parts=…; parts.get(0)` unless `parts` is reused.

### 7. Stdlib combinators over manual loops (where it reads clearly)
Prefer `arr.map`/`filter`/`fold` to a `lp` that rebuilds an array, when the
transform is a simple per-element function. Keep an explicit `lp` when the body
is stateful or early-exits.

### 8. No dead `mut`, no redundant bindings
`let x=mut.v` only when `x` is actually reassigned (lint: `mutable-never-mutated`).
Inline a `let` used exactly once into its use site.

### 9. `match` (`mt`) for error unions, with `<` early-return in the err arm
```
GOOD  let v=mt str.toint(s){$ok:n n; $err:e <0};   (* propagate/handle inline *)
```

## Anti-patterns (the idiom judge rejects these)
- `let X=mut.<lit>` immediately followed by an `if` that only assigns to `X`.
- Two or more `if`s that set the same flag (use `||`/`&&`).
- A `lp` scanning `s.charat`/`s.slice` one char at a time to parse structured
  text for which a stdlib parser exists (`json`/`csv`/`str.*`).
- `str.concat` nested ≥ 2 deep.
- `=` used as equality (a v0.4 compile error, but flag the *intent* upstream).
- `let` bound once and used once.

## Enforcement
- **`tkc --min`** (Workstream B2) emits the deterministic single-line canonical
  form — the training target and tokenizer input.
- **Idiom lint** (`lint-rules-v1.md` extension) flags the anti-patterns above.
- **The idiom judge** (`qwen_judge.py`, B3) makes an idiom score a *hard* corpus
  acceptance gate (not the current soft, ignored 20% weight).
- **The generation prompt** (`generate_toke.md`, Workstream C) teaches these
  rules instead of the old verbose survival rules.
