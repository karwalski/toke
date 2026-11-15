---
title: Gate 2 Decision Record
slug: gate2-decision
section: decisions
order: 21
---

**Date:** 2026-11-15
**Status (2026-11-15):** the live record of the Gate 2 verdict and of Gate 3. Recorded PASS 2026-05-24 — **withdrawn 2026-11-15: Gate 2 has no standing verdict** (owner decision D4). Gate 3 never ran and has been void since 2026-10-01 (story 128.24).
**Deciders:** Matthew Watt
**Stories:** 2.5.1 (the evaluation), 102.15 (the functional re-evaluation), 128.24 (Gate 3 bannered void)
**Decision register:** `docs/owner-decisions-2026-11-12.md`, § Answers recorded 2026-11-15, item D4
**Gate summary:** `PROJECT_STATUS.md`, § Gate Status. **Figures:** `docs/metrics-baseline.md`, § Correctness.

---

## Decision

**The PASS recorded 2026-05-24 is withdrawn; Gate 2 has no standing verdict.**

The PASS was declared against a compile-only criterion: 100% of the generated solutions
compiled. Neither pre-registered Gate 2 criterion (next section) is a compile rate. Both
name Pass@1, which the specification defines by hidden tests passed, separately from
first-pass compile success (`docs/spec/toke-spec-v0.3.md` §21.3).

The record also set that compile rate against Gate 1's published 63.7% and concluded that
the model "significantly outperforms baseline". The 63.7% was a functional figure, not a
compile rate, and it has since been corrected to 58.8% (story 128.19). The comparison put a
compile rate against a functional one.

The owner withdrew the PASS and recorded no verdict in its place; this record does not
substitute one. Every functional figure measured on the Gate 2 run is below both
pre-registered thresholds (>= 65% and >= 75% Pass@1, next section): 55.6% (272/489), and
the roughly 8% (4 of 50) first reported before the stdlib fix (below). The caveats only
make the 55.6% less favourable, never more: the 200-eval half of the Gate 2 set may be
corpus-derived, which would inflate it; the 2026-05-24 record never stated which 489 of
the 700 tasks it counts; and it is on v0.3 syntax, so it does not carry forward to v0.4
(`docs/metrics-baseline.md`, caveat 1). The full-local re-audit below is context, not a
second Gate 2 figure: it measures a different population, the programs of the Epic 101
generation pipeline, and it is not a Pass@1 run of the Gate 2 model. Model evaluation now
follows the pre-registered protocol of story 128.1 (BENCH-128), under which no Gate 2
number is a target.

