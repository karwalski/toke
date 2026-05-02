# Audit: `"` (String Delimiter) and `\` (Escape Character)

**Story:** 77.1.8
**Date:** 2026-05-01
**Status:** PASS — no issues found

---

## 1. How `"` Is Handled in the Lexer

In `/Users/matthew.watt/tk/toke/src/lexer.c`, the main `lex()` loop (line 603) detects `"` and dispatches to `lex_string()`:

```c
if (c == '"') {
    advance(&l);
    if (lex_string(&l, start, line, col) < 0) return -1;
    continue;
}
```

The opening `"` is consumed by `advance()` before entering `lex_string()`. Inside `lex_string()`, the closing `"` is consumed by another `advance()` (line 410). The emitted `TK_STR_LIT` token spans from the opening quote to one past the closing quote (inclusive of both delimiters in the byte range), but no separate token is emitted for either quote character.

Key behaviour: `"` never produces its own token kind. It is not in the symbol `switch` statement. It exists only as a trigger for `lex_string()`.

---

## 2. Escape Sequences Defined (Spec S7.3)

The spec (Section 7.3) and lexer implementation agree on the following escape sequences:

| Sequence | Meaning | Lexer validates? |
|----------|---------|:---:|
| `\"` | literal double-quote | Yes |
| `\\` | literal backslash | Yes |
| `\n` | newline (U+000A) | Yes |
| `\t` | horizontal tab (U+0009) | Yes |
| `\r` | carriage return (U+000D) | Yes |
| `\0` | null byte (U+0000) | Yes |
| `\xNN` | byte with hex value NN | Yes (exactly 2 hex digits required) |

Any other `\X` sequence triggers E1001 (invalid escape sequence).

Additionally, `\(` is recognised to emit W1010 (string interpolation not supported in Profile 1) and the interpolation expression is skipped by tracking parenthesis depth. This is graceful error recovery, not a supported feature.

---

## 3. Does `\` Appear Outside String Literals?

**No.** The backslash is not in the symbol `switch` statement in `lex()`. If `\` appeared in source outside a string literal, it would fall through to the `default` case and trigger E1003 ("character outside allowed character set").

The lexer's header comment (line 18) lists `\` in the character set, but this is documenting "characters that may appear in source" (including inside strings), not "characters that produce structural tokens."

The spec (S7.2) explicitly lists `\` under excluded characters: it is not assigned as a structural symbol. Its only role is inside string literals as the escape introducer.

---

## 4. Is the Spec's Treatment of `"` Consistent and Defensible?

**Yes.** The spec's position is:

- Section 7.1 character table: `"` is NOT counted in the 56 characters.
- Section 7.1 note: "The double-quote `"` appears in source as the string delimiter for string literals but is not a structural symbol -- it is consumed by the lexer during string literal scanning and never produces a token. It is analogous to whitespace in this regard."
- Appendix operators table: repeats the same note.

This is defensible because:

1. **No token is emitted.** Unlike `(` which produces `TK_LPAREN`, `"` produces nothing -- it triggers string scanning but has no independent token kind.
2. **Analogy to whitespace is apt.** Whitespace separates tokens but is not itself a token. `"` delimits string content but is not itself a token.
3. **The 56-char count is about structural symbols** -- characters that participate in the grammar as tokens. `"` participates at the lexer level only.
4. **Precedent in other languages.** Most language specifications treat string delimiters as lexer-level constructs rather than grammar-level operators.

Minor inconsistency to note: the lexer.c header comment (line 18) lists both `"` and `\` in its "Symbols" line alongside `(`, `)`, etc. This is technically informal documentation (not the spec), but could confuse a reader. The comment is documenting "characters the lexer must handle" rather than "structural symbols."

---

## 5. Could the Language Use a Different String Delimiter?

Alternatives considered:

| Candidate | Assessment |
|-----------|-----------|
| `'` (single quote) | Listed as excluded in S7.2. Would work technically but adds a second quote character with no benefit. |
| `` ` `` (backtick) | Explicitly excluded in S7.2. Commonly used for template literals (JS) or raw strings (Go). Excluded to keep character set minimal. |
| `#"..."#` (Swift-style) | Requires `#` which is excluded. |
| Heredoc syntax | Would require multi-character delimiters foreign to toke's design. |

**Conclusion:** `"` is the correct choice. It is universally understood, requires no additional characters, and has unambiguous open/close semantics (same character for both, with `\"` for embedding).

---

## 6. Ambiguity or Confusion

**No functional ambiguity exists.** The grammar is unambiguous:

- `"` in source always starts a string literal (there is no other context where it could appear).
- `\` inside a string always starts an escape (there is no "raw string" mode).
- `\` outside a string is always an error (E1003).

**One documentation note:** The lexer.c header comment lists 24 symbols for default mode including `"` and `\`, but the spec counts only 18 structural symbols (excluding `"`, `\`, `^`, `~`, `&`, `%`). The header is counting "characters the lexer must recognise" which is a different (valid) perspective, but the discrepancy could confuse a reader cross-referencing with the spec. This is cosmetic; no code change is needed.

**Potential user confusion:** A programmer might wonder why `\` triggers E1003 outside strings rather than a more specific message like "backslash only valid in string literals." This is acceptable -- E1003 is the catch-all for invalid characters and the message is clear enough.

---

## Summary

| Question | Answer |
|----------|--------|
| Does `"` produce a token? | No -- consumed during lexing |
| Does `\` appear outside strings? | No -- would trigger E1003 |
| Is the spec consistent? | Yes |
| Are escape sequences spec-compliant? | Yes -- exact match with S7.3 |
| Any alternative delimiter needed? | No |
| Any ambiguity? | No functional ambiguity |

**Verdict: Uncontroversial. No changes needed.**
