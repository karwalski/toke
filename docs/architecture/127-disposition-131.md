# Epic 127 disposition (Story 131.2)

Story 131.2 — decide, for each of 127.1–127.10, whether the defect is
**fix-now** (the fix lands as its 127.x story before 131.11 / syntax card v2)
or **workaround-is-canonical** (the card's workaround becomes the catalogue's
canonical form and the fix is not on the 131 critical path).

Evidence: `patterns/probe/` (one program per probe, `run_probes.sh`,
`probe_results.json`) and `docs/reference/combinator-status.md`.

**Pinned compiler:** `tkc --version` = `toke 2.8.0`;
`git rev-parse HEAD` = `439068b40acef1470bb43b3a6ff96c248a80a423`.
`tkc` is a symlink to `toke` built 2026-07-13 12:06; the newest commit touching
`src/` is `386cf3d` (2026-07-13), so the binary reflects HEAD's compiler
source. Line numbers below refer to that tree. (HEAD advanced to `67db04a`
while the probes ran — `git diff --stat 439068b..67db04a -- src/` is empty,
so the pin is unaffected; `probe_results.json` records the SHA at run time.)

> **Most of the defects analysed below have since been fixed (136.53,
> 2026-09-22).** This remains an accurate record *of its pin* (`439068b`), but
> do not read it as current status. On `toke 2.8.0` / binary sha256
> `a620f201b58c…` / tree `445f5199`, 101 of 130 probes are correct, and
> **127.4, 127.6, 127.7, 127.8, 127.9 and 127.10 no longer reproduce** — 127.4
> in particular was fixed exactly as the two-line change proposed below, so
> `a.fold(0;&addf)` builds and returns 6. Still open: **127.5** (`s.join` with
> swapped arguments segfaults with no check-time diagnostic) and **127.1**
> (surplus arguments unchecked in receiver/UFCS position; the module form and
> user functions now both give E4026). Current status lives in
> `docs/reference/combinator-status.md`.

## Summary table

| story | reproduced on 439068b | disposition | size | root cause (file:line) |
|---|---|---|---|---|
| 127.1 UFCS surplus args dropped | yes (`xxA`) — also module-style `s.concat(a;"A";"B")` | workaround-is-canonical (correct arity is the canonical form; nothing to contaminate) — fix bundled with 127.5 | M | `src/types.c:1125-1133` (no check for non-user callees); `src/llvm.c:3183-3211` (forwards every arg) |
| 127.2 bare `.push(v)` crashes | **no** — silent no-op, exit 0 in every variant | close as not-reproducible at this SHA; 127.3 (diagnostic) becomes **fix-now** | S | `src/lint.c:1223,1315` (expression-statement hook), method set at `src/llvm.c:5946-5956` |
| 127.4 `arr.fold` E9003 | yes (undefined symbol `_fold`) | workaround-is-canonical — but the canonical form is **`xs.reduce(init;&f)`** (works today), not `lp`+`append`; optional 2-line `fold` alias | S | `src/llvm.c:3128-3170` (no `fold` entry) → `src/llvm.c:3320-3330` (emits a call to user fn `fold`) |
| 127.5 swapped `s.join` SIGSEGV | yes (exit 139, check clean) | workaround-is-canonical (`s.join(sep;arr)` is the real signature) — check-time E4031 bundled with 127.1 | M | `src/types.c:1125-1133`; glue `src/stdlib/str_glue.c:194` derefs the array as a string |
| 127.6 method `upper/lower/ends/replace` E9003 | yes (+ `fields`, `join`, `pop`) | **fix-now** | S | `src/llvm.c:3128-3170` dispatch table lacks entries; glue exists (`src/stdlib/str_glue.c:41,45,278`, `tk_str_replace_w`) |
| 127.7 interpolating a method-call str prints a pointer | yes | **fix-now** | S | `src/llvm.c:5188-5250` (`expr_struct_type` tags `$str` only via `resolve_stdlib_call`, which is NULL for non-import receivers, `src/llvm.c:1520-1531`); `src/llvm.c:5489-5540` (`expr_llvm_type` same); interp falls to `tk_str_fromi64_w` at `src/llvm.c:2166-2250` |
| 127.8 `x.len` on str wrong | yes (returns 1) | **fix-now** | S | `src/llvm.c:3088-3108` (`.len()` method) and `src/llvm.c:3753-3768` (`.len` property) load `ptr[-1]` unconditionally; `src/types.c:1335` types `.len` only for array/map |
| 127.9 interpolating `s.fields()` element prints a pointer | yes | **fix-now** | S | `src/llvm.c:5225-5233` (`@str`-returning wrapper list lacks `tk_str_fields_w`, which resolves via the generic fallback `src/llvm.c:1773`) |
| 127.10 interp-built strings "dangle" via `append` | **reclassified**: not a lifetime bug; interpolated *reads* of any `.append`-built str array print addresses (concat-built and literal-built too) | **fix-now** | S | `src/llvm.c:6080-6105` (`rhs_proves_str_array` ignores `x.append(e)` / `x.push(e)`) → `mut.@()` stays `@i64` → `.get(j)` typed `i64` at `src/llvm.c:5660-5668` → interp prints the address |

