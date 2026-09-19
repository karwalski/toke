# Macro check — real programs in canonical patterns (story 131.8)

Micro-benchmarks can mislead. This directory rewrites real programs in the
catalogue's canonical forms (`patterns/catalogue.json`, protocol
`docs/spec/patterns-protocol-v0.4.md` §5.4) and checks that each rewrite is
output-identical to its original and regresses neither wall time nor peak RSS
by more than 5 %. A regression re-opens the responsible pattern verdict (in the
catalogue, by the owner — this directory records, it never edits the catalogue).

## Program set (44 pairs)

* the 12 `bench/programs/*.tk` (toke-vs-C suite; untouched, copied here)
* a stratified sample of the verified library
  (`~/tk/toke-test-programs/results/library/<cat>.json`, the 1,583 passing
  programs): **2 per category, 16 categories = 32 programs**, drawn with
  `random.Random("131:<category>").sample(sorted ids, 2)` — reproducible,
  independent of dict order. The story says "30"; with 16 categories the
  2-per-category rule gives 32, which is what is here.

`prepare.py` draws the set, copies every original beside its rewrite as
`programs/<name>.orig.tk` with a provenance header
`(* source: <path> sha256 <sha256 of the original bytes> [manifest <path>] *)`,
and writes `pairs.json` (source, sha, and the manifest test cases for library
programs, so the directory is self-contained).

Per pair:

| file | what |
|---|---|
| `programs/<name>.orig.tk` | verbatim original + provenance header |
| `programs/<name>.canon.tk` | the rewrite: catalogue canonical forms applied wherever a pattern applies, **nothing else changed** (no renames, reordering, dead-code removal, operator or stdlib substitutions) |
| `programs/<name>.patterns.json` | `applied` (pattern id, site count, note), `not_applied` (pattern id, `issue` = the open 127.x bug that blocks it, reason), `reformatted` |

`reformatted: true` marks a library program whose original is a single minified
line: its canon is laid out from `tkc --fmt` of the original so the rewrite is
reviewable. The formatter's own normalisations (`@($str)` → `@$str`) are not
pattern changes, so for those pairs `run_macro.py` also measures the fmt'd
original (`orig_fmt`) and reports the token delta against it (marked † in the
table). Tokens are counted on the `tkc --min` text, so layout never affects them.

## Scripts

```bash
python3 bench/patterns/macro/prepare.py              # (re)draw the set, copy originals, pairs.json
python3 bench/patterns/macro/verify.py [names…]      # output-identity proof -> results/verify_<stamp>.json
python3 bench/patterns/macro/run_macro.py [names…] [--runs 15] [--allow-load] [--external] [--md FILE]
```

Both harness scripts pin the compiler for the whole run (`scripts/patterns/tkc_pin.py`,
131.39) and stamp its sha256 into the results; `run_macro.py` refuses to run when the
1-min loadavg is above `--max-load` (2.0) unless `--allow-load`, which sets
`meta.load_warning: true`.

### verify.py — output identity

For every pair: `tkc --check` both, build both (`tkc -O2 --allow-all`, as
`bench/run_bench.sh`), run both on the pair's own workload and require the **raw
stdout bytes and the exit status to be equal**:

* bench programs: one run, no stdin — they print nothing; their result is
  `main`'s return value, i.e. the exit status (`nested_loops` exits 144);
* library programs: one run per manifest test case with the case input on
  stdin, fresh cwd per case — the stdin runner semantics of
  `toke-corpus/regen/validate.py` (imported when present); additionally both
  sides must still **pass the manifest** (`norm_stdout(stdout) == expected`,
  exit 0), so the canon is proven identical to the original *and* still correct.

The `tkc --min` sha256 of both sides is recorded too, so a pair whose canon
differs only in layout is visibly unchanged.

### run_macro.py — wall, RSS, tokens, verdict

Same measurement method as `run_patterns.py`: wall = `perf_counter` around
fork/exec → exit; peak RSS = `ru_maxrss` from `os.wait4`; 2 warm-up + 15 timed
runs, median + percentile-bootstrap 95 % CI. Original and canon are
**interleaved** (o c c o o c …) so load drift hits both sides equally. A run of a
library program executes every manifest case once (wall summed, RSS max);
stdout sha + exit status are checked on every timed run.

