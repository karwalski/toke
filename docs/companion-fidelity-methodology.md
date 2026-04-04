# Companion File Round-Trip Fidelity Methodology

**Story:** 11.5.3
**Status:** Framework defined; LLM evaluation pending live model.
**Date:** 2026-04-05

---

## What "Functionally Equivalent" Means

Two toke programs are **fidelity-equivalent** when they produce the same
observable output for all valid inputs. Concretely, a regenerated program
`P_regen` is fidelity-equivalent to the original `P_orig` when:

1. **Same module name** — the `M=` (or `m=`) declaration matches.
2. **Same function signatures** — names, parameter names, parameter types, and
   return types match exactly.
3. **Same type declarations** — all `T=` (or `t=`) types are present, with the
   same field names and types.
4. **Same control flow shape** — the regenerated code produces the same result
   value for every input in the conformance suite.

Variable names used in local `let` bindings and loop counters may differ. The
specific expression forms used to compute a value may differ (e.g., a `lp` vs
an equivalent recursive formulation) as long as behaviour is preserved.

**Not required for equivalence:**

- Identical variable names.
- Identical comment placement (toke has no comments; companion files carry the
  documentation).
- Identical whitespace or formatting.
- Identical token sequence.

---

## Evaluation Protocol

The protocol has four stages:

### Stage 1 — Generate companion

Run the toke compiler with `--companion-out` to produce the `.tkc.md` companion
for the source file under evaluation:

```
tkc --companion-out prime_sieve.tkc.md prime_sieve.tk
```

### Stage 2 — Show companion to LLM

Present the full companion file (YAML frontmatter + all sections) to a
toke-trained LLM with the following prompt:

```
You are reading a toke companion file. The companion describes a toke program
in plain language. Regenerate the toke source code described by this companion.

Rules:
- Output only the .tk source file. Do not include explanation or markdown fences.
- Use the default toke syntax (56-char set, M= for module, F= for functions,
  T= for types, @() for arrays).
- Match every function signature and type declaration exactly as described.
- Implement the logic described in each function's "Logic" section.

COMPANION FILE:
<insert companion content here>
```

### Stage 3 — Compile and compare

1. Save the LLM response as `regen.tk`.
2. Compile both files:
   ```
   tkc --emit-ir prime_sieve.tk   > orig.ll
   tkc --emit-ir regen.tk         > regen.ll
   ```
3. If both compile, run the conformance suite against both binaries and compare
   result values.
4. Record outcome as one of:
   - **PASS** — compiles clean, type-checks, produces identical results.
   - **PARTIAL** — compiles but produces different results on ≥1 test.
   - **FAIL** — does not compile (syntax error or type error).

### Stage 4 — Aggregate

Repeat stages 1–3 for a representative sample of toke source files (suggested
minimum: 50 files drawn from the bench/ and stdlib/ directories).

Compute:

```
fidelity_rate = count(PASS) / count(total)
```

The target is `fidelity_rate >= 0.90`.

---

## Target Metric

| Metric | Target |
|--------|--------|
| Compilation success rate | ≥ 95% of regenerated files compile without errors |
| Type-check pass rate | ≥ 92% of compiled regenerations pass type checking |
| Full fidelity rate | ≥ 90% of regenerations are fidelity-equivalent to the original |

These targets apply to the standard patterns found in the toke benchmark
corpus (bench/programs/) and standard library tests (test/stdlib/).

---

## Dry-Run Mode

The `test/companion_fidelity.sh` harness supports `--dry-run` mode, which
validates the companion format and hash without performing LLM inference.

In dry-run mode the harness:

1. Generates the companion file via `tkc --companion-out`.
2. Validates the YAML frontmatter fields (`format_version`, `source_hash`,
   `source_file`).
3. Verifies the hash is fresh via `tkc --verify-companion`.
4. Checks that required sections (`## Module`, `## Functions`) are present.
5. Optionally compiles the original source to IR to confirm it is well-formed.
6. Skips LLM inference and reports `SKIPPED`.

Dry-run exits 0 if the companion is well-formed and the hash is fresh.

