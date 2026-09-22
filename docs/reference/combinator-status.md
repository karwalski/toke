---
title: Combinator and String Method Status
slug: combinator-status
section: reference
order: 10
---

# Combinator and string method status (tkc 2.8.0)

Story 131.2; re-measured in full for story 136.53.

**This table is a transcript of a measurement, not a hand-maintained list.**
Every row below is transcribed from `patterns/probe/probe_results.json`, which
is produced by `patterns/probe/run_probes.sh` (one complete program per probe).
Nothing generates this Markdown from that JSON, so the two can drift — and on
2026-09-22 they had: the JSON was current while roughly twenty rows here still
described a compiler from before 127.4/127.6/127.7/127.10 and 137.12 landed,
**every one of them claiming breakage that no longer existed**. If you change a
row, re-run the probes and re-transcribe; do not edit a verdict by hand.

## Measurement pin

| field | value |
|---|---|
| measured | 2026-09-22 (story 136.53) |
| compiler | `toke 2.8.0` |
| binary sha256 | `a620f201b58c03c94325feb60b745f205710194af555a4fc169665cff3dd8058` |
| source tree | `445f5199a444e4c84f84e7f56e262b2e693012ac` |
| result | **101 of 130 probes correct** (check clean 111, build clean 101, ran clean 100) |

The `toke` binary is an untracked local build artifact, so the source SHA
identifies the tree the probes were **measured in**, not provably the tree the
binary was **built from**; the binary sha256 is the authoritative pin. The run
was cross-checked: an independent sequential re-probe of all 130 probes
reproduced it exactly, and reproduced the previous 2026-09-19 JSON in 129 of
130 probes (the one change is `ufcs_extra_arg_module`, below).

Columns: `--check` = `tkc --check` exit 0; `build` = `tkc --out` produced a
binary; `runs` = binary exited 0; `correct` = stdout matched the oracle. The
oracle is the syntax card's known-good form, or — for symbols the card does
not define — the natural semantic stated in the notes. `bool` values
interpolate as `1`/`0` on 2.8.0 (native comparisons included), so bool
oracles use `1`/`0`.

Legend: **ABSENT** = no compiler dispatch entry and no runtime glue symbol for
that call form (link fails with an undefined symbol, E9003); **REJECTED** =
the call is refused at `--check` with a named diagnostic; **CRASH** = check and
build clean, binary dies; **WRONG** = runs, wrong output.

> **WITHDRAWN (136.47), and the failure mode has improved (136.53).** The
> fourteen module-style `std.array` combinators — `map` `fold` `filter` `each`
> `all` `any` `count` `first` `last` `max` `min` `reduce` `sort` `sum` — are
> **not implemented** and remain withdrawn from the documentation until they
> are. They would resolve to `tk_array_*_w` symbols that exist in no file under
> `src/stdlib`.
>
> **What changed:** they no longer type-check clean and die at `ld`. Since the
> generated `stdlib/array.tki` landed (137.12) every one of them is **rejected
> at `--check` with E4027**, naming the member and the symbol the call would
> have linked against — e.g. *"module 'std.array' has no member 'fold':
> neither the interface nor the runtime declares it (the call would link
> against 'tk_array_fold_w')"*. Any prose elsewhere still saying these
> "type-check clean and die at `ld`" is describing a compiler older than this
> pin.
>
> **The receiver form is unaffected and is the spelling to use**: `a.map(&f)`,
> `a.filter(&p)`, `a.reduce(0;&f)`, `a.fold(0;&f)`, `a.sort(&cmp)` are
> implemented and correct (rows below). Nine receiver combinators — `each`
> `all` `any` `count` `sum` `min` `max` `first` `last` — are still genuinely
> ABSENT (E9003 at link).
>
> `scripts/gen_tki.py` can only declare a method whose symbol is defined, so
> the interface cannot drift back into promising the fourteen.