Tokens: `scripts/patterns/count_tokens.py` on the **whole program** (not
`--function main` — the rewrites live in helper functions as often as in
`main`); `proxy8k` is the decision metric, `byte256`/`min_bytes` recorded.

Verdict (canon relative to original, protocol §5.4):

| verdict | rule |
|---|---|
| `ok` | median wall ≤ +5 % and median RSS ≤ +5 % |
| `regressed` | wall or RSS > +5 %; `suspects` = the pattern ids applied to that pair |
| `unchanged` | canon's `tkc --min` text equals the original's — no pattern applied; the numbers are the harness noise floor |
| `output_mismatch` / `compile_error` / `timeout` | should not happen after `verify.py` |

Results: `results/<YYYYMMDD-HHMMSS>.json` (`meta`, per-pair `sides`, `summary`).

## Results

Run: **2026-09-19T10:56:18**, toke 2.8.0 @ tkc sha256 `fe380d8d660d5eeb…` (toke HEAD `c22edaa000d9`,
src dirty). The 127.25/26/27/38 fixes had just landed — interpolation now lowers to a
single allocation and linear-array arguments no longer force copies — so every earlier
measurement was discarded and the whole set re-measured on this binary.
Results: `results/20260919-110102.json`.

**Load caveat (recorded, accepted).** 1-min loadavg **30.19** at the start of the run,
far above the harness's 2.0 refusal threshold, so the file carries
`meta.load_warning: true` and must not feed a catalogue verdict on its own; the run
used `--allow-load`. Consequence: the **wall** column is noisy at the few-millisecond
scale of the library programs — repeating the identical pass flags a *different* set of
pairs each time, always with overlapping 95 % CIs. The `cpu Δ%` column
(`ru_utime+ru_stime` from the same `wait4`) is load-insensitive and is the trustworthy
signal here; the verdict rule itself is unchanged (wall + RSS, §5.4).

**Output identity: 44/44 pairs byte-identical** (`results/verify_20260919-105611.json`,
same compiler). Every canon produces the **same raw stdout bytes and the same exit
status** as its original on the pair's own workload — 89 runs in total: one per bench
program, one per manifest test case for the library programs — and every library canon
still passes its manifest.

**Verdicts:** 30 `ok`, 8 flagged `regressed`, 6 `unchanged` (no pattern applied),
0 `output_mismatch`, 0 `compile_error`. **All 8 flagged pairs cleared at a higher run
count** on the same machine (‡ in the table: 51 runs, SCI-086 at 101) with cpu within
±1.5 % and RSS unchanged, so **this macro check re-opens no pattern verdict**. The
flags are the load noise floor: the median over the 38 changed pairs is
**-0.54 %** wall, **+0.12 %** cpu, **+0.00 %** RSS — and the 6 `unchanged` pairs
(byte-identical binaries on both sides) move **-0.58 %** on wall, which calibrates it.

**Tokens (`proxy8k`, whole program):** Σ **12830 → 12202** over the 38 changed pairs,
**-628** (-4.9 %); per-pair median **-10.5** tokens (-4.35 %). Across real
programs the canonical forms are cheaper in tokens at no runtime or memory cost.