### Dry-Run Results

*(To be filled with real numbers once a live model is available.)*

| Source file | Companion generated | Hash valid | Format valid | LLM: PASS | LLM: PARTIAL | LLM: FAIL |
|-------------|--------------------:|:----------:|:------------:|:---------:|:------------:|:---------:|
| prime_sieve.tk | — | — | — | — | — | — |
| fib_iterative.tk | — | — | — | — | — | — |
| collatz.tk | — | — | — | — | — | — |
| binary_search.tk | — | — | — | — | — | — |
| struct_ops.tk | — | — | — | — | — | — |
| **Total** | | | | **—** | **—** | **—** |

---

## Sample Companion → Regenerated Code Comparison

The following worked example uses `prime_sieve.tk` from the benchmark corpus.
It illustrates what a successful round-trip looks like.

### Source file: `prime_sieve.tk`

```
M=bench;
F=isprime(n:i64):i64{
  if(n<2){<0}el{
    let d=mut.2;
    let found=mut.1;
    lp(let dummy=0;d*d<n+1;dummy=0){
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
F=main():i64{
  let count=mut.0;
  lp(let i=2;i<50000;i=i+1){
    count=count+isprime(i)
  };
  <count
};
```

### Companion file: `prime_sieve.tkc.md` (relevant excerpt)

```markdown
## Functions

### `isprime(n: i64): i64`

**Purpose:** Tests whether `n` is a prime number using trial division.

**Logic:**

1. If `n < 2`, return `0` immediately (not prime).
2. Initialise a mutable divisor `d` to `2`.
3. Initialise a mutable flag `found` to `1` (assume prime).
4. Loop while `d * d <= n` (condition: `d * d < n + 1`):
   a. Compute `n - (n / d) * d`. If this equals `0`:
      - Set `found` to `0`.
      - Set `d` to `n` to force loop exit.
   b. Otherwise, increment `d` by `1`.
5. Return `found`.
```

### Regenerated code (expected output from LLM)

A fidelity-equivalent regeneration may use a different loop variable name but
must preserve the algorithm:

```
M=bench;
F=isprime(n:i64):i64{
  if(n<2){<0}el{
    let d=mut.2;
    let found=mut.1;
    lp(let k=0;d*d<n+1;k=0){
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
F=main():i64{
  let count=mut.0;
  lp(let i=2;i<50000;i=i+1){
    count=count+isprime(i)
  };
  <count
};
```

### Verdict: PASS

The regenerated code uses `k` instead of `dummy` for the unused loop counter
variable. All function signatures match exactly. The divisibility check and
loop-exit pattern are identical. Both programs produce the same prime count
(5133) for input range [2, 49999].

---

## Companion Quality Guidelines for High Fidelity

To maximise fidelity rate, companion files must satisfy the guidelines in
`docs/companion-file-spec.md` (Round-Trip Fidelity Guidelines section):

| Requirement | Why it matters |
|-------------|----------------|
| Logic narratives must be algorithmic, not vague | LLM cannot infer "check divisors" into a specific loop structure |
| Branching conditions must be explicit (`if n < 2`) | Vague conditions ("if n is small") produce wrong code |
| Loop bounds must be stated numerically | "iterate over candidates" loses the exact range |
| Mutability must be stated (`let x=mut.0`) | LLMs default to immutable; mutable bindings need explicit marking |
| Every type field must be listed | LLMs cannot invent fields they were not told about |

---

## Tooling

| Tool | Purpose |
|------|---------|
| `test/companion_fidelity.sh` | Main harness; `--dry-run` for CI, full mode for evaluation |
| `tkc --companion-out <path> <src>` | Generate companion from source |
| `tkc --verify-companion <comp>` | Verify hash freshness |
| `tkc --emit-ir <src>` | Compile to LLVM IR for comparison |

---

## Related Stories

| Story | Description |
|-------|-------------|
| 11.5.1 | Companion file format spec |
| 11.5.2 | Companion file generation (`tkc --companion`) |
| 11.5.3 | This story — round-trip fidelity framework |
| 11.5.6 | Companion hash verification |
