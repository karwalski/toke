---
title: Companion File Format Specification
slug: companion-file-spec
section: compiler
order: 2
---

**Status:** Normative
**Format version:** 1.0
**Date:** 2026-04-04
**Authority:** Story 11.5.1

---

## Overview

A **companion file** is a plain-language explanation of a toke source file. It is designed for three audiences:

1. **Human reviewers** — peer review, security audit, onboarding.
2. **LLMs** — an LLM reading the companion should regenerate functionally equivalent toke code without seeing the original source.
3. **Tooling** — the `source_hash` field enables staleness detection, diff-based review workflows, and CI checks.

toke has no comment syntax. Companion files are the canonical mechanism for documenting toke programs.

## File Naming

The companion file for `foo.tk` is `foo.tkc.md`. It lives in the same directory as the source file.

```
stdlib/
  http.tk
  http.tkc.md    <-- companion for http.tk
bench/programs/
  prime_sieve.tk
  prime_sieve.tkc.md
```

The `.tkc.md` extension is load-bearing. Tools should match on it exactly. The `tkc` stands for "toke companion".

---

## YAML Frontmatter

Every companion file begins with a YAML frontmatter block fenced by `---` lines.

```yaml
---
source_file: "prime_sieve.tk"
source_hash: "a1b2c3d4e5f6...full 64-char SHA-256 hex digest"
compiler_version: "0.1.0"
generated_at: "2026-04-04T09:30:00Z"
format_version: "1.0"
---
```

### Required fields

| Field | Type | Description |
|-------|------|-------------|
| `source_file` | string | Relative path from the companion file to the source `.tk` file. Typically just the filename when they share a directory. |
| `source_hash` | string | SHA-256 hex digest (lowercase, 64 characters) of the source file content, byte-for-byte. |
| `compiler_version` | string | Semver version of `tkc` used to compile or validate the source. `"unknown"` if not compiler-generated. |
| `generated_at` | string | ISO 8601 timestamp in UTC. Example: `"2026-04-04T09:30:00Z"`. |
| `format_version` | string | Version of this companion file format. Currently `"1.0"`. |

### Staleness detection

A companion file is **stale** when the SHA-256 of the current source file does not match `source_hash`. Tools should warn on stale companions. A stale companion is still useful but must not be treated as authoritative.

---

## Structured Sections

The body of the companion file uses markdown headings. All sections below are listed in their required order. Sections may be omitted only if they would be empty (e.g., a module with no types omits `## Types`).

### `## Module`

Describes the module declaration and its purpose.

**Must include:**
- The module name (the value from `m=`).
- A one-to-three sentence description of what this module does.
- A list of imports, if any, with a brief note on what each import provides.

**Example:**

```markdown
## Module

**Name:** `bench`

This module implements the Sieve of Eratosthenes to count primes below a
given limit. It is a benchmark program used to measure loop and branch
performance.

**Imports:** None.
```

### `## Types`

Describes every type declaration (`t=`) in the source file.

**Must include for each type:**
- The type name.
- A plain-language description of what the type represents.
- Every field, with its name, type, and purpose.

**Example:**

```markdown
## Types

### `$vec3`

A three-dimensional integer vector used for spatial arithmetic.

| Field | Type | Description |
|-------|------|-------------|
| `x` | `i64` | The x-axis component. |
| `y` | `i64` | The y-axis component. |
| `z` | `i64` | The z-axis component. |
```

### `## Functions`

Describes every function declaration (`f=`) in the source file. This is the most detailed section. Each function gets its own `###` subsection.

**Must include for each function:**
- **Signature summary** — name, parameters with types, return type.
- **Purpose** — one-to-two sentence description.
- **Parameters** — name, type, and role of each parameter.
- **Return value** — type and meaning.
- **Logic narrative** — step-by-step description of the implementation, detailed enough that an LLM could regenerate functionally equivalent code.
- **Error handling** — how errors are detected and reported, if applicable.

**Example:**

```markdown
## Functions

### `isprime(n: i64): i64`

**Purpose:** Tests whether `n` is a prime number using trial division.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `n` | `i64` | The integer to test for primality. |

**Returns:** `i64` — `1` if `n` is prime, `0` otherwise.

**Logic:**

1. If `n < 2`, return `0` (not prime).
2. Initialise a mutable divisor `d` to `2` and a mutable flag `found` to `1`
   (assume prime).
3. Loop while `d * d <= n`:
   a. If `n` is divisible by `d` (i.e., `n - (n / d) * d == 0`), set `found`
      to `0` and force the loop to exit by setting `d = n`.
   b. Otherwise, increment `d` by `1`.
4. Return `found`.

**Error handling:** None. All `i64` inputs are valid.
```

### `## Constants`

Describes any module-level constant bindings.

**Must include for each constant:**
- Name and type.
- Value (or description if the value is computed).
- Purpose.

If the module has no constants, omit this section.

### `## Control Flow`

A high-level narrative of the program's overall execution flow. This section is about how the pieces fit together, not about individual function internals (those are in `## Functions`).

**Must include:**
- Entry point and what it does.
- Key branching or looping patterns that span multiple functions.
- Data flow between functions.

**Example:**

```markdown
## Control Flow

`main` iterates over all integers from 2 to 49,999, calling `isprime` on
each. It accumulates a running count of primes found. The final count is
returned as the program's exit value.

The only branching is inside `isprime`, which uses an early return for
`n < 2` and a trial-division loop that exits early on finding a divisor.
```