Net: **fix-now = 127.3, 127.6, 127.7, 127.8, 127.9, 127.10** (all S; 127.6–127.10
touch the same three functions in `src/llvm.c` and should land as one change).
**Workaround-is-canonical = 127.1, 127.4, 127.5** (with the 127.4 canonical
form corrected to `reduce`). **127.2 = cannot reproduce; close, keep 127.3.**

This differs from the plan's default in two places: 127.2 is not fixable
because it no longer fails (its diagnostic sibling 127.3 takes its slot), and
127.4's "workaround" should be `reduce`, not a loop, because the runtime
already implements it correctly.

## Per-story detail

### 127.1 — UFCS method call silently drops surplus arguments

Repro (`patterns/probe/ufcs_extra_arg.tk`, `ufcs_extra_arg_module.tk`):

```
m=t;i=io:std.io;i=s:std.str;f=main():i64{let a="xx";io.println(a.concat("A";"B"));<0};
m=t;i=io:std.io;i=s:std.str;f=main():i64{let a="xx";io.println(s.concat(a;"A";"B"));<0};
```

Observed: `--check` clean, both print `xxA`. `user_fn_extra_arg.tk`
(`dbl(1;2)`) is correctly rejected with E4026.

Root cause: `src/types.c:1125-1133` — `NODE_CALL_EXPR` looks the callee token
up as a *user* function (`tc_lookup`); a `NODE_FIELD_EXPR` callee (any
`x.m(...)` or `alias.m(...)`) fails the lookup, the arguments are inferred for
side effects, and the call is typed `TY_UNKNOWN` with no arity or parameter
check. Codegen `src/llvm.c:3183-3211` forwards every argument; the emitted IR
is `call i64 @tk_str_concat_w(i64,i64,i64)` against
`declare i64 @tk_str_concat_w(i64, i64)` (verified with `--emit-llvm`), which
LLVM accepts and the C ABI silently truncates.

Fix: in `types.c` `NODE_CALL_EXPR`, when the callee is a field expression,
resolve the signature — import alias → `.tki` export (the cache at
`src/llvm.c:891-1067` needs lifting into a shared header), receiver type →
a method table (str/array/map) — and emit E4026 / E4031. Size M because the
`.tki` cache lives in the backend today. Shares its fix with 127.5.

Disposition: **workaround-is-canonical**. There is no "workaround form" that
could contaminate the catalogue — canonical code simply has the right arity.
The defect matters for *acceptance* (surplus-arg programs pass `--check`),
which is a harness concern, so it does not block 131.11.

Contaminates if unfixed: none in the catalogue; corpus acceptance only.

### 127.2 — bare `.push(v)` statement crashes at runtime

Repro attempts (all on 439068b, all exit 0, no crash):

