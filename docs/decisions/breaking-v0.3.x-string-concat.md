---
title: BREAKING CHANGE — toke v0.3.x removes `+` for string concatenation
slug: breaking-v0.3.x-string-concat
section: decisions
order: 99
---

**Date:** 2026-05-30
**Audience:** maintainers of any project that imports or generates toke code
(loke, ooke, moke, toke-website, toke-test-programs, and any third-party
projects using toke).

## What changed

Pre-v0.3.x, `+` was overloaded to do string concatenation when both operands
were `$str`. As of v0.3.x, `+` is strictly numeric. Any toke source that
uses `+` on strings will fail at compile-time with `E4031`:

    type mismatch: expected 'str', got 'str'

    fix: `+` is numeric-only in toke. For string templates use interpolation
    "\(a)\(b)"; for delimiter-joined collections use s.join(arr;sep); for
    dynamic accumulation use s.builder()/s.add()/s.build(). See ADR-0004.

This applies to every form: `<a+b`, `let x=mut.""; x=a+b`, `f(a+b)`, etc.

## Why

See `docs/decisions/ADR-0004.md` for full rationale. Summary:

- The previous behaviour was inconsistent — the type-checker rejected `+`
  on strings in some contexts (`<a+b`, assignment) but the let-binding
  initialiser bypassed it, producing the misleading
  `"expected 'str', got 'str'"` diagnostic that wasted repair iterations.
- `+` and `tk_str_concat` couple two unrelated operations on a single
  operator, splitting BPE merge density for the custom toke tokeniser.
- The naïve loop pattern `r = r + part + sep` is O(N²) — there was no
  first-class StringBuilder equivalent. v0.3.x ships `s.builder()` to fix
  this; the canonical pattern reads better and runs in O(N).

## Canonical replacements

| Old (`+`) | New |
|---|---|
| `"hi " + name` | `s.concat("hi "; name)` |
| `"k=" + s.fromint(n) + " items"` | `s.concat(s.concat("k="; s.fromint(n)); " items")` |
| Loop accumulator `r=r+part+sep` | `let b=s.builder(); lp(…){s.add(b;part);s.add(b;sep)}; let r=s.build(b);` |
| Delimiter join over array | `s.join(parts; ",")` |

(Once story 111.5a ships, `"k=\(s.fromint(n)) items"` interpolation will
also be available; until then use `s.concat` chains.)

## How to update

1. **Scan your tree** for affected files:
   ```sh
   find . -name '*.tk' -exec tkc --check {} \; 2>&1 | grep -B1 'E4031'
   ```

2. **Auto-migrate**:
   ```sh
   find . -name '*.tk' | xargs python3 /path/to/toke/tools/migrate-strconcat.py
   ```

   The tool rewrites the unambiguous cases (literal + literal, function-
   parameter + literal, `let`-bound string + literal) and flags ambiguous
   cases (bare identifier with no resolvable type) for manual review.

3. **Manual cases**: search for any reported `ambiguous=N` rows in the
   migration output; review and apply the canonical pattern by hand.

4. **Re-check** after migration:
   ```sh
   find . -name '*.tk' -exec tkc --check {} \;
   ```
   No `E4031` should remain.

## Downstream adoption tracking

| Project | Status | Migrated commit |
|---|---|---|
| toke-test-programs | in progress (story 111.7) | — |
| loke | pending | — |
| ooke | pending | — |
| moke | pending | — |
| toke-website | pending | — |
| toke-cloud (sandbox / Lambda) | pending | — |
| toke-mcp (codegen examples) | pending | — |

## Compatibility window

There is no soft-deprecation window. v0.3.x compiles `+` on strings as an
error from the first release. Pinning to a pre-v0.3.x `tkc` build is the
only way to keep old source running unchanged; we don't recommend that.

## Contact / questions

Open an issue on the relevant repo or comment on the v0.3.x release notes.