### `## Notes`

Optional section for anything that does not fit the other sections.

**Typical content:**
- Security considerations (e.g., "this function does not validate untrusted input").
- Performance notes (e.g., "trial division is O(sqrt(n)) per call").
- Known limitations.
- Design rationale or references to spec sections.

If there is nothing to note, omit this section.

---

## Round-Trip Fidelity Guidelines

The companion file must contain enough information for an LLM to regenerate functionally equivalent toke source code. "Functionally equivalent" means:

1. **Same module name** — the `m=` declaration matches.
2. **Same type declarations** — all fields present with correct names and types.
3. **Same function signatures** — names, parameter names, parameter types, and return types match exactly.
4. **Equivalent logic** — the regenerated code produces the same output for all valid inputs. Variable names and loop structure may differ as long as behaviour is preserved.

To achieve this:

- **Function logic narratives must be algorithmic, not vague.** Write "initialise `d` to `2`, loop while `d * d <= n`", not "check divisors".
- **Type descriptions must list every field.** Do not summarise with "and other fields".
- **Mutability must be stated.** If a binding is mutable (`let x=mut.0`), the narrative must say so.
- **Branching conditions must be explicit.** Write "if `n < 2`", not "if n is small".
- **Loop bounds must be explicit.** Write "from 2 to 49,999 inclusive", not "for each candidate".

---

## Complete Example

### Source file: `prime_sieve.tk`

```
m=bench;
f=isprime(n:i64):i64{
  if(n<2){<0}el{
    let d=mut.2;
    let found=mut.1;
    lp(let i=0;d*d<n+1;i=0){
      if(n-n/d*d=0){
        found=0;
        d=n
      }el{
        d=d+1
      }
    };
    <found
  }
};
f=main():i64{
  let count=mut.0;
  lp(let i=2;i<50000;i=i+1){
    count=count+isprime(i)
  };
  <count
};
```

### Companion file: `prime_sieve.tkc.md`

```markdown
---
source_file: "prime_sieve.tk"
source_hash: "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
compiler_version: "0.1.0"
generated_at: "2026-04-04T09:30:00Z"
format_version: "1.0"
---

## Module

**Name:** `bench`

Counts the number of prime numbers below 50,000 using trial division. This is
a benchmark program for measuring loop and branch performance in the toke
runtime.

**Imports:** None.

## Functions

### `isprime(n: i64): i64`

**Purpose:** Tests whether `n` is a prime number using trial division.

**Parameters:**

| Name | Type | Description |
|------|------|-------------|
| `n` | `i64` | The integer to test for primality. |

**Returns:** `i64` — `1` if `n` is prime, `0` otherwise.

**Logic:**

1. If `n < 2`, return `0` immediately (not prime).
2. Initialise a mutable divisor `d` to `2`.
3. Initialise a mutable flag `found` to `1` (assume prime until proven
   otherwise).
4. Loop while `d * d <= n` (the loop condition is `d * d < n + 1`):
   a. Compute `n - (n / d) * d`. If this equals `0`, then `d` divides `n`
      evenly:
      - Set `found` to `0` (not prime).
      - Set `d` to `n` to force the loop condition to fail, exiting early.
   b. Otherwise, increment `d` by `1`.
5. Return `found`.

**Error handling:** None. All `i64` inputs are valid.

### `main(): i64`

**Purpose:** Entry point. Counts primes below 50,000.

**Parameters:** None.

**Returns:** `i64` — the count of prime numbers in the range [2, 49999].

**Logic:**

1. Initialise a mutable accumulator `count` to `0`.
2. Loop with a mutable counter `i` from `2` (inclusive) to `50000`
   (exclusive), incrementing by `1` each iteration:
   a. Call `isprime(i)` and add the result (`0` or `1`) to `count`.
3. Return `count`.

**Error handling:** None.

## Control Flow

`main` iterates over all integers from 2 to 49,999, calling `isprime` on each.
It accumulates a running count of primes found. The final count is returned as
the program's exit value.

The only branching is inside `isprime`, which uses an early return for `n < 2`
and a trial-division loop that exits early when a divisor is found by forcing
the loop variable past the termination bound.

## Notes

- Trial division is O(sqrt(n)) per call, giving O(k * sqrt(k)) total work
  where k = 49,999. This is intentionally naive for benchmarking purposes.
- The divisibility check uses `n - (n / d) * d == 0` rather than a modulo
  operator because toke does not have a `%` operator; integer division
  truncates toward zero.
```

---

## Tooling Expectations

Tools that produce or consume companion files should:

1. **Generate:** Compute `source_hash` from the source file at generation time. Use SHA-256, lowercase hex, no prefix.
2. **Validate:** Warn when `source_hash` does not match the current source file content.
3. **Lint:** Check that all functions and types in the source have corresponding entries in the companion.
4. **Round-trip test:** Optionally, feed the companion to an LLM, compile the regenerated code, and diff the output against the original binary or test suite results.

---

## Versioning

This spec is `format_version: "1.0"`. Future versions will increment the minor version for backward-compatible additions (new optional sections, new frontmatter fields) and the major version for breaking changes (removed sections, changed semantics of existing fields).

Consumers should reject companion files with a major version they do not understand and accept files with a higher minor version by ignoring unknown sections.