```
m=t;i=io:std.io;f=main():i64{let a=mut.@(1;2);a.push(9);io.println("\(a.len)");<0};   → 2
m=t;i=io:std.io;f=main():i64{let a=mut.@(1;2);a.push(9);<0};                            → exit 0
m=t;i=io:std.io;f=main():i64{let a=mut.@();a.push(9);io.println("x");<0};               → x
m=t;i=io:std.io;f=main():i64{let a=mut.@();lp(let i=0;i<3;i=i+1){a.push(i)};io.println("\(a.len)");<0}; → 0
m=t;i=io:std.io;f=main():i64{let a=mut.@(1;2);a.push(9);a=a.push(1);io.println("\(a.len)");<0};  → 3
```

Probes `push_bare.tk`, `push_bare_empty.tk`, `set_bare.tk` agree: the bare
statement is a silent no-op (value semantics), never a crash. The 127.2 row
was filed 2026-07-13, the same day as `386cf3d` (126.8) landed; the crash was
most plausibly observed on a pre-`386cf3d` binary. `NODE_EXPR_STMT` lowering
(`src/llvm.c:6669-6671`) just evaluates the call and discards the result;
`tk_array_append_w` (`src/stdlib/collections_glue.c:71-79`) allocates a fresh
block and cannot fault on a valid receiver.

Disposition: **close as not-reproducible at the pinned SHA.** The real
catalogue risk is the silent no-op, which is 127.3: a W-level "value-semantic
result discarded" diagnostic with a `fix` field. Fix pointer: the lint
expression-statement walk at `src/lint.c:1223` / `src/lint.c:1315`, reusing
the method set already defined for linearity at `src/llvm.c:5946-5956`
(`append`, `push`, `set`). Size S. **127.3 is promoted to fix-now** because
bare `arr.set(i;v);` is the single most common LLM logic bug observed and
it passes every gate.

Contaminates if 127.3 unfixed: every mutation/accumulator family (sort swaps,
counters, builders) — wrong programs compile and run.

### 127.4 — `arr.fold(init;&fn)` fails to build (E9003)

Repro (`fold_method.tk`): `let a=@(3;1;2);io.println("\(a.fold(0;&addf))");`
→ clang: `Undefined symbols: "_fold"`. `reduce_method.tk` with the identical
call shape `a.reduce(0;&addf)` prints `6`.

Root cause: `fold` is not in the instance-method dispatch list at
`src/llvm.c:3128-3140`; the call falls through to the qualified-call path,
`a` is not an import alias, so `src/llvm.c:3320-3330` emits a call to a user
function literally named `fold`, which does not exist. The IR is well-formed;
the link fails. The runtime already implements the reduction
(`tk_arr_reduce`, `src/stdlib/collections_glue.c:186-195`) and `reduce` is
wired at `src/llvm.c:3159`.

Fix: two lines — add `fold` to the list at `src/llvm.c:3131` and
`else if (!strcmp(method_im,"fold")) fn_im="tk_arr_reduce";` at
`src/llvm.c:3159`. Size S.

Disposition: **workaround-is-canonical, with the canonical form corrected to
`xs.reduce(init;&f)`.** The card currently steers reductions to
`lp`+`append`; that is unnecessary because `reduce` compiles, runs, and is
correct today. The catalogue should use `reduce`; the `fold` alias is a
nice-to-have that can ride along with the 127.6 dispatch-table change.

Contaminates if unfixed: reduction family only, and only if the card keeps
teaching loops instead of `reduce`.

### 127.5 — swapped `s.join(arr;sep)` passes `--check`, SIGSEGVs

Repro (`join_swapped.tk`): `let p=@("a";"b");io.println(s.join(p;"-"));` →
`--check` exit 0, binary exit 139. `join_module.tk` (`s.join("-";p)`) → `a-b`.

Root cause: same checker hole as 127.1 (`src/types.c:1125-1133` — stdlib
calls have no parameter checking, although `stdlib/str.tki` declares
`str.join: ["str","[str]"]`). At runtime `tk_str_join_w(sep, arr)`
(`src/stdlib/str_glue.c:194`) walks the array block as a NUL-terminated string.

Fix: the 127.1 signature check (M); the `.tki` already carries the types.

Disposition: **workaround-is-canonical** — `s.join(sep;arr)` is not a
workaround, it is the signature. The catalogue teaches it; the check-time
E4031 is a diagnostic-quality improvement bundled with 127.1.

Contaminates if unfixed: none (the correct order is what the card teaches).

