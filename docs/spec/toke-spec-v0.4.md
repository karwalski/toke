---
title: toke Language Specification — v0.4 Amendment
slug: toke-spec-v0.4
section: spec
order: 1
---

# toke Language Specification — v0.4 (Normative Amendment)

**Status:** normative. **Date:** 2026-07-02. **Supersedes:** the listed sections of
[toke-spec-v0.3](/docs/spec/toke-spec-v0.3/). **Deciders:** Matt Watt.

This document is a *delta amendment*. Everything in `toke-spec-v0.3.md` remains
normative **except** the sections superseded below. v0.4 is a **breaking** release
of the surface grammar, motivated by the "toke-for-LLM foundations" audit
(Epic 116): the language must be strictly deterministic to parse and must align
with the priors every base model already holds (`==` for equality, `if` as an
expression, `&&`/`||` for short-circuit logic), so that constrained decoding and
from-scratch training see one canonical, backtrack-free form.

The five v0.4 changes are governed by [ADR-0007](/docs/decisions/ADR-0007/)
(expression-`if`), [ADR-0008](/docs/decisions/ADR-0008/) (`=`/`==` split), and
[ADR-0009](/docs/decisions/ADR-0009/) (backtrack-free parser). The machine-readable
grammar of record is [`grammar.ebnf`](https://github.com/karwalski/toke/blob/main/docs/spec/grammar.ebnf) (with its normative
**Appendix A**, FIRST-sets & bounded-lookahead) and [`toke.gbnf`](https://github.com/karwalski/toke/blob/main/docs/spec/toke.gbnf)
(GBNF for constrained decoding).

---

## A. Keywords (supersedes v0.3 §8, the "13 keywords" list)

The keyword set is **14** (verified against the lexer keyword table):
`m i t f let if el lp br rt as mt sc mut`. The v0.3 "13 keywords" wording
(and the `grammar.ebnf` header, and `guide/01-why-toke.md`, which each drop a
different one) is retired — both `mut` and `sc` are keywords. The logical
operators are lexical, not keywords. Two-character operator tokens `==`, `!=`,
`<=`, `>=`, `&&`, `||` are recognised by the lexer. (The keyword *count* is not
itself a v0.4 change; it is corrected here because v0.4 is now the authoritative
spec.)

## B. `=` is assignment/binding; `==` is equality (supersedes v0.3 §8, §10, §13)

In v0.3, `=` denoted **equality** and there was no `==`; binding and assignment
also used `=`, resolved positionally. v0.4 removes this overload:

- `=` — **only** binding (`let x = e`), mutable binding (`let x = mut.e`),
  assignment (`x = e`), declarations (`m=`, `i=`, `t=`, `f=`), and the loop
  init/step clauses. Always statement position.
- `==` — **equality comparison**. `!=` is inequality. Both are expression position.
- A bare `=` used in expression/comparison position is a **hard parse error**,
  **E2002** — `` `=` is assignment; use `==` for equality ``.

Grammar (supersedes the `EqExpr` production):

```
EqExpr = CompareExpr [ ( '==' | '!=' ) CompareExpr ] ;
```

**Rationale / effect:** this is the change that makes the loop-init ambiguity
(`lp(go=1)` — init vs `go == 1`) disappear, letting the parser drop its unbounded
forward scan (see §D). It also matches the universal base-model prior for `==`.

## C. `if` is an expression (supersedes v0.3 §11 "IfStmt")

`if` may appear in expression position and yields a value. An expression-`if`
**requires** an `el` arm (no partial value). Both arms are blocks whose value is
the block's tail expression; the two tails must have the same type.

```
IfExpr = 'if' '(' Expr ')' Block ( 'el' IfExpr | 'el' Block ) ;
Block  = '{' StmtList '}' ;
Expr   = IfExpr | MatchExpr | LogOrExpr ;
```

The statement form (`if(c){..}` with optional `el`, value discarded) is unchanged.
Idiomatically, expression-`if` replaces the v0.3 "declare a `mut` flag, then assign
it inside an `if`" pattern (see [idiom-v0.4](/docs/spec/idiom-v0.4/)).

```
let sign = if(n<0){ -1 } el if(n>0){ 1 } el { 0 };
```

## D. `&&` and `||` — short-circuit logical operators (supersedes v0.3 §11)

`&&` (logical and) and `||` (logical or) are normative, left-associative, and
short-circuit. Precedence: `||` binds looser than `&&`, which binds looser than
equality/comparison.

```
LogOrExpr  = LogAndExpr { '||' LogAndExpr } ;
LogAndExpr = EqExpr { '&&' EqExpr } ;
```

They replace the v0.3 "flag-soup" idiom (multiple `if(..){flag = ..}` writing the
same variable). Operands are `bool`.

## E. Grammar is backtrack-free with bounded lookahead (supersedes v0.3 §3 ¶"LL(1)", §10 tail, and the "Appendix X" promise in the Changelog)

v0.3 asserted a strict LL(1) grammar decidable with **exactly one** token of
lookahead, and promised a FIRST/FOLLOW "Appendix X" that was never delivered. That
claim was **not accurate** for the real grammar. v0.4 states the honest, verified
property:

> The toke grammar is **backtrack-free**: the parser never rescans input it has
> already consumed. It is **not** pure LL(1); a small, **enumerated** set of
> productions require **bounded lookahead of up to 3 tokens** (never more). These
> exceptions (E1–E5) are listed normatively in **Appendix A of `grammar.ebnf`**,
> together with the FIRST-sets. An implementation that backtracks, or that requires
> unbounded lookahead at any production, is non-conforming.

The `=`/`==` split (§B) is what removed the worst former offender (loop-init
unbounded scan). The `.get(...)` postfix no longer backtracks. This section
supersedes v0.3 §3 (the "LL(1) … one token of lookahead" sentence), §10's closing
LL(1) paragraph, and lines asserting "more than one token of lookahead is
non-conforming".

## F. `str.fields` (extends v0.3 §16 stdlib `str`)

`str.fields(s: $str) -> [$str]` splits `s` on runs of whitespace, discarding empty
fields (whitespace-run split). Complements `str.split(s; sep)`. Normative signature
lives in `stdlib/str.tki`.

## F.1 `str.indexfrom` (extends v0.3 §16 stdlib `str`; story 127.91)

`str.indexfrom(s: $str; sub: $str; start: i64) -> i64` is `str.indexof` with a
start offset: the byte offset, counted from the start of `s`, of the first
occurrence of `sub` at or after byte `start`, or `-1` if there is none. A
negative `start` searches from 0; a `start` past the end of `s` finds nothing;
a `start` equal to the length of `s` finds only the empty string. It is a new
name rather than a third parameter on `str.indexof`, so every existing
two-argument call keeps its meaning. A scanner can therefore advance through a
string without re-slicing its tail before every search. Normative signature
lives in `stdlib/str.tki`.

Cost: the search is `strstr` from `start`. Because a toke string is a bare
NUL-terminated buffer with no stored length, checking that `start` lies inside
`s` reads the `start` bytes before it (one `memchr`, no allocation). A scan
that calls it once per token is therefore still O(tokens × length) in bytes
READ, but it no longer allocates or copies per token.

---

## G. Character set is 59 (supersedes v0.3 §7.1 and the RFC's "56")

The default-profile alphabet is **59 characters**: 26 lowercase letters `a-z`,
10 digits `0-9`, and **23 symbols**

```
( ) { } = : . ; + - * / < > ! | " $ @ % & ^ ~
```

No character outside this set SHALL appear in a structural position. Arbitrary
UTF-8 is permitted inside string literal content, where `\` is the escape
introducer.

**Normatively excluded** from structural positions: uppercase `A-Z` (the parser
rejects an uppercase identifier with E2002), underscore `_` (no-underscore rule,
story 113.2a), and `` ' , ? [ ] ` `` — together with `\` outside a string
literal. The lexer accepts `#` only to emit W1020 ("Python comment detected") and
skip the line; that is error recovery, not language surface, and `#` is **not** a
member of the alphabet.

**Why this supersedes two published counts.** v0.3 §7.1 and the derived
documentation stated **55** (19 symbols) and
`spec/rfc/draft-karwalski-toke-lang-00.md` §5.1 stated **56** (a table listing 21
symbols under a total of 20). Neither matched the reference compiler:

- `^` and `~` were described as "reserved and unassigned, MUST NOT be used".
  Story **114.8** assigned them **bitwise XOR** and **bitwise NOT**. They are
  live operators and `let e=a^b; let g=~a;` compiles.
- `%` (modulo) and `&` / `&&` (bitwise and, short-circuit and) were listed as
  excluded symbols while being live operators — `&&` is normative in §D of this
  document.

`src/lexer.c` is the ground truth for the alphabet, as E1003 is the character-set
diagnostic. The count is derived mechanically by
`scripts/verify_project_facts.py` (`--probe` re-derives it against the built
compiler) and recorded once in `docs/metrics-baseline.md` § Project facts, which
is the single source for it. Resolved by story **132.14**, 2026-09-19.

The **legacy** profile (`--legacy`, non-normative, historical) is **86**
characters: 26 lowercase + 26 uppercase + 10 digits + 24 symbols. `tkc --legacy`
rejects only `` $ @ ' , ? \ ` ``. The "80-character" figure in earlier
documentation was never measured.

---

## H. Comments are `(* ... *)` (supersedes v0.3 §8.10 and the §7.2 "not a language feature" bullet)

`(* ... *)` is toke's comment form. It is part of the language, not a
tolerance mechanism:

- A comment opens with `(*` and closes with the matching `*)`. Comments
  **nest**: `(* outer (* inner *) *)` is one comment, so every `(*` inside a
  comment needs its own `*)`. A comment still open at end of file is
  **E1007**.
- A comment produces no token. It separates tokens the way whitespace does,
  may appear wherever whitespace may, and may span lines. Inside a string
  literal, `(*` and `*)` are ordinary text.
- `(`, `*` and `)` are already members of the alphabet (§G), so the comment
  form adds no character.
- The grammar's productions are over tokens and never mention comments; see
  the token-class note in `grammar.ebnf`.

`//` and `#` are **not** comment syntax. The lexer skips each to end of line
only as error recovery and warns **W1020** ("toke comments are written
(* comment *)"). The warning's `fix` is the rewrite to `(* comment *)`, offered
only when the comment's text holds no `(*` or `*)`, since either would change
where the rewritten comment ends (AGENTS §6).

Companion files (`.tkc`, v0.3 §24.11) remain the home for documentation that
tools consume: API docs, parameter descriptions, line-targeted annotations.
A `(* *)` comment is for the reader of the source.

`tkc --migrate` keeps the comments of the code it rewrites: each goes back on
its line, `//` and `#` become `(* *)`, and a comment with code on both sides
of it moves to the end of its line with a note. When it cannot place one
safely it declines (E9026, exit 4) instead (story 127.161).

Resolved by story **127.198** (owner decision 2026-10-01 §2.7).

---

## Conformance & migration

- `make conform` remains the gate (228 cases: 222 YAML + 6 shell, 0 failed as of
  2026-09-19) and now encodes the v0.4 forms.
- Source migration `=`→`==` (equality only) is mechanical and compiler-driven:
  the compiler emits E2002 at the exact byte offset of every equality `=`, so
  migration is a precise, false-positive-free rewrite (`scripts/migrate_eq*.py`).
- v0.3 sources do **not** compile under v0.4 unmodified (equality `=` is now an
  error). This is intentional and is the reason v0.4 is a major, breaking bump.

## Changelog

- **v0.4 (2026-07-02):** `=`/`==` split; `if` is an expression (requires `el`);
  `&&`/`||` short-circuit operators; grammar characterised honestly as
  backtrack-free with bounded ≤3-token lookahead (retires the inaccurate strict
  "one-token LL(1)" claim and the undelivered "Appendix X"); `str.fields`;
  keyword count corrected to 14.
- **2026-09-19 (story 132.14):** §G added — the character set is **59**, not the
  55 stated in v0.3 §7.1 or the 56 stated in the RFC. Resolved against
  `src/lexer.c`; `^ ~ % &` are live operators, none is reserved.
- **2026-09-24 (story 127.91):** §F.1 added — `str.indexfrom(s; sub; start)`,
  a start-offset form of `str.indexof`. Additive; no existing call changes.
- **2026-10-06 (story 127.198):** §H added — `(* ... *)` is the official
  comment form (supersedes v0.3 §8.10's "tolerance only"). W1020's text for
  `//` no longer says toke "has no comment syntax".
