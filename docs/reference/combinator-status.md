---
title: Combinator and String Method Status
slug: combinator-status
section: reference
order: 10
---

# Combinator and string method status (tkc 2.8.0)

Story 131.2. Every row below was produced by `patterns/probe/run_probes.sh`
(one complete program per probe; results in `patterns/probe/probe_results.json`).
Pinned compiler: **tkc 2.8.0**, source SHA `439068b40acef1470bb43b3a6ff96c248a80a423`
(`tkc` is a symlink to `toke`, built 2026-07-13 from the last codegen commit
`386cf3d`; no `src/` change has landed since).

Columns: `--check` = `tkc --check` exit 0; `build` = `tkc --out` produced a
binary; `runs` = binary exited 0; `correct` = stdout matched the oracle. The
oracle is the syntax card's known-good form, or — for symbols the card does
not define — the natural semantic stated in the notes. `bool` values
interpolate as `1`/`0` on 2.8.0 (native comparisons included), so bool
oracles use `1`/`0`.

Legend: **ABSENT** = no compiler dispatch entry and no runtime glue symbol
for that call form (link fails with an undefined symbol); **CRASH** = check
and build clean, binary dies; **WRONG** = runs, wrong output.

> **WITHDRAWN (136.47, 2026-09-22).** Every module-style `arr.*` row marked
> ABSENT below is **withdrawn from the documentation** until it is
> implemented. The fourteen — `arr.map`, `fold`, `filter`, `each`, `all`,
> `any`, `count`, `first`, `last`, `max`, `min`, `reduce`, `sort`, `sum` —
> resolve to `tk_array_*_w` symbols that exist nowhere in `src/stdlib`, so a
> call type-checks clean and dies at `ld`. Documented-and-broken is worse than
> absent: this table recorded them as ABSENT for months while
> `docs/spec/idiom-v0.4.md` section 7 told authors to *prefer* them, so the
> form kept reaching new code.  The rows are kept here, marked, because this
> file's job is to record what a probe measured — but nothing else in the docs
> should present them as available.
>
> **The receiver form is unaffected and is the spelling to use**: `a.map(&f)`,
> `a.filter(&p)`, `a.reduce(0;&f)` are implemented and correct (rows below).
>
> The generated `stdlib/array.tki` (137.12) carries the 19 `std.array` members
> that have glue and none of these fourteen — `scripts/gen_tki.py` can only
> declare a method whose symbol is defined, so the interface cannot drift back
> into promising them.