| pair | kind | patterns applied | tokens orig→canon (Δ) | wall ms orig→canon (Δ%) | cpu Δ% | RSS KB orig→canon (Δ%) | verdict |
|---|---|---|---|---|---|---|---|
| fib_recursive | bench | `fn-recursion-vs-loop` | 23→25 (+2) | 43.3→43.5 (+0.5%) | +0.2% | 1408→1408 (+0.0%) | ok |
| fib_iterative | bench | — | 32→32 (+0) | 24.5→24.0 (-2.0%) | -1.3% | 1408→1408 (+0.0%) | unchanged |
| sum_array | bench | — | 104→104 (+0) | 8.7→8.8 (+0.8%) | +1.6% | 1408→1392 (-1.1%) | unchanged |
| nested_loops | bench | — | 23→23 (+0) | 8.2→8.8 (+7.2%) | +4.8% | 1392→1408 (+1.1%) | unchanged |
| binary_search | bench | `cond-elif-chain` | 97→97 (+0) | 15.9→15.3 (-4.2%) | -1.8% | 1408→1408 (+0.0%) | ok |
| prime_sieve | bench | `err-validate-early`, `iter-find-first` | 54→45 (-9) | 9.2→8.7 (-5.3%) | -5.5% | 1392→1408 (+1.1%) | ok |
| deep_recursion | bench | `cond-elif-chain`, `fn-recursion-vs-loop` | 34→35 (+1) | 171.4→170.0 (-0.8%) | -0.5% | 1648→1648 (+0.0%) | ok |
| struct_ops | bench | — | 172→172 (+0) | 8.9→7.4 (-16.4%) | -5.6% | 1392→1408 (+1.1%) | unchanged |
| large_expr | bench | — | 76→76 (+0) | 9.3→7.8 (-16.2%) | -7.1% | 1408→1408 (+0.0%) | unchanged |
| chained_calls | bench | — | 72→72 (+0) | 8.5→8.8 (+3.9%) | -5.6% | 1408→1392 (-1.1%) | unchanged |
| collatz | bench | `cond-bind-if` | 46→47 (+1) | 100.7→102.6 (+1.9%) | +0.0% | 1392→1392 (+0.0%) | ok |
| gcd_euler | bench | `acc-count-if` | 64→61 (-3) | 1602.0→1593.7 (-0.5%) | -0.1% | 1392→1392 (+0.0%) | ok |
| AIA-118 | library | `str-interp-vs-join`, `acc-array`, `cond-bind-if`, `cond-elif-chain`, `str-build-loop`, `str-array-render`, `fn-chain-vs-let` | 776→778 (+2)† | 15.6→15.7 (+0.7%) | +3.8% | 1472→1456 (-1.1%) | ok |
| AIA-100 | library | `cond-bind-if`, `iter-find-first`, `acc-array`, `fn-chain-vs-let`, `str-num-format` | 1196→1144 (-52) | 17.7→19.2 (+8.1%) | +6.9% | 1488→1488 (+0.0%) | **ok**‡ (-0.8% wall / -0.2% cpu at 51 runs) |
| FIN-072 | library | `cond-elif-chain` | 128→128 (+0) | 16.9→17.3 (+2.0%) | -5.3% | 1440→1440 (+0.0%) | ok |
| FIN-122 | library | `acc-array`, `cond-elif-chain` | 250→248 (-2) | 16.1→16.4 (+1.9%) | +0.4% | 1456→1456 (+0.0%) | ok |
| CRY-119 | library | `str-build-loop`, `str-interp-vs-join`, `fn-chain-vs-let`, `acc-array`, `cond-elif-chain` | 276→264 (-12)† | 24.8→24.5 (-1.2%) | -1.9% | 1456→1456 (+0.0%) | ok |
| CRY-041 | library | `acc-array`, `str-build-loop`, `iter-find-first`, `fn-chain-vs-let` | 1771→1744 (-27) | 15.3→15.8 (+2.9%) | +7.8% | 2048→2032 (-0.8%) | ok |
| DAT-121 | library | `cond-bool-combine`, `cond-elif-chain`, `cond-bind-if`, `str-num-format`, `str-build-loop`, `str-interp-vs-join`, `fn-chain-vs-let` | 849→743 (-106) | 14.0→15.2 (+8.7%) | -1.1% | 1504→1472 (-2.1%) | **ok**‡ (+1.0% wall / -3.1% cpu at 51 runs) |
| DAT-045 | library | `str-build-loop`, `iter-find-first`, `fn-chain-vs-let`, `str-interp-vs-join` | 243→236 (-7) | 15.6→14.1 (-9.6%) | +1.8% | 1440→1440 (+0.0%) | ok |
| DEV-104 | library | `str-interp-vs-join`, `str-num-format`, `cond-bind-if`, `cond-bool-combine`, `acc-count-if`, `fn-chain-vs-let`, `str-build-loop` | 433→357 (-76)† | 14.4→14.4 (-0.2%) | +1.0% | 1456→1456 (+0.0%) | ok |
| DEV-045 | library | `acc-array`, `fn-chain-vs-let`, `cond-bool-combine`, `cond-elif-chain`, `str-interp-vs-join`, `str-num-format` | 186→155 (-31)† | 16.0→16.1 (+1.0%) | +1.5% | 1440→1456 (+1.1%) | ok |
| EDU-098 | library | `fn-chain-vs-let`, `cond-bind-if`, `acc-array`, `coll-swap` | 383→357 (-26)† | 15.4→14.5 (-6.1%) | +1.3% | 1456→1440 (-1.1%) | ok |
| EDU-085 | library | `str-build-loop`, `fn-chain-vs-let`, `acc-array`, `cond-bool-combine`, `str-interp-vs-join`, `str-num-format` | 580→563 (-17)† | 17.8→14.1 (-21.1%) | -9.7% | 1456→1456 (+0.0%) | ok |
| GAM-117 | library | `acc-array`, `str-build-loop`, `fn-chain-vs-let`, `cond-bool-combine`, `str-interp-vs-join` | 264→252 (-12)† | 15.4→14.7 (-5.1%) | -7.3% | 1440→1456 (+1.1%) | ok |
| GAM-072 | library | `cond-bool-combine`, `fn-chain-vs-let`, `cond-bind-if`, `cond-elif-chain` | 263→250 (-13)† | 27.4→26.2 (-4.5%) | -3.7% | 1456→1456 (+0.0%) | ok |
| GAZ-041 | library | `cond-bind-if`, `str-build-loop`, `str-interp-vs-join`, `str-num-format` | 432→427 (-5) | 46.4→46.1 (-0.6%) | -8.3% | 1440→1440 (+0.0%) | ok |
| GAZ-012 | library | `str-build-loop`, `str-num-format`, `str-interp-vs-join`, `fn-chain-vs-let`, `cond-bool-combine`, `cond-elif-chain`, `cond-bind-if` | 697→680 (-17) | 106.2→104.8 (-1.3%) | +2.6% | 39808→36032 (-9.5%) | ok |
| MFG-051 | library | `fn-chain-vs-let`, `acc-array`, `cond-bind-if`, `cond-bool-combine`, `cond-elif-chain`, `str-build-loop`, `str-array-render`, `str-interp-vs-join` | 640→579 (-61) | 6.7→7.9 (+18.2%) | -6.6% | 1456→1440 (-1.1%) | **ok**‡ (-2.7% wall / -5.0% cpu at 51 runs) |
| MFG-007 | library | `cond-bind-if`, `str-num-format`, `str-interp-vs-join` | 399→370 (-29)† | 7.4→8.6 (+17.0%) | +5.6% | 1456→1440 (-1.1%) | **ok**‡ (+4.6% wall / -3.5% cpu at 51 runs) |
| MED-062 | library | `str-build-loop`, `cond-bool-combine` | 182→178 (-4)† | 19.3→18.2 (-5.7%) | +2.3% | 1456→1456 (+0.0%) | ok |
| MED-036 | library | `str-build-loop` | 252→248 (-4)† | 20.6→19.1 (-7.0%) | +5.3% | 1456→1440 (-1.1%) | ok |
| MSG-061 | library | `str-build-loop`, `cond-bind-if`, `acc-array`, `cond-bool-combine`, `str-interp-vs-join`, `str-num-format` | 266→253 (-13) | 16.5→16.5 (-0.1%) | -1.0% | 1440→1440 (+0.0%) | ok |
| MSG-038 | library | `cond-elif-chain`, `cond-bool-combine`, `str-interp-vs-join`, `fn-chain-vs-let` | 285→275 (-10)† | 18.6→17.4 (-6.3%) | -1.1% | 1456→1440 (-1.1%) | ok |
| NET-111 | library | `cond-bind-if`, `cond-bool-render` | 116→105 (-11)† | 19.2→17.1 (-11.1%) | +2.6% | 1424→1440 (+1.1%) | ok |
| NET-108 | library | `cond-elif-chain` | 49→42 (-7)† | 16.7→19.5 (+16.6%) | +4.3% | 1440→1440 (+0.0%) | **ok**‡ (+4.0% wall / +1.4% cpu at 51 runs) |
| SCI-024 | library | `acc-array`, `coll-swap`, `str-build-loop`, `str-num-format`, `str-interp-vs-join` | 121→119 (-2)† | 21.1→19.8 (-6.2%) | -2.3% | 1456→1440 (-1.1%) | ok |
| SCI-086 | library | `fn-chain-vs-let`, `str-build-loop`, `str-num-format`, `acc-array`, `str-interp-vs-join` | 205→190 (-15)† | 19.6→22.3 (+13.8%) | +5.8% | 1440→1440 (+0.0%) | **ok**‡ (-5.7% wall / -0.0% cpu at 101 runs) |
| SEC-024 | library | `fn-chain-vs-let`, `cond-bool-combine`, `cond-bool-render` | 46→38 (-8)† | 22.6→21.5 (-5.0%) | +0.1% | 1440→1440 (+0.0%) | ok |
| SEC-010 | library | `fn-chain-vs-let`, `str-interp-vs-join` | 335→320 (-15)† | 41.7→41.0 (-1.8%) | +2.1% | 1488→1488 (+0.0%) | ok |
| SOC-139 | library | `str-interp-vs-join`, `str-num-format` | 127→113 (-14)† | 8.7→9.2 (+5.9%) | -4.5% | 1440→1440 (+0.0%) | **ok**‡ (-4.9% wall / -4.1% cpu at 51 runs) |
| SOC-037 | library | `str-interp-vs-join`, `fn-chain-vs-let` | 258→220 (-38) | 13.2→11.5 (-13.2%) | +0.1% | 1488→1472 (-1.1%) | ok |
| SYS-066 | library | `acc-array`, `str-build-loop`, `cond-elif-chain`, `cond-bool-combine`, `str-array-render` | 222→232 (+10)† | 39.4→42.0 (+6.8%) | +0.5% | 1456→1472 (+1.1%) | **ok**‡ (-0.3% wall / -2.1% cpu at 51 runs) |
| SYS-010 | library | `acc-array`, `cond-elif-chain`, `cond-bool-combine`, `str-build-loop`, `str-array-render` | 282→284 (+2)† | 22.3→22.9 (+2.4%) | -5.0% | 1440→1456 (+1.1%) | ok |

