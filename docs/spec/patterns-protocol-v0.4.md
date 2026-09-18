---
title: toke v0.4 — Pattern catalogue protocol (token + runtime co-equal gate)
slug: patterns-protocol-v0.4
section: spec
---

**Status:** normative for Epic 131 (story 131.1 — gates the epic). **Owner ratification:** pending (filed 2026-09-18).
**Audience:** catalogue authors (131.6/131.7), the measurement harnesses (131.4 token proxy, 131.5 runtime
bench), the enforcement layer (131.9 lint, 131.10 judge/validate gates, 131.11 rendered spec/guide/card v2),
and the rewrite waves (131.13–131.17). **Companion:** `idiom-v0.4.md` (the prose idiom rules this protocol
turns into measured verdicts); `patterns-v0.4.md` (the rendered catalogue — generated, do not hand-edit).

## 1. Why a protocol

The custom v0.4 tokenizer learns whatever surface the corpus shows it. So the corpus must already be written
in the most token-efficient form, and that form must be the *default* idiom — not a second pass. "Most
efficient" is decided by **measurement, not taste**, on two co-equal axes:

1. **Tokens** — fewest tokens under the decision tokenizer (§4), tie-break `tkc --min` bytes.
2. **Runtime** — fastest wall time, lowest peak RSS, same or better asymptotic class (§5).

A form is **canonical** only if it is best-or-tied on *both* axes (§6). Where the axes conflict the catalogue
carries two forms — `canonical` (default) and `hot_path` — with a written rule for when to choose the hot
path. Nothing becomes canonical by argument alone; every verdict cites its numbers.

## 2. Pattern taxonomy — 10 families

| family | intent | typical patterns |
|---|---|---|
| `cond` | conditional binding and boolean logic | expr-`if` vs mut-flag; `el if` chain vs nested `if`; `&&`/`\|\|` vs flag soup; clamp / min / max |
| `acc` | accumulation into a value or collection | numeric sum/count/min/max loop vs combinator; array accumulator forms; map/struct building in a loop |
| `str` | building and formatting strings | interpolation vs `s.join` vs builder vs `s.concat` chain; building in a loop; number formatting; repeat / pad |
| `err` | error unions and early exit | `!` propagate; `mt … <0`; map error to default; validate-then-return |
| `parse` | turning text into values | `json.dec`; `csv.*`; delimiter split; whitespace `fields`; int/float parse; key=value lines |
| `iter` | traversals | map; filter; filter+aggregate; find-first / early exit; indexed loop vs `.map(&f)`; nested loop with early exit |
| `coll` | collection queries | lookup-with-default; membership; sort+take; dedupe; group-by into map; reverse |
| `cli` | program boundary | argv handling; stdin line processing; printing results |
| `fn` | decomposition | small helper vs inline; chained postfix vs intermediate `let`; recursion vs loop |
| `io` | files | read all / lines; accumulate-then-write vs append-per-line |

Pattern ids are `<family>-<kebab-name>` (e.g. `cond-bind-if`, `str-build-loop`). Target 40 patterns (±10),
hard cap 60. A pattern earns a place when it (a) recurs in the corpus (131.3 shape mining, `source` names the
shape), (b) is named by an idiom rule or the syntax card, or (c) is a before/after pair from Epic 126.1.

## 3. Catalogue entry schema

Source of truth: `patterns/catalogue.json` — a JSON object `{"protocol": "0.4", "entries": [ … ]}`.
Fixtures: `patterns/<id>/<form>.tk`, one complete `m=main;` program per candidate form; the measured region is
the single function `f=pat(...)`; everything else is harness (§5.1). Preferred-but-compiler-broken forms are
committed as `patterns/<id>/<form>.blocked.tk` — present, never compiled by CI, listed in `bug_caveats`.

Every entry has exactly these keys (validated by `scripts/patterns/validate_catalogue.py`):

| key | type | meaning |
|---|---|---|
| `id` | string | `<family>-<kebab>`; unique; matches the fixture directory |
| `family` | enum | one of the 10 families |
| `intent` | string | one sentence: what the programmer is trying to do |
| `applicability` | string | when this pattern applies / does not apply |
| `candidates` | array ≥ 2 | see below |
| `verdict` | object | `canonical` (form id), `hot_path` (form id or `null`), `choose_hot_path_when` (string or `null`), `status` ∈ `measured` \| `provisional` \| `blocked` |
| `source` | array ≥ 1 | provenance strings: `idiom-v0.4#<rule>`, `card:<line-hint>`, `ast-mine:<rank>`, `126.1-pair:<task_id>` |
| `bug_caveats` | array | `{issue: "127.x", effect, preferred_when_fixed: <form id or null>}` — may be empty |
| `lint` | object or null | `{rule, severity ∈ error\|warning\|hint, fixable: bool}` — the rule 131.9 emits for the non-canonical forms |
| `measured_at` | object | `{tkc_sha, tkc_version, proxy_sha, corpus_sha, bench_result, date}` |