## Arrays (`@i64` receiver `a=@(3;1;2)`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| map | `a.map(&dbl)` | ok | ok | ok | yes | canonical |
| map | `arr.map(a;&dbl)` (`i=arr:std.array;`) | ok | E9003 | – | no | ABSENT module-style (`tk_array_map_w` undefined) — **WITHDRAWN 136.47** |
| filter | `a.filter(&iseven)` | ok | ok | ok | yes | canonical |
| filter | `arr.filter(a;&p)` | ok | E9003 | – | no | ABSENT module-style — **WITHDRAWN 136.47** |
| reduce | `a.reduce(0;&addf)` | ok | ok | ok | yes | canonical reduction form — works today (`tk_arr_reduce`) |
| reduce | `arr.reduce(a;0;&f)` | ok | E9003 | – | no | ABSENT module-style — **WITHDRAWN 136.47** |
| fold | `a.fold(0;&addf)` | ok | ok | ok | yes | **127.4 closed** — re-measured 2026-09-22: `llvm.c` aliases receiver `fold` to `tk_arr_reduce`, and `@(3;1;2).fold(0;&addf)` returns 6. This row previously read ABSENT/E9003 and was stale; `syntax_card.md` already said so |
| fold | `arr.fold(a;0;&f)` | ok | E9003 | – | no | ABSENT — **WITHDRAWN 136.47** |
| sort | `a.sort(&cmp)` | ok | ok | ok | yes | canonical (comparator returns `a-b`) |
| sort | `arr.sort(a;&cmp)` | ok | E9003 | – | no | ABSENT module-style — **WITHDRAWN 136.47** |
| each | `a.each(&pr)` / `arr.each(a;&pr)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| all | `a.all(&p)` / `arr.all(a;&p)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| any | `a.any(&p)` / `arr.any(a;&p)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| count | `a.count(&p)` / `arr.count(a;&p)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| sum | `a.sum()` / `arr.sum(a)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| min | `a.min()` / `arr.min(a)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| max | `a.max()` / `arr.max(a)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| first | `a.first()` / `arr.first(a)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| last | `a.last()` / `arr.last(a)` | ok | E9003 | – | no | ABSENT (both forms) — **WITHDRAWN 136.47** |
| find | `a.find(2)` | ok | ok | SIGSEGV | no | **NEW** CRASH: array receiver dispatched to `tk_str_find_w` (string glue) |
| find | `arr.find(a;2)` | ok | E9003 | – | no | ABSENT module-style |
| indexof | `a.indexof(1)` | ok | ok | SIGSEGV | no | **NEW** CRASH: routed to `tk_str_indexof_w` |
| indexof | `arr.indexof(a;1)` | ok | E9003 | – | no | ABSENT module-style |
| contains | `a.contains(2)` | ok | ok | SIGSEGV | no | **NEW** CRASH: routed to `tk_str_contains_w` |
| contains | `arr.contains(a;2)` | ok | E9003 | – | no | ABSENT module-style |
| slice | `a.slice(1;3)` | ok | ok | SIGSEGV | no | **NEW** CRASH: routed to `tk_str_slice_w` |
| slice | `arr.slice(a;1;3)` | ok | E9003 | – | no | ABSENT module-style |
| append | `a=a.append(9)` | ok | ok | ok | yes | canonical accumulator |
| append | `a=arr.append(a;9)` | ok | E9003 | – | no | **NEW** (low): ill-typed IR (`ptr` where `i64` expected) — module form only |
| `+@()` | `a=a+@(9)` | ok | ok | ok | yes | canonical alternative; the ONLY append form whose element type is inferred for interpolation (see 127.10) |
| push | `a=a.push(9)` | ok | ok | ok | yes | alias of append |
| push | `a.push(9);` (bare) | ok | ok | ok | yes* | **127.2 NOT reproducible** on this SHA: silent no-op (`len` unchanged), exit 0, also for `mut.@()` and inside loops. *Correct only in the value-semantics sense; see 127.3 |
| push | `a=arr.push(a;9)` | ok | ok | ok | yes | module form works |
| pop | `a.pop()` | ok | E9003 | – | no | ABSENT method-style (no dispatch entry; glue `tk_array_pop_w` exists) |
| pop | `arr.pop(a)` | ok | ok | ok | yes | returns array minus last element |
| set | `a=a.set(0;7)` | ok | ok | ok | yes | canonical |
| set | `a.set(0;7);` (bare) | ok | ok | ok | yes* | silent no-op (value semantics) — 127.3 diagnostic wanted |
| set | `a=arr.set(a;0;7)` | ok | ok | ok | yes | |
| get | `a.get(1)` | ok | ok | ok | yes | canonical |
| get | `arr.get(a;1)` | ok | ok | ok | yes | |
| get | `a.1` / `a.0` (constant index) | E2002 | – | – | no | **NEW**: parse error "expected field, got '0'" — the card's "constant index may use arr.0" is false on 2.8.0 |
| len | `a.len` | ok | ok | ok | yes | canonical |
| len | `a.len()` | ok | ok | ok | yes | |
| len | `arr.len(a)` | ok | ok | ok | yes | |