### 127.6 — method-style `upper`/`lower`/`ends`/`replace` link-fail

Repro (`upper_method.tk` etc.): `let x="ab";io.println(x.upper());` →
`Undefined symbols: "_upper"`. Same for `lower`, `ends`, `replace`, and also
for `x.fields()`, `p.join("-")`, `a.pop()` (probes `fields_method`,
`join_method`, `pop_method`). Module forms all correct.

Root cause: identical mechanism to 127.4 — the instance-method table at
`src/llvm.c:3128-3170` lists `split trim contains charat slice find starts
indexof substr concat chars sub substring eq` but not these; the call falls
through to `src/llvm.c:3320-3330` and links against a non-existent user
function. The glue exists: `tk_str_upper_w` (`src/stdlib/str_glue.c:41`),
`tk_str_lower_w` (`:45`), `tk_str_ends_w` (`:278`), `tk_str_replace_w`,
`tk_str_fields_w` (`:92`), `tk_arr_join_w` (`collections_glue.c:311`, note the
`(arr, sep)` order matches method style), `tk_array_pop_w`
(`collections_glue.c:248`).

Fix: add seven entries to the two lists (S). Must land together with 127.7,
otherwise the newly-linking results still interpolate as pointers.

Disposition: **fix-now.** The workaround (`s.upper(x)`) is fine, but the
catalogue cannot present method-style strings as a family while four
verbs of the family crash the build.

Contaminates if unfixed: the entire "method-style string" family and every
chained postfix idiom (`line.trim().upper()`).

### 127.7 — interpolating a method-call-derived str prints a pointer

Repro (`interp_trim_method.tk`, `interp_trim_let.tk`, `interp_concat_method.tk`,
`interp_slice_method.tk`, and new `interp_split_get.tk`):

```
m=t;i=io:std.io;f=main():i64{let x="  a  ";io.println("[\(x.trim())]");<0};      → [4305804192]
m=t;i=io:std.io;f=main():i64{let x="a,b,c";io.println("\(x.split(",").get(1))");<0}; → 4375649344
```

Direct `io.println(x.trim())` and `io.println(x.split(",").get(1))` are
correct; module forms interpolate correctly.

Root cause: string-ness is tracked by name tags, not types. `expr_struct_type`
(`src/llvm.c:5188-5250`) tags a call `$str` only when `resolve_stdlib_call`
returns a known wrapper; that function returns NULL unless the receiver token
is an import alias (`src/llvm.c:1520-1531`), so `x.trim()` is untagged.
`expr_llvm_type` (`src/llvm.c:5489-5540`) has the same shape and answers
`i64`. The interpolation lowering (`src/llvm.c:2166-2250`) then routes an
`i64` through `tk_str_fromi64_w`, printing the address. A `let y=x.trim()`
inherits the missing tag at `src/llvm.c:6233-6240`. Method-style `split`
results miss the `@str` tag for the same reason (only the `s.split` wrapper
name is in the list at `src/llvm.c:5225-5233`), so `.get(i)` on them is
`i64` — and `m.keys()` (`tk_map_keys_w`) has the same gap.

Fix (S): in both helpers, when the callee is a field expression whose base is
not an import, map the method name to a result tag: `trim concat slice
substr sub substring charat upper lower replace` → `$str`; `split chars
fields keys` → `@str`. About 20 lines, mirrors the existing `get`-on-`@str`
special case at `src/llvm.c:5498-5510`.

Disposition: **fix-now.** The workaround ("never interpolate a method result")
would force the catalogue to teach two different string styles depending on
whether the value is later interpolated, which is exactly the contamination
131.2 is meant to prevent.

Contaminates if unfixed: every formatting/reporting family that combines a
transformed string with `"\(...)"`, and the card's own chain idiom
`line.split(",").get(0)`.

### 127.8 — `x.len` on a str returns wrong values

Repro (`strlen_prop.tk`, `strlen_method.tk`, `strlen_lit_prop.tk`):
`let x="abcdef";io.println("\(x.len)");` → `1` (also `x.len()` → 1,
`"abcdef".len` → 1). `s.len(x)` → 6. `io.println(x.len)` segfaults
(the i64 is passed where a str is expected).