Each **candidate**:

| key | type | meaning |
|---|---|---|
| `form` | string | `a`, `b`, `c` … (matches fixture file) |
| `label` | string | human name, e.g. "s.concat chain" |
| `fixture` | string | `patterns/<id>/<form>.tk` or `.blocked.tk` |
| `min_bytes` | int | UTF-8 bytes of the measured function after `tkc --min` |
| `tokens` | object | `{proxy8k, byte256, v03, qwen25coder, cl100k}` — ints; `null` allowed for external columns when the tokenizer is unavailable, never for `proxy8k`/`byte256` |
| `wall_ms_median` | number | from the 131.5 harness |
| `wall_ci95` | [lo, hi] | bootstrap 95% CI (1000 resamples) |
| `rss_kb_median` | int | peak RSS |
| `allocs` | object | `{calls, bytes}` |
| `bigO_ratio` | number | wall(4N) / wall(N) |
| `pat_n` | int | workload size used |
| `runtime_verdict` | enum | `best` \| `tied` \| `slower` \| `worse-bigO` \| `blocked` |
| `token_verdict` | enum | `best` \| `tied` \| `more` \| `blocked` |

A blocked candidate (`.blocked.tk`) carries `null` for every measured field and `blocked` for both verdicts.

## 4. Token measurement protocol (before the v0.4 tokenizer exists)

The real tokenizer is trained *from* the pattern-canonical corpus, so pattern token costs must be measured
with a stand-in now and re-measured later (§8).

- **Decision metric — `proxy8k`.** A byte-level BPE (HuggingFace `tokenizers`), vocab 8192, `min_frequency`
  2, trained by `scripts/patterns/train_proxy.py` on the `tkc --min` text of the frozen regen corpus with
  every string-literal body masked to `"_"` (escape-aware; `\(...)` interpolation interiors are code and are
  kept). Artifact `patterns/proxy/proxy8k-<corpus_sha>.json`; its sha is pinned in `measured_at.proxy_sha`.
- **Floor — `byte256`.** The untrained byte-level tokenizer (vocab 256): a tokenizer-independent lower bound
  that equals UTF-8 length; kept so a verdict can never depend on a proxy artefact alone.
- **Informational — `v03`, `qwen25coder`, `cl100k`.** Reported, never used for verdicts.
- **Tie-break — `min_bytes`.** Two forms are token-*tied* when their `proxy8k` counts differ by ≤ 1 token
  **or** by ≤ 5% (whichever is looser — at the 13–17 tokens typical of a `pat` function a single token is
  measurement noise, not a verdict; amended 2026-09-18 after the first 131.4 measurements); the tie is then
  broken by `min_bytes`, then left tied.
- **Region.** Only the `f=pat` function is counted (its extent from `tkc --dump-ast`), on its `--min` text.
- **Known bias.** The proxy is trained on the *current, verbose* corpus, so verbose shapes have cheaper merges
  and canonical forms are measured pessimistically. This is the safe direction: a form that wins under a
  hostile proxy wins more under the real tokenizer. It is still why every verdict starts `provisional` (§8).

## 5. Runtime measurement protocol

### 5.1 Fixture and workload

Each fixture reads the workload size from the environment variable `PAT_N` (see `bench/patterns/README.md`
for the exact env accessor the card permits), builds an N-sized input, runs `pat` and prints **one
deterministic checksum line**. All forms of a pattern must print byte-identical stdout for the same `PAT_N`;
a mismatch fails the whole entry (`output_mismatch`) — a pattern that changes behaviour is not a pattern.
`PAT_N` is auto-sized by the harness (doubling from `patterns/<id>/bench.json:start_n`, default 1000) until
the **best** form runs ≥ 50 ms, and the value used is recorded as `pat_n`.

### 5.2 Measurement

`bench/patterns/run_patterns.py`: `tkc -O2`; refuses to run if 1-minute loadavg > 2; 2 warm-up + 15 timed
runs per form; median wall + bootstrap 95% CI (1000 resamples); median peak RSS; one extra run under
`liballoccount.dylib` (`DYLD_INTERPOSE` on malloc/calloc/realloc/free) for `allocs`; one run at 4N for
`bigO_ratio`; records `hw.model`, thermal state, tkc version + sha, date. Results are immutable files under
`bench/patterns/results/` and are ingested into the catalogue by `render_catalogue.py --ingest`.