## Arrays (`@i64` receiver `a=@(3;1;2)`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| map | `a.map(&dbl)` | ok | ok | ok | yes | canonical |
| map | `arr.map(a;&dbl)` (`i=arr:std.array;`) | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47**; was recorded as clean-check/E9003-link, corrected 136.53 |
| filter | `a.filter(&iseven)` | ok | ok | ok | yes | canonical |
| filter | `arr.filter(a;&p)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| reduce | `a.reduce(0;&addf)` | ok | ok | ok | yes | canonical reduction form (`tk_arr_reduce`) |
| reduce | `arr.reduce(a;0;&f)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| fold | `a.fold(0;&addf)` | ok | ok | ok | yes | **127.4 closed** — `llvm.c` aliases receiver `fold` to `tk_arr_reduce`; returns 6. Was recorded ABSENT/E9003 here for months after the fix; corrected 136.53 |
| fold | `arr.fold(a;0;&f)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| sort | `a.sort(&cmp)` | ok | ok | ok | yes | canonical (comparator returns `a-b`) |
| sort | `arr.sort(a;&cmp)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| each | `a.each(&pr)` | ok | E9003 | – | no | ABSENT (`tk_array_each_w` undefined) |
| each | `arr.each(a;&pr)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| all | `a.all(&p)` | ok | E9003 | – | no | ABSENT |
| all | `arr.all(a;&p)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| any | `a.any(&p)` | ok | E9003 | – | no | ABSENT |
| any | `arr.any(a;&p)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| count | `a.count(&p)` | ok | E9003 | – | no | ABSENT |
| count | `arr.count(a;&p)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| sum | `a.sum()` | ok | E9003 | – | no | ABSENT |
| sum | `arr.sum(a)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| min | `a.min()` | ok | E9003 | – | no | ABSENT |
| min | `arr.min(a)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| max | `a.max()` | ok | E9003 | – | no | ABSENT |
| max | `arr.max(a)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| first | `a.first()` | ok | E9003 | – | no | ABSENT |
| first | `arr.first(a)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| last | `a.last()` | ok | E9003 | – | no | ABSENT |
| last | `arr.last(a)` | **E4027** | – | – | no | REJECTED at check — **WITHDRAWN 136.47** |
| find | `a.find(2)` | ok | ok | ok | yes | **corrected 136.53** — was recorded as a SIGSEGV via string glue; gives 2 |
| find | `arr.find(a;2)` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT; `tk_array_find_w` in `collections_glue.c`, linked for `std.array` via `stdlib_deps.c:132`; gives 2 |
| indexof | `a.indexof(1)` | ok | ok | ok | yes | **corrected 136.53** — was SIGSEGV; gives 1 |
| indexof | `arr.indexof(a;1)` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT; gives 1 |
| contains | `a.contains(2)` | ok | ok | ok | yes | **corrected 136.53** — was SIGSEGV; gives 1 |
| contains | `arr.contains(a;2)` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT; gives 1, and 0 for an absent value |
| slice | `a.slice(1;3)` | ok | ok | ok | yes | **corrected 136.53** — was SIGSEGV; gives `@(1;2)` |
| slice | `arr.slice(a;1;3)` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT; gives `@(1;2)` |
| append | `a=a.append(9)` | ok | ok | ok | yes | canonical accumulator |
| append | `a=arr.append(a;9)` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT and separately suspected of ill-typed IR; neither reproduces, `len` goes 3 → 4 |
| `+@()` | `a=a+@(9)` | ok | ok | ok | yes | canonical alternative (see 127.10) |
| push | `a=a.push(9)` | ok | ok | ok | yes | alias of append |
| push | `a.push(9);` (bare) | ok | ok | ok | yes* | **127.2 NOT reproducible** on this pin: silent no-op (`len` unchanged), exit 0, also for `mut.@()` and inside loops. *Correct only in the value-semantics sense; see 127.3 |
| push | `a=arr.push(a;9)` | ok | ok | ok | yes | module form works |
| pop | `a.pop()` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT method-style; builds and runs |
| pop | `arr.pop(a)` | ok | ok | ok | yes | returns array minus last element |
| set | `a=a.set(0;7)` | ok | ok | ok | yes | canonical |
| set | `a.set(0;7);` (bare) | ok | ok | ok | yes* | silent no-op (value semantics) — 127.3 diagnostic wanted |
| set | `a=arr.set(a;0;7)` | ok | ok | ok | yes | |
| get | `a.get(1)` | ok | ok | ok | yes | canonical |
| get | `arr.get(a;1)` | ok | ok | ok | yes | |
| get | `a.1` / `a.0` (constant index) | ok | ok | ok | yes | **corrected 136.53** — was recorded as a parse error E2002; the card's "constant index may use `arr.0`" is true on this pin |
| len | `a.len` | ok | ok | ok | yes | canonical |
| len | `a.len()` | ok | ok | ok | yes | |
| len | `arr.len(a)` | ok | ok | ok | yes | |

