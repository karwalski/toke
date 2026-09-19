# The toke-eval v0.3 → v0.4 syntax-drift decision

**Story 132.0(c).** Epic 132's article and site copy need to state the *basis* of any
benchmark number taken from `toke-eval/benchmark/solutions/`. That set was written in
**April 2026** by the Gate-1 fine-tuned model, in **v0.3** syntax, against a compiler that
no longer exists. Epic 116 then shipped the breaking v0.4 change. This file records what
the drift was, how it was handled, what is still outstanding, and — the part every
downstream surface must copy — **what a number from that set may and may not claim**.

Primary artefacts: `toke-eval/docs/gate1-60-v04.md`, `toke-eval/scripts/gate1_60_v04.py`,
`docs/metrics-baseline.md` (2026-09 section).

---

## 1. What the drift was

The 1,000 `benchmark/solutions/task-*.toke` files are Gate-1-era v0.3 programs. Story 130.2
committed a mechanical migration of them and flagged the problem honestly at the time:
**8 of 12 sampled compile failures were pre-existing v0.3 constructs, not migration damage.**
Four classes account for nearly all of it:

| Class | v0.3 form | v0.4 form | Diagnostic |
|---|---|---|---|
| **Bare `=` equality** | `if(x=1)` | `if(x==1)` | E2002 |
| **Bracket types and literals** | `as [T]`, `[K:V]`, `+[x]`, `a[i]`, `a[i]=v` | `@T`, `@(K:V)`, `+@(x)`, `a.get(i)`, `a=a.set(i;v)` | E1003 |
| **`:void` on bool-returning functions** | `f=even(n:i64):void{<n%2==0}` | `:bool` | E4031 |
| **Immutable rebinding** | `let x=0; x=1` | `let x=mut.0; x=1` | E4070 |

Two further residue classes surfaced during the repair (127.36): legacy `str + str`
concatenation, which v0.4 rejects outright (ADR-0004), and v0.2-era `s.get(i)` character
indexing on a `str`.

Scale at the start: of the 1,000 solutions at `851f6d8b`, **784 migrated clean**. Of the 60
task ids the third-party KERN benchmark used, **18 of 60** compiled after migration. A
third party was, at that moment, publishing token numbers measured on the broken set.

## 2. How it was handled

**Fix the migrator first, hand-repair only the residue.** The alternative — hand-editing a
thousand files — would have produced numbers nobody could reproduce.

| Story | Change to `tkc --migrate` | 1,000 solutions | Gate-1 60 |
|---|---|---:|---:|
| — | (130.2 mechanical migration, baseline) | 784 | 18 |
| **127.18** | `postpass_equality()` — re-parses the migrated text, finds the parser's recovery nodes for bare `=` in every boolean position (`if`/`el if`/`lp`, `&&`/`\|\|`, `!`, expression-`if` in `mt` arms) and rewrites right-to-left | — | 33 |
| **127.19** | bracket handler rewritten to the Profile-2 rules (`[T]`→`@T`, `[a;b]`→`@(a;b)`, `a[i]`→`a.get(i)`, `a[i]=v`→`a.set(i;v)`) | **920** (E1003 143 → 0) | 50 |
| **127.32** | `postpass_return_types()` (classifies the return expression and rewrites `:void`→`:bool`/`:str`/`:f64`) + `postpass_mut_bindings()` (scope-aware E4070 mirror, adds `mut.`) | **949** (E4070 20 → 0) | 58 |
| **127.36** | `postpass_str_concat()` (str `+` → interpolation, else `alias.concat`) + `postpass_str_index()` (`s.get(i)` → `s.slice`/`s.charat` by context); prepass no longer mistakes a variable named `t` for a type-decl head | **991** | 58 |

Then **133.4 Part A** hand-repaired the residue for the 60 KERN-benchmarked ids and
verified them by execution: **60/60 `tkc --check`, 60/60 hidden tests (120 cases each),
`tkc --lint` 0 errors / 0 warnings**, each file stored as its `tkc --min` canonical form
(`toke-eval` `88f02b6` + `c48b7d3`). Of the 60: **27 are pure `--migrate` output with no
hand edit**; 33 were repaired. The repairs were overwhelmingly *April model semantics*, not
syntax — exit-code returns instead of printing, off-by-one loop bounds, C-truncation where
Python floor/mod semantics were expected, a bubble sort discarding its `arr.set` result,
rotation and empty-list crashes — plus two migrate-residue ids (0015, an original truncated
to a 1,957-character run of `let tempN=mut.0;` with no body; 0057, a shadowing conflict).