Root cause: `.len` is lowered as an inline load of the array header word
`ptr[-1]` regardless of receiver type — property form at
`src/llvm.c:3753-3768`, method form at `src/llvm.c:3088-3108`. A str is a
bare `char*` with no header, so the word before the string data is read.
The checker (`src/types.c:1335`) only types `.len` on `TY_ARRAY`/`TY_MAP`,
so nothing objects. The same unconditional load explains the **new** finding
that `m.len` on a map returns 0 (a map is a `TkMap` struct, not an array
block; no `tk_map_len` glue exists yet).

Fix (S): in both sites, consult `ptr_local_struct_type` / `expr_struct_type`
of the receiver: `$str` → `call i64 @tk_str_len_w`; `__map__` → a new
`tk_map_len_w` (glue: return `m->len`, `src/stdlib/collections_glue.c`).

Disposition: **fix-now.** `x.len` is the natural form and the card had to be
patched to forbid it; a catalogue built on `s.len(x)` for strings but `a.len`
for arrays teaches an arbitrary asymmetry.

Contaminates if unfixed: every counting/validation family over strings, and
map-size logic (`m.len`).

### 127.9 — interpolating an `s.fields()` element prints a pointer

Repro (`fields_interp.tk`, `fields_module.tk`):
`let x="a b  c";io.println("\(s.fields(x).get(0))");` → `4308261728`;
`let w=s.fields(x);"\(w.len) \(w.get(1))"` → `3 4314553232`;
`io.println(s.fields(x).get(0))` → `a`.

Root cause: `s.fields` is not in the `str.tki`-independent name table at
`src/llvm.c:1546-1580`, so it resolves via the generic fallback
(`src/llvm.c:1773`) to `tk_str_fields_w`; the list of wrappers that produce a
`@str` array (`src/llvm.c:5225-5233`) names only `tk_str_split_w`,
`tk_str_chars_w`, `tk_json_keys_w`, `tk_json_values_w`. The result is untagged,
`.get(0)` types as `i64`, and interpolation prints the address.

Fix: add `"tk_str_fields_w"` to that list (one line, S).

Disposition: **fix-now.** Trivial, and the workaround (`s.split(x;" ")`) is
semantically different (it does not collapse whitespace runs), so it is a
silent behaviour change rather than a workaround.

Contaminates if unfixed: whitespace-tokenising families (CLI/word-count/log
parsing).

### 127.10 — interpolation-built strings stored via `arr.append` "dangle"

Reproduced in the reported shape (`interp_append_dfio.tk`, the pre-repair
D-FIO-0014v230 body) — but the diagnosis in the row is wrong. Probes:

| build the element with | read back directly (`io.println(items.get(j))`) | read back interpolated (`"\(items.get(j))"`) |
|---|---|---|
| `"\(i): item"` (interpolation) | correct | addresses |
| `s.concat(s.fromint(i);": item")` | correct | addresses |
| plain literal `"x"` | correct | addresses |
| `items=items+@("\(i): item")` | correct | correct |

(`concat_append_dfio.tk` passes only because it never interpolates
`labels.get(i)` — it hands the element straight to `s.concat`, which is a
direct read.) The strings are intact — nothing dangles. What differs is whether the
`mut.@()` array is known to hold strings. `compute_str_arrays`
(`src/llvm.c:6113-6140`) tags a mut array `@str` only when
`rhs_proves_str_array` (`src/llvm.c:6080-6105`) accepts the assignment's RHS,
and that function recognises only array literals containing a `STR_LIT`
(possibly under `+`) and calls to user functions declared `@str`. An
`x.append(e)` / `x.push(e)` RHS is a `NODE_CALL_EXPR` with a field-expression
callee and is never examined. The array stays `@i64`, `.get(j)` types as
`i64` (`src/llvm.c:5660-5668`), and interpolation prints the address exactly
as in 127.7. The audit saw `s.concat`-built strings "work" because those
records read the elements directly.