† token delta measured against `tkc --fmt` of the original (that pair's canon is laid
out by the formatter), so the formatter's own normalisations are excluded.
‡ flagged `regressed` on the 15-run pass, `ok` on a longer confirmation pass
(`results/20260919-110207.json`, 51 runs; `results/20260919-110223.json`, 101 runs for
SCI-086) — recorded here, not silently absorbed.

### Patterns applied

| pattern | pairs | sites |
|---|---|---|
| `acc-array` | 15 | 61 |
| `str-interp-vs-join` | 19 | 38 |
| `fn-chain-vs-let` | 19 | 37 |
| `str-build-loop` | 18 | 30 |
| `cond-bool-combine` | 14 | 28 |
| `cond-bind-if` | 13 | 26 |
| `str-num-format` | 12 | 26 |
| `cond-elif-chain` | 15 | 18 |
| `iter-find-first` | 4 | 5 |
| `str-array-render` | 4 | 5 |
| `coll-swap` | 2 | 3 |
| `acc-count-if` | 2 | 2 |
| `cond-bool-render` | 2 | 2 |
| `fn-recursion-vs-loop` | 2 | 2 |
| `err-validate-early` | 1 | 1 |
| **total** | **38** | **284** |

#### Canonical forms that could NOT be applied

Because of an open 127.x compiler bug:

* `acc-array` — blocked by **127.10** in AIA-100
* `cond-bind-if` — blocked by **127.7** in AIA-100
* `str-interp-vs-join` — blocked by **127.10** in AIA-118, EDU-098
* `str-interp-vs-join` — blocked by **127.7** in MED-036

Not a compiler bug — out of the pattern's stated applicability:

* `acc-array` — the pattern's applicability rule excludes the site in GAZ-012
* `acc-min-max` — the pattern's applicability rule excludes the site in GAZ-041
* `cond-bind-if` — the pattern's applicability rule excludes the site in MFG-051
* `cond-elif-chain` — the pattern's applicability rule excludes the site in AIA-100, MFG-007, SEC-010, SYS-066
* `err-default` — the pattern's applicability rule excludes the site in DAT-121
* `iter-find-first` — the pattern's applicability rule excludes the site in DEV-045, EDU-085, EDU-098, GAM-117, GAZ-012, MFG-051
* `parse-csv-line` — the pattern's applicability rule excludes the site in SYS-066
* `parse-json` — the pattern's applicability rule excludes the site in EDU-085, EDU-098, MSG-038
* `str-build-loop` — the pattern's applicability rule excludes the site in AIA-118, CRY-119, DAT-121
* `str-repeat-pad` — the pattern's applicability rule excludes the site in DEV-104

## Caveats

* Library programs are stdin-driven and run for a few milliseconds each: wall is
  dominated by process start-up, so ±5 % there is inside the noise floor of an
  idle machine — read the `unchanged` pairs as the calibration.
* The 12 bench programs print nothing and carry no stdlib patterns; only the
  conditional / early-return / count-if patterns apply to them.