## Maps (`m=@("a":1;"b":2)`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| get | `m.get("b")` | ok | ok | ok | yes | canonical |
| set | `m=m.set(k;v)` | ok | ok | ok | yes | canonical |
| len | `m.len` | ok | ok | ok | **WRONG** | **NEW**: returns 0 for a 2-entry map (array-header load on a map block; no `tk_map_len` glue exists) |
| keys | `m.keys` (property — the card's form) | ok | ok | exit 1 / ok | **WRONG** | **NEW**: yields len 0 (literal map) or garbage (`34359738370` after `m=m.set`); `k.get(0)` then exits 1 |
| keys | `m.keys()` | ok | ok | ok | yes (direct) | `k.len`=2, `io.println(k.get(0))`="a"; but `"\(k.get(0))"` prints a pointer (untagged `@str`, 127.7 class) |
| values | `m.values` | ok | ok | exit 1 | no | ABSENT (property reads garbage) |
| values | `m.values()` | ok | E9003 | – | no | ABSENT |

## Strings — `s.` module style (`i=s:std.str;`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| len | `s.len(x)` | ok | ok | ok | yes | canonical |
| upper / lower | `s.upper(x)` / `s.lower(x)` | ok | ok | ok | yes | canonical; interpolates correctly |
| replace | `s.replace(x;a;b)` | ok | ok | ok | yes | canonical; interpolates correctly |
| trim | `s.trim(x)` | ok | ok | ok | yes | interpolates correctly |
| concat | `s.concat(a;b)` | ok | ok | ok | yes | |
| concat | `s.concat(a;"A";"B")` (surplus arg) | ok | ok | ok | **WRONG** | **NEW / 127.1 extension**: prints `xxA` — module-style stdlib calls are not arity-checked either |
| split | `s.split(x;",")` | ok | ok | ok | yes | element interpolation `"\(w.get(1))"` correct (tagged `@str`) |
| fields | `s.fields(x)` | ok | ok | ok | yes (direct) | `io.println(s.fields(x).get(0))` correct; `"\(...get(0))"` and `let w=s.fields(x); "\(w.get(1))"` print a pointer — **127.9** |
| join | `s.join("-";p)` | ok | ok | ok | yes | canonical (separator first) |
| join | `s.join(p;"-")` (swapped) | ok | ok | SIGSEGV | no | **127.5** — no E4031 at check time |
| startswith / endswith | `s.startswith(x;"a")` / `s.endswith(x;"b")` | ok | ok | ok | yes | bool renders `1` |
| contains | `s.contains(x;"b")` | ok | ok | ok | yes | bool renders `1`; `io.println(s.contains(...))` (bool arg) segfaults |
| indexof | `s.indexof(x;"c")` | ok | ok | ok | yes | |
| find | `s.find(x;"c")` | ok | ok | ok | yes | not in `str.tki`; resolves via generic fallback |
| slice | `s.slice(x;1;3)` | ok | ok | ok | yes | `str.tki` says `str!SliceErr`, but the plain value prints fine |
| fromint | `s.fromint(i)` | ok | ok | ok | yes | interpolates correctly |

## Strings — method / UFCS style (`x.op(...)`)

| symbol | call form | --check | build | runs | correct | notes / 127.x cross-ref |
|---|---|---|---|---|---|---|
| len | `x.len` / `x.len()` / `"abcdef".len` | ok | ok | ok | **WRONG** | **127.8** — returns 1 (array-header load); `io.println(x.len)` segfaults |
| upper | `x.upper()` | ok | E9003 | – | no | **127.6** — ABSENT dispatch entry (glue exists) |
| lower | `x.lower()` | ok | E9003 | – | no | **127.6** |
| ends | `x.ends("b")` | ok | E9003 | – | no | **127.6** (glue `tk_str_ends_w` exists) |
| replace | `x.replace("-";"+")` | ok | E9003 | – | no | **127.6** |
| fields | `x.fields()` | ok | E9003 | – | no | ABSENT method-style (same class as 127.6) |
| join | `p.join("-")` | ok | E9003 | – | no | ABSENT method-style (same class as 127.6) |
| starts | `x.starts("a")` | ok | ok | ok | yes | bool renders `1` |
| contains | `x.contains("b")` | ok | ok | ok | yes | |
| indexof | `x.indexof("c")` | ok | ok | ok | yes | |
| find | `x.find("c")` | ok | ok | ok | yes | |
| slice | `x.slice(1;3)` | ok | ok | ok | yes (direct) | `"\(x.slice(1;3))"` prints a pointer — **127.7** |
| trim | `x.trim()` | ok | ok | ok | yes (direct) | `"\(x.trim())"` and `let y=x.trim(); "\(y)"` print a pointer — **127.7** |
| concat | `x.concat("b")` | ok | ok | ok | yes (direct) | `"\(x.concat("b"))"` prints a pointer — **127.7** |
| concat | `a.concat("A";"B")` (surplus arg) | ok | ok | ok | **WRONG** | **127.1** — prints `xxA`; user-fn surplus args are correctly rejected (E4026) |
| split | `x.split(",")` | ok | ok | ok | yes (direct) | `w.len`, `io.println(w.get(1))`, and the chained `io.println("a,b,c".split(",").get(1))` are correct; **`"\(x.split(",").get(1))"` prints a pointer** (**NEW**, 127.7 class — the card's chain idiom is unsafe under interpolation) |

## Interpolation-built strings stored via `append` (127.10)

| pattern | --check | build | runs | correct | notes |
|---|---|---|---|---|---|
| `let line="\(i): item"; items=items.append(line)` then `io.println(items.get(j))` | ok | ok | ok | yes | strings are NOT dangling |
| `items=items.append("\(i): item")` then `io.println(items.get(j))` | ok | ok | ok | yes | |
| same, read back as `"\(items.get(j))"` | ok | ok | ok | **WRONG** | prints three distinct addresses |
| `s.concat`-built strings, read back as `"\(items.get(j))"` | ok | ok | ok | **WRONG** | the card's "concat is stable" workaround does NOT help interpolated reads |
| plain literal `items=items.append("x")`, read back as `"\(items.get(j))"` | ok | ok | ok | **WRONG** | |
| `items=items+@("\(i): item")`, read back as `"\(items.get(j))"` | ok | ok | ok | yes | only `+@()` marks the `mut.@()` array as `@str` |
| D-FIO-0014v230 shape (`labels=labels.append("\(i+1)")` → `"\(labels.get(i)): …"`) | ok | ok | ok | **WRONG** | reproduced; see disposition doc. The `s.concat` control passes only because it never interpolates the element |

**Reclassification:** 127.10 is not a lifetime bug. Every str appended to a
`mut.@()` array via `.append`/`.push` is readable directly; only the
*interpolated* read prints an address, because the array is never inferred to
be `@str` (see `docs/architecture/127-disposition-131.md`).

## Symbols ABSENT from the compiler entirely

No dispatch entry in `src/llvm.c` and no runtime glue in `src/stdlib/`:
**`fold` `each` `all` `any` `count` `sum` `min` `max` `first` `last` `values`**
(both call styles). Module-style (`std.array`) `map filter reduce sort find
indexof contains slice` are also absent (only the instance/UFCS dispatch
exists). Method-style `pop`, `fields`, `join`, `upper`, `lower`, `ends`,
`replace` have runtime glue but no dispatch entry (link error).

## Safe for the canonical catalogue today

- Arrays: `xs.map(&f)`, `xs.filter(&p)`, `xs.reduce(init;&f)`, `xs.sort(&cmp)`,
  `xs=xs.append(v)`, `xs=xs.push(v)`, `xs=xs+@(v)`, `xs=xs.set(i;v)`,
  `xs.get(i)`, `xs.len`. Element interpolation of a `mut.@()` string array is
  only safe when it was built with `xs=xs+@(...)` (until 127.10 lands).
- Maps: `m.get(k)`, `m=m.set(k;v)`, `m.keys()` (call form, direct reads).
  Avoid `m.keys`, `m.values`, `m.len`.
- Strings: use the `s.` module form for everything: `s.len s.upper s.lower
  s.replace s.trim s.concat s.split s.join(sep;arr) s.startswith s.endswith
  s.contains s.indexof s.find s.slice s.fromint`; `s.fields` for direct reads
  only. Method-style `x.starts x.contains x.indexof x.find` are correct;
  `x.trim x.concat x.slice x.split` are correct only when the result is not
  interpolated.
- Never in the catalogue until fixed: `x.len` (str), `x.upper/lower/ends/
  replace/fields/join`, `a.contains/find/indexof/slice` on arrays, `a.pop()`,
  `m.keys`/`m.len`, `arr.N` constant index, any of the ABSENT combinators.