The record of 2026-05-24 is kept, not erased: `docs/spec/gate2-decision.md` at toke
`1c5ffce` ([link](https://github.com/karwalski/toke/blob/1c5ffce/docs/spec/gate2-decision.md)).
It was moved out of the tree on 2026-08-19 (story 130.3) and is byte-identical to the
local archive copy (story 128.18).

---

## The pre-registered criteria

Two documents pre-registered a Gate 2 criterion. Neither is a compile rate.

| Source | Criterion | Threshold |
|---|---|---|
| The language specification: `toke-spec/spec/toke-spec-v02.md` §22.5, restated in `docs/spec/toke-spec-v0.3.md` §21.5 (Gate 2, Month 14) | Extended language features maintain token efficiency, **and** a trained 7B model reaches the threshold Pass@1 on D2C tasks | **>= 65% Pass@1** |
| The April criteria sheet: `toke-spec/docs/gate-criteria.md`, § Gate 2 (2026-04-04, story 10.10.2) | Pass@1 on held-out tasks, same harness as Gate 1, greedy decoding. All eleven criteria in the sheet must be met; a compile rate of >= 95% is a separate one of them | **>= 75% Pass@1** |

The 2026-05-24 record cited neither. It quoted a third wording, attributed to Epic 10.10:
"7B fine-tuned model outperforms baseline on toke code generation using default (55-char)
syntax". Its own results table set a target of ">70%" against a compile column.

---

## What was measured

| Evaluation | Compile | Functional | Set | Source |
|---|---|---|---|---|
| Gate 2 run, 2026-05-22 | 100% | 55.6% (272/489), **with the caveat below** | 500 hidden + 200 eval; Qwen 2.5 Coder 7B + QLoRA, v0.3 syntax | 2.5.1; functional re-evaluated 2026-05-25 (102.15) |
| Full-local re-audit, 2026-05-28 (the honest floor) | **37.5%** (655/1,748), metrics-baseline's figure; it counts the 403 NO_SOURCE programs as compiled (see below) | **about 2.2%** fully correct (38 PASS of 1,748) | 1,748 programs from the Epic 101 generation pipeline, 1,345 with a solution file; v0.3.9 | 101.R1 / 102.17 |

**The caveat on 55.6%, which travels with the number.** The 200-eval half of the Gate 2
set may be corpus-derived. The evaluation protocol of story 128.1 found that it most
plausibly maps to `toke-model/benchmark/tasks.jsonl`, which is generated from the training
corpus: 160 of its 161 base tasks intersect the v0.4 corpus families
(`toke-model/docs/training-reset-128.md`, K7). That file is quarantined (story 128.16). Whether the Gate 2 "200 eval" is that file is
unproven, so the provenance of that half is unresolved (`docs/owner-decisions-2026-10-01.md`
§4.11, carried as item 6.2 of `docs/owner-decisions-2026-11-12.md`). The 2026-05-24 record
also does not say which of the 700 tasks make up the 489 behind the functional figure.

The functional figure was first reported as about 8% (4 of 50). The cause was
infrastructure, not the model: `io.readln()` was declared in its interface file but had no
C implementation, so programs that read standard input compiled but could not be linked and
tested. With that fixed, the re-evaluation of 2026-05-25 gave 272/489 (story 102.15).

The re-audit (story 101.R1, retested under toke v0.3.9 in 102.17) covered 1,748 programs.
They are the output of the Epic 101 generation pipeline: initial generation through the
toke API, with Claude generating instead whenever that API was unavailable
(`toke-test-programs/infra/worker-generate.py`, the fallback added in 47692b84), then a
Claude repair loop of up to five iterations (story 101.5;
`toke-test-programs/infra/README.md`). The fallback and the repair loop already make the
re-audit something other than a Pass@1 of any single model, so it is not scored against
Gate 2's thresholds; which model the toke API served was not traced either. Of the 1,748, 1,345 had a solution file (403 NO_SOURCE, 101.R1); of
those, 38 PASS, 141 WRONG_OUTPUT, 64 RUN_FAIL, 9 BUILD_FAIL and 1,093 COMPILE_FAIL. It is
the harsher floor, and it is cited beside any Gate 2 figure.

The 37.5% compile figure above is `docs/metrics-baseline.md`'s, cited as that file gives it.
It is (1,748 − 1,093) / 1,748, so it counts the 403 NO_SOURCE programs as compiled, while
the about 2.2% fully correct (38 / 1,748) counts them as failures. Both figures are being
re-derived with one decided treatment of NO_SOURCE under story 127.467; until then
metrics-baseline's figures stand as published.

Every number in this section is on v0.3 syntax. No v0.4-native model exists.

---

## Gate 3

Gate 3 is folded into this record because it never ran.

- **The specification's Gate 3** (`docs/spec/toke-spec-v0.3.md` §21.5, Month 26): two or
  more independent LLM families reach >= 70% Pass@1 on D2C tasks, and the self-improvement
  loop demonstrably improves corpus quality over successive iterations.
- **The pre-registration** (`docs/spec/gate3-criteria.md`, 2026-05-24): five criteria, from
  C1 (functional Pass@1 >= 35% on the 500-task hidden benchmark) to C5 (self-improvement),
  all five required for GO. The GO/NO-GO decision was dated **2026-08-17**, with a retry
  deadline of 2026-09-14.
- **It never ran.** No model was trained, fine-tuned or evaluated in August 2026
  (`docs/metrics-baseline.md`, § 2026-08), and no Gate 3 evaluation exists.
- **Void since 2026-10-01.** Story 128.24 opened `docs/spec/gate3-criteria.md` with an
  archive banner. Its thresholds are not inherited by Epic 128
  (`toke-model/docs/training-reset-128.md`, K3), and its claim that the Gate 2 result
  "already exceeds C1's 35% threshold" is void.
- **Superseded by the 128.15 lane scorecard.** Every training lane is scored on one
  pre-registered comparison on BENCH-128, relative to the no-training baseline Lane A
  (128.10), and no lane advances without beating it. The scorecard
  (`toke-model/docs/lane-scorecard.md`) is written by 128.15, which is planned; it has no
  results yet.

---

## References

- Owner decision D4: `docs/owner-decisions-2026-11-12.md`, § Answers recorded 2026-11-15
- The withdrawn record: `docs/spec/gate2-decision.md` at toke `1c5ffce`
- Figures: `docs/metrics-baseline.md`, § Correctness and § 2026-08
- Gate 2 criteria: `docs/spec/toke-spec-v0.3.md` §21.5; `toke-spec/spec/toke-spec-v02.md`
  §22.5; `toke-spec/docs/gate-criteria.md`, § Gate 2
- Gate 3: `docs/spec/gate3-criteria.md` (archived 2026-10-01, story 128.24)
- The 200-eval provenance question: `toke-model/docs/training-reset-128.md`, K7;
  `docs/owner-decisions-2026-10-01.md` §4.11
- The protocol that replaces both gates: `toke-model/docs/training-reset-128.md` (story 128.1)
- Gate 1, for comparison: `docs/decisions/gate1-decision.md`
- Story tracker: `docs/progress.md` (2.5.1, 102.15, 128.15, 128.16, 128.24)
