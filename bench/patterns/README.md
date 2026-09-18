# Pattern micro-benchmarks (Epic 131)

Runtime harness for the pattern catalogue: for each pattern it measures every
candidate form (`patterns/<id>/<form>.tk`) and applies the 131.1 runtime gate,
so the catalogue can say whether the token-cheapest form is also runtime-tied.
The toke-vs-C suite in `bench/run_bench.sh` is separate and untouched.

macOS only (Mac Studio). Python 3 stdlib only.

## Usage

```bash
make -C bench/patterns                       # builds liballoccount.dylib (also done on demand)
python3 bench/patterns/run_patterns.py       # all patterns, 2 warm-up + 15 timed runs
python3 bench/patterns/run_patterns.py --pattern acc-array --pattern str-build-loop
python3 bench/patterns/run_patterns.py --runs 5 --no-alloc      # quick look
python3 bench/patterns/run_patterns.py --json-only               # prints only the results path
```

Flags: `--pattern <id>` (repeatable), `--runs N` (default 15), `--warmup N`
(default 2), `--bigo-runs N` (timed runs at 4N, default 3), `--no-alloc`,
`--json-only`, `--max-load X` (default 2.0 — the run is refused when the 1-min
loadavg is higher), `--allow-load` (run anyway; the results file then carries
`meta.load_warning: true` and must not feed the catalogue). `meta.loadavg_1min`
is recorded in every results file.

Exit status: 0 when every pattern has status `ok`, 1 otherwise, 2 on refusal.

Results: `bench/patterns/results/<YYYYMMDD-HHMMSS>.json` plus a table on stdout.
Build products go to `bench/patterns/build/` (gitignored).

## Fixture convention (what 131.6 / 131.7 authors write)

```
patterns/<pattern-id>/a.tk         one complete program per candidate form
patterns/<pattern-id>/b.tk         forms are single letters a-z
patterns/<pattern-id>/c.blocked.tk preferred-but-compiler-broken form: kept, never compiled
patterns/<pattern-id>/bench.json   optional: {"start_n": 1000, "timeout_s": 30, "max_n": 1073741824}
```

Every fixture:

* is a complete `m=main;` program in card-verified 2.8.0 syntax
  (`toke-corpus/regen/syntax_card.md`); mind the 127.x known-broken constructs
* has exactly one function `f=pat(...)` — that function IS the measured pattern;
  `main` is harness and must be the same across the forms of a pattern
* reads the workload size with `i=env:std.env;` and
  `let n=env.getint("PAT_N";1000);` (a missing or non-numeric `PAT_N` yields the
  default, so fixtures also run standalone); the harness sets `PAT_N`
* builds an N-sized input (or loops N times for a tiny pattern), calls `pat`,
  and prints exactly one deterministic checksum line with `io.println` — the
  harness requires byte-identical stdout across forms (else `output_mismatch`)
* is compiled `tkc -O2 --allow-all` (same as `run_bench.sh`; `--allow-all`
  because `std.env` is capability-gated)

Reference fixtures: `patterns/str-build-loop/{a,b,c}.tk` (concat chain /
`s.builder` / `arr.append` + `s.join`) and `patterns/acc-array/{a,b}.tk`
(`x=x.append(v)` / `x=x+@(v)`).

## What is measured

| quantity | how |
|---|---|
| `pat_n` | `PAT_N` doubled from `start_n` (default 1000) until the fastest completing form runs ≥ 50 ms; each probe is the min of two runs (the first launch of a fresh binary is cold on macOS); confirmed against the timed median, re-doubled if needed |
| wall | `time.perf_counter` around fork/exec → exit of the fixture binary (includes ~1–2 ms process start, identical for all forms); 2 warm-up + 15 timed runs; median + percentile-bootstrap 95 % CI of the median (1000 resamples, fixed seed) |
| peak RSS | `ru_maxrss` from `os.wait4` on the child — the same kernel counter `/usr/bin/time -l` prints; read directly because `time -lp`'s `real` has 10 ms resolution, too coarse for a 5 % gate at 50 ms |
| allocs | one extra run with `DYLD_INSERT_LIBRARIES=liballoccount.dylib` (`alloccount.c`, `DYLD_INTERPOSE` of malloc/calloc/realloc/free): `calls` = malloc+calloc+realloc, `bytes` = sum of requested sizes (realloc at its new size) |
| big-O | 1 warm-up + 3 timed runs at 4N; `bigO_ratio` = median(4N)/median(N) |
| output | stdout sha256 at N and 4N; must match across forms, and across runs within a form (else `nondeterministic`) |
| timeouts | a form exceeding `timeout_s` (default 30 s; 4× at 4N) is dropped with status `timeout`, keeping its last completed `(N, ms)` probe and a growth exponent from the probe series (`~O(N^2.0)` = quadratic) |

Environment recorded in `meta`: date, `hw.model`, core count, OS, 1-min loadavg,
`pmset -g therm` lines, `tkc --version`, repo git sha + dirty flag, sha256 of the
`tkc` binary, run counts and the gate parameters.

## Gate rule (131.1, fixed)

`best` = form with the lowest median wall. Another form is **`tied`** iff all of:

1. median wall within 5 % of best,
2. the two 95 % CIs overlap,
3. median peak RSS within 5 % of best,
4. same big-O class: its 4N/N wall ratio is within 1.5× of best's.

Otherwise: `worse-bigO(+x%)` when (4) fails, else `slower(+x%[; rss ±y%][; ci-disjoint])`.
Forms that never complete get `timeout(>30s; N=…:…ms; ~O(N^k))`; a pattern whose
forms print different checksums gets every verdict set to `output_mismatch`.
Allocation counts are recorded but are a tie-break only, never a gate input.

## Feeding `patterns/catalogue.json`

Each candidate entry copies these per-form fields from the results file
(`patterns.<id>.forms.<form>`), names identical:

| field | type | source |
|---|---|---|
| `wall_ms_median` | float | median of the 15 timed runs at `pat_n` |
| `wall_ci95` | `[lo, hi]` | bootstrap 95 % CI of the median |
| `rss_kb_median` | int | median peak RSS in KB |
| `allocs` | `{calls, bytes}` | from the interposer run (`malloc/calloc/realloc/free` also present) |
| `bigO_ratio` | float or null | wall(4N)/wall(N); null when the 4N run timed out |
| `pat_n` | int | pattern-level; the N every form of the pattern was measured at |

plus `verdict` per form and `measured_at` from `meta` (`date`, `tkc_git_sha`,
`hw_model`). `scripts/patterns/render_catalogue.py --ingest <results.json>`
(131.11) performs the copy; until it exists, copy by hand from the newest file
in `bench/patterns/results/`.
