# Audit: `(* *)` Comment Delimiters

**Story:** 77.1.9
**Date:** 2026-05-01
**Status:** PASS — no new characters introduced

---

## 1. Spec Reference

Section 8.10 (normative) of `toke-spec-v0.3.md` defines:

- Block comments: `(* ... *)` with arbitrary nesting
- Documentation comments: `(** ... *)`
- Comments are scanned and discarded by the lexer; they produce no AST nodes and have no semantic effect
- Comments may span multiple lines

## 2. Character Set Impact

The comment delimiters use only `(`, `*`, and `)`. All three are already members of the 23-symbol structural set (spec Section 7.1):

```
Symbols  ( ) { } = : . ; + - * / < > ! | $ @ & ^ ~ % _   23
```

**Verdict:** Comments add zero new characters to the toke character set.

## 3. Nesting Implementation

The lexer (`src/lexer.c`, lines 612-632) implements nesting via a depth counter:

```c
advance(&l); /* skip ( */
advance(&l); /* skip * */
int depth = 1;
while (l.pos < l.len && depth > 0) {
    if (src[l.pos] == '(' && src[l.pos + 1] == '*') {
        advance(&l); advance(&l);
        depth++;
    } else if (src[l.pos] == '*' && src[l.pos + 1] == ')') {
        advance(&l); advance(&l);
        depth--;
    } else {
        advance(&l);
    }
}
```

**Nesting is fully implemented.** `(* outer (* inner *) still comment *)` works correctly.

## 4. Doc-Comments vs Regular Comments

The lexer has **no special handling** for `(** ... *)`. A doc-comment beginning `(**` is scanned identically to a regular `(*` comment — the second `*` is simply consumed as comment content. There is no separate token kind, no extraction logic, and no reference to "doc" or "**" in lexer.c.

The spec confirms: "Documentation comments are treated identically to block comments by the compiler. Tooling may extract them for API documentation."

## 5. Training Corpus Treatment

The spec states (Section 8.10, non-normative note):

> The toke project's training corpus is comment-stripped before LLM training. This is a pipeline policy, not a language constraint.

Comment stripping is a pipeline decision external to the language definition. The language supports comments; training data simply omits them.

## 6. Edge Cases and Ambiguity

### Is `(` followed by `*` always a comment start?

**Yes.** The lexer checks for `(*` greedily at the point it encounters `(`. If the next character is `*`, it enters comment-scanning mode unconditionally. This means the two-character sequence `(*` can never appear as `TK_LPAREN` followed by `TK_STAR` in the token stream.

**Is this a problem?** No. In toke syntax, there is no valid context where a bare `(` would be immediately followed by `*` as a dereference or multiplication operator. Parenthesised expressions that begin with `*` (e.g., subexpression starting with multiplication) are not valid because `*` is a binary operator requiring a left operand. The grammar prevents ambiguity.

### Unterminated comment (EOF inside comment)

The lexer loop exits when `l.pos >= l.len` with `depth > 0`. In this case, **no diagnostic is emitted** — the comment is silently consumed and the lexer returns whatever tokens were accumulated before the comment started. This is a minor gap; a proper `E1007` (unterminated comment) diagnostic would improve developer experience.

**Recommendation:** File a backlog story to emit a diagnostic for unterminated block comments.

## 7. Summary

| Question | Answer |
|----------|--------|
| New characters added? | No |
| Nesting implemented? | Yes, depth-counted |
| Doc-comments distinct? | No, identical to block comments at lexer level |
| Corpus treatment? | Stripped by pipeline policy |
| Ambiguity with `(*`? | None — grammar prevents conflict |
| Unterminated comment diagnostic? | Missing (minor gap) |