Fix (S): in `rhs_proves_str_array`, accept a call whose callee is
`<ident>.append` / `.push` (and `arr.append(x;e)` module form) when
`expr_struct_type(c, arg)` is `$str` — string literals already return `$str`
at `src/llvm.c:5074`, `s.*` wrappers via the list at `:5240`, and `$str`
locals via `ptr_local_struct_type`. About 12 lines. Consider also seeding the
tag from the first `.append` of a `$str` at the assignment site
(`src/llvm.c:6242-6246` already upgrades `@i64`→`@str` when `is_str_arr`).

Disposition: **fix-now.** The card's current workaround ("build stored
strings with `s.concat` or the builder") is ineffective for the failing case
and must be removed from the card; the only working form (`xs=xs+@(...)`)
contradicts the card's preferred accumulator idiom (`xs=xs.append(v)`).

Contaminates if unfixed: every "collect lines then format" family — the most
common shape in CLI/report/FIO tasks.

## New defects found by the probes (not 127.1–127.10)

Minimal repros; not filed — the main thread files stories.

1. **Array receiver `.contains/.find/.indexof/.slice` segfaults** (check and
   build clean). `src/llvm.c:3163-3168` routes these names to `tk_str_*`
   glue for any receiver.
   `m=t;i=io:std.io;f=main():i64{let a=@(3;1;2);io.println("\(a.contains(2))");<0};` → exit 139.
2. **`m.keys` property form (the card's form) is silently wrong; `m.len` on a
   map is 0.** Only `m.keys()` works, and its elements interpolate as
   pointers (127.7 class). Root: `src/llvm.c:3111` handles only the call
   form; the property form falls into struct-field access; `.len` is the
   127.8 header load.
   `m=t;i=io:std.io;f=main():i64{let m=@("a":1;"b":2);let k=m.keys;io.println("\(m.len) \(k.len)");<0};` → `0 0` (expected `2 2`).
3. **Method-style `x.split(d).get(i)` interpolates as a pointer** — the
   card's endorsed chain idiom. 127.7 class (untagged instance result).
   `m=t;i=io:std.io;f=main():i64{let x="a,b,c";io.println("\(x.split(",").get(1))");<0};` → address.
4. **`arr.0` / `arr.1` constant index is a parse error** (E2002 "expected
   field, got '0'", `src/parser.c:693` requires an identifier after `.`); the
   card says it is allowed.
   `m=t;i=io:std.io;f=main():i64{let a=@(3;1;2);let v=a.0;io.println("\(v)");<0};`
5. **Module-style stdlib calls also drop surplus args** (127.1 extension):
   `s.concat(a;"A";"B")` → `xxA`.
6. **`io.println(<non-str>)` segfaults with a clean check** (`io.println(x.len)`,
   `io.println(s.contains(x;"b"))`): same checker blind spot as 127.1/127.5;
   `println` takes a `str`; E4031 should fire at check time.
7. **Bool renders as `1`/`0` under interpolation** (native `"\(1==1)"` too).
   Language-level, not glue; needs a decision for card v2 (document or
   render `true`/`false`).
8. Low: `std.array` module form `arr.append(a;9)` emits ill-typed IR
   (`ptr` where `i64` expected) while `push/pop/get/set/len` work; and
   `arr.join` resolves to `tk_array_join_w` but the glue is `tk_arr_join_w`.
9. `x.fields()`, `p.join(sep)`, `a.pop()` method forms link-fail like 127.6
   (glue exists; no dispatch entry) — fold into the 127.6 change.

## Recommended landing order

1. One `src/llvm.c` change for 127.6 + 127.7 + 127.8 + 127.9 + 127.10 (+ new
   items 1, 2, 3, 9 and the `fold` alias): dispatch table, result tagging,
   receiver-aware `.len`, `rhs_proves_str_array`. All S, all in the same
   three functions; re-run `patterns/probe/run_probes.sh` as the gate.
2. 127.3 lint warning (S) in `src/lint.c`.
3. 127.1 + 127.5 + new item 6: `.tki`-driven signature check in
   `src/types.c` (M) — after 131.11, not blocking it.
4. Card v2: drop the `s.concat`/builder claim (127.10), drop `arr.0`,
   switch reductions to `reduce`, change `m2.keys` to `m2.keys()`, state the
   bool rendering, and re-teach method-style strings once (1) lands.