## Maps (`m=@("a":1;"b":2)`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| get | `m.get("b")` | ok | ok | ok | yes | canonical |
| set | `m=m.set(k;v)` | ok | ok | ok | yes | canonical |
| len | `m.len` | ok | ok | ok | yes | **corrected 136.53** — was WRONG (returns 0); returns 2 for a 2-entry map, and 2 after `m=m.set` adds a key. *Not covered by any file in `patterns/probe/`* — measured ad hoc, so it is the one row here without a probe behind it; a probe should be added |
| keys | `m.keys` (property — the card's form) | ok | ok | ok | yes | **corrected 136.53** — was WRONG (len 0, or garbage after `m=m.set`); `k.len`=2 and `k.get(0)`="a", including after `m=m.set` |
| keys | `m.keys()` | ok | ok | ok | yes | `k.len`=2, `k.get(0)`="a", and the interpolated read is correct too (the 127.7 pointer symptom is gone) |
| values | `m.values` | **E4035** | – | – | no | REJECTED at check (property form not declared) — **corrected 136.53**, was recorded as building and exiting 1 on garbage |
| values | `m.values()` | ok | E9003 | – | no | ABSENT (`tk_array_values_w` undefined) |

## Strings — `s.` module style (`i=s:std.str;`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| len | `s.len(x)` | ok | ok | ok | yes | canonical |
| upper / lower | `s.upper(x)` / `s.lower(x)` | ok | ok | ok | yes | canonical; interpolates correctly |
| replace | `s.replace(x;a;b)` | ok | ok | ok | yes | canonical; interpolates correctly |
| trim | `s.trim(x)` | ok | ok | ok | yes | interpolates correctly |
| concat | `s.concat(a;b)` | ok | ok | ok | yes | |
| concat | `s.concat(a;"A";"B")` (surplus arg) | **E4026** | – | – | yes | **corrected 136.53 — the one behaviour change since 2026-09-19.** Module-style stdlib calls **are** arity-checked now: *"wrong number of arguments for 'std.str.concat': the implementation takes 2 arguments, the call passes 3"*. Previously printed `xxA` (127.1 extension) |
| split | `s.split(x;",")` | ok | ok | ok | yes | element interpolation `"\(w.get(1))"` correct (tagged `@str`) |
| fields | `s.fields(x)` | ok | ok | ok | yes | direct and interpolated reads both correct — **127.9 no longer reproduces**, corrected 136.53 |
| join | `s.join("-";p)` | ok | ok | ok | yes | canonical (separator first) |
| join | `s.join(p;"-")` (swapped) | ok | ok | **SIGSEGV** | no | **127.5 STILL OPEN** — no E4031 at check time; the binary dies at runtime (exit 139). One of only two rows in this document that still record a live defect |
| startswith / endswith | `s.startswith(x;"a")` / `s.endswith(x;"b")` | ok | ok | ok | yes | bool renders `1` |
| contains | `s.contains(x;"b")` | ok | ok | ok | yes | bool renders `1` |
| indexof | `s.indexof(x;"c")` | ok | ok | ok | yes | |
| find | `s.find(x;"c")` | ok | ok | ok | yes | not in `str.tki`; resolves via generic fallback |
| slice | `s.slice(x;1;3)` | ok | ok | ok | yes | `str.tki` says `str!SliceErr`, but the plain value prints fine |
| fromint | `s.fromint(i)` | ok | ok | ok | yes | interpolates correctly |

## Strings — method / UFCS style (`x.op(...)`)

**Every row in this table that previously recorded a 127.6 link failure or a
127.7 pointer-instead-of-string now passes.** Those defects are fixed on this
pin; the table had not been re-transcribed since.

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| len | `x.len` / `x.len()` / `"abcdef".len` | ok | ok | ok | yes | **corrected 136.53** — **127.8 no longer reproduces**; returns 6, and the interpolated read is correct |
| len | `io.println(x.len)` (direct, unwrapped) | **E4031** | – | – | no | REJECTED at check — passing a length property straight to `io.println` is a type error, no longer a segfault |
| upper | `x.upper()` | ok | ok | ok | yes | **corrected 136.53** — **127.6 fixed**; was E9003 |
| lower | `x.lower()` | ok | ok | ok | yes | **corrected 136.53** — 127.6 fixed |
| ends | `x.ends("b")` | ok | ok | ok | yes | **corrected 136.53** — 127.6 fixed |
| replace | `x.replace("-";"+")` | ok | ok | ok | yes | **corrected 136.53** — 127.6 fixed |
| fields | `x.fields()` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT method-style |
| join | `p.join("-")` | ok | ok | ok | yes | **corrected 136.53** — was ABSENT method-style |
| starts | `x.starts("a")` | ok | ok | ok | yes | bool renders `1` |
| contains | `x.contains("b")` | ok | ok | ok | yes | |
| indexof | `x.indexof("c")` | ok | ok | ok | yes | |
| find | `x.find("c")` | ok | ok | ok | yes | |
| slice | `x.slice(1;3)` | ok | ok | ok | yes | **corrected 136.53** — `"\(x.slice(1;3))"` interpolates correctly; 127.7 fixed |
| trim | `x.trim()` | ok | ok | ok | yes | **corrected 136.53** — `"\(x.trim())"` and `let y=x.trim(); "\(y)"` both correct; 127.7 fixed |
| concat | `x.concat("b")` | ok | ok | ok | yes | **corrected 136.53** — interpolates correctly; 127.7 fixed |
| concat | `a.concat("A";"B")` (surplus arg) | ok | ok | ok | **WRONG** | **127.1 STILL OPEN** — prints `xxA`. Receiver/UFCS surplus args are *not* arity-checked, though the module form now is (E4026) and user-fn surplus args are (E4026). The second of the two live defects in this document |
| split | `x.split(",")` | ok | ok | ok | yes | **corrected 136.53** — `w.len`, `io.println(w.get(1))`, the chained `"a,b,c".split(",").get(1)`, **and** `"\(x.split(",").get(1))"` are all correct |

## Interpolation-built strings stored via `append` (127.10)

**127.10 no longer reproduces.** Every shape below that previously printed
addresses now prints the strings. Corrected 136.53.

| pattern | --check | build | runs | correct | notes |
|---|---|---|---|---|---|
| `let line="\(i): item"; items=items.append(line)` then `io.println(items.get(j))` | ok | ok | ok | yes | |
| `items=items.append("\(i): item")` then `io.println(items.get(j))` | ok | ok | ok | yes | |
| same, read back as `"\(items.get(j))"` | ok | ok | ok | yes | **was WRONG** (three distinct addresses) |
| `s.concat`-built strings, read back as `"\(items.get(j))"` | ok | ok | ok | yes | **was WRONG** |
| plain literal `items=items.append("x")`, read back as `"\(items.get(j))"` | ok | ok | ok | yes | **was WRONG** |
| `items=items+@("\(i): item")`, read back as `"\(items.get(j))"` | ok | ok | ok | yes | |
| D-FIO-0014v230 shape (`labels=labels.append("\(i+1)")` → `"\(labels.get(i)): …"`) | ok | ok | ok | yes | **was WRONG**; no longer reproduces |
| seeded `mut.@()` variant (`interp_append_seeded`) | **E4070** | – | – | no | REJECTED at check — the seeded-literal shape is now a type error rather than a silent wrong read |

The reclassification in `docs/architecture/127-disposition-131.md` (127.10 is
an inference problem, not a lifetime bug) is consistent with this: once the
array is inferred as `@str`, the interpolated read is correct. That inference
now happens for `.append`/`.push` as well as `+@()`.

## Symbols still ABSENT from the compiler

No dispatch entry in `src/llvm.c` and no runtime glue in `src/stdlib/`, so the
link fails with E9003:

- **Array receiver combinators:** `each` `all` `any` `count` `sum` `min` `max`
  `first` `last`
- **Map:** `values()` (and `m.values` as a property is rejected at check, E4035)

Module-style (`std.array`) `map filter reduce sort fold each all any count sum
min max first last` are **not absent at link** — they are **rejected at
`--check` with E4027**, which is the better failure and the one to expect.

Module-style `find indexof contains slice append push pop get set len join`
**do** exist and are correct; the previous edition of this document listed the
first four as absent, which was wrong.

## Safe for the canonical catalogue today

- **Arrays:** `xs.map(&f)`, `xs.filter(&p)`, `xs.reduce(init;&f)`,
  `xs.fold(init;&f)`, `xs.sort(&cmp)`, `xs=xs.append(v)`, `xs=xs.push(v)`,
  `xs=xs+@(v)`, `xs=xs.set(i;v)`, `xs.get(i)`, `xs.0`, `xs.len`,
  `xs.find(v)`, `xs.indexof(v)`, `xs.contains(v)`, `xs.slice(i;j)`,
  `xs.pop()`. Element interpolation of a `mut.@()` string array is safe
  however the array was built.
- **Arrays, module form** (`i=arr:std.array;`): `arr.get arr.set arr.len
  arr.append arr.push arr.pop arr.find arr.indexof arr.contains arr.slice
  arr.join`. **Not** the fourteen combinators — those are E4027.
- **Maps:** `m.get(k)`, `m=m.set(k;v)`, `m.len`, `m.keys`, `m.keys()`.
  Avoid `m.values` / `m.values()`.
- **Strings, module form:** `s.len s.upper s.lower s.replace s.trim s.concat
  s.split s.fields s.join(sep;arr) s.startswith s.endswith s.contains
  s.indexof s.find s.slice s.fromint`.
- **Strings, method form:** `x.len x.upper x.lower x.ends x.starts x.contains
  x.indexof x.find x.slice x.trim x.concat x.split x.fields p.join` — all
  correct, direct and interpolated.
- **Never in the catalogue until fixed:** the nine absent array receiver
  combinators, the fourteen module-style combinators, `m.values`,
  `s.join(arr;sep)` with the arguments swapped (**127.5**, segfaults with no
  check-time diagnostic), and any reliance on surplus arguments being caught
  in receiver/UFCS position (**127.1**).