Three **tkc 2.8.0 regressions** were found and worked around on the way (filed as
127.43/127.44/127.45): `j.print` of an i64 ≥ 2^32 segfaults; `j.print` of an `@i64` returned
from a user function prints it as a byte string; `j.print` of a nested `@@i64` prints element
pointers. All three reproduce on the untouched originals in `--legacy` mode and the April
Gate-1 run scored those ids 120/120, so they are regressions in the compiler, not program
faults.

## 3. What remains

- **~9 originals are unfixable mechanically** (127.36 residue): 4 truncated originals, 3
  `E3011` `^`/`std` cases, one shadowing (0057), one brace defect (0178). These are defects
  in the April *model output*, not in the migrator; nothing short of rewriting the program
  fixes them.
- **Part B — the other ~940 solutions — is pending** (133.4). It is now a mechanical
  `--migrate` pass plus execution verification: a 0501–0600 sample runs 99/100 check-clean
  from `851f6d8b`, but the *pass* gate (not just `--check`) is expected to need ~5–10%
  semantic repairs of the same kind as the 60, plus the three compiler regressions fixed.
- **Until Part B lands there is no set-wide v0.4 number**, and none may be quoted.

---

## 4. What a benchmark number from this set may and may not claim

**It MAY claim:**

- That the **v0.4 language can express all 60 Gate-1 tasks correctly**, verified by
  execution — 60/60 `--check` and 60/60 on 120 hidden test cases each, reproducible with
  `benchmark/run_benchmark.py --language toke` and `scripts/gate1_60_v04.py`.
- **Same-set, same-tokenizer representation comparisons**, because all three
  representations solve the same 60 tasks and are counted by the same tokenizer on the
  `--min` canonical form: cl100k — Python 3,565, Kern Compact 3,012, **toke 4,787** (o200k
  4,778; 11,537 bytes); toke/Python **1.34× [1.22, 1.48]**, Kern/toke 0.63, and on the
  equal-vocab native lane Kern-16K/Toke-16K 1.145.
- That **writing the programs the v0.4 way cut toke's cost on this set by 24.6%** (6,347 →
  4,787 cl100k) — a like-for-like comparison of two toke texts under one tokenizer, which
  is a legitimate reduction claim.
- That **`tkc --migrate` handles 991/1,000 Gate-1-era v0.3 solutions mechanically** (784 →
  991) and 58/60 of the KERN ids.

**It MAY NOT claim:**

- **Any model capability, Pass@1, or generation quality.** These 60 programs are
  *hand-repaired by a human-directed agent*, not generated: 33 of 60 were edited. The set
  measures what the language and the compiler can do, not what a model produces. It is not
  comparable to Gate 1's 63.7% Pass@1 or Gate 2's 55.6% functional correctness, and must
  never be presented beside them as progress.
- **A token reduction versus Python.** Under a shared general-purpose tokenizer toke costs
  **more** than Python here (1.34×). toke-vs-Python is a cross-language density ratio
  (TEMSpec §2.3), informational only — never a reduction.
- **Anything about the other ~940 solutions, or about `benchmark/solutions/` as a whole.**
  Part B is unfinished; the 60 are the ids a third party chose to benchmark, not a random
  sample, and they are narrow (integer/list JSON-CLI tasks).
- **Anything at N > 60, or a generalisation beyond these task shapes.** N = 60, and the CIs
  above are bootstrap sum-ratios on exactly this set.
- **Use as a held-out evaluation set for any model trained on the repaired files.** The
  repaired solutions are now published; a model that has seen them is contaminated with
  respect to these ids (contamination governance: `toke-eval/docs/contamination-analysis.md`).
- **A clean-toolchain result.** Four ids are written around three live tkc 2.8.0 codegen
  regressions (127.43–127.45). The number is "what the language can do with the documented
  workarounds", not "what the shipped compiler does".

**One-sentence form for downstream copy (132.2/132.3):** *"The 60 Gate-1 benchmark tasks
have been re-delivered as v0.4 toke: 60/60 compile and 60/60 pass their hidden tests, at
4,787 cl100k tokens versus Python's 3,565 and Kern's 3,012 on the same tasks — these are
hand-verified reference solutions demonstrating what the language expresses, not model
output, and they carry no Pass@1 or token-reduction-versus-Python claim."*