### 5.3 Runtime gate (per candidate, relative to the best form of the same pattern)

A candidate is **runtime-tied** iff all of:

1. median wall ≤ 1.05 × best median wall, **and** its 95% CI overlaps the best form's CI;
2. median peak RSS ≤ 1.05 × best RSS;
3. same asymptotic class: `bigO_ratio` within a factor of 1.5 of the best form's ratio.

Otherwise it is `slower` (or `worse-bigO` when rule 3 fails — that verdict is terminal regardless of wall).
`allocs` never decides a verdict; it breaks a runtime tie when everything else is tied, and it is published so
authors can see *why* a form is slower.

### 5.4 Macro check (131.8)

Micro-benchmarks can mislead. Before card v2 ships, the 12 `bench/programs/*.tk` plus a 30-program stratified
sample of the verified library are rewritten in canonical forms; each must produce byte-identical output and
regress ≤ 5% on wall and RSS versus the original. A regression **re-opens the responsible verdict** (status
back to `provisional`, note in `bug_caveats` or a new `source` line) — it is never silently absorbed.

## 6. Verdict algorithm

For each pattern, over its non-blocked candidates:

1. Compute `token_verdict`: `best` = minimal `proxy8k` (after §4 tie rule); `tied` = within the tie rule;
   else `more`.
2. Compute `runtime_verdict` per §5.3.
3. **Canonical** = the form that is `best`-or-`tied` on **both**. If several qualify, choose the one with the
   fewer `proxy8k` tokens, then fewer `min_bytes`, then fewer `allocs.calls`.
4. If **no** form qualifies on both axes: `canonical` = the token-best form *provided* it is not `worse-bigO`;
   `hot_path` = the runtime-best form; `choose_hot_path_when` must be written (e.g. "input > 10k elements or
   inside a loop body executed > 1k times"). If the token-best form *is* `worse-bigO`, the runtime-best form
   is canonical and there is no hot path — a quadratic default is never taught.
5. `status` = `provisional` on first measurement (§8); `measured` after 131.25 confirms it under a proxy
   trained on the rewritten corpus; `blocked` when the preferred form is a `.blocked.tk` (then the canonical
   is the best *available* form and `bug_caveats.preferred_when_fixed` names the blocked one).

The catalogue never records a verdict without the numbers that produced it; `validate_catalogue.py` rejects an
entry whose verdict does not follow from its candidates' fields.

## 7. Enforcement — how a verdict becomes the default

| layer | what it carries | story |
|---|---|---|
| `tkc --lint` | a rule per non-canonical form that is AST-decidable with low false positives; `fixable` only when the rewrite is deterministic in every case (AGENTS.md §6 rule for the `fix` field) | 131.9 |
| idiom judge | `idiom_judge.py` re-based on those lint rule ids; regex kept only for `hand-rolled-parser` | 131.10 |
| `run_shard.py validate` | hard-fail on any pattern-lint error/warning; `proxy_tokens` recorded; `over-budget` flag at > 1.5 × the category median | 131.10 |
| syntax card v2 | a generated ≤ 20-line "Patterns" block; new `card_sha` | 131.11 |
| spec + guide | `patterns-v0.4.md` (normative) and `guide/11-patterns-and-efficiency.md`, both generated; `make check-patterns` fails on drift | 131.11 |
| prompt pack | 128.2 derives from card v2 + catalogue | Epic 128 |

Spec-mandated styles (task descriptions that explicitly require a verbose form) remain exempt at the corpus
gate, as in `quality_rubric.md`; the catalogue itself has no exemptions.

## 8. Provisional-until-remeasured rule

Every verdict is `provisional` when first recorded. It becomes `measured` only after 131.25 re-runs the token
proxy (retrained on the *rewritten* corpus) and the runtime harness and the verdict is unchanged. It is
re-run once more when 116.9 Phase 3 locks the real v0.4 tokenizer (TEMSpec). Any flip is **listed and
re-opened** — never silently changed. Compiler-bug closures trigger `scripts/patterns/recheck_caveats.py`
(131.26), which re-measures every entry whose `bug_caveats` name the closed issue.

## 9. What this protocol does not decide

- Whether `over-budget` becomes a hard corpus gate (131.13 decides from the sweep distribution).
- The vocabulary or merges of the real tokenizer (116.9 Phases 2–4; this protocol only supplies the
  must-merge list, 131.23).
- Anything about training (Epic 128).
