# Changelog — toke

All notable changes to the toke language and its reference compiler `tkc`.
The version printed by `tkc --version` is the repository-root `VERSION` file,
mirrored in `src/version.h` (`TKC_VERSION`, which `src/main.c` prints);
`python3 scripts/verify_project_facts.py --check` fails when they disagree.
Those are the only places a downstream should read it from.

This project uses semantic versioning **for the language and the compiler CLI
together**. A major bump means source that compiled under the previous major
may no longer compile.

---

## Unreleased

Vehicle: **toke 3.3.0** (owner answer R1, 2026-11-15, `docs/owner-decisions-2026-11-12.md`
"Answers recorded 2026-11-15"). **ooke 3.0.1 ships first**: it is migrated to spellings
toke 3.2.0 already accepts, so it builds on both.

### Breaking changes (with migration)

| # | Change | Story, commit | Migration |
|---|---|---|---|
| 1 | **`==` and `!=` with an array operand are E4031** (loke T-60, the other half). The emitter has no array equality: against the literal `@()` it compared the array's address with a fresh allocation, so `a==@()` was false for every empty `a` (loke's `h==@()` spawn-failure checks never fired), and between two named arrays it compared the elements' bytes as a C string, so `@(256)==@(512)` and `@(1;2)==@(1;2;3)` were true. Both now fail at `--check`, whatever the other operand is: `'==' does not compare arrays ('@(i64)' == '@(?)'): toke has no array equality; test emptiness with .len or compare the elements`. The refusal sits beside 3.2.0's struct/sum refusal (138.9, loke T-14). `<`/`>` on arrays and `==` on maps are not changed. Measured before landing (read-only, with this commit's `tkc --check`): no site in the toke tree, ooke main (`make check`, every file), toke-website or toke-test-programs (including the library solutions; the sources there that stop at an older error first hold no comparison against `@()` either). In the banked v0.4 regen set, 4 records draw it, all the shape `if(val==other){..}` on two arrays, and their banked output came from the defect: A-ARR-0033v2, A-ARR-0090v2, MIG-P2-A-A-ARR-0010v2 and MIG-P2-A-A-ARR-0027v2 go to correctness repair (127.460). | 138.20 (owner decision 2026-11-15, register 1.2 (a)) — `62806474` | `x==@()` becomes `x.len==0` and `x!=@()` becomes `x.len!=0`: the `fix` field carries exactly that rewrite when the other operand is an identifier of array type. Compare two arrays by length and then element by element in a loop; test a process handle's failure in the `$err` arm (`process.badhandle()`, 3.2.0 #6), not against `@()`. |
| 2 | **A std call whose interface declares `T!E` is the error union it is** (owner decision 127.30, register 1.1 (a) 2026-11-15). The 183 such functions across 37 `.tki` -- `str.toint`/`tofloat`/`parsefloat` (`!parseerr`), `str.slice` (`!sliceerr`), `str.frombytes`, `encoding.*decode` (`!encodingerr`), `json.*` (`!jsonerr`), `file.*` (`!fileerr`), `os.write` (`!oserr`), `args.get` (`!argserr`), `env.get` (`!enverr`), `toml.*`, `csv.*`, `db.*`, `http.param`/`header`, ... -- report failure on the error slot, but the checker typed them as nothing, so `str.toint("x")+1` checked clean and computed with the zero filler. A use of the result as a plain `T` -- an operand, a comparison, a `T` parameter, a return, a `let` (or `let ..=mut.`) read later, an array element -- is now **E4031** naming the union (`got 'i64!$parseerr'`), **with no `fix`**. Every accepted program lowers exactly as before. | 127.319 | `mt str.toint(s){$ok:v v;$err:e <your value>}`, `!` in a fallible function, or `getor` for a map; a statement call whose result is unused is unchanged |
| 3 | **An error union read as its value is rejected in more positions**, for a user `T!E`, a map lookup and a std `T!E` alike: an `if` or `lp` condition, the operand of `!`, a string interpolation (`"\(m.get(k))"` -- rejected, not printed), an argument of a std call or of a value-receiver method (`str.fromint(g())`, `xs.append(str.toint(s))`), the operand of an `as` cast (`str.toint("x") as i64` was 0: `as` converts the slot's value, it does not unwrap), each union operand of a comparison (`str.toint("x")==str.toint("y")` compared two fillers and was true; a union compared with an untyped operand was not judged either), and an assignment to an untyped `mut` binding that is not consumed as a union. Each is **E4031**, no `fix`. A `mut` binding is consumed only as an `mt` scrutinee, the operand of a postfix `!`, or the value of `<` in a function whose return type is a union: `let r=mut.0; r=s.toint(x); mt r{..}` (135.16a) is accepted, while `let z=r`, a bare `r;` or an arm value do not consume it. The other reads of a consumed binding are not judged (`docs/spec/semantics.md` 5.1 lists every position that is not). Also: the cast `fix` on a non-integer array index (the cast compiles and reads the filler) and the `<` `fix` on a discarded tail (it returns a union as `T`) are no longer offered when the value is a union; v3.2.0 offers both for a map lookup (127.466). | 127.323 | as above |
| 4 | **The receiver spelling of `str.slice` is the union its module form is.** `t.slice(a;b)`, `t.substr(a;b)` and `t.substring(a;b)` on a `str` lower to the same error-slot glue as `str.slice(t;a;b)` (`mt t.slice(2;9){..}` takes `$err`) but checked as a plain `str`, so `io.println(t.slice(2;9))` printed the null filler and rewriting the module form to the receiver form silenced E4031. They are now `str!$sliceerr`, judged in the same positions, no `fix`; `t.slice(0;9)!$e` propagates (it was E3020). An array `.slice` clamps and is unchanged. | 127.462 | as above |
| 5 | **An integer other than the literal `0` is E4031 where an opaque std handle or a runtime C string is read** (loke T-67). `process.wait(5)`, `process.stdout(7)`, a helper `f=waitfor(h:i64)` passing any integer on to `process.wait`, and `str.toupper(42)` / `str.reverse(12345)` (a `gen:"abi"` member whose glue reads a C string) checked clean and died with SIGSEGV; 3.2.0 refused a str, float or array there (138.21, 127.201) but not an integer. Newly refused forms, each **E4031 with no `fix`**: an integer literal other than `0`, an `i64` local or parameter (including one bound to `0`), arithmetic (`n+1`), and `h as i64`, passed where a std function declares an opaque record (`handle`, `vechandle`, `strbuf`, `tomlval`, `pdfdoc`, `xlsxbook`, `ziparchive`, `securebuf`) or to a user parameter declared `$handle` / `process.$handle` (same module, imported, or through a function value or closure); and an integer other than the literal `0` where a std wrapper reads a C string. The literal `0` (the null handle a `$err:e 0` arm yields; `process.wait(0)` still answers -1) and a real handle are unchanged, and so is a handle passed where an integer is declared (136.36). A value forged out of the checker's sight (`n as $handle`, a value through C) is the runtime half, 138.36. Measured before landing (batch Z217, this checker on `b518b41c`): no source file in the toke tree (695 sources; 510/510 doc programs, examples, stdlib-tk), ooke `main` (`make check-toke-candidate` passes) or the website (`make check-toke` passes); toke-test-programs: 1 of 3,573 files (`results/solutions/crypto-blockchain/CRY-017/solution.tk`, vec handles carried in `$i64` parameters); corpus: 0 of the 23,382 regen records and 0 of the 1,583 library records. In the toke tree, conformance script C005 carried a toml document in a `cfg:i64` helper parameter and was migrated to `cfg:$tomlval`. loke's helpers that carry a handle in an `i64` break, as decided: `src/core/inference/sidecar.tk` (`process.kill`/`wait` on an `i64`) and `packages/shared/src/config.tk` (`toml.section` on a `doc:i64`). | 138.35 (owner answer T-67, 2026-11-15) | Declare the parameter or field that holds a handle with its type: `h:$handle` (or `process.$handle`, `$vechandle`, `$strbuf`, `$tomlval`), valid on 3.2.0 too (138.32, C1906); keep `0` as the null handle; pass a `str` where a string is read. |
| 6 | **A str slice on a record field or an array element, and the module form `str.substr`, are the union `str.slice` is** (extends row 4). `p.name.slice(2;9)` on a `name:$str` field, `p.sub.tag.slice(..)`, `xs.get(0).slice(..)` / `.substr` / `.substring` on an `@($str)` element, and `str.substr(s;a;b)` (whose std.tki record is an untyped `"gen":"abi"` placeholder; it lowers to `tk_str_substr_w`, which is `tk_str_slice_w`) checked clean and printed the null filler on a failure. They are now `str!$sliceerr`, judged in the same positions, **E4031 with no `fix`**; codegen does not move (IR identical on the 61 toke-tree, ooke and website programs that slice; the element receiver's IR gains only a comment). The interface record is not changed. Measured before landing (`--check`, this checker against the same tree without the rule): no change in the toke tree (695 sources; doc programs 513/513 build), ooke `main` (31) or the website (25); toke-test-programs: 38 of 3,573 files gain 119 diagnostics, 25 of them newly refused, every one the module form `s.substr(..)` (`results/solutions/`). | 127.475 | as row 4: `mt str.substr(s;a;b){$ok:v v;$err:e <your value>}`, or `!` in a fallible function |
| 7 | **`str.substr`'s interface record states the signature `str.slice` has** (`str;u64;u64 -> str!$sliceerr`); it was an untyped `"gen":"abi"` placeholder (`i64;i64;i64 -> i64`) although it is `str.slice`'s glue, and it is now in `docs/stdlib/str.md` and `stdlib-signatures.md`. Two effects. A **str or `f64` where an index is read** (`str.substr(s;"x";2)`, `str.substr(s;1.5;2)`) checked clean and passed the string's address or the float's bits as the index; it is now **E4031** naming the declaration (`argument 2 of 'str.substr' is declared 'u64' (str.tki), got 'str'`), **no `fix`**. And `mt str.substr(..)` was lowered against the return value instead of the error slot, so the `$err` binding was 0 and `e.msg` (or a `!$e` propagation's payload) died with RT005; it now binds the `$sliceerr` (`slice index out of range`), as `str.slice` does. 127.475's by-name rule is removed, since the record carries the union. Measured with `--check` against the base on every `.tk` file that mentions `substr` (60: this tree, ooke `main`, the website, toke-test-programs): no diagnostic changes; the 3 of them that build emit identical IR. | 127.493 | pass a `str` and two integer indices; `mt str.substr(s;a;b){$ok:v v;$err:e e.msg}` now works as written |
| 8 | **A bool where an opaque std handle is declared is E4031, as an integer is** (extends row 5). `process.wait(true)` and a `bool` passed to a user parameter declared `$handle` checked clean, and the runtime read handle 1. `false` is refused too: the null handle is spelled `0`. **No `fix`**. Measured before landing (`--check` with this checker against the same tree without the rule, every `.tk` file): no diagnostic changes in the toke tree (695), ooke `main` (31; `make check-toke-candidate` passes), the website (25; `make check-toke` passes) or toke-test-programs (3,573). The corpus was not measured by this batch. | 138.55 | pass the handle, or the literal `0` for the null handle |
| 9 | **The element type of an array argument to a std call is judged.** `csv.serialize(@("a";"b"))` and `http.postheaders(url;body;@("a";"b"))` checked clean where the interface declares `[[str]]`, and the glue read each string as a row array (an out-of-bounds read); a user call already refused the same argument. Elements are compared by the representation class a scalar argument is judged by (a number, a str, a float, an array, a map, a record), so element width is still not judged and an integer stays accepted where a record is declared. **E4031 with no `fix`** (`argument 1 of 'csv.serialize' is declared '@(@(str))' (csv.tki), got '@(str)'`). Measured before landing (`--check` with this checker against the same tree without the rule, every `.tk` file): no diagnostic changes in the toke tree (695), ooke `main` (31; `make check-toke-candidate` passes), the website (25; `make check-toke` passes) or toke-test-programs (3,573). The corpus was not measured by this batch. | 127.497 | pass the declared shape: `@(@("a";"b"))` for one row or one header pair |
| 10 | **A field read on an unimported module's record is E4034 at `--check` in every shape the compile refuses** (extends 138.40). An element of an array or map a user-module call returns (`rs.get(0).text`, in place, bound, or in a loop), the `$ok`/`$err` payload of its `T!E`, a sum type's variant payload bound by `mt`, and a field of an imported record whose type is such a record (`wr.r.text`) passed `--check` and failed the compile with E4034; with another struct in scope declaring a field of the same name, codegen took that struct's offset and the read built and printed the wrong slot. **E4034, no `fix`**. As a consequence `--lint` no longer reports an import that only such a read needs as unused, so `--lint --fix` no longer deletes it into an E4034 compile. Measured before landing (`--check` with this checker against the same tree without the rule, every `.tk` file): no diagnostic changes in the toke tree (695), ooke `main` (31; `make check-toke-candidate` passes), the website (25; `make check-toke` passes) or toke-test-programs (3,573). The corpus was not measured by this batch. | 138.58 | import the module that declares the record (`i=p:proto;`) |

| 8 | **An integer other than the literal `0` is E4031 where a std call declares a record with fields** (the T-67 class beyond opaque handles; extends row 5). `http.clientget(5;..)`, `tls.read(5)`, `tls.protocol(5)`, `svg.render(5)`, `image.fliph(5)`, `vecstore.flush(5)`, `vecstore.count(5)` checked clean and died with SIGSEGV: the runtime reads a std record through its address. Row 5 judged only records with no fields; every std record slot is now judged the same way -- an integer literal other than `0`, an `i64` local or parameter, arithmetic, `h as i64` -- **E4031 with no `fix`** (`... -- the runtime reads a std record through its address, so only the record itself or the literal 0 may be passed`). The literal `0` (the null record) and a value of the record's own type are unchanged. Not covered: std.db's `row.` calls (`row.str(5;"k")`), which the checker does not judge at all (138.62). Measured before landing (batch Z237, this checker on `ee27591c`, `--check`): no source in the toke tree (695), ooke `main` (32; `make check-toke-candidate` passes), the website (25; `make check-toke` passes), toke-test-programs (3,573) or the corpus library (1,583) draws it; the corpus regen set (23,382) none either. Three conformance scripts carried a record in an `i64` callback or helper parameter and were migrated: C022 and C575 (`f=onconn(conn:i64)` for `tls.listen` -> `conn:$tlsconn`) and C1830 case c7 (`c:i64` passed to `sse.emitdata` -> `c:$ssectx`); C295 case 10, which pinned the old exemption, and C707 case 7 (`let p:i64=json.parse(..); json.has(p;..)`), now assert the refusal; `docs/stdlib/tls.md` documented the `conn:i64` callback and now writes `conn:$tlsconn`. | 138.46 | declare the parameter that holds the record with its type (`conn:$tlsconn`, `c:$httpclient`, `ctx:$ssectx`, `img:$imgbuf`); keep `0` as the null record |
| 11 | **std.db's `row.` calls are argument-checked** (extends row 8). `row.str`, `row.i64`, `row.u64`, `row.f64` and `row.bool` name no import (`row` is a sub-namespace std.db's interface exports), so the checker judged none of their arguments: `row.str(5;"k")` checked clean and died with SIGSEGV, and a number in the column-name slot or a str in the row slot passed too. They are now judged against std.db's records as any std call is: an integer other than `0` where `$dbrow` is declared, and a value of another kind in either slot, is **E4031 with no `fix`**. Measured before landing (`--check`, this checker against the same tree without it): every source that calls `row.*` -- the toke tree (`test/e2e/e2e_db_open.tk`), ooke `main` (`src/store.tk`, `test/store/test_store.tk`) and the corpus's doc examples (6 records) -- draws the same diagnostics as before; toke-test-programs and the website have none. | 138.62 | pass the `$dbrow` (or a `$dbrow{cols:..}` literal) and a `str` column name; keep `0` as the null row |
| 12 | **A field read of an opaque std type is E4034 at `--check`** (138.65). A type its `.tki` marks `"opaque": true` -- `$csvreader`, `$csvwriter`, `$dataframe`, `$jsonstream`, `$router`, `$handler`, `$middleware`, `$modelhandle` -- declares fields that are no layout toke can read, and the flag was never read: `w.sep` on `csv.writer(44)` printed 0, `d.ncols` on a frame printed its column array's address (`d.nrows` the column count), `r.id` on a router its first route pointer, and `r.pos` on a `$csvreader` (a registry token since 138.63) died with SIGSEGV. The read is now refused, with no `fix`. `$dashboard`, `$htmldoc`, `$llmstream`, `$canvasop` and `$dtreemodel`, whose fields are their checked C layout, lose the flag and read as before; `make check-tki-layout` fails a `tki-record` that is marked opaque. Record literals of opaque types still build. | 138.65 | Use the module's functions: `df.shape(d).rows`/`.cols` for a frame's counts; the csv, json stream, router and infer handles expose nothing else. Measured over the toke tree, ooke main, the website and toke-test-programs: no source reads one of these fields. |
| 13 | **E5001 is emitted: inside an `{arena}` block, a heap value assigned to a module-level `mut` is rejected** (127.526). The check walked the block's children, and the block's one child is its statement list, so no input ever drew E5001 (`m=test;let g=mut.0;f=bad():i64{{arena g=1}<g};` checked clean). Now, at any depth of the block (inside a nested `if` or `lp` too), assigning a `str`, array, map, struct or sum, closure, or an error union whose value is one of these to a module-level `mut` binding is **E5001, with no `fix`**; `got` is the value's type. Not rejected: a scalar (the rule memory-model §2.4 stated before this change also rejected `g=1`, which allocates nothing and cannot escape; §2.4 now states the narrower rule, the module-level-mut part of route E1 of the `{arena}` ADR, ADR-0016, still Proposed), a store to a binding of the enclosing function (the ADR's open question), a value whose type the checker leaves unresolved, a local that shadows the module name. `{arena}` still releases nothing. Measured before landing (`--check`, this checker against its base `3a8f0ae6`): no program newly refused in the toke tree (695 sources), ooke `main` (32), the website (26) or toke-test-programs (3,573); none of them assigns inside `{arena}`. Tests: D770-D773. | 127.526 | Bind the value inside the block, or move the store to the module-level binding out of the block. |
| 14 | **What `task.spawn(s;f)` and `sc{ spawn f }` run must be a function value taking no parameters: E4081** (127.549). `task.tki` declares the parameter `i64`, so any integer checked clean and the pool thread called it as code; a function with parameters read garbage for them; and `task.spawn(s;w())` / `spawn w()` called `w` at once, on the spawning thread, and spawned its result. Each is now E4081 at `--check` (type_check, `expected` `():T`, `got` the operand's type), with no `fix`. `&w`, a bare function name, a closure and a call returning a `():T` closure are unchanged. Measured: no site in the toke tree (outside the parse-only grammar fragments and the python-view fixture, which are not type-checked), ooke main or the website calls task.spawn or `spawn`. | 127.549 | write `&w` (or a closure) for the function the task should run; to run a function with arguments, spawn a closure that calls it |

Measured before the flip with this checker against main `685aa852` (rows 127.319, 127.323
and 127.462, batch Z211): 92 of 695 toke-tree sources and 15 of 509 documentation programs;
1,625 of 3,573 toke-test-programs files; ooke `main` `src/run.tk` (2 sites) and
`src/store.tk` (3 sites); the website's `main.tk` (3 sites); and in the corpus 2,479 of the
23,382 regen records and 1,244 of the 1,583 library records, all banked-accepted (repaired by
agents, owner answer R2; never by a mechanical filler). Row 4 alone accounts for 1,243 of
those regen records and 4 of the tree sources. The toke tree was migrated with the internal
`scripts/migrate_std_errunion.py`, which writes the old behaviour out as an explicit
`mt ..{$ok:v v;$err:e <zero filler>}`; that filler is a migration device for existing
programs only, never a recommendation, a `--fix` rule or a diagnostic `fix`.

- **Breaking: a bare `lp(i=0;..)` init assigns the binding in force; only
  `lp(let i=0;..)` declares a loop variable** (127.289; owner decision
  2026-10-01 §2.18 (a); toke-spec-v0.4 §K). Before, a bare init made a fresh
  loop variable whatever was in scope: with `let i=mut.7`, the loop
  `lp(i=0;i<5;i=i+1){..}` left `i` at 7, and over an immutable `let i=7` or a
  parameter `i` it compiled with no error. Now the init is the assignment
  `i=0`: the loop leaves `i` at 5; an immutable `let` (local or module-level),
  a parameter, an `mt` arm binding, a module constant or an import alias is
  **E4070** (no `fix`: `let i=mut.` and `lp(let i=..` are both plausible and
  run differently; the step's E4070 has none either when the step assigns the
  same binding), and no binding of `i` in force is **E3011** at the init, with
  no "did you mean" `fix`. A module `mut` global is assigned. `--migrate`
  still adds no `mut.` for a bare init, so a migrated file that assigns an
  outer `let` this way reports E4070.
  `--lint` counts a bare init as a write, so `mutable-never-mutated` no longer
  fires on a binding only a bare init assigns. Measured before the change: one
  corpus regen record (`P3-MIG-P2-A-A-MTH-0092v13-fa507f0f`), one library
  record (toke-test-programs `DAT-094`: `solution.tk` and
  `solution.stripped.tk`) and one more toke-test-programs file
  (`SEC-064/toke_api_original.tk`) use a bare init. Each assigns an outer
  `mut` binding that is not read after the loop, so none checks or runs
  differently (the stripped file's E4031 and the SEC-064 file's E1002 are
  unrelated and unchanged). Nothing in the toke tree, ooke or the website uses
  one. `--check` run with this compiler and with `685aa852` over 716 toke-tree,
  32 ooke, 25 website and 3,573 toke-test-programs files and the 23,382 regen
  records reports the same diagnostics for every one. Migration: write
  `lp(let i=..` where a fresh loop variable was meant. Conformance: C1820,
  D575-D578.
- **An assignment to a module-level `let` or to an import alias is E4070**
  (127.465). The assignment check looked for a local binder and, finding
  none, knew only module constants: `let g=5;` at module level and `g=0;` in
  a function checked clean and overwrote the immutable binding, and `io=0;`
  with `i=io:std.io;` checked clean and then failed in codegen with E9003,
  reported at the function. Both are now E4070 at the assignment. The
  module-level `let` carries the local `let` fix, "change 'let g=' to 'let
  g=mut.' to make it mutable" (`let g=mut.<v>` is a valid module-level
  binding for every initialiser `let g=` takes, and `--migrate` makes the
  same rewrite), except on the step of a loop whose bare init assigns it;
  the alias has no fix. Measured with `--check`, 685aa852 against this
  compiler: 716 toke-tree, 32 ooke, 25 website and 3,573 toke-test-programs
  files, 829 doc blocks, 344 conformance YAML inputs, 605 conformance-script
  programs and the 23,382 regen records report the same diagnostics, so no
  program in them is newly refused. Conformance: C1821, D579.
- **grammar.ebnf `LoopStmt` states the clauses the parser accepts** (127.290):
  `( 'let' IDENT | IDENT ) '=' Expr ';' Expr ';' IDENT '=' Expr`, or the
  one-clause `Expr`. It read `'lp' '(' Stmt Expr ';' Stmt ')'`, which admitted
  any statement as the init and the step. Written in line, so the published
  production count is unchanged. `toke.gbnf` already matched the parser, and
  both tree-sitter grammars already had the `['let'] IDENT '=' Expr` init.
- **Variant arms on a known scalar subject are E4031 at `--check`** (138.37,
  loke T-66). `f=h(e:i64):$r{<mt e {$notfound:v ..;$permission:v ..;$io:v ..}}`
  passed `--check` and the build stopped with E9005 (2 or 3+ arms) or invalid
  IR (a `u8` subject); a one-arm form over an i64 built. A match with no
  `$ok`/`$err` arm, over a parameter or immutable `let` of a scalar type, a
  numeric literal or a call returning a scalar, whose arm names a label that
  is not a primitive, `some`/`none` or a declared type, is now E4031 at that
  label (`expected` "a sum type", `got` the scalar), with no `fix`. `--check`
  over the 683 toke-tree, ooke `main` and 3,573 toke-test-programs sources
  draws it nowhere; ooke `make check-toke-candidate` and the website's
  `make check-toke` pass. Conformance: C1920, D635; C027 (cases 2, 5), C373
  (case 1) and C400 (case 9) now meet the refusal at `--check`, and keep the
  E9005 backstop pinned through a `mut` copy, which is not judged. The
  subject's binder is found lexically, so a sum-typed closure parameter, body
  `let` or loop-init `let` that shadows a scalar parameter of the same name is
  not judged by the parameter's annotation (C1920 cases 5-7).
- **`$ok`/`$err` over a shadow of a scalar parameter no longer draws a false
  E4031** (138.56). 127.202's check read the function's parameter list before
  the bindings in force, so `f=h(e:i64):i64{let e=str.toint("7"); <mt e
  {$ok:v v;$err:x 0}}` was refused although it builds and returns 7. The
  parameter's annotation now counts only when that parameter is the binder in
  force at the subject. Conformance: C1920 case 6.
- **A closure parameter annotated with a scalar is judged as a match subject**
  (138.57). `f=h(e:$fe):i64{let g=fn(e:i64):i64{<mt e {$a:v 1;$b:v 2}};
  <g(3)}` passed `--check`, built, and the binary died with SIGSEGV (3.2.0
  and later); `fn(e:i64)` matched with `$ok`/`$err` built and took `$err` for
  0 unless an outer function happened to have a same-named scalar parameter.
  A parameter that is the binder in force at the subject, whether of the
  function or of a closure inside it, is now judged by its own annotation:
  variant arms are E4031 `expected` "a sum type" and `$ok`/`$err` arms are
  E4031 `expected` "T!E", both with no `fix`. A variant match over a scalar
  closure parameter with no outer parameter, which stopped at build with
  E9005, is now refused at `--check`. Conformance: C1920 case 8; D682 and
  D683 pin the absent `fix`. Measured (Z230): `--check` over the toke tree,
  ooke `main`, the website and toke-test-programs (4,346 sources) refuses no
  source that `465ebeb6` (the commit before the change) accepted for this
  reason.
- **An `mt` arm binding of a sum-variant payload has the payload's type**
  (127.484, silent wrong value). With `t=$shape{$circle:f64;$sq:f64}`,
  `mt s{$circle:r r*r;$sq:n n}` over `$circle:2.0` gave 4616189618054758400
  (the double's bits) and `f=area(s:$shape):f64{<mt s{$circle:r r*r;..}}`
  returned 4.61619e+18, on 3.2.0 and later: the match's result slot was
  sized with the binding unbound, as an i64. The checker typed every arm
  binding other than `$ok`'s as unknown, which also lost a record or array
  payload's layout: `q.w` failed E4034 when two records declare `w`, an
  `@f64` payload's elements were multiplied as integers (RT002),
  `q.get(1).len()` on an `@str` payload read the array's length, a bool
  payload printed 1, and a sum payload matched in the arm stopped at build
  with E9005. The binding of an arm naming a variant of a declared sum now
  has that variant's payload type, for the checker and the lowering alike.
  Because it is typed, an arm body can now disagree: an f64-payload arm
  beside an i64-payload arm is E4011, and the match's value assigned to a
  `str` is E4031, both with no `fix`. Measured (Z230): `--check` over the
  toke tree, ooke `main`, the website and toke-test-programs (4,346 sources)
  refuses nothing new. Conformance: C2020, D680, D681.
- **A user name can no longer collide with an IR temporary or block label**
  (138.38, loke T-65). `f=g(t0:i64):i64{<t0+1}` passed `--check` and clang
  refused the IR ("multiple definition of local value named 't0'"); a local
  or parameter named like a block label (`svarm3`) gave "'%svarm3' is not a
  basic block". A user name that ends in a digit (or, under `--legacy`,
  starts with `_`) is now always given a dotted LLVM name, for parameters,
  closure parameters, closure captures and locals alike; other names keep
  their spelling in the IR. The pointer-tag and local-type registries are
  keyed by that LLVM name, so a lookup made with the source spelling now
  retries under the alias in force when the spelling misses: without it,
  `let m1=mut.@("":0);m1=m1.set("a";40)` lowered as an array in-place set
  and trapped with RT003, which a local named `t1` already did on 3.2.0
  (114.15's aliasing), and a captured str local got an i64 slot.
  Conformance: C1921 (cases 7-13, including a plain-vs-digit-suffixed IR
  differential).
- **E4070's `let` fix is worded from the declaration** (127.468). It was the
  fixed text "change 'let g=' to 'let g=mut.'", which does not occur in
  `let g:i64=5`; it is now "change 'let g:i64=' to 'let g:i64=mut.' to make it
  mutable", taken from the source, and absent when that spelling occurs more
  than once in the file or a module-level name is declared more than once.
  An assignment in the body of a loop whose bare init assigns the same
  binding now has no fix, as the init and step already had none.
  Conformance: D636-D638.
- **`+` over two different non-numeric operands names each side** (127.474):
  `str.slice(t;0;1)+"z"` said "both sides are 'str!$sliceerr'"; it now says
  "the left side is 'str!$sliceerr' and the right side is 'str'". Conformance:
  D639.
- The checker's interpolation anchor (`Ctx.interp_host`) is initialised in
  `type_check` (127.476); it was read uninitialised.
- **Every integer-to-integer `as` cast builds, and gives C's value** (127.506;
  corpus pointer 131.131). `a as i32` on a `u32`, `a as u32` on an `i32` (any
  same-width signedness change below 64 bits) and a widening cast into a
  sub-64-bit target (`a as i32` on an `i16`) checked clean and failed the build
  with E9003 (invalid LLVM IR: the cast left the value at the target's width
  where its consumer expected the i64 the narrowing path produces). A signed
  source widened into an unsigned target is now sign-extended, as C, Rust and Go
  do: `b as u16` on an `i8` of -1 is 65535 (it was 255, while `b as i64 as u16`
  was already 65535). Conformance: C2040 (all 64 pairs over 20 values, against
  C's output).
- **`.map` / `.filter` / `.reduce` / `.fold` / `.sort` call any function value
  correctly, on every target** (127.548, a silent miscompile on x86-64). The
  runtime called the function it was handed as `int64_t f(int64_t)`. That is
  right only for an `i64 (i64)` toke function. A `bool` predicate returns an
  i1, and x86-64 defines only bit 0 of the return register, so on linux/amd64
  `@(0-3;7;0;9).filter(&pos)` kept the -3 (CI run 45433635073,
  `diff-codegen` on test_127_44). arm64 happened to clear the register, so the
  same program passed there. Three related failures had the same cause:
  `.map` of an `f64` function returned its input unchanged on every target
  (the value travelled in an integer register, the function used a float
  one), and a closure or a closure-bound local passed to any of these
  combinators was called as code and died with SIGBUS. The compiler now hands
  the runtime a pair: a C-ABI adapter emitted once per signature
  (`@__tk_fvc_<key>`), and the function value. The adapter converts each
  array slot to the parameter's type, calls the function or closure as a toke
  call does, and widens the result to a defined i64 (zext of a `bool`, the
  bits of an `f64`, a narrow integer by its own signedness). Predicates are
  not masked: an `i64` predicate returning 2 still keeps its element. The
  glue signatures change (`tk_arr_map/filter/sort(arr, adapter, value)`,
  `tk_arr_reduce(arr, init, adapter, value)`). No toke source changes, and
  calls through a function value in toke (`let g=&f; g(x)`) are unchanged.
  A sort comparator that itself sorts no longer replaces the outer sort's
  comparator. Tests: C2295–C2299, `test/standalone/test_127_548.tk`.
- **`xs.map(f)` is an array of `f`'s return type** (127.550). After 127.548
  `@(1.5;2.5).map(&dbl)` stored the right doubles, but the result was still
  typed `@i64`: `j.enc(..)` printed `[4613937818241073152,...]` and
  `"\(ds.get(0))"` printed the bit pattern. A `str` map encoded its string
  pointers as numbers, a record map encoded addresses, and a `bool` map
  encoded and interpolated `0`/`1`. The checker now types `.map` on a known
  array as `@(R)`, where `R` is the declared return of the function value:
  `&f`, a closure with a declared return, a `let g=&f` value or a function
  parameter. The emitter's fallback tag, used for a receiver the checker does
  not type (a record field, a reassigned `mut`), now finds the function by its
  module-qualified symbol. Before, it looked up the bare name, never found it,
  and answered `@i64`. Two consequences can change a program. A `bool` map
  now encodes as `[false,true]` (127.548's tests recorded `[0,1]`). A
  mismatched use of the result is now E4031, for example
  `let a:@i64=fs.map(&dbl)` or `let s:str=fs.map(&dbl).get(0)`. A closure with
  no declared return and a fallible function are left untyped, as before.
  Tests: C2300–C2304, `test/standalone/test_127_550.tk`.
- **`task.spawn` and the `spawn` keyword run any function value correctly,
  on every target** (127.549, the 127.548 defect in std.task). task.c called
  the value it was handed as `int64_t f(void)`: a `bool` task's
  `task.result` read an i1 register whose upper bits x86-64 leaves
  undefined, an `f64` task's result sat in a float register C never read
  (0 on arm64 too), and a closure was called as code (SIGBUS; a closure
  literal did not even build). Compiled code now calls
  `tk_task_spawn_fv(scope, adapter, value)` with the 127.548 C-ABI adapter
  (no arguments), and `task.result` is the result widened to `i64`: 0/1 for
  a `bool`, the bits of an `f64` (`math.frombits`), a narrow integer by its
  signedness. A closure task holds a reference to its environment from the
  spawn until the function has returned, or until the task is refused or
  skipped by `task.cancel` (task.c releases it; the count is atomic), so it
  outlives the spawning function safely. The completion flag is now
  published with release/acquire ordering, and a task whose queue entry
  cannot be allocated finishes with -1 instead of hanging `awaitall`.
  `tk_task_spawn(scope, fn)` remains as the C entry for a C function. See
  Breaking change 14 for E4081. `sc` without `i=task:std.task;` still fails
  to link (127.553). Tests: C2305–C2309, `test/standalone/test_127_549.tk`.

### Security fixes

- **`yaml.fromjson` always returns** (127.542, a denial of service). A 35-byte
  malformed flow mapping found by the security nightly's libFuzzer run
  (`crash-yaml-oom-bcd1502b`: libFuzzer died at a 2 GiB malloc; the standalone
  replay never returned) held the converter in a loop: the sequence loop
  appended `"\n- "` for an element that consumed no input -- a byte that is no
  JSON value (`[1,2@]`) or any element nested past the 256-level depth cap
  (`[` x 400) -- and then saw the same byte again, forever, growing its output
  without bound. Each loop now stops at an element that made no progress
  (other than an empty element on a comma, which consumes the comma) and the
  whole conversion stops there, so the work is linear in the input and the
  output bounded by it. Every input that returned before converts to exactly
  the same text, including elided elements such as `[1,,2]`. Test: C2285; the inputs are kept as
  `test/fuzz/corpus-adversarial/yaml_flow_seq_no_progress_127542.json` and
  `yaml_deep_seq_past_cap_127542.json`, which `make fuzz-smoke` replays.
- **`yaml.fromjson` no longer reads past the end of a document that stops
  inside `true`, `false` or `null`** (127.544, a heap over-read found auditing
  the converter for 127.542). The literals were skipped as a fixed 4 or 5
  bytes, so `[t`, `{"a":n` or `[1,f` left the cursor past the string's
  terminator and every later read was out of bounds (heap-buffer-overflow
  under ASan; without it the converter walked whatever memory followed). A
  literal is now skipped over at most its length and never past the end; a
  complete literal converts as before. Test: C2286; the input is kept as
  `test/fuzz/corpus-adversarial/yaml_truncated_literal_127544.json`.
- **`tkc` no longer reads freed memory when it warns that `pkg.lock` is stale**
  (127.449). A build or `--check` of a program with a `pkg.*` import whose
  `pkg.lock` no longer matches `pkg.toml` formatted W9010's dependency name from
  the parsed manifest after freeing it: a heap use-after-free in the compiler,
  found by the ASAN+UBSAN CI job (C1162). An ordinary build usually printed the
  right name because the block had not been reused yet; with freed memory
  filled, the message carried the filler. W9010 is now emitted before the
  manifest is freed. No change to which programs are accepted. Test: C1960.
- **`image.decode` / `image.tiffdecode` refuse a TIFF whose strips cannot back
  its declared page** (127.488, a decompression-bomb guard). Every TIFF
  decompressor zero-fills what a strip does not cover, so a 1,686-byte file
  found by libFuzzer, declaring a 2600 x 65435 page over one 1,590-byte
  uncompressed strip, decoded to a 170 MB page of zeros (3.2 s CPU and ~1 GB
  peak RSS in the fuzz harness, which also encodes and flips the result). Each
  codec now has a ceiling on what a strip's bytes can decode to (none 1:1,
  PackBits 64:1, LZW 4096 bytes per 9-bit code, Deflate 1032:1, CCITT one row
  per bit); when the declared bytes beyond those ceilings total more than
  1 MiB, the page is refused before anything is allocated with
  `image_tiff_decode: the strips are too short for the declared page
  (decompression-bomb guard: ...)`. A well-formed file is never refused, and a
  short strip within the 1 MiB slack still zero-fills as before. Tests: C2030;
  the input is kept as `test/fuzz/corpus-adversarial/image_tiff_short_strip_127488.tif`.
- **std.cache no longer writes past its key buffer on a long URI** (127.516, a
  heap overflow). `cache_build_key` built `METHOD|URI|vary...` in a fixed
  1024-byte block and kept `snprintf`'s untruncated length as its offset, so for
  a URI of about 1020 bytes or more a matched Vary header was appended from past
  the block with a wrapped bound (`http_cache_get` or `http_cache_put` with a
  1101-byte URI and `Vary: Accept-Encoding`: heap-buffer-overflow under ASan).
  Without Vary the key was cut at 1023 bytes, so two URIs sharing their first
  ~1019 bytes shared one entry for get, put and purge. The key, and
  `http_cache_stale_if_error`'s `METHOD|URI` (a 1024-byte stack buffer), are
  now allocated to their full length; matching stays exact (127.514). Nothing
  in the tree calls `http_cache_*`. Test: C2160.
- **`toon.fromjson` terminates on every input** (127.543, a denial of
  service). After each array element the converter skips what is not the next
  object, and a stray `}` at array level made that skip consume nothing, so
  `[{"a":1}}` never returned (found by the security nightly's libFuzzer run as
  a 1201 s timeout on a 50-byte input). Every pass of the skip now consumes at
  least one byte: the stray byte is dropped like any other non-object element,
  so `[{"a":1}},{"a":2}]` converts to the two rows `[{"a":1},{"a":2}]` does, and
  the conversion is linear in the input. Well-formed JSON converts unchanged.
  Test: C2290; the input is kept as
  `test/fuzz/corpus-adversarial/toon_from_json_stray_close_127543.json`.

### Build and CI gates

- **`make fuzz-smoke` fails on an input that never returns, instead of hanging**
  (127.545). Each replayed input now has `FUZZ_REPLAY_TIMEOUT` seconds (default 25,
  `0` turns it off): the standalone driver arms an alarm per input and exits 70 with
  `fuzz-replay: TIMEOUT after N s on <path>`, and libFuzzer gets `-timeout=`. Before,
  the 127.542 and 127.543 inputs made the standalone replay run forever and the
  libFuzzer replay take 20 minutes per input. C2287 checks it with a harness that
  spins on one input.
- **`make check-tk-parse`: every committed `.tk` outside `test/` parses** (131.142).
  131.52 found five that did not: the GitHub Linguist samples
  (`linguist-submission/samples/toke/`, underscored identifiers and `=` for `==`) and
  a stale `results/` copy of toke-test-programs' MED-009 solution. All are fixed (the
  samples by `tkc --migrate` and the current std API; the map sample's size line reads
  4, since std has no map delete). The gate runs `tkc --dump-ast` on every
  `git ls-files '*.tk'` path outside `test/`, requires the Linguist samples to
  `--check` with no output at all, and runs a `--selftest` negative control first. It
  is in `make ci` and the CI workflow.
- **`docs/spec/toke.lark`, and `make check-grammar-artefacts`** (131.52). The Lark
  form of `toke.gbnf`, generated rule for rule by `scripts/gbnf_to_lark.py --write`
  (one character per terminal, so llguidance's greedy lexer cannot change the
  language). The new gate, in `make ci`, fails when `toke.lark` is not a fresh
  generation of `toke.gbnf`, and, for each of Lark, XGrammar and llguidance that
  is importable (`GRAMMAR_PY=<venv>/bin/python`, `bench/mask_cost/requirements.txt`),
  when the engine's verdict differs from the reference recogniser's on
  `test/gbnf/fragments.json`; an engine that is not installed is reported as
  SKIPPED. All three agree on every fragment checked (Lark: every 8th, `--full`
  for all). `make bench-mask-cost` measures per-token mask time and compile time
  for toke against a generated Python-subset grammar
  (`bench/mask_cost/python-subset.gbnf`, checked by `make bench-mask-cost-check`:
  accepted ⇒ CPython `ast.parse` accepts) and JSON, under one vocabulary
  (cl100k_base); results: `docs/about/reviews/mask-cost-2026-11-23.md`.
- **Engine-tuned grammars: `docs/spec/toke-lexeme.lark` and `docs/spec/toke-xgr.gbnf`**
  (131.141). Both are generated from `toke.gbnf` by `scripts/gbnf_tune.py --write`.
  `toke-lexeme.lark` is for llguidance: the parser rules stay, and the lexical rules
  (identifiers, numbers, strings, comments, whitespace runs) become regex lexemes.
  Comments and string interpolation nested deeper than 8 levels are rejected, which
  the parser would accept, so this form accepts a subset of the language of
  `toke.gbnf`. `toke-xgr.gbnf` is for XGrammar and describes the same language as
  `toke.gbnf`: whitespace is inlined, and the rules that end a word token carry
  lookahead assertions. `make check-grammar-artefacts` now also fails when either file
  is stale. With the engines installed, it also fails when llguidance or XGrammar
  disagrees with the reference recogniser on any fragment of `test/gbnf/fragments.json`.
  Without them (as in CI today), the agreement checks report SKIPPED and only the
  staleness check runs. Agreement on the fragment set is evidence, not a proof of
  equivalence. The Python-subset baseline gets the same two forms, checked by
  `make bench-mask-cost-check`, and `make bench-mask-cost` measures all four. Results:
  `docs/about/reviews/mask-cost-2026-11-24.md`.
- **The tuned Python baseline `bench/mask_cost/python-subset-lexeme.lark` fits
  llguidance's default limits** (131.148). Under llguidance 1.9.1's default
  `step_max_items` (50,000) it finished 8 of the 48 mask-cost programs and stopped on
  the others with "Too many items". Each mask predicted many alternatives at every
  operand position, and every enclosing indentation level added its own blank-line
  loop and dedent continuations after a newline. The generator
  (`python_subset_gbnf.py`) now hands `lexeme_lark` rewrites that accept the same
  strings: the precedence chain is flattened, fixed-string choices (operator sets,
  string kinds, constants) are single lexemes, and blank lines plus indentation are
  one lexeme per level. It now finishes 48 of 48 at default limits (cl100k_base,
  N = 48 programs), and `make bench-mask-cost-check` still agrees on 3857 of 3857
  inputs. The Python subset's language, `python-subset.gbnf`, `python-subset-xgr.gbnf`
  and the toke grammars are unchanged. `scripts/gbnf_tune.py` now escapes `/` inside a
  regex lexeme. Before this fix, a lexeme containing `/` broke the generated grammar;
  no existing lexeme file had one. The new numbers and a corrected sentence about
  `own_token_disallowed` are in the 2026-11-25 addendum to
  `docs/about/reviews/mask-cost-2026-11-24.md`.
- **`make bench-mask-cost-check` exercises every alternative of the tuned Python forms,
  and a mutation pass measures it** (131.149). The agreement set (383 `tkc --python-view`
  programs, 27 traps, 3447 single-edit mutants) contained no `**`, so deleting `**` from
  the operator lexeme of `python-subset-lexeme.lark` still agreed on 3857 of 3857. The
  new `scripts/grammar_mutation.py` deletes one alternative at a time, in every rule and
  every regex lexeme of a Lark or GBNF grammar, and counts the deletions some agreement
  input notices. `make bench-mask-cost-mutation` (`python_subset_gbnf.py --mutation`,
  slow, not a gate) scores both tuned forms. Before: 305 of 1152 deletions killed in
  `python-subset-lexeme.lark` (fixed-string lexeme alternatives 21 of 37) and 300 of 1107
  in `python-subset-xgr.gbnf`. `python_subset_gbnf.py` now carries 30 hand-written accept
  cases (operators, string kinds and escapes, numbers, augmented assignment, decorators,
  `try` forms, `global` / `nonlocal`, lambda, slices, patterns, the identifier trie, and
  every compound statement at each indentation level). `--check` fails unless
  `python-subset.gbnf` (XGrammar and `gbnf_earley`) and `ast.parse` accept each one.
  After: 1142 of 1152 and 1107 of 1107 killed. The 10 survivors are in `TSTR` / `TSTRQ`,
  terminals no rule reachable from `root` uses, which the pass reports as equivalent.
  `make bench-mask-cost-check` now agrees on 3887 of 3887 inputs. No grammar changed.
  For toke (report only, `scripts/grammar_mutation.py --toke`): 260 of 502 deletions in
  `toke-lexeme.lark` and 225 of 320 in `toke-xgr.gbnf` are killed by
  `test/gbnf/fragments.json`; adding fragments is 131.150. Details: limit 4 in
  `docs/about/reviews/mask-cost-2026-11-24.md`.
- **The toke grammar fragment set exercises every alternative of the tuned forms**
  (131.150). `test/gbnf/fragments.json` had fallen behind its seeds and is regenerated
  (2081 -> 2473 fragments: 99 newer diagnostics inputs and changed standalone and
  example programs). 17 hand-written seeds, `test/gbnf/extra/x78.tk`-`x94.tk`, add
  68 fragments (2541 in all). They cover comments and string interpolation nested
  to the 8 levels the llguidance form unrolls, every string escape, one name per
  branch of the keyword-excluding identifier trie (as a const, a let name and a
  value), `fn` as a name, all 14 scalars, every type and parameter-type form, every
  operator, every unary and cast form after `!`, after `rt` with no space and as a
  `mt` subject, postfix chains, and a comment wherever whitespace is required.
  `scripts/grammar_mutation.py --toke`: before (2081 fragments) 260 of 502 deletions
  killed in `toke-lexeme.lark` and 225 of 320 in `toke-xgr.gbnf`; the 68 new
  fragments alone kill 502 of 502 and 320 of 320, and a full run over all 2541
  confirms it, so no survivor needs an equivalence argument. `make check-gbnf` and `make check-grammar-artefacts` agree
  on 2541 of 2541. No grammar changed. Writing the fragments found that `toke.gbnf`
  rejects reading a field named `fn` (`x.fn`), which `tkc` parses (filed 131.152).
- **The generated tuned grammars carry no rule or terminal unreachable from `root`**
  (131.151). `bench/mask_cost/python-subset-lexeme.lark` defined `TSTR` and `TSTRQ`,
  which no rule used: the 131.148 rewrite folds the plain string kinds into the `PSTR`
  lexeme, so `tstr` / `tstrq` are only inlined into its regex. `lexeme_lark` in
  `scripts/gbnf_tune.py` now emits a lexeme's terminal only when a rule reachable from
  `root` references it, and both generators (`lexeme_lark`, `xgrammar_tuned`) refuse
  to return a form with an unreachable rule or terminal. `gbnf_tune.py --check`
  (`make check-grammar-artefacts`) and `python_subset_gbnf.py --check`
  (`make bench-mask-cost-check`) also check the files on disk; the previous
  `python-subset-lexeme.lark` fails with `TSTR, TSTRQ`. The regenerated file loses
  exactly those two lines; the language is unchanged (3887 of 3887 inputs still agree
  for llguidance and XGrammar). `docs/spec/toke-lexeme.lark` and `toke-xgr.gbnf` had
  no dead rules and are byte-identical. `scripts/grammar_mutation.py` gains
  `dead_rules(text, dialect)`, the reachability its mutation pass already used.
- **`toke.gbnf` accepts reading a field named `fn`** (131.152). A record may declare
  a field `fn` (`t=$p{fn:i64}`, the literal `$p{fn:1}`), and `tkc` parses `x.fn`,
  `x.fn(1)`, `x.0.fn` and `h().fn` as field reads, but the grammar's member name
  after `.` was `vname`, which excludes `fn`, so a constrained decoder could never
  emit a read of such a field. The member name is now `ident` (`tlg-d`); `fn` as a
  plain name with a postfix (`fn.x`) stays a KNOWN under-approximation, and
  `x.fn(a:i64):i64{..}` and `x.fn=1` stay rejected by both. Regenerated
  `docs/spec/toke.lark`, `toke-lexeme.lark` and `toke-xgr.gbnf` (one line each).
  New seed `test/gbnf/extra/x95.tk` (field reads in let, call, index chains, comments
  around `.`, cast, propagate, `if`/`lp` conditions, unary operands, a struct literal
  receiver and a `mt` subject); `test/gbnf/fragments.json` 2541 -> 2549. Against the
  previous `toke.gbnf` the seed is a parser-only accept and the gate fails. Production
  and nonterminal counts unchanged (339 / 658).

- **`make fuzz-smoke` replays its inputs under libFuzzer** (127.480). It passed
  libFuzzer a seed directory plus adversarial files, a mix libFuzzer refuses
  ("Not a directory: ...; exiting", exit 1) before running anything, so every
  target with adversarial inputs (json, yaml, toon, xml, multipart, ws_frame,
  image, http_parse) failed in the Security nightly, which runs every day except
  Monday. The one green run, on 2026-11-16, was the Monday CVE job; the fuzz job
  was skipped. It was not a sanitizer finding: every seed and adversarial input
  replays clean under ASAN+UBSAN. libFuzzer now gets the seed files, the
  `Executed` lines are counted against the inputs, and a failure with no
  sanitizer line prints the output's tail.
- **`make test-asan-compiler` runs the conformance suite with a 64 MiB stack**
  (127.449). C584's cases just under the nesting limit need 2-4 MiB in the
  shipped build but 16-32 MiB under ASan, and overflowed the default 8 MiB.
  C080 now relinks the tree's objects with the `-fsanitize` flags they were
  built with (from `.build-flags`), not without them.
- **`make lint-nonhost` runs under `LINT_CC` (default clang), as `make lint`
  does** (127.450). Under GCC (CI's default `cc`) the macro-only headers
  `src/build_info.h` and `src/version.h` failed as empty translation units,
  reported only as `cc1: all warnings being treated as errors`. A failure now
  always names its file. Also, the `nonhost-uncoverable` skips for `log.c` and
  `sys_glue.c` are now `nonhost-uncoverable@darwin`: they hold on macOS and were
  stale on Linux. Test: C1961.
- **`make test-ci-green` is green again** (127.510). Two of its 82 targets
  failed at the integration base on rules that are correct (rows 3 and 4
  above), not on a regression. test-codegen's `multimod/mapret` interpolated
  map lookups and now reads them through `mt`. test-migrate's `str_index`
  migrates a legacy str index used as a str to `.slice(i;i+1)`, which is
  `str!$sliceerr` since 127.462, so the migrated output is five E4031s: the
  fixture now pins those diagnostics (`str_index.check`) instead of expecting a
  clean compile, because `--migrate` writes no filler error arm (owner answer
  R2). `--migrate` now says so on stderr: `note: each .slice(i;i+1) is
  str!$sliceerr: read it through 'mt' or '!' (or use .charat(i) for the byte)
  before it compiles`. Test: C2100.
- **`tkc --migrate` reads a one-character comparison as bytes** (127.512).
  A legacy str index compared with another str index or with a one-byte str
  literal (`a[0]=b[1]`, `v[i]="a"`, `"\n"=v[0]`) migrated to
  `.slice(i;i+1)`, which is `str!$sliceerr`, so the migrated comparison did not
  compile. Both sides now read the byte: `a.charat(0)==b.charat(1)`,
  `v.charat(i)==97`, `10==v.charat(0)` (owner answer R2's single-character read,
  no union, no filler arm). A str index used as a str (returned, concatenated,
  printed, compared with a longer string) still migrates to `.slice(i;i+1)` with
  the 127.510 note: std.str has no non-union one-character str read.
  test-migrate's `str_index` now pins three E4031s, not five. Test: C2151.

### Fixed for loke (the 2026-11-15 handoff, Epic 138)

- **T-67, the runtime half: std.process refuses a handle it did not create.** `process.wait(5)`, loke's helper shape `f(h:i64){process.wait(h)}` called with any integer, a handle changed by arithmetic, a value cast with `as`, and a `process.spawndetached` pid passed as a handle all checked clean, built, and died with SIGSEGV: the runtime read the integer as a process record. `process.spawn` now hands out a registry token instead of the record's address, and every call that takes a handle (`wait`, `stdout`, `stderr`, `kill`, `exitcode`, `hasexited`, `poll`, `isalive`) looks it up without reading it as memory. A value it did not hand out is answered exactly as `process.badhandle()` is: `-1`, the `$err` arm with `$io`, `false`, `0` or `1`. A real handle behaves as before, a waited handle stays valid, and `process.wait(0)` is still `-1`. No source change is needed. The checker half (an integer refused at `--check` where a handle is read) is 138.35. 138.36 — C1850.

- **T-67 class, the runtime half for std's opaque handles: std.vec, str's builder, std.securemem, std.pdf, std.xlsx, std.zip, std.toml and std.svg refuse a handle they did not create** (138.45). Each module handed its C object's address to the program and read through whatever came back, so a value the checker cannot see (`5 as $vechandle`, a value through C) died with SIGSEGV: `vec.len`, `str.done`, `securemem.wipe`, `pdf.pagecount`, `xlsx.sheets`, `zip.entries`, `toml.keys`, `svg.render`, a forged element in `svg.group`'s array. 138.36's process registry is now one table in `tk_runtime.c` with a kind per slot, and every handle these modules hand out is registered and looked up without being read: any other value, one of another kind, and one already closed or finished is answered as the null handle is (`vec.len` 0, `str.done` "", `pdf.pagecount` 0, `zip.entries` `@()`, `toml.*` `$err`, `svg.render` the null string, ...). So a second `pdf.close`/`xlsx.close`/`zip.close`/`str.done` is a no-op instead of a double free, and a `toml.section` (or `sectionarr` element) is a view that dies with its document: read after `toml.free` it is `$err` instead of a read of freed memory. The kinds with a release call (str's builder, pdfdoc, xlsxbook, ziparchive, tomlval and its section views) now hand out a registry token instead of the address, so a released handle stays refused even after malloc gives its memory to a newer object of the same kind (keyed by address, `str.done(b1)` then `b2=str.buf()` let `str.add(b1;..)` write into `b2`, and `toml.i64` on a freed document read the next one). The opaque values are not addresses any more; no toke code can tell, and no C caller in the tree takes them. vec, securebuf, svg and $llmclient keep their address (they have no release call). Cost: a 4-way per-thread cache serves repeated lookups; 20 M iterations of `a.get(j)+b.get(j)` measured user 0.09 s -> 0.27 s, `a.get(j)+a.get(j)` 0.09 s -> 0.23 s, 20 M `str.addbyte` 0.04 s -> 0.06 s. Not covered yet: the opaque types the Z213 survey did not measure (csvreader, dataframe, router, jsonstream, ...) and the std records with fields (138.46's runtime half). Kinds for std.task's scopes (138.47) and $llmclient (138.50) are reserved in the same table. Test: C2062.

- **std.task refuses a scope it did not create** (138.47). `task.awaitall(5)`, `task.cancel(5)` and `task.spawn(5;&f)` read the integer as a scope and died with SIGSEGV; task.tki declares every parameter `i64`, so no checker rule can see it. Scopes are registered in the same registry and each call answers -1 for anything else, as for `0`. Test: C2063.

- **A `$llmclient` record literal is no client** (138.50, the llmclient part). `$llmclient` declares {provider; model} and keeps base_url, api_key, timeouts, retries and usage counters private in C, so `llm.complete`/`chat`/`chatstream`/`countokens` (and `llmtool.*`) on a literal read its base URL and API key from past the literal's end, and a value cast with `as` was read through. Only a client `llm.client` or `llmtool.withtools` made is now used; anything else is answered as the null client is (`$err`, `countokens` 0). Unlike a `$httpclient` literal (138.24) it gets no defaults: it cannot carry a base URL. `html.htmlnode`, `http.httpstream` and `canvas.canvas` literals are not covered yet. Test: C2064.

- **T-67 class, the runtime half for the std records with fields: std.tls, std.vecstore, std.html's `$htmldoc`, std.image, std.db's rows and std.http's client refuse a value the runtime cannot vouch for** (138.61). 138.46's checker half refuses an integer in these slots, but a value cast with `as` still reached the glue and was read as the record's address: `tls.protocol(5 as $tlsconn)`, `vecstore.flush(5 as $vecstorehandle)`, `vecstore.count(5 as $veccollection)`, `html.render(5 as $htmldoc)`, `image.fliph(5 as $imgbuf)`, `row.str(5 as $dbrow;"k")` and `http.clientget(5 as $httpclient;..)` died with SIGSEGV. Records only the runtime makes are now registered in the runtime's handle registry (138.45) and looked up first: a forged value, a record literal or a closed one is the null handle (`tls.*` answer an unknown connection: `""`, `false`, none; `vecstore.*` `false`/0/`@()`/`$err`; `html.*` do nothing). `tls.close` forgets its record (it stays allocated, so `.id` still reads), and `vecstore.close` forgets the store and its collections, so a second close is a no-op where it freed the store twice. The closed store and collection records stay allocated (their entries are released; `.path`/`.name` still read), so their addresses are never reused: after a close and a reopen of the same directory the old handles stay the null handle instead of naming the new store (138.64; an upsert through the old collection used to land in the new store). `$imgbuf`, `$dbrow` and `$httpclient` may be written as record literals (image.md, db.md, 138.24), so no registry can vouch for one; their glue refuses only a value that cannot be the address of any record (below 4096, negative, or not 8-aligned) and answers it as the null record (`image.*` the empty image, `row.*` `$err` "null row", `http.client*`/`http.stream` `$err` "null client"); a literal works as before, and a value that looks like an address but is not a record is still read. Not covered: the opaque types the Z213 survey did not measure (csvreader, csvwriter, dashboard, dataframe, dtreemodel, router, handler, middleware, jsonstream, llmstream; 138.63). Test: C2110 (vsreopen for 138.64).
- **T-67 class, the runtime half for the opaque std types: std.csv's readers and writers, `$dashboard`, `$dataframe` (std.dataframe and std.analytics), std.ml's `$dtreemodel`, std.router's `$router`, `$handler` and request, and std.llm's `$llmstream` refuse a value the runtime did not make** (138.63). A value cast with `as` still reached their glue and was read as the object's address: `csv.next(5 as $csvreader)`, `df.tocsv(5 as $dataframe)`, `ml.dtreepredict(5 as $dtreemodel;..)`, `llm.streamnext(5 as $llmstream)` and `dashboard.serve(5 as $dashboard;0)` died with SIGSEGV, `router.get(5 as $router;..)` wrote through it, and a forged `$handler` was called at dispatch. Each object is now registered in the runtime's handle registry (138.45) where the glue hands it out and looked up first; anything else is the null object (`csv.next`/`csv.header` `$err` kind `badarg`, `csv.atend` true, `csv.flush` empty; `df.*` and `analytics.*` `$err` or `""`, `df.shape` `{0;0}` where it was the null record; `ml.dtreepredict` and `llm.streamnext` `""`, as for an empty literal; `dashboard.serve`/`router.serve` `$err`; `router.get`/`post`/`put`/`delete` register nothing). A `$csvreader` is now a registry token, not its address: `csv.close` forgets it, so a closed reader is refused even after a newer reader takes its memory, and a second `csv.close` is a no-op where it freed the reader twice (exit 133). Its fields (never documented) can no longer be read. A literal such as `$llmstream{id:1}` or `$dtreemodel{..}` no longer selects a live stream or model. The request a router handler is given is a token for that dispatch only: a request kept past its handler reads `""` instead of a stack frame since gone. `router.get` and friends refuse a `$handler` that is not code in a loaded image (macOS; elsewhere one below 4096 or negative) with `DIAGNOSTIC: kind=router_register_failed`, and the route answers 404. `router.use` never reads its `$middleware`. `$jsonstream` is unchanged: `json.streamparser` has no implementation (E4027), so no glue reads one. Test: C2140.

- **`$htmlnode`, `$httpstream` and `$canvas` record literals are the null value** (138.50, the rest). These .tki records are only the leading slots of a larger C struct (`tki-record-prefix`), and `html.append`, `http.streamnext` and `canvas.fillrect`/`filltext`/`tohtml` read or wrote the private fields past a literal's end. `html.h1`/`html.table`, `http.stream` and `canvas.new` now register what they make, and those calls use only a registered one: `html.append` skips any other node, `http.streamnext` answers `$err` "null stream" (the null stream used to be a bare 0 with the error slot untouched, so `$ok` bound a null `[byte]`), and the canvas calls return the null canvas. As for `$llmclient`, a literal gets no defaults. Test: C2111.

- **`@(x)` of a binding of an `"opaque": true` std type made with `as` is a one-element array** (138.60). `let fe=c as $svgelem; let a=@(fe)` was lowered as a spread of `fe`: the IR read word -1 of the element as a length (`a.len` 0 for a real element, SIGSEGV for a forged one), so `svg.group(@(fe;c);"")` lost its first element. An element the checker types as a record is never spread. A type declared `"fields": []` (`$vechandle`) was unaffected. Test: C2112.

- **std.process: a reaped child holds no file descriptor** (138.48). No toke call frees a process handle, and the child's stdin pipe was closed only when the handle was freed, so every spawn kept one descriptor for the life of the program: 400 spawn+wait+stdout+stderr iterations under `ulimit -n 256` failed from the 247th spawn on. The runtime now closes the stdin pipe when it reaps the child (`process.wait`, or a `process.poll`/`hasexited`/`isalive` that sees the exit) and reads what is already in the output pipes into the handle without blocking, so a poll-only spawner that never reads the output is bounded too. `wait`, `stdout`, `stderr` and `kill` answer a reaped handle as before; no API change (row option (a)). Test: C2060.

- **`http.withmaxbody` bounds the response header block too** (138.49). The client read the header block until its blank line with no ceiling, cap or no cap, so a server that never ends its headers grew a capped client's memory for as long as the request lasted. A client carrying `http.withmaxbody` now refuses a header block over 65536 bytes with `$err` `response header block exceeds the cap (65536 bytes): ...` and closes the connection; a client without it reads as before (the 138.24 amendment's opt-in convention; a cap on every client is an open owner question). Test: C2061.

### Added — OS-level enforcement of capability grants, opt-in (138.44)

This implements D5 of `docs/decisions/138.19-scoped-capability-grants.md`, which the owner made a separate story on 2026-11-15; the design is `docs/decisions/138.44-os-enforcement.md`. **Nothing changes for a binary that does not opt in**: its IR is byte-identical, and the runtime never reaches the new code.

- **`os_enforce = true`** in `tkc.toml [capabilities]`, or **`tkc --cap-os-enforce`**, bakes `@__tk_cap_baked_os`. At start-up, after the grants are final and before `main`, the runtime then has the kernel refuse what the grants do not admit:
  - macOS: `sandbox_init(3)` with a profile generated from the grants. No entitlement is needed.
  - Linux: Landlock, for file-system read, write and execute and, from ABI 4, TCP bind and connect ports. A seccomp filter refuses IPv4/IPv6 sockets and `io_uring_setup` when `net` is not granted.
- **What it catches:** a user `extern` C function, an FFI call, or a sink with its gate removed. Each is refused outside the grants, where before it succeeded. C2230 shows `fopen` and a broker-removed stdlib build (`file.read`/`file.write`/`process.spawn`/`net.listen`/`http.geturl`) succeeding without the key and refused with it.
- **The broker is unchanged and answers first.** CAP001 and CAP002 are the same text and JSON. Every capability conformance script (C188x, C470, C502, C511, C674, C1864, C2056, C545, C631, C112) passes with the layer forced on.
- **The kernel rules are a superset of the grants.** They narrow `net` to ports rather than hosts, and leave `env.write` alone. They add a read-only system baseline (the OS image, time zones, resolver and CA files, `/dev`) with no home or temporary directory.
- **Two side effects for a binary that opts in:** child processes inherit the rules, and the standard library's own reads outside the baseline are refused. A granted child runs to completion under the rules (C2230 2o–2q), but:
  - it is refused any helper it execs itself (the macOS `git` shim's `xcrun`, `git-*` helpers);
  - it is refused reads of its own files when they lie outside `fs.read` and the baseline (Homebrew, `/usr/local`). CAP004 names such a command at start-up.
  - The `#!` interpreter of a granted script (and, for `#!/usr/bin/env NAME`, the `NAME` that resolves on `PATH`) is added to the exec rules, so the script itself runs.
- **Review fixes before merge (round 2):**
  - On macOS every granted child was killed with SIGABRT before `main`, because the profile denied dyld's read of `/`. The root directory itself (not beneath it) is now readable.
  - The Linux seccomp filter let a system call of another architecture through. On x86-64, `int 0x80` could open a UDP socket under `net_none`. The 32-bit compat entry is now filtered (i386 `socketcall` is refused outright), and any other architecture is killed.
  - When Landlock refuses a rule, no Landlock rules are applied, and CAP004 says so. Before, the ruleset could be narrower than the grant.
  - `log.openaccess("/dev/stderr")`, which the broker admits with no grant, was refused under the OS layer. The process's own standard output and error are now writable: `/dev/fd` on macOS, and on Linux the file behind descriptors 1 and 2.
- **New runtime warning CAP004.** Where the kernel cannot carry a grant, CAP004 says so once at start-up, as a text line and, under `TK_DIAG_JSON`, a JSON line with `layer` and `reason` and no `fix`. The program then runs with the broker alone for that grant. CAP004 is given when:
  - there is no OS layer on the platform;
  - the kernel has no Landlock;
  - Landlock has no TCP rules;
  - an `fs.write` scope does not exist yet;
  - a scope holds a control character;
  - Landlock refuses a rule (then none is applied);
  - a granted command lies outside `fs.read` and the read baseline;
  - the seccomp filter is refused.
- **Diagnostics and records:** `TK_CAP_OS_SHOW=1` prints the generated rules. `--show-limits` prints `cap-os`. The `--companion` record says when the layer is on.
- **Tests:** C2230, plus `test/stdlib/test_cap_os.c` in `make test-stdlib-capabilities`.
- **Not yet run on a Linux kernel:** the Linux path was compiled here (zig cc, glibc x86_64 and aarch64, and musl) and its rule planning was unit-tested, but it has not been executed on Linux. On Linux CI its kernel cases run where Landlock is present and are named skips where it is not.

### Added — HTTP client controls, a header-carrying GET and a resolver (spec amendment 138.24)

One spec amendment, decided by the owner on 2026-11-15 (register 2.1 (a), 2.2, 2.3; `docs/decisions/138.24-http-client-controls.md`), for loke's handoffs T-63 and the T-58 addendum. Every control is opt-in and off on a client from `http.client`. Two corrections change existing calls, and only where they were unsafe: a client written as a record literal no longer reads settings from past its end, and a URL that would rewrite the request is refused (last two rows).

| Change | Story, commit |
|---|---|
| **`http.withdeadline(c;ms):$httpclient`** — each request must finish (connect, TLS handshake, send, response headers and body) within `ms` of the call, else `$err` `request deadline exceeded (<ms> ms) <stage>`. `timeoutms` restarts on every byte, so a server dripping a byte every 5 s held a request for 40 s and it answered `$ok`. | 138.24 — `d14f878b` |
| **`http.withmaxbody(c;bytes):$httpclient`** — a response body over the cap is `$err` `response body exceeds the cap (<bytes> bytes): ...` and the read stops (a `Content-Length` over it before the body is read, a chunked body on the size line that passes it). A 64 MiB body used to be read whole. | 138.24 — `d14f878b` |
| **`http.withconnectaddr(c;addr):$httpclient`** — connect to an IPv4/IPv6 literal (no lookup) while `Host`, SNI and certificate verification use the URL's host, the `curl --resolve` equivalent; a non-literal, or one with `withproxy`, is `$err` at request time. | 138.25 — `d14f878b` |
| **`net.resolve(name:$str):@($str)!$neterr`** — every address a name has (A and AAAA), each once, as numeric strings; needs the `net` capability. `$neterr{msg;code}` is new, shaped as `$httperr`. | 138.25 — `0e2d3f1c` |
| **`http.getheaders(url:$str;headers:@(@($str))):$httpresp!$httperr`** — `http.postheaders` without a body, as `GET`; `Host` and `Content-Length` reserved, CR/LF refused, https verified. | 138.27 — `da9755d6` |
| `http.stream` and `http.downloadfile` refuse a client that carries one of the three controls (they read on paths the controls do not reach). | 138.24 — `d14f878b` |
| **Documentation:** `$httpclient.timeoutms` is a per-read timeout, not a per-request one, as `docs/stdlib/http.md` used to say. | 138.34 — `113dc13b` |
| **Fixed: a `$httpclient` written as a record literal** (`$httpclient{baseurl:..;poolsize:..;timeoutms:..}`) is a client with no proxy and no controls. The client read its proxy (since 42.1.3) and the new controls from past the literal's three fields, so a literal client took a spurious cap, deadline or connect address, and `http.withproxy` on one could segfault. std now reads those settings only from a client it made. | 138.24 — `c79119a7` |
| **Fixed: every client call refuses a URL that would rewrite the request** — a host that is not a DNS name or IPv4 address, or a port or path with a control character or a space — with `$err` `URL refused: ...` (`http.downloadfile`: `-1`) before anything is sent. A CR/LF in the URL used to be sent, so `http.getheaders` with `"http://h/x HTTP/1.1\r\nHost: evil.example\r\n..."` delivered the caller's own `Host`; a space or tab drew a `400`. | 138.24 — `e5678505` |

Tests: `C1860`–`C1866`, `D605`–`D608` (`6055e9ca`, `c79119a7`, `e5678505`, `e25bbd7d`, `c44d48a4`). Open: the response header block is not capped (138.49).

### Fixed — standard library

- **`df.filter(d;col;op;val)` reads its `op` and `val` strings** (138.66). The
  glue passed `op`'s pointer to the C filter as its operator code and `val`'s
  pointer bits as the f64 threshold, so every documented call
  (`df.filter(d;"a";">";"2")`) kept no rows and a malformed `op` or `val` was
  never refused. `op` is now exactly one of `<` `<=` `=` `>=` `>` `!=` (the C
  filter gains `!=`, the one documented op it lacked) and `val` must parse in
  full as an `f64` with C `strtod` syntax (so `"0x1p1"`, `"nan"` and `"inf"`
  are numbers) and no leading whitespace; anything else, like `" 2"` or an
  unknown or non-numeric column, is
  `$err`, with the message `df.filter: the filter could not be applied
  (unknown or non-numeric column, unknown op, or a val that is not an f64)`.
  Tests: C2161, `test_dataframe` T65.
- **`df.groupby(d;col)` returns the `[dataframe]` its interface declares**
  (127.518). The glue asked the C group-and-aggregate routine for an
  aggregate over no column, so every call was `$err`, and on success it would
  have handed back a C `DfGroupResult*` where toke reads an array of
  dataframes. It now returns one registered `$dataframe` per distinct value of
  the `$str` column `col` -- every column, rows in their original order,
  groups in order of each value's first row -- so `.len`, `.get(i)`,
  `df.shape`, `df.tocsv` and `df.filter` work on the groups. A frame with no
  rows gives `@()`; an unknown or non-`$str` column is still `$err`
  (`df.groupby: the column could not be grouped`). Tests: C2180,
  `test_dataframe` T66.
- **`df.columnstr(d;name)` returns the `[str]` its interface declares**
  (127.520). The glue handed back the C routine's malloc'd `char**`, which
  has no toke array header, so `.len` of the `$ok` value read the word before
  the block (0 for a 2-row column) and `.get(i)` was bounds-checked against
  it. It now returns a toke array of sealed strings, one per row in row
  order. A `$str` column of a frame with no rows gives `@()`; an unknown or
  `f64` column is still `$err` (`df.columnstr: no such string column`).
  Test: C2190.

- **std.cache's disk spill writes the whole entry or leaves no file** (127.445).
  The spill to `disk_dir` ignored `write(2)`'s result and counted the body in
  `disk_used` whatever happened, so a short or failed write (ENOSPC, EFBIG,
  EINTR) left a truncated `.cache` file and the byte count drifted; Ubuntu's gcc
  also refused the file at `-O1 -Werror`. Writes now loop over short writes and
  EINTR; a failed open, write or close removes the partial file and counts
  nothing. `http_cache_put` has no error channel, so a failed spill is a later
  MISS. Test: C2000.

- **std.cache's disk tier is reachable: evicted entries spill, and are read
  back** (127.496). `http_cache_put` ran the LRU eviction before its spill
  test, so the test could never hold and every evicted entry was dropped;
  `http_cache_get` never read `disk_dir`, and `disk_max_size` was never
  consulted. Now each entry the eviction drops (over `max_entries` or
  `max_size`) is spilled first while `disk_used` stays within `disk_max_size`
  (0 spills nothing), and a memory miss reads the key's spill file back,
  moves the entry to the front of memory and removes the file. The file now
  stores the key (an FNV-1a filename collision is a MISS, not another key's
  body), the insertion time (freshness keeps counting from the store) and the
  field lengths; files in the old layout are a MISS. A put of a key drops its
  disk copy, and `http_cache_purge` / `http_cache_purge_pattern` purge matching
  spill files too and count them. Nothing in the tree calls `http_cache_*`.
  The read-back leaves `last_modified` and `vary` unset (they are not in the
  file; nothing reads them after a put, and Vary is part of the key). A spill
  file is parsed length by length against the bytes left, so a crafted file
  with lengths that wrap is a MISS; spill files are created `O_EXCL |
  O_NOFOLLOW`, mode 0600, so a symlink planted at the path is not followed.
  **Lifetime:** the body, etag and content-type pointers `http_cache_get`
  returns are valid only until the next `http_cache_get`, `http_cache_put` or
  purge call: a get that reads an entry back from disk can now evict, and
  free, the entries an earlier get returned. Copy what must outlive that.
  Test: C2085.

- **std.cache's stale-if-error fallback reads the disk tier too** (127.511).
  `http_cache_stale_if_error` searched only the in-memory list, so an entry
  spilled to `disk_dir` was never served when the origin failed. With no
  matching memory entry it now scans `disk_dir`'s spill files through the
  same bounds-checked read-back as `http_cache_get`, takes the matching file
  with the latest insertion time inside the same window (TTL + 300 s), moves
  that entry to the front of memory and removes the file. A crafted or
  truncated spill file is not a fallback. The pointers it returns follow the
  127.496 lifetime rule: valid until the next `http_cache_get`,
  `http_cache_put`, purge or `http_cache_stale_if_error` call. Nothing in the
  tree calls `http_cache_*`. Test: C2130.

- **std.cache's stale-if-error fallback is the request's own entry** (127.514).
  `http_cache_stale_if_error` accepted any key that began with `METHOD|URI`, so
  with `/a` not cached a lookup of `GET /a` served a cached `/ab`, `/a/b` or
  `/api` body, from memory or (since 127.511) from a spill file. A key now
  matches only when it is `METHOD|URI` itself or a Vary variant of it
  (`METHOD|URI|...`), the split `http_cache_purge` already uses. Nothing in the
  tree calls `http_cache_*`. Test: C2150.

- **`csv.serialize(rows:@(@($str))):$str` is usable from toke** (127.354; owner
  register 2.11, recommendation). Its interface declared an `i64` ABI
  placeholder and the glue read a packed C block that no toke value has: a call
  with an array of rows compiled and segfaulted, and an integer argument
  checked clean. It now takes an array of rows and returns the CSV text,
  byte-for-byte what `csv.writer(44)` + `csv.writerow` per row + `csv.flush`
  produce (RFC 4180 quoting, CRLF between rows, no trailing line break; no rows
  is `""`). Spec (generated std.csv block), `csv.tki` and `docs/stdlib/csv.md`
  change together. Test: C2001.

- **std.infer's streaming loader refuses instead of returning a handle that is never loaded** (136.78). In a llama.cpp build (`TK_HAVE_LLAMACPP`), `tk_infer_load_streaming` returned success with a handle whose model and context stayed NULL, because its layer loop is a TODO: `tk_infer_is_loaded` said 0 and every `generate` / `embed` on it failed after a load that had succeeded. After the directory and shard checks (code -1) it now returns `is_err=1`, code -2 ("not implemented"), no handle and no prefetch thread; the prefetch machinery is kept behind `TK_INFER_STREAM_LAYER_LOOP`. The stub core and this repository's glue already failed and are unchanged, so no toke program changes behaviour. `docs/stdlib/infer.md` documents streaming as incomplete. Test: C2220.

- **`process.exit(0)` after a recorded failure exits `1`, as `os.exit(0)` does** (127.532).
  A failed `test.eq` / `test.ok` / `test.assert*` (or an `http.servetls` that
  could not start) is recorded and makes the process exit `1` (127.231), and
  `os.exit(0)` honoured that; `process.exit` called `_exit()` directly, so
  `test.eq(1;2); process.exit(0)` exited `0` with no
  `DIAGNOSTIC: kind=exit_status` line (also on v3.2.0). It now prints that
  line and exits `1`, and under `TK_MEM_STATS=1` prints the report once, like
  `os.exit(0)`. A non-zero status is unchanged. Survey of the other exit paths
  in `docs/stdlib/process.md`. Test: C2270.
- **`os.exit(N)` and `process.exit(N)` whose low 8 bits are 0 (`256`, `512`, `-256`, ...) after a recorded failure exit `1`** with the `DIAGNOSTIC: kind=exit_status status=1` line, as `main` returning `256` does (127.536). Both checked for a status of exactly `0`, then `_exit()` truncated `256` to `0`, so a failed `test.eq` followed by `os.exit(256)` exited `0` with no line (also on v3.2.0). Both paths now share one helper, `tk_exitfail_before_exit` in `src/stdlib/tk_exitfail.h`, applying main's rule. With no recorded failure such a status still exits `0`, as `main` returning `256` does; a status with non-zero low bits is unchanged. Conformance: C2280.

### Added — runtime: `TK_MEM_STATS=1` allocation report at exit (127.529)

- **`TK_MEM_STATS=1` makes a toke program print one allocation report line to
  stderr as it exits** (127.529): `tk-mem-stats: v=1 pid=… sealed_bytes=…
  sealed_committed=… region_released_bytes=0 malloc_calls=… calloc_calls=…
  realloc_calls=… alloc_bytes=… free_calls=… freed_bytes=…
  live_bytes_delta=…`. It is the counter source that ADR-0016's measurement
  plan gates region slices on (bytes per iteration). It replaces the scratch
  `tks_top` destructor and `DYLD_INSERT_LIBRARIES` interposer of the 127.299
  harness. The report prints on a normal exit, a trap, `os.exit`,
  `process.exit` and a recorded failure (a failed `test.eq`). It prints at
  most once. The malloc fields come from libmalloc's `malloc_logger` hook and
  read `na` off Darwin and in ASan builds. Counters are atomic, so every
  thread is counted. With the variable unset nothing is installed: CPU time
  on a 3M-iteration interpolation loop was 0.747 s median for both the base
  and the candidate. Format and recipe: `docs/runtime-abi.md` §10.1. Test:
  C2260.

### Changed — code generation: interpolation frees its number conversions (127.299)

- **A string interpolation frees the number conversions it made** (127.299).
  In `"k\(i)=\(x)"` every int or float segment was converted into a fresh
  `malloc`'d string, copied by the literal's single `tk_str_join_n`, and then
  kept forever, although no toke value could reach it. The literal now frees
  those conversions right after the join. Nothing a program can see changes:
  the result is the same sealed string, a one-segment `"\(n)"` still returns
  its conversion, and string, bool and u64/u8/u16/u32 operands are not
  touched. Measured on 1M iterations of `"k\(i)=\(i*3);f=\(1.5)"`: peak RSS
  went from 154.9 MB to 32.3 MB, frees from 14 to 3,000,015, with identical
  output and time. The ooke template harness (127.257) is unchanged, because
  its bytes are sealed token slices and builder output. This is the one slice
  that `docs/decisions/127.299-function-arena.md` finds provably safe without
  escape analysis. That note also records the per-function-arena and
  `{arena}` designs the owner asked for (answer 1.13). Test: C2200
  (ASan+UBSan, `TK_STR_VERIFY=1`).

### Added — lint rules for the pre-132.15 spec-page shapes (131.138)

Four new hints, one per long form the withdrawn specification page prescribed. They
compile and are not errors; a hint does not change the `--lint` exit status. A
downstream that ratchets its `tkc --lint` counts sees new findings on unchanged source:
measured with this build, the toke tree gains 66 (27 `negated-comparison`, 24
`nested-if-and`, 14 `modulo-emulation`, 1 `negated-equality`, in 29 of 695 sources,
20 of them under `bench/patterns/macro/programs/`), ooke `main` 33 (25 `negated-equality`, 8
`negated-comparison`), the website 0, and loke (1,091 `.tk` files, read-only) 110: 52
`negated-comparison`, 50 `negated-equality`, 5 `modulo-emulation`, 3 `nested-if-and`.

| Rule id | Severity | Reports | Message | `fix` | Story |
|---|---|---|---|---|---|
| `negated-equality` (new) | hint | `!(a==b)` | ``"`!(a==b)` negates a comparison toke has as one operator — write `a!=b`"`` | `a!=b`, only on integer or bool operands (by the checker's types) and where the parent takes a comparison unparenthesised; none on floats (`!=` is `fcmp one`: NaN) or str | 131.138 |
| `negated-comparison` (new) | hint | `!(a<b)`, `!(a>b)` | as above, naming `<`/`>=` or `>`/`<=` | `a>=b` / `a<=b`, integer operands only (NaN: `!(x>y)` is true, `x<=y` false) | 131.138 |
| `modulo-emulation` (new) | hint | `a-a/b*b`, `a-(a/b)*b` with the same name, field path or literal both times | ``"`a-a/b*b` computes the remainder the long way — write `a%b`"`` | `a%b` on integer operands: equal for every sign; both trap RT004 for `b==0` and MIN by -1 (the trap message says `remainder` for `division`) | 131.138 |
| `nested-if-and` (new) | hint | `if(a){if(b){..}}` with no `el` on either and nothing else in the outer block; a chain once, at its outermost `if` | ``"`if(a){if(b){..}}` with no `el` on either is `&&` — write `if(a&&b){..}`"`` | `if(a&&b){..}` (an `\|\|`, `mt` or expression-`if` operand parenthesised, since none parses bare beside `&&`); none when a comment sits in the outer block | 131.138 |

Tests: D730–D734, C2101 (a differential of each fix over every sign combination at
`-O0`/`-O2`, and the two traps). Reference: `docs/compiler/lint-rules-v1.md`
"Page-shape rules".

### Notes for downstream builds

- **Which stdlib C sources a program links is documented** (138.42): what `tkc
  --emit-deps` prints for each module, plus the flags it leaves to the caller
  (`docs/compiler/build-deps.md`, "Linking a program yourself"). From this release,
  an entry names every change to what `--emit-deps` prints for a module, and every
  lint rule added or changed (138.41; `make check-lint-rules` checks the rule ids).
  Neither has changed on main since 3.2.0 (checked at `987d9f6f`); 3.2.0's changes
  to both are in its addenda.
- **`tkc --emit-deps` given several files prints one merged set** (138.54). It
  printed each file's sections back to back, so the second file's C sources
  followed the first file's `---` and read as link flags to every consumer that
  splits on `---`. With more than one file it now prints the union in the
  one-file layout -- sources, then `---` and flags, then `---` and user modules as
  `<path>.ll` -- each entry once, in first-seen order, a `-framework X` pair kept
  together; a file that does not type-check prints nothing on stdout for the whole
  run. One file's output is byte-identical. ooke's `scripts/emit_deps.sh` and the
  `build-deps.md` recipe run one file per call and are unaffected. Test: C2055.

### Changed — diagnostics

- **`{arena ...};` parses** (127.527). An arena block now takes one optional
  trailing `;`, as an `if` or `lp` block does; it was E2002 ("unexpected token in
  expression, got ';'"), so every documented example written with the `;` failed
  to parse. `toke.gbnf` and `toke.lark` (`item ::= ... | arena (ws ";")?`) and a
  `grammar.ebnf` note follow; no production, keyword or lookahead is added.
  Tests: D774, `make check-gbnf` (`test/gbnf/extra/x38.tk` now accepted by both).
- **E5001's documentation describes the check the compiler makes** (127.528):
  `docs/spec/errors.md`, `docs/reference/errors.md`, `docs/reference/statements.md`,
  `docs/spec/semantics.md` §7.3, `docs/spec/memory-model.md` §2.4 and the guide, with
  an example that parses and draws exactly E5001.
- **E4026 (wrong argument count) has no `fix` field** (138.51, AGENTS.md §6). The
  four texts it carried were advice, not a repair right in every case: which side
  is wrong (the call, or an interface that disagrees with its implementation) and
  whether an argument is missing or surplus cannot be decided at the call site. An
  alias or module call's message now ends with the advice ("...; match the call to
  the declaration, or reconcile the interface with the implementation if they
  disagree"); the code, `expected` and `got` are unchanged. Tests: D710-D713, C2057.
- **CAP001's JSON `fix` names the tkc.toml key** (138.53). On a binary with no
  baked grants it read `grant --allow-write or tkc.toml [capabilities] fs.write=true`;
  tkc refuses `fs.write` as an unknown capability. It now reads `... fs_write = true`
  (`fs_read`, `fs_write`, `net`, `env_write`, `process_spawn`), the spelling the
  narrowing-binary fix already used. The text lines are unchanged. Tests: C2056,
  C1884 case 8.

### Documentation and claim gates

- **The memory model describes the allocation model that ships** (127.524, owner answer
  1.13 (b)). `docs/spec/memory-model.md` promised a per-function arena freed at scope exit
  (§1.1, §1.3) and a copy of every returned value into the caller's arena (§2.3); no
  implementation ever did either. The amended text, normative, says what ships: values come
  from `malloc` or the S1 sealed-string heap and are not released before exit, except
  closure environments (124.0c) and an interpolation's number conversions (127.299); an
  implementation may release only what no toke value can reach. New §1.6 states the leak
  budget per workload (bytes per iteration; unbounded growth for a long-running server).
  `{arena}` parses and type-checks but releases nothing until the ADR of 127.525, and E5001
  is reserved for an escape from it (not emitted today: 127.526). §3's per-task arenas, §5's
  "reference counting: not present" and §6.4's bound are corrected. The docs that restated
  the old promise (semantics §7.3, both E5001 entries, the guide's arena section, the
  statements reference, design.md, known-limitations, runtime-abi) now say the same. No
  compiler or runtime behaviour changes.
- **The canonical `models` fact states the Gate 2 withdrawal** (127.459, owner decision
  D4): `docs/about/canonical.md` §8 and `canonical.json` "models" now say the PASS of
  2026-05-24 was withdrawn on 2026-11-15, that Gate 2 has no standing verdict and Gate 3
  is void, and carry the 55.6% with its possibly-corpus-derived caveat.
  `check_canonical.py` Rule 2 now fails a present-tense "Gate 2 PASS" anywhere in the
  workspace unless the withdrawal is stated in the same window (generic correction
  markers do not excuse it). It also catches the table form `| Gate 2 | **PASSED** |`,
  "Gate 2 passed" in any case and "Gate 2 verdict: PASS", and the excuse has to tie
  "withdrawn" to the PASS or the verdict in the same sentence (or say "no standing
  verdict"); a nearby date or an unrelated withdrawn headline no longer excuses it.
- **The repositioning brief is v2 and is gated like any other page** (132.7, owner
  decision of 2026-11-15). `docs/about/positioning-2026-09.md` keeps the 19-word
  one-liner, drops "compact in whatever unit the model generates in" from the claim
  sentence and "compact to emit" from the paragraph (now 79 words), replaces every number
  in §3 with a pointer to `docs/metrics-baseline.md`, states Gate 2 per D4 and Gate 1's
  token criterion per D3, and dates the falsification rules per D7. Its entry in
  `check_metrics_claims.py`'s `PENDING` list is deleted, so rules 1 and 2 now fail it
  instead of warning. The canonical block and its downstream copies still carry v1's
  sentence and paragraph until 132.154 copies v2 everywhere at once.
- **The canonical block carries v2's claim sentence and 79-word paragraph** (132.154,
  owner decision of 2026-11-15). `canonical.json` (`purpose_one_sentence`,
  `blocks.paragraph`) and `canonical.md` §3/§9 drop "compact in whatever unit the model
  generates in" and "compact to emit"; this repository's README, the whitepaper's quotes
  and the registry-descriptions note are re-copied. `make check-canonical` now enforces
  v2 on every declared surface, so each sibling copy (repo READMEs, model cards, the
  site's home page and `llms.txt`) reports DRIFTED until its own repository re-copies it.
- **`make check-docs` compiles the tutorials' fragments** (127.486).
  `scripts/check_tutorial_fragments.py` requires every toke fence in `docs/tutorials/`
  to be a whole-line excerpt of one file under `examples/` (which `make check-examples`
  builds), or to pass `tkc --check` inside the program a `<!-- fragment-context ...
  {{fragment}} ... -->` comment above it declares, or to be a std-only program that
  checks. Anything else fails, with a self-test as its negative control; on the tutorials
  as they stood before 127.472 it reports 18 failures. `static-site.md` (ooke page code,
  whose modules are not in this repository) warns under story 132.156 instead.
- **The security guide recommends an escaper that exists** (132.157).
  `docs/security/building-secure-apps.md` told readers to escape hand-built HTML with
  `html.escape`, a quarantined `std.html` export with no implementation (`tkc` rejects
  the call with E4027). It now recommends `tpl.escape` from `std.template`, says when
  `tpl.render`'s positional escaping is needed instead, and its example is a complete
  program that `make check-docs` builds. The `str.concat` examples beside it used `,`
  as the argument separator and now use `;`.
- **The security guide describes the escaping that ships** (132.158).
  `docs/security/building-secure-apps.md`'s "Read this first" banner said HTML
  escaping was opt-in and auto-escape not yet shipped, and its OOK-02 section said
  `ooke`'s `{=title=}` performs no HTML encoding and to add `|escape` everywhere.
  `ooke` 3.0.0 escapes template output for its position by default, and `tpl.render`
  does the same since 121.37, so following the banner escaped values twice. Both now
  describe the shipped behaviour (OOK-02 closed): leave the default, keep `|raw` for
  HTML you produced, and `|md` is written unescaped behind `std.md`'s safe mode,
  whose limits are stated. The checklist line follows.
- **`std.html`'s docs point at an escaper that exists** (132.159). `docs/stdlib/html.md`
  told readers to pass untrusted text through `html.escape`, which is quarantined
  (E4027). It now names `tpl.escape` from `std.template`, with a program
  `make check-docs` builds.
- **The static-site tutorial is ooke 3.x, and its page code is checked against ooke**
  (132.156). `docs/tutorials/static-site.md` taught an ooke that does not exist: page
  handlers calling `store.slug`/`store.all`/`tpl.renderfile` (not ooke's API, and
  `req.param("slug")` read a `T!E` as its value), `{% for %}`/`{% if %}` template
  tags, chained filters and `site.title`. Its pages are now the route markers ooke 3.x
  reads (`m=pages.blog.slug;`), its templates use `layout`/`block`/`yield`, its posts
  live in `content/blog/`, and every step was built with `ooke build` from ooke main.
  `scripts/check_tutorial_fragments.py` gains a rule for ooke page fences (a `m=pages.`
  module or an `ooke.` import): `tkc --check -I <ooke>/src` against `$OOKE_DIR` or a
  sibling `toke-ooke/` checkout; with no checkout the page is a WARN, not a pass. The
  page's `UNCHECKED` entry is deleted. On the old page the rule reports both fences
  (E4036 `req.param`, E3011).
- **toke-model's Gate 2 statements are enforced copies of the canonical `models` fact**
  (127.495, owner decision D4). `check_canonical.py` now requires the fact word for word
  in toke-model's `README.md`, `huggingface/README.md` and `ollama/README.md`, which were
  re-copied on toke-model `docs/127.495-models-fact`; until that branch merges, the
  check fails against toke-model main, whose copies lack the withdrawal and the caveat.
- **`docs/spec/stdlib-signatures.md` is held to the `.tki` interfaces, which are
  normative** (127.456). `scripts/check_stdlib_signatures.py`, run by `make check-tki`,
  fails on a hand-written entry that names a non-export or states a parameter count or
  type that differs from the `.tki`; the page's last section is generated from the
  interfaces for the 41 modules that had no block (`make render-stdlib-signatures`),
  and for every export a hand-written block leaves out (108 functions in 16 modules,
  e.g. most of std.str, std.time and std.http), so a new export in any module fails
  `make check-tki` until the page is re-rendered or the block is extended.
  38 drifted entries were corrected to the interfaces (std.str, toon, yaml, i18n,
  file, env, process, time, log, test); no `.tki` or runtime signature changed.
- **The stale `tree-sitter-toke/` copy is archived** (127.457); the maintained grammar is
  `tree-sitter-toke/` in karwalski/toke-spec.

- **The pattern catalogue gains `str-char-read`: one character is read with
  `s.charcode(t;i)`, not a one-character slice** (131.143; owner answer R2, 2026-11-12).
  Since 127.462 `t.slice(i;i+1)` is `str!$sliceerr`, so a corpus record that compared one
  character as a slice no longer checks; R2 routes those records to agent repair with a
  catalogue pattern. Two forms were measured through the catalogue process (fixtures
  `patterns/str-char-read/{a,b}.tk`, byte-identical output; tokens by `count_tokens.py`,
  runtime by `bench/patterns/run_patterns.py`, verdicts derived by `render_catalogue.py
  ingest`): `s.charcode(t;i)==44` is canonical (best on both axes), `mt t.slice(i;i+1){..}==","`
  is more tokens and slower. Recorded under load, so the entry is provisional with
  `load_warning` until the 131.25 re-measure. The card gains the rule
  `one char: s.charcode(t;i)==44, no slice`; the spec page, guide chapter and card snippet
  are re-rendered and the must-merge inputs re-pinned. toke-tokenizer's must-merge list is
  re-derived (branch `fix/131.143-mustmerge`; merge both together).

- **The pattern verdict tooling implements the identical-code tie, and acc-array is
  re-measured** (127.440, 127.437; owner decision 4.11 (d), 2026-11-15). `derive_verdicts`
  (`scripts/patterns/validate_catalogue.py`, used by `render_catalogue.py ingest`) now
  treats forms with an `identical_code` proof (new optional catalogue key, written by
  `scripts/patterns/prove_identical_code.py`: identical `tkc -O2` text under the measured
  tkc, plus the conformance test pinning the lowering) as runtime-tied, and a tie keeps the
  incumbent. Identical-code forms that are not token-tied, and a tie with no incumbent among
  the tied forms (a first measurement), are refused for an owner verdict, not resolved
  (127.501). acc-array was re-measured on tkc `0558545d5b78` (under load, so it stays
  provisional) and ingested: `x=x.append(v)` stays canonical, `x=x+@(v)` is now runtime
  `best` beside it instead of the stale 2.8.0 timeout, and the card rule reads
  `x=x.append(v)` without the old "never x=x+@(v)" clause. toke-tokenizer's must-merge
  list is re-derived (branch `fix/127.440-mustmerge`; merge both together).

### Changed — scoped capability grants (spec amendment 138.19)

The owner accepted D1–D4 of `docs/decisions/138.19-scoped-capability-grants.md` as
written on 2026-11-15 (register item 3.4); D5, OS-level enforcement, is 138.44.
loke's T-59 / B9. A grant is now the whole class **or a list of scopes** — fs paths,
`connect:HOST[:PORT]` / `listen:ADDR:PORT`, commands, env keys — and a binary that
bakes grants can no longer be widened by whoever launches it. `true`, a bare
`--allow-x` and `--allow-all` behave exactly as before. Normative text:
`docs/spec/capabilities.md`.

#### Breaking changes (with migration)

| # | Change | Story, commit | Migration |
|---|---|---|---|
| 1 | **A value on a capability flag is a scope.** `--allow-read=/srv/www` grants `fs.read` under `/srv/www` only (paths compared component by component after `realpath`; a `..` or symlink escape is denied); before, the value was ignored and the flag granted every path. The same for `--allow-write`, `--allow-net` (`connect:HOST[:PORT]`, `listen:ADDR:PORT`), `--allow-run` (a command) and `--allow-env` (a key or `PREFIX*`), at compile time and at run time; a flag may repeat or list scopes with commas. A value that is not a valid scope (`--allow-net=api.example.com`) is refused: at compile time as an error, at run time with **CAP003** before `main`. Flag names now match exactly (`--allow-readers` is no longer `--allow-read`). | 138.19 — `a4350649`, `4e17ab3f` | Drop the value for the whole class (`--allow-read`), or write the scope you mean (`--allow-net=connect:api.example.com:443`). No caller in toke, toke-ooke or toke-website passed a value. |
| 2 | **Run-time flags narrow a baked grant set** (D2). A binary built with any grant (a class key or `all` in `tkc.toml [capabilities]`, or a compile-time `--allow-*`) admits a run-time-flagged class only where both the baked grant and the flag allow it; a class no flag names keeps its baked grant. A run-time flag can no longer add a class or a scope, and `--allow-all` on such a binary is refused (CAP003, exit 1) unless the baked set is already everything. A binary with no baked grants — and one built with only `enforce` / `--cap-enforce`, which bake none — takes its run-time flags as the grant, as before. | 138.19 — `a4350649`, `4e17ab3f` | Declare every class the program needs in `[capabilities]`; to let a launcher widen the baked set, add `runtime = "union"`. |
| 3 | **Every capability sink checks the value it uses** (a scoped gate at each acquisition site), and the class-only check passes only for the whole class, so a scoped grant fails closed wherever the scope cannot be checked. Under a scoped grant a value the sink cannot use (a null path, an unparseable URL) is denied, and `db.open(":memory:")` is checked as a path. | 138.19 — `a03e974a` | Grant the scope the sink names in its CAP002 line. |
| 4 | **Under a scoped fs grant, sinks that open more than the value they are given are checked on what they open.** `file.listglob` returns only matches found through directories inside the scope: a match reached through `..` (which `.?`, `.*` and `.[.]` match) or a symlink that leads out is left out, not reported. `db.open` refuses an SQLite URI filename (`file:...`, CAP002, no fix). `ATTACH DATABASE`, `VACUUM INTO` and `PRAGMA temp_store_directory` go through the same gates as `db.open` (CAP002 and exit), and an `ATTACH` with a computed file name is refused. The whole classes behave as before. | 138.19 — `c952a84b` | Name the database by its path (`data/app.db`), write an `ATTACH` file name as a string literal, and grant the directories a glob walks. |
| 5 | **Under a scoped grant, a path is checked on the way to its target, and four more ways round an fs scope are closed** (138.19 review, round 3). A path that enters a directory outside the scope before coming back in (`www/../outside/../www/f`, or out and back through a symlink) is refused with CAP002 naming that directory, and no fix, though it ends inside; the same for a command path given to `process.spawn`. `file.listglob` refuses a pattern holding `\` (glob reads it as an escape; CAP002, no fix) and checks each match's text. `http.servepages` skips a page entry that leads out of the scope and fails the render (500) of a page or layout that does. `log.openaccess` / `log.openerror` need the log's directory under a scoped `fs.write` grant (they create a `.lock` sidecar and rotated files beside the log). The whole classes behave as before. | 138.19 — `10e7fd50`, `5c5e7dcf`, `04e226d5`, `323378c1`, `9c766d25` | Name the target without going through outside directories; write a literal `*`, `?` or `[` in a glob as `[*]`, `[?]` or `[[]`; grant a log's directory, not the log file alone. |
| 6 | **A CAP002 `fix` is offered only when applying it admits the call** (138.19 review, round 4). A path that ends outside a scope and also passes through an outside directory (`www/../outside/../other/f`) used to get its target's fix, which stopped again at the outside directory once applied; its CAP002 now names that directory, with no fix. The same for a `process.spawn` command path and a `file.listglob` pattern's literal directory. `log.openaccess` / `log.openerror` check the log's directory before the log, and the directory's fix comes only when the log resolves inside it. | 138.19 | Name the target without going through outside directories, then apply the fix the next CAP002 gives. |

#### Added

- **`tkc.toml [capabilities]` scope lists:** `fs_read = ["./content", "/etc/ssl/certs"]`, `net = ["connect:api.example.com:443", "listen:127.0.0.1:8080"]`, `process_spawn = ["/usr/bin/git"]`, `env_write = ["TZ"]` — one line, quoted strings, no trailing comma; a malformed array, an unknown scope prefix or an empty string is a parse error naming the line. `runtime = "union" | "narrow"`. A `# comment` may follow a value. (138.19 — `4e17ab3f`)
- **CAP002** (D3), "capability granted, but not for this scope", a new runtime code beside CAP001. Its `TK_DIAG_JSON` line carries `capability`, `scope` (the canonical path, `connect:`/`listen:` host and port, resolved command or key) and a `fix` naming that exact scope as a run-time flag or a manifest line — whichever would admit the call — and none where no single edit would. CAP001 is unchanged, except on a narrowing binary, where its hint and `fix` name the manifest (the run-time flag it suggested cannot add a class there). The text lines escape control characters in the value they print (a newline in a path cannot forge a diagnostic line), and when no fix fits the second line says why. (138.19 — `a4350649`, `8ceed01a`)
- **CAP003**, "run-time capability flag refused": a malformed `--allow-x=VALUE`, `--allow-all=VALUE`, or `--allow-all` on a narrowing binary stops the program before `main` with `CAP003: <flag> refused: <reason>` and, under `TK_DIAG_JSON`, a JSON line with `flag` and `reason` and no `fix`. (138.19 — `8ceed01a`)
- **W9012** (D4, the warning release): a quoted-string grant (`fs_read = "/srv/www"`) **keeps its whole-class meaning in this release** and is reported, naming **toke 3.4.0** as the release in which it becomes a one-element scope list (the switch is 138.52). Write `true` for the whole class, or the array form. (138.19 — `4e17ab3f`)
- `--show-limits` prints `cap-runtime` and one `cap-scope` line per scope; the `--companion` record lists each class with its scopes and the run-time mode. (138.19 — `4e17ab3f`)
- `scripts/check_capability_sinks.py` (C470) fails with `CLASSONLY` when a sink is reached only through a class-only gate. (138.19 — `a03e974a`)

#### Fixed

- **`all = true` in `tkc.toml [capabilities]` parses.** It was refused as "unknown capability 'all'": the unknown-key sentinel was the same all-ones value as `TK_CAP_ALL`. (`bdca26cd`)
- **The `--companion` record no longer calls a binary built without `enforce = true` "advisory (allow-all)"**; enforcement is the default and `enforce` is vestigial. (138.19 — `4e17ab3f`)
- **`--diag-sarif` locates each result in the file it was emitted for.** The flush used the file current at the end of compilation, so W9012 (emitted for `tkc.toml`) was reported in the `.tk` source at the manifest's line number. (138.19 — `2f03cde1`)
- **Documentation: `enforce = false` does not opt a build out of enforcement** (loke T-59 point 6). `docs/reference/capabilities.md` said it did; a binary built with only `enforce = false` has always stopped with CAP001 at its first ungranted call. The only opt-out is `all = true` / `--allow-all`.

Not changed (not covered by the design note): `env = true` is still the short
spelling of `env_write`, with no warning (loke T-59 point 5).

Tests: C1880–C1886, D615–D616, C470's new CLASSONLY control (`4d2cb26d`, `c952a84b`, `8ceed01a`, `2f03cde1`, `541c388d`).

- **`--check` refuses a field read on a record whose module is not imported (E4034)**
  (138.40, loke T-39). `let r=srv.handle(x); <r.text`, where `handle` returns
  `$res` of a module `proto` this file does not import, passed `--check` while
  the compile refused it with E4034. A type is in module scope only through an
  import (`docs/spec/semantics.md` 4.1, 6.1), so `--check` now refuses it too,
  bound or read in place, naming the call and the record; no `fix` (which import
  supplies the record is not decidable from the file). Measured before landing
  with `--check` against `b518b41c`: no change in any of 4,325 sources (the toke
  tree, ooke `main`, toke-website, toke-test-programs); ooke
  `make check-toke-candidate` and the website's `make check-toke` pass.
- **`unused-import` no longer reports an import used only as a struct-field type,
  or needed only for a field read** (138.39, loke T-39). A type named inside a
  `t=` field (`t=$holder{items:@$item}`, `it:$item`) was taken for this file's
  own, and an import needed only so `r.text` resolves has no name in the file;
  `--lint --fix` deleted both, and the next check was E3011 or E4034. Only a
  `t=`'s declared name and field labels now count as local. And when the file
  type-checks, an import with no syntactic use is reported only if the file
  without it (and without every import already reported) still type-checks
  clean. That re-check sees only what `--check` judges: an import that only
  codegen needs (an array of another module's record, a `T!E` payload) is not
  seen, so the 137.8 caution stays in force in every file -- a foreign
  unqualified `$type` still keeps all of its imports. A genuinely unused import
  in a file without one is still reported and fixed. Unchanged from 3.2.0, and
  open as 138.58: in a file without a foreign `$type`, an import that only
  codegen needs can still be reported.
- **`args.get` no longer formats its error message on every miss** (127.470).
  Since 113.B.8 the glue ran `vsnprintf("args.get: %s")` into the message ring
  for each out-of-range index, so the canonical default form
  `mt args.get(1){$ok:v v;$err:e "default"}` ran about 11x slower than the
  `args.count()` check (pattern bench 2026-11-15: 1016 ms vs 89 ms). The one
  reason `args_get` reports is `ARGS_ERR_OOB` (now exported by `args.h`,
  recognised by address), so the box holds the static literal
  `args.get: index out of bounds` (same text; any other reason is still
  formatted). Measured on the cli-argv fixtures at `PAT_N=16384000`, user
  CPU: form a 1.11 s (3.2.0) -> 0.19 s, form b 0.07 s; the pattern bench
  (`run_patterns.py --pattern cli-argv`, N=8,192,000, load ~45) a 662 ms ->
  143 ms against b 84 / 62 ms. The rest of the gap is the error-box raise
  itself. The catalogue entry is not re-ingested (127.461). One visible
  difference, a fix: a let-kept `args.get` error read after eight or more
  later raises that use the ring (`toml.*`, `file.*`, ...) printed one of
  their messages; it now keeps its own. Conformance: C1980.
- **Every std raise is cheaper: the error box is zeroed without a call**
  (127.492). `tk_err_box`, which every std error return calls, zeroed the box
  with a variable-size `memset` (a libc call and a heavier prologue on every
  raise). A box that fits the inline 64 bytes, which is every 2-slot sum and
  every record of up to 8 fields, is now zeroed with a constant-size store;
  the buffer's capacity is never below 64 once it exists, so the contract
  (a zeroed box of at least `nbytes`, one reused buffer per thread) is
  unchanged. Measured on Darwin arm64, cli-argv form a
  (`mt args.get(1){$ok:v v;$err:e "default"}`, all misses), `PAT_N=16384000`
  user CPU: 0.20 s -> 0.12-0.13 s, against form b's 0.06 s (a payload-less
  raise, the floor without any box, measures 0.09 s); the pattern bench
  (`run_patterns.py --pattern cli-argv`, N=8,192,000, load ~18, so
  `load_warning`) a 137.8 ms -> 90.5 ms against b 57.3 / 64.0 ms, from 2.4x
  b to 1.4x. The rest is the two thread-local slots (`tk_current_error` and
  the box), each a `tlv_get_addr` call on Darwin, which the emitted code
  addresses by name. The catalogue entry is not re-ingested (127.461).
  Conformance: C2031.
- **A std raise makes one thread-local lookup in C, not two** (127.504). The
  slot `tk_current_error` and the error box's buffer are now one thread-local
  object (`TkErrTls {slot, buf, cap}` in `tk_runtime.h`), and `tk_err_box`'s
  fast path is inline in the header, so a glue function's raise reaches the
  box and the slot through one `tlv_get_addr` on Darwin. The slot is the
  object's first member and the symbol is still `tk_current_error`, so the
  emitted code (`@tk_current_error = external thread_local global i64`) is
  unchanged. Measured on Darwin arm64 (user CPU, min of 9, `PAT_N=65536000`,
  load 7-50): cli-argv form a 0.46-0.52 s -> 0.35-0.41 s, form b
  0.26-0.28 s -> 0.24-0.26 s (a/b from about 1.85x to 1.5x); at
  `PAT_N=16384000` a 0.12 s -> 0.10 s against b 0.06 s. The emitted code's own
  raise (a user `$err`) still calls the out-of-line `tk_err_box` and stores
  the slot itself. For C: a file that includes `tk_runtime.h` must not
  redeclare `tk_current_error` or `tk_err_box` (11 glue files did; they now
  include the header), and a test that stands in for the runtime defines the
  `TkErrTls` object, not an `int64_t`. Conformance: C2086.
- **A trailing `;` before `}` in a type declaration is one E2002, not four
  diagnostics** (127.339). `m=a;t=$p{x:i64;};` reported E2002, E2004, E2002,
  E2002: `parse_field_list` stopped before the `;` without consuming it, so
  the caller's `}` check cascaded, while its comment claimed the form was
  allowed. grammar.ebnf (`FieldList = Field { ';' Field }`) admits no
  trailing `;`, so it is still refused, now with ONE E2002 at the `;`
  ("trailing ';' before '}' in a type declaration ..."), no `fix` (E2002
  carries none), and parsing continues after it. Nothing that compiled
  before changes. Whether `;`-separated lists take a trailing `;` at all is
  the open owner question 127.360. Conformance: C1981, D660.

- **Module-level `let` globals read back their value** (127.469; silent
  wrong results before). A global is held in an `i64` cell, and every read
  was an `i64` with no conversion to the value's type: `let y=1.5;` then
  `"\(y)"` printed 4609434218613702656 (1.5's bits), `let z=y+0.0` added the
  bits as an integer, and a global `byte`/`i32`/`bool` read was untyped.
  A read now converts to the global's type (its annotation, else the
  checker's type of the initialiser), as the initialiser was converted when
  stored. A global function value (`let h=fn(x:i64):i64{<x+1};`) was never
  recorded as one by the checker, so `h(1)` lowered as its argument (1) and
  a zero-argument one failed the build with E9003; it is now called through
  like a local function value, from a function or from another global's
  initialiser, and its calls are judged against its signature (wrong arity
  E4026, a mistyped argument E4031, no fix). A copy of one (`let q=ad;`,
  at module level or in a function) carries its signature too; `q(2)` also
  lowered as its argument. Inside a module-level closure the closure's own
  parameters and locals now shadow a global, as they do in a function: a
  call through a function-typed parameter (`let kp=fn(p:(i64):i64):i64
  {<p(4)};`) lowered as its argument, and a parameter, local or loop
  variable named like a global was judged against the global (false E4026
  and E4031). A closure parameter that shadows a module name is typed as
  the parameter in a function too (`fn(ad:i64):i64{<ad*2}` beside a global
  or function `ad` was a false E4031). An assignment inside a
  module-level closure (`let k=fn(){g=7;..}`) now gets 127.465's E4070 and
  its fix. Same-named globals in two modules each read their own value.
  `--check` on the 313 workspace sources that declare a module-level `let`
  (toke tree, ooke main, website, toke-test-programs), b518b41c against this
  compiler: same diagnostics in all. grammar.ebnf `SourceFile` now lists the
  module-level `let` globals between the constants and the functions, where
  the parser and `toke.gbnf` already had them (no production added).
  Conformance: C1940, D645, D646.

### Build, platform and release

- **The release archives carry the release build of `tkc`** (127.490). `release.yml` built
  the release binary in place (`BAKED_STDLIB_DIR=src/stdlib`, `-DNDEBUG -g0`), and the next
  step's `make check-site-compile`, whose target depends on the binary, saw the build flags
  change back and rebuilt it with the development flags; the packaging step then copied
  that rebuild. The 3.2.0 archives' `tkc` bakes the runner's absolute stdlib path
  (`/Users/runner/work/toke/toke/src/stdlib` in the macos-arm64 archive) and was built
  without `-DNDEBUG`. The release binary is now built in a pristine worktree of the same
  commit and packaged from there, and the website examples are compiled with the `tkc`
  unpacked from the tarball, the bytes that ship.
- **The signatures and SBOMs cover the published archives** (127.453). Through 3.2.0 the
  release job rebuilt `tkc` on ubuntu-latest and signed (`tkc.sig`) and SBOM'd
  (`tkc-sbom.spdx.json`) that rebuild, which is in none of the four archives. The job now
  downloads the four archives the build jobs of the same run uploaded and writes, per
  archive, an SPDX SBOM (`<archive>.spdx.json`) and a cosign keyless bundle
  (`<archive>.cosign.bundle`), plus `checksums.txt` over every archive and SBOM, itself
  signed (`checksums.txt.cosign.bundle`). Each bundle is verified with the published
  command and the tamper test runs on each signed file before anything is published.
  `tkc.sig`, `tkc-sbom.spdx.json` and its `.sha256` are no longer release assets.
  Publishing is still tag-only; a manual run with `sign_test` signs and verifies without
  publishing (it writes a Rekor log entry). A manual run may set `skip_gates` to test the
  pipeline on a branch whose gates are red for other reasons; it is refused on a tag ref
  (the run fails at its first step), and the publish step never runs for a run that set it.
  Verification: `docs/security/release-signing.md`.
- **Lower platform floors for the release binaries** (127.454): **glibc 2.35** (Ubuntu
  22.04+, Debian 12+, Fedora 36+) and **macOS 14**, down from 3.2.0's glibc 2.38 and
  macOS 15, which were the floors of the runner images. The Linux release binaries are
  built in an `ubuntu:22.04` container (gcc-12, so `_FORTIFY_SOURCE=3` is still enforced;
  the baked clang and linker are stated as `/usr/bin/clang` and `/usr/bin/ld`), the macOS
  ones with `MACOSX_DEPLOYMENT_TARGET=14.0`; a step checks each binary's newest `GLIBC_x`
  symbol version or Mach-O `minos` against the floor, and the release notes state the
  floors from the same workflow variables. The full conformance suite still runs on the
  development build on the newest runner images; the shipped `tkc`, unpacked from its
  tarball, now builds and runs a program on every run (from outside the checkout, so the
  baked stdlib and clang paths are exercised) and passes the YAML conformance cases
  (`conform-check`) on a tag. The floor values are the conservative choice pending owner
  decision R4.
- **The perf gate measures on CI** (127.451). `bench.yml`'s perf-gate failed with "harness
  failure: ./tkc exited 0" on every pull request: GitHub's macOS runners are VMs without
  performance counters, `/usr/bin/time -l` prints no "instructions retired" line there,
  and `scripts/perf_gate.py` took any macOS host as having a counter. The gate now probes
  each counter before using it (macOS `/usr/bin/time -l`, Linux `perf`, then valgrind's
  cachegrind, which counts instructions exactly in software), records which counter made
  a baseline and refuses to compare across counters, and runs once per count under
  cachegrind. The CI gate runs on ubuntu-latest under cachegrind with `--require-host` on
  both the record and the compare step (no counter is a failure, not a SKIP), and can be
  dispatched against a named base. Allocation counts stay macOS-only.

---

## 3.2.0 — loke's second handoff, a checker that closes silent-wrong paths, and packages

**Status: released as `v3.2.0` on 2026-11-15 (story 138.17).** Cut from main
`fffec674` on 2026-11-14 under owner decision
2026-11-12 §5.2 (a); 985 commits since `v3.1.0` (`86c432da`, 2026-09-30), 683 of
them not merges (`git log v3.1.0..fffec674`). Every entry below names the tracker
story in `docs/progress.md` and a commit, so a consumer can bisect.

As with 3.1.0 this is a minor version by the letter of semver, and **Breaking
changes** come first: most of the release is the checker rejecting, at `--check`,
programs that 3.1.0 compiled and then ran wrongly (printing an address, reading a
slot nobody wrote, or segfaulting). A program that 3.1.0 compiled *and ran
correctly* mostly still compiles; the migrations below cover the ones that do not.

Not in this release: the compact `[byte]` buffer (135.18) and the `[byte]` into
`@f64`/`@str`/`@i64` rejection that rides with it (127.342) are on the held
integration branch, not on main; `let y:@f64=s.bytes("ab")` still checks clean in
3.2.0.

### Breaking changes (with migration)

| # | Change | Story, commit | Migration |
|---|---|---|---|
| 1 | **`std.tls` clients verify the server by default.** `tls.connect` with `tls.tlsconfig`, or a `$tlsconfig{...}` literal without a pin, now checks the server's chain against the system trust store (`SSL_CERT_FILE`/`SSL_CERT_DIR` override it) and checks the host name (DNS name, or IP address for an address literal). A self-signed, expired, wrongly named or untrusted certificate now makes `tls.connect` return `0`; before, any certificate was accepted (audit-120 TLS-02). Pinned configurations (`tls.pinconfig`, `tls.mutualconfig`) are unchanged. `tlsconfig` gains a fifth field, `insecure`; a literal must give it (E4077, #9 below). An IP-address `host` is no longer sent as SNI. | 121.64 (owner decision 2026-10-01 §3.1) — `781c32b8`; 127.365 — `6e67239a` | Install the CA or point `SSL_CERT_FILE` at it; to trust one self-signed peer, pin it with `tls.pinconfig("";"";peercert)`; to keep the old unverified connection, say so: `tls.insecureconfig(cert;key)` (new). A `$tlsconfig{...}` literal writes `insecure:false`. No caller in toke, toke-ooke or toke-website relied on the old behaviour. |
| 2 | **Text-format log lines are logfmt.** The message is now the field `msg=`, quoted by the same rule as every value (quoted when empty or holding a space, `=`, `"`, `\` or a control byte, with `\"` `\\` `\n` `\r` `\t` `\xHH` escapes): `[INFO] msg="request received" request_id=abc123`. Before, the message was written bare, so a message holding ` user=admin` read as a `user` field. JSON-format logs and the access logs are unchanged. An unknown `log.accessformat` name is now echoed escaped. | 121.41 (3) (owner decision 2026-10-01 §3.2) — `1cf24940` | A reader of text logs that took everything after `[LEVEL] ` up to the first `key=` as the message should parse the `msg` field instead. |
| 3 | **`ocr.lasterr`, `ocr.lasterrkind`, `pdf.lasterr`, `pdf.lasterrkind` and `xlsx.lasterr` are retired.** They duplicated what the `$err` arm has bound since 127.241: the `$ocrerr`/`$pdferr`/`$xlsxerr` variant (the old kind token) and its message. Calling one is now E4027. `ocr.setlanguages` still returns `false` for an unknown tag or a missing engine; `ocr.isavailable()` tells those apart. | 127.246 (3) (owner decision 2026-10-01 §3.3, as 127.102 for csv/file/zip) — `18be0e31` | Match `$err:e` and read the variant and its payload: `mt e {$encrypted:m …; $baddoc:m …}` |
| 4 | **The stdlib `.tki` type names are lower-case.** The 40 upper-case types (`FileErr`, `DbErr`, `JsonErr`, `ProcessErr`, `TimeParts`, `SecureBuf`, ...) are lower-cased (`$fileerr`, `$dberr`, ...), and the seven whose lower-case spelling is a module or a sub-namespace are named `<module><noun>`: `Json` → `$jsonval`, `Row` → `$dbrow`, `Toon` → `$toonval`, `Yaml` → `$yamlval`, `Vec` → `$vechandle`, `VecStore` → `$vecstorehandle`, `Duration` → `$timeduration`. A bare `e:DbErr` no longer compiles. A `$name` that names only a module alias is now E3011: `$json`, `$row`, `$toon` and `$yaml`, which the module pages documented, compiled against no type at all (a `b:$json` parameter accepted a str). | 127.251 — `16c3088d`, `6cb3ae6f` | Write the `$` spelling. Every old spelling is E3011 with `fix` "did you mean '$dberr'?" when its module is imported, and `tkc --migrate` rewrites bare and `$` old spellings (not `$row` beside `std.ml`, which has its own `row`, nor a name the program declares as its own type). |
| 5 | **A match-arm label must name a type or a variant.** A label that names nothing was never checked: beside a recognised label it lowered by position (`{$i64:v v;StrErr:e 0}` worked, `{StrErr:e 0;$i64:v v}` gave a wrong value), and with none recognised (`{Handle:h ..;ProcessErr:e ..}`, the pre-127.251 spellings) the build died with E9005 after `--check` passed. It is now E3011 at `--check`. | 127.292 — `d3d4d73e`, `dd9821ec` | Spell the label as the type or variant it means. An old upper-case stdlib type spelling (`ProcessErr:e`) carries `fix` "did you mean '$processerr'?" when its module is imported, and `tkc --migrate` rewrites it; `StrErr` was never a type -- `s.toint`'s error is `$parseerr`. |
| 6 | **Opaque std handles are judged by their declared `.tki` type** (loke T-60, the spawn half). A field-less std record (`handle`, `vechandle`, `strbuf`, `tomlval`, `pdfdoc`, `xlsxbook`, `ziparchive`, `securebuf`, `svgelem`) passed wherever one was declared, whatever the value: `process.wait("abc")` built and read the string's bytes as a process handle. A str, float, array, map or other record where a handle is declared (or a handle where one of those is) is now **E4031**; a `$err` arm of `@()` or a string beside `$ok:h h` is **E4011**. `j.enc`/`j.print` of an opaque handle, which died with E9002 at build, is E4031 at `--check`. | 138.21 — `daab5644`; 127.402 — `839ad02a` | Use `process.badhandle()` as the failure value (`$err:e process.badhandle()`), or return from the `$err` arm; encode values read from a handle, not the handle |
| 7 | **An expression-`if` whose two branches have different known types is E4011** ("if branches have inconsistent types"). `let v=if(n>3){ n }el{ "s" }` checked clean and yielded the string's address as an i64. Unknown and void tails, `u64` beside `i64` and integer literals that fit are tolerated; `el if` chains are checked link by link. | 138.22 — `433f469f` | Make both branches yield the same type |
| 8 | **A `mt` over a std call that returns an inline `record!err` types its `$ok` payload as that record** (`httpresp`, `jsonval`, `timeduration`, `csvrow`, `dbrow`, `wsmsg`, ...). `{$ok:x x; $err:e "oops"}` and `$err:e 5` are now **E4011**, `x.nosuch` is **E4025**, and binding the result to a `str` is **E4031**; `let j=mt json.dec("{bad"){$ok:v v;$err:e "oops"}; io.println(j.raw)` segfaulted. `$err:e 0` stays legal (the null filler). Reading `r.status as i64` now draws W1001, since `status` is `u64` in `http.tki`. | 138.33 — `82ac08d4`; examples fixed in `5745bfa3` | Return from the `$err` arm (`$err:e <http.resjson(400;..)`), or give it a value of the record type; pass the payload as its record type (`o:$jsonval`, not `o:i64`) |
| 9 | **Struct literals are checked field by field.** A literal must give every declared field (**E4077**, new, naming each omitted field) and each field once (**E4078**, new); each value is held to its declared field type (**E4031**). Before, an omitted field read whatever the slot held (`$r{a:5}` printed `r.b` as 1), a repeated field kept the last write, and `$r{a:"x";b:3}` for `a:i64;b:f64` printed a pointer and a denormal. Sum-variant literals (local, `$err` boxes, void payloads) are exempt. | 127.365, 127.366 — `63facdc3`, `41cee56a`; 127.356 — `7d8d1e76`, `37efd5d0` | Give every field exactly once, with a value of its type (`$tlsconfig{…; insecure:false}`) |
| 10 | **More silent-wrong programs are new check-time errors:** `x as str` on a record, sum, array or map is **E4079** (new; it read the value as a C string); `mt` on a record value with `$variant` arms is **E4080** (new; it failed at build); a field read on a `str` other than `.len` is **E4034** at `--check` (it failed at build, or segfaulted); re-raising a user error into a different error type (`inner()!$eb` where `inner` declares `!$ea`) is **E4076** (new), with **RT010** as the runtime backstop where the checker cannot judge (owner decision 2026-10-01 §2.3); assigning to a closure parameter or a `mt` arm binding is **E4070**. | 127.362 — `c3173af9`; 127.367 — `7f8c3a6a`, 127.380 — `b6b53bb0`; 127.430 — `a8dc06fc`; 127.225 — `7afd78e4`; 127.281 — `729515d1` | Interpolate a field instead of casting the record; match the sum, or read the record's field; declare the field or drop the read; raise the outer error type explicitly; bind a fresh `let` |
| 11 | **Std-call and cross-module arguments are judged.** **E4031** now catches: a str passed for a typed `i64` std parameter (`str.fromint("x")`), and a std result that is a record or a `T!E` bound to a scalar-annotated `let` (127.304); a std `[T]` result, let-bound or `mut`-bound, reaching a `str` parameter (`s.len(s.split(..))`, 127.314, 127.315); a `json.parse` result as the document argument of a `std.json` reader (138.9, loke T-17); every argument of a call into another user module, including sum-typed parameters, and an imported sum's variant literal used as the wrong type (127.378, 127.382, 127.385). A `mt` over an `i64!E` std call compares its arms (**E4011**; `{$ok:n n;$err:e "bad"}` over `s.toint` segfaulted, 127.337). The `.tki` records that spelled a str parameter `i64` are retyped `str` (`json.getint/geti64/geti32/getbool/getf32/getfloat` key, `http.resok/resbad/reserr/resjson` body). The checker's exemption for a str where the glue reads a C string is gone (127.255). | 127.304 — `b738143f`, `5af5548f`; 127.314 — `10f5a079`; 127.315 — `d8a46b0a`; 138.9 — `12c71a58`; 127.378 — `63558d92`; 127.382 — `2ce71a0c`; 127.385 — `8f261a6f`; 127.337 — `3054de7f`; 127.255 — `8fd95d9f` | Pass the declared type; use `json.decode` for a document; match a `T!E` std result before binding it to a scalar |
| 12 | **A map lookup `m.get(k)` is the error union `V!$err`.** `m.get(k)+1`, a lookup passed or returned as an `i64`, and the same through `let v=m.get(k)` checked clean and used the 0 filler on a miss; they are now E4031 ("the left side is 'i64!$err'"). `!` on a lookup propagates on the error channel, so a stored `0`/`false` is a hit. Every accepted program lowers as before. | 127.244 (owner decision 127.30) — `42d669ed`, callers migrated in `5b419052` | `m.getor(k;default)`, or `mt m.get(k) {$ok:v …; $err:e …}` |
| 13 | **A `let` inside a block goes out of scope at the end of the block** (owner decision 2026-10-01 §2.2). Reading it after the block is E3011. Measured first: no corpus record, toke/ooke/website program or toke-test-programs file relied on the leak. | 127.196 — `fe71f602` | Declare the binding before the block (`let x=mut.0;`) and assign it inside |
| 14 | **Advice-only `fix` fields removed from E4031**, so a repair loop reading `fix` finds it absent where it was never a deterministic repair (AGENTS §6). The six binary-operator sites (array concatenation of mismatched element types or of an array and a scalar, `&&`/`\|\|` left and right, bitwise, modulo) and four more (unary minus, bitwise NOT, a map key of the wrong type, `[]` on a non-array) no longer emit `fix`; codes and messages are unchanged. Also: E4070 on an assigned function parameter no longer carries a `let x=mut.` fix (it is wrong for every parameter), and E2030 for an unresolved **user** module no longer guesses a stdlib module in `fix`. Still carrying prose, pending an owner decision (127.438): the non-integer index hint and the str `+` advice. | 127.436 — `984eb9ef`; 127.438 — `aff79119`; 127.281 — `4763bba2`; 127.309 — `5db3a4ad` | Read `message`/`expected`/`got` |
| 15 | **Toolchain and CLI.** tkc runs the clang found when it was built, by absolute path (baked into `build_info.h`), never a `clang` from `$PATH`; `TKC_CC` overrides it; a missing or non-executable compiler is **E9027** (new), exit 3 (owner decision §2.10 (c)). Every limit **E9010** exits **1** (a user error), not 2, and `--check` refuses the caps the build refuses. A declined `--migrate` is **E9026** (new) with exit **4**, source echoed byte for byte, and no longer stops a multi-file run. `tkc --version` appends the build commit, `toke 3.2.0 (git <sha>[, dirty])`, and `--version --json` prints `name`/`version`/`git_sha`/`git_dirty`. `tkc --help` names the binary `tkc`, not `toke`. | 121.28 — `7b2c1991`, `426d3ed5`; 127.264 — `eaeb9ff6`, `161d7dc5`; 127.197 — `e98150c2`, `6abd889e`; 131.33 — `096c48a9`; 127.352 — `102fd974` | A packager whose install moves clang sets `TKC_CC` (packaged installs: 132.122, owner decision §5.4); scripts that match `tkc --version` exactly read `--version --json`; treat exit 1 as a limit error and 4 as a decline |
| 16 | **Wider capability gates:** `i18n.load`, `template.load` and `vecstore.collection` need `fs.read` (each opened a program-chosen path with no gate). | 121.42 — `04654c6a` | `--allow-read` or a `tkc.toml [capabilities]` grant |
| 17 | Behaviour changes in other modules: **`tpl.render` escapes each value for its HTML context** (text, quoted and unquoted attribute, URL with an http/https/mailto scheme allowlist, JS string and code in `<script>` and `on*`, CSS), and `tpl.renderfile` now escapes too (it escaped nothing); `file.read` on a directory is `$err $isdir`, not `$ok ""`; `xml.*` and `soap.*` results are typed `str` and a miss is `""` (they typed as `i64`, and `xml.get` returned NULL); `std.vec` `get`/`set` outside `[0, len)` traps RT003 (it returned 0 or did nothing); the HTTP client answers `$err` for a response cut short — a header block with no blank line, a body short of its `Content-Length`, a chunked body that ends before its last chunk, a close-delimited body that ends in a timeout or reset (loke T-64); `log.info/warn/error/debug` return `true` when the line is written (they always returned `false`), and level names are case-insensitive; an HTTP/1.0 request without keep-alive closes the connection, gets no `Transfer-Encoding: chunked`, and a handler's `Connection: close` is honoured. | 121.37 — `be8086d0`; 121.65 — `0b4c4b96`; 131.48 — `44b1d9f6`; 121.31 — `ef4b101a`; 138.23 — `60c319af`; 138.31 — `1046f364`; 127.406 — `7ab40d50`; 127.407 — `c25e9b2b`; 121.102 — `38413d04`; 121.103 — `f66b5496`; 121.101 — `06410e29` | Mark a trusted value `{{x\|raw}}`; match `$isdir`; test `xml.get(..)==""` for a miss; treat a cut HTTP response as the error it is |

### Fixed for loke (the 2026-11-09 handoff, Epic 138)

- **T-62** (loke F10.42) — chained `.get` on a map of arrays, an `@@str` row
  subscripted inline as a call argument, no longer emits invalid IR (E9003).
  127.274 — `75209d81`.
- **T-64** — the HTTP client's cut responses (above, #17). 138.23 — `60c319af`;
  138.31 — `1046f364`.
- **T-48 addendum** — an `httpclient` passed where a `str` URL is declared
  (`http.postheaders(http.client(url);…)`, `http.geturl(http.client(url))`) is
  E4031, as a let binding already was. 138.26 — `aa6f65e7`.
- **T-60, the spawn half** — opaque handles (above, #6). 138.21 — `daab5644`.
- **T-17** — a `json.parse` result is not a JSON document (above, #11). 138.9 —
  `12c71a58`.
- The client's `Host` header names a non-default port (RFC 9110 §7.2). 138.28 —
  `a37cd26e`.
- Found while probing T-60: expression-`if` branch types (above, #7, 138.22) and
  the `$ok` payload of a std `record!err` (above, #8, 138.33).
- **Still open for loke**: `==` on arrays (T-60's other half, 138.20); a
  whole-request deadline and a body-size cap for the HTTP client (T-63, 138.24);
  connecting to a given address with host-name verification (T-63, 138.25); a
  header-carrying GET (T-58 addendum, 138.27); scoped capability grants (T-59 /
  B9, 138.19).

### Security fixes

- **TLS clients verify by default** (above, #1). 121.64.
- **Template injection** — per-context escaping (above, #17): `href="{{u}}"`
  passed `javascript:alert(1)`, and `<script>var s={{x}};` took the value as code.
  121.37 — `be8086d0`.
- **HTTP response splitting** — `std.http` drops handler response headers whose
  name or value could split the response (CR/LF), and HTTP/2 drops header values
  with any control byte, as HTTP/1.1 does. 121.92 — `912b9979`; 121.98 —
  `dcc599d9`.
- **Toolchain substitution** — tkc ran `execvp("clang")`, so a directory early on
  `$PATH` could substitute the compiler (above, #15). 121.28 — `7b2c1991`.
- **Hybrid post-quantum key exchange** — every `std.http` and `std.llm` TLS
  context sets an explicit group list that prefers `X25519MLKEM768`, falls back
  to classical groups, and fails closed. 122.15 — `ace7ed58`, `56745750`.
- **TLS, ACME and LLM hardening** — a TLS registry refcount and per-connection
  I/O lock, ACME storage, payload and renewal hardening (121.19 — `3eb9ebef`);
  TLS for `std.llm`, refusal of a cleartext key, escaped bridge JSON and a bounded
  `.vecs` loader (121.23 — `585889e1`).
- **Compiler front end** — parser nesting depth is bounded (one located E9010
  instead of SIGSEGV); the auto-glue temp file is created with `mkstemps`, not a
  pid name; a stdlib or runtime directory holding whitespace is refused (E9024,
  new). 121.28 COM-02/03 — `e9ac0498`, `71172560`, `a880238c`.
- **Runtime reliability** — process pipes drain while waiting, a request that
  cannot allocate fails cleanly, the request buffer grows on demand, and worker
  respawns back off. 121.29 — `6049cd97`, `481e3f0c`, `d84825d7`, `59f879a9`.
- **Over-reads** — `encoding.jsonfield` and `std.llm`'s JSON string decoder read
  past the NUL when a value ended in a backslash. 127.317 — `e7757ed1`; 127.331 —
  `ec32c74a`.
- **Borrowed environment pointers** — `os.getenv`, `env.get` and `env.getor`
  return owned copies. 121.75 — `e7c829a6`; 121.78 — `db07f4b8`.
- **Hangs on a FIFO** — `file.read`, `file.write/append/copy` and the driver's
  source readers no longer block forever on a FIFO with no peer. 121.73 —
  `c68bfb43`; 121.77 — `10526c11`; 121.85 — `478c1956`.
- **SIGPIPE** — a peer that closes first no longer kills the process: the plain
  HTTP client, the WebSocket client and router WebSocket server, `router.serve`,
  `std.tls`, and `std.llm` over http and https. 127.341 — `75aa44a0`; 127.347 —
  `6f96b32d`; 127.350 — `72c9ad27`; 127.351 — `a788172c`; 127.388 — `dd3a51e1`;
  127.394 — `c1b1ae49`. The https client on Linux (127.346 — `ab9a4051`) is merged
  and awaits verification there.

### Everything else

- **Packages**: `tkc pkg init|add|remove|resolve|fetch|list` manages `pkg.toml`
  and `pkg.lock`; a `pkg.*` import resolves through the lock, every cached
  package is re-hashed against it, and fetch is git-based; new codes E9011–E9014,
  E9025, E9028–E9035, W9010 and W9011 (ADR-0004). 76.1.3b, 76.1.3d — `9f20178f`;
  76.1.3c — `461c5724`; `tkc pkg --help` 132.142 — `711ef103`.
- **Binary IR round trip**: `.tkir` 1.2 carries the token text and the parse
  tree; `--from-tkir` compiles it through the same resolver, checker and backend
  as source, and `--read-tkir` summarises one. 76.1.6c — `fd4c03fc`; 127.345 —
  `7ab5ea7e`; the type and data tables grow to the u16 limit, E9010 past it
  (127.344 — `1d6e119b`).
- **`tkc --python-view`**: a read-only, Python-like rendering of a toke file, for
  review only. 96.7 — `85831037`.
- **Lexer and lint**: a raw control byte in a string literal is **W1021** (a
  warning) with an exact `\xHH` fix (131.32 — `d5dee55b`); a nested-`if`-ladder
  lint rule with a deterministic `el if` fix (131.64 — `5800ac34`); `--lint --fix`
  keeps a binding's type when it drops `mut.` or inlines a single-use `let`
  (131.104 — `f324d408`; 131.59 — `02c5e457`); E1003's message for an underscore
  speaks v0.4 (127.338 — `a1e9828f`); `(* *)` is the one comment form (127.198 —
  `5dc9b99e`).
- **Checker, typing results it left untyped** (each printed a pointer or a raw
  bit pattern): `m.getor(k;d)` and its default (127.320 — `0e42cdff`, 127.324 —
  `67113149`, 127.325 — `8dff5994`, 127.379 — `87153202`); the `$ok` binding of a
  `mt` over a map lookup, a `[stdrecord]!E` call and an array `.get` (127.322 —
  `8a56271d`, 127.424 — `551946cb`, 127.428 — `ab9d1a7b`); an expression-`if` or
  `mt` bound by `let` (127.326 — `e98c69d3`, 127.327 — `dcf648f0`, 127.375 —
  `6dca2454`); receiver-method results (127.364 — `9c90a288`, 127.371 —
  `213baf2f`, 127.358 — `838f5c89`); std record fields that are arrays
  (127.333 — `7ce76983`, 127.335 — `98c68127`, 127.336 — `c2a469f7`); a std
  record or handle returned and used in place (127.386 — `6d270505`, 127.392 —
  `0d347948`); imported function types, records and sums (127.377 — `4ef15404`,
  127.384 — `91f4effd`); a stdlib `@f64!E` and cross-module `@$str` result
  (127.297, 127.302, 127.308 — `7e0bd556`); a false E4011 on a returned match
  whose arm yields the function's error value (127.340 — `61f8fb31`) and a false
  E4010 on a result match over a local record (127.426 — `55918886`); E3012 names
  the import alias a declaration clashes with (127.285 — `91825094`).
- **Codegen**: `!` through a function value keeps the error (127.276 —
  `98a8a4be`); a closure calls only its own closure-bound locals (127.282 —
  `558d8056`); an `mt` arm binding ends with its arm (127.321 — `2ac20568`);
  libm declarations match by exact symbol (127.305 — `da15a4e8`); a cross-module
  `@f32` element reads as f32 (127.313 — `77951e74`); in-place append survives
  `std.image`, cross-module callees and struct literals (127.306 — `340af4b0`,
  127.307 — `2e05f5e5`); `j.enc` and `j.print` of a record are typed from its
  descriptor, never guessed (127.361 — `08b401fc`, 127.353 — `026a93b5`); a
  function-value `getor` default is released (127.383 — `183fb159`).
- **Runtime**: strings carry their length in a sealed string heap, so `.len` and
  offset checks are linear, not a scan (owner decision 2026-10-01 §2.1);
  conversion of the remaining producers continues under 127.295. 127.170 —
  `df8a53db`, `152dc4a9`, `7cd91c72`, `e979c4a9`.
- **HTTP and router**: HTTP/2 over TLS is offered in ALPN again (121.80 —
  `f919e344`); binary bodies are served whole on HTTP/1.1, TLS, h2 and through
  `std.router` (121.86 — `3353d92c`, 121.88 — `4914d373`); h2 responses are
  gzipped and already-compressed types are not (121.87 — `f388b26e`, 121.89 —
  `41afa79d`); `std.router` dispatches `:param` and `*` routes, carries request
  and response headers and reason phrases, and grows its tables instead of
  dropping routes (127.343 — `3f76d385`, 121.90 — `9a7429ef`, 127.348 —
  `75ad77ba`, 127.349 — `6dc2d288`); `http.servedir` decodes paths, serves
  precompressed variants by `Accept-Encoding` weight, and its misses get the
  `setnotfound` body (121.81 — `b092a73c`, 121.99 — `01694b44`, 121.96 —
  `23f40f40`, 121.62 — `8a0e16ed`); HEAD carries the GET's framing (121.100 —
  `581c094c`); one Content-Length per response (121.97 — `d8db68af`).
- **Standard library additions** (spec amendments): `file.realpath` (121.60 —
  `69de0b75`); `io.eprint`, and `os.write` writes every byte or returns `$err`
  (121.70 — `d43cc6fa`, `077f38c3`); `toml.has`, `toml.f64`, `toml.f64arr`,
  arrays, keys and distinct accessor errors (121.68 — `2265c710`; 113.B.4 —
  `fee72f55`, `99faa861`); a typed `std.json` object surface — keys, entries,
  has, get, scalar reads, object building (113.B.5 — `1df07046`); `args.all`
  and `args.get`'s reason (113.B.8 — `22e678b6`); `md.renderfile`'s `$mderr`
  (113.B.7 — `3dbc426e`); `str.checkre` returns an invalid pattern's error as a
  value (121.45 — `25d7a3cc`); `os.onofollow` and `log.closeerror` (136.77 —
  `68537a23`).
- **Standard library fixes**: `std.json` member names with escapes are found and
  `json.kv` encodes its key (127.300 — `f0349377`, 127.301 — `62680600`);
  `json.u64`/`json.i64` read a whole integer (127.303 — `90dc487d`); log fields
  decode as `[[str]]` pairs (127.400 — `8d2c7366`); access-log retention keeps
  the newest (127.401 — `60823920`); SIGHUP reopens every log in every serving
  mode (121.53 — `45e170bf`, 121.72 — `7f49d2f3`); `html.render`/`svg.render`
  results survive a re-render (127.316 — `146e3d47`); leaks in csv, file.listall,
  toml and llm tool calls (127.422 — `01382180`, 127.415 — `021a9fe5`, 127.272 —
  `12c0489f`, the llm tool-calling response `f74d3645`); the infer streaming thread is joined on unload (136.75
  — `a1c71e29`); stdlib TLS builds against OpenSSL 4 (121.104 — `69a26c66`).
- **Removed dead code**: 14 duplicate C implementations and 52 unreachable glue
  wrappers (136.67 — `b65162bb`, 136.71 — `409ae154`); the unreachable
  `server_ops.c` and the never-built Postgres/MySQL backends, archived (121.71 —
  `e79285b9`); `ws_server.c`'s dead handshake with the wrong GUID (127.332 —
  `7409df12`).
- **Diagnostics**: `source_line` is written by length and cut on a UTF-8
  boundary, so a raw NUL or a long line no longer truncates it or makes invalid
  JSON. 127.267 — `b47547e6`; 127.269 — `7216aa26`.
- **Licensing**: the standard library is MIT-licensed, with `stdlib/LICENSE` and
  SPDX headers; the full Apache-2.0 text is in the tree. 135.15a — `0a5fd452`;
  `ab2d88cf`.

### Build, platform and release

The release workflow could not pass on GitHub's current runners (Ubuntu with gcc
15 and a C23 glibc; macOS 13 retired). Fixing it found real defects as well as
environment ones; these are the changes a user can observe. 127.439 is the
umbrella story.

- **`std.http` servers set `TCP_NODELAY`** on every accepted connection, plain and
  TLS. A response goes out in several writes, and Nagle held each write after the
  first until the client's delayed ACK: about 40 ms per keep-alive response on
  Linux. C1790 checks the option and times 300 keep-alive responses. 127.443 —
  `7b03be29`.
- **A restarted server can rebind its port on Linux.** `tk_bind_exclusive` now sets
  `SO_REUSEADDR` on every Linux attempt (a `TIME_WAIT` socket keeps its
  listener's flag, and the rebind needs both); binding over a live listener is
  still refused. macOS keeps its probe-first order. Covers the http, router, net
  and tls listeners. 127.444 — `b081cd83`.
- **tkc names its linker on Linux.** `make` records the linker clang names at build
  time (`clang -print-prog-name=ld`) as `TKC_LD_PATH`, and tkc passes it as
  `--ld-path` when its baked clang links for the host. Before, clang looked `ld` up
  on `$PATH`: an empty `PATH` could not link (E9003), and a planted `ld` on `PATH`
  would have linked every program. Not used when `TKC_CC` is set or with
  `--target`. 127.447 — `a394a330`, `c6ef75f1`.
- **tkc takes Homebrew's prefix from `brew --prefix` at build time** instead of a
  hard-coded `/opt/homebrew`; on an Intel Mac every link printed "search path
  '/opt/homebrew/lib' not found". With no brew at build time no Homebrew flags are
  passed. A release tkc carries the prefix of the machine that built it. 127.446 —
  `4852e582`, `10164977`.
- **Emitted IR states PIC/PIE level 2**, so programs link as position-independent
  executables on Linux. 127.439 — `6ccf348b`.
- **Runtime and stdlib fixes:** the inline error box is `int64_t`-aligned
  (`5713b92b`); stdlib allocations abort on out-of-memory where nothing checked
  the result (`db80e19b`); `ml.kmeans` no longer reads past an empty point array
  (127.441 — `0f9718db`, `4b92f854`); the inference backend's hardware probe no
  longer closes a directory twice (127.442 — `09d27141`); an empty
  `sha512_update` is a no-op (`58b10d7f`); the capability provenance label has
  room for the module name (`21f68dfa`); compress copies only the schema keys it
  read (`53c71644`).
- **Builds:** the compiler and stdlib build clean under gcc 15 `-Werror` and with
  glibc's C23 string macros under `-std=c99` (`6490a01a`, `2e8692c6`); the release
  binary is built with the Makefile's hardening flags (`ec3f8be7`); macos-x86_64
  is built on `macos-15-intel` and the release archives ship the stdlib MIT text
  (`f2a7f347`).

Known on this release: the linux-arm64 runner's OpenSSL 3.0 lacks the
`X25519MLKEM768` hybrid group, so the hybrid-handshake cases of C1180 and C1200
report a skip there; the classical fallback is tested in full.

### Publishing

Tagged `v3.2.0` and published 2026-11-15 with four platform archives (linux-x86_64,
linux-arm64, macos-arm64, macos-x86_64) and `checksums.txt`. Check a download against
`checksums.txt`, fetched over HTTPS from the release page. The release also carries
`tkc.sig` and an SPDX SBOM, but in 3.2.0 both cover a `tkc` the workflow rebuilt on
Linux for signing, which is in none of the archives; they do not verify a downloaded
archive (toke 127.453, fixed from the next tag).

Platform floors: the macOS binaries need macOS 15 (Sequoia) or later, and the Linux
binaries glibc 2.38 or later (Ubuntu 24.04+, Debian 13+, Fedora 39+); older systems build
from source (toke 127.454). ooke 3.0.0 requires toke 3.2.0; Homebrew installs both
(`brew install karwalski/toke/tkc`, `karwalski/toke/ooke`).

### Addendum, 2026-11-15: lint rules

Added after the release at loke's request (loke's handoff of 2026-11-15, N-7; story
138.41). The entries above mention the new lint rule only in passing and name no rule
id. A downstream that ratchets its `tkc --lint` counts sees new findings on unchanged
source as a regression: loke's count went from 552 to 737 on 3.2.0, all of it the new
rule.

One rule is new and five changed between `v3.1.0` and `v3.2.0` (`git diff v3.1.0
v3.2.0 -- src/lint.c`). The messages of the five are unchanged. Measured with both
compilers over ooke's sources, toke's examples and test programs and the website's
programs, only `nested-if-ladder` adds findings; the others change which findings carry
a `fix`, or report less.

| Rule id | Severity | Change | Effect on unchanged source | Story, commit |
|---|---|---|---|---|
| `nested-if-ladder` (new) | hint | Reports an `if` whose `el` block holds exactly one statement, another `if`: one finding per ladder, at its first nested `if`. Message, for a one-rung ladder: ``else block holds only another if (1 nested rung) — write `el if(...)`: an el-if chain is one nesting level, each `el{if}` rung adds one (quality_rubric: depth > 3 flags, > 4 fails)``. `--lint --fix` splices the inner `if` up into `el if(...)`; no fix when a comment sits in the block | New hints wherever the shape occurs. A hint: the `--lint` exit status does not change | 131.64 — `5800ac34` |
| `single-use-let` | hint | `--lint` type-checks the file silently first, and the `fix` (inline the binding) is offered only when the checker typed the use and the initialiser identically; a file that does not type-check keeps the hint without a fix | Same findings; fewer carry a `fix` | 131.59 — `02c5e457`, `ac12618f`, `e5a917b3` |
| `mutable-never-mutated` | warning | The `fix` (delete `mut.`) is offered only when the program with the deletion type-checks and types every node identically | Same findings; fewer carry a `fix` | 131.104 — `f324d408` |
| `string-concat-chain` | warning | The fix that `TKC_LINT_CONCAT_FIX=1` arms is offered only for a module-style `std.str` chain whose every part the checker typed `str` | Same findings; without that variable no finding carries a fix, as before | 131.110 — `f4dae47c` |
| `unused-import` | warning | Reads a qualified type's alias from the parse tree, not from the source bytes before the type | Fewer findings: an import used as `alias.$type` with a comment between the tokens was reported unused, and `--fix` deleted it | 131.81 — `968de8c6` |
| `discarded-value-result` | error | Its `std.vec` exemption (a bare `push`/`set` on a handle mutates in place) recognises the renamed handle type `$vechandle` (was `Vec`, #4 above) | None on source that compiles on 3.2.0 | 127.251 — `16c3088d` |

The rules `tkc --lint` runs in 3.2.0, for a downstream that ratchets per rule:
`discarded-value-result`, `empty-fn-body`, `flag-soup`, `loop-rebuilds-array`,
`mut-flag-if`, `mutable-never-mutated`, `nested-if-ladder`, `redundant-bind`,
`single-use-let`, `string-concat-chain`, `unreachable-code`, `unused-import`,
`unused-let`. Each is described in `docs/compiler/lint-rules-v1.md`. From the next
release, an entry names every lint rule added or changed (CONTRIBUTING.md).

### Addendum, 2026-11-15: the stdlib C sources a program links

Added after the release at loke's request (loke's handoff of 2026-11-15, N-8; story
138.42). loke compiles toke's stdlib C sources itself from a list of its own, and on
3.2.0 its link failed on `tk_infer_stream_cleanup` until it added `infer_stream.c`.

What a program links is what `tkc --emit-deps` prints for each of its modules, the set
tkc's own driver links (`docs/compiler/build-deps.md`, "Linking a program yourself").
Link that and do not keep a list. These are the changes 3.2.0 made to it
(`git diff v3.1.0 v3.2.0 -- src/stdlib_deps.c`); a build that keeps its own list must
make each of them:

| Module | Change to its link set | Why | Story, commit |
|---|---|---|---|
| `std.infer` | adds `infer_stream.c` and `-lpthread` | `tk_infer_unload` now joins the streaming thread, through `tk_infer_stream_cleanup` in `infer_stream.c` | 136.75 — `8347a71c` |
| `std.os` | adds `os_glue.c` | `os.write`'s wrapper moved there when it learned to write every byte | 121.70 — `077f38c3` |
| `std.toml` | depends on `std.file` (`file.c`, `file_glue.c`) | `toml.loadfile` reads through `std.file` | 121.74 — `b2191dcf` |
| `std.db` | depends on `std.time` | `db_glue.c` calls `tk_time_nowms_w`; every program already linked it through `std.str`, so no file is new | 127.269 — `19da1fd3` |
| `std.llm` | adds `-lssl -lcrypto` | `llm.c` speaks https over OpenSSL; tkc compiles the sources with `-DTK_HAVE_OPENSSL` when `-lssl` is among the flags | 121.23 — `585889e1` |
| `std.http` | drops `server_ops.c` | archived as unreachable | 121.71 — `e79285b9` |

---

## 3.1.0 — loke's handoff, the typed standard library, and security fixes

**Status: prepared on branch `release/3.1.0` (story 138.17). Not tagged, not
published.** Built from main as of 2026-09-30 (`b88090c3`, then Batches X1, X2,
F491, F491b, Y1, Y2 and Y3 merged in, through main `f605aec9`); more than 760 commits since `v3.0.0` (`0b0bc4e`, 2026-09-23). Every entry below names the tracker
story in `docs/progress.md` and a commit, so a consumer can bisect.

This is a minor version by the letter of semver, but read **Breaking changes**
first: most of the release is the checker learning to reject programs that
3.0.0 compiled and then ran wrongly, and several standard-library calls changed
shape. A program that 3.0.0 compiled *and ran correctly* mostly still compiles;
the migrations below cover the ones that do not.

The loke reply `docs/loke-handoff-2026-09-27-reply.md` has a per-issue verdict
for every item of loke's 2026-09-27 handoff, measured against this build.

### Breaking changes (with migration)

| # | Change | Story, commit | Migration |
|---|---|---|---|
| 1 | **HTTP client calls are separate from route registration.** `http.get/post/put/delete(path; &handler)` register routes and are type-checked as such: `http.get(client;path)` is E4031 ("the call registers a route and sends nothing"). The 1-argument `http.get(url)` and 2-argument `http.post(url;body)` client forms are retired (E4026/E4031). | 138.3 — `0d48bda`, `06a7dca`, `109db36`, merge `880b9b4b` | `http.clientget(c;path)`, `http.clientpost(c;path;bytes;ctype)`, `http.clientput(...)`, `http.clientdelete(c;path)` → `httpresp!httperr`; `http.geturl(url)` → `str!httperr` (errs on transport failure and on non-2xx, `code` = status) |
| 2 | **`http.postheaders` is typed and takes 3 arguments**: `(url:str; body:str; headers:[[str]]) -> httpresp!httperr`. The 4-argument `[abi]` form is gone (E4026). A caller's Content-Type replaces the default; CR/LF in a header is refused; the `net` capability is enforced. | 138.8 — `ce65291` | `http.postheaders(url;body;@(@("x-api-key";k);@("content-type";"application/json")))`, matched as a result |
| 3 | **The https client verifies server certificates** (peer and host name, default trust store; `SSL_CERT_FILE`/`SSL_CERT_DIR` override). A call to a host with an untrusted or mismatched certificate now takes `$err`. | 135.25 — `6b82978` | Install the CA, or point `SSL_CERT_FILE` at it; there is no opt-out |
| 4 | **Standard-library calls are checked against the typed `.tki`** — argument kinds and scalar returns. A wrong argument that used to compile (and print an address, or crash) is E4031 at `--check`: `str.tof32(12345)`, `str.isdigit(str.charat(..))`, `net.listen(8080)`, `json.i64(<str>;..)`, arithmetic on `str.nowunix()`. | 138.3, 138.11, 138.12, 138.13 — merge `880b9b4b` | Pass the declared type (`str.tof32("12345")`, `net.listen("127.0.0.1:8080")`) |
| 5 | **Retyped string and std functions.** `str.frombool/replaceall/replacere/setat/splitfirst/sub` and `json.encode/encstr/quote/stringify` return `str` (they printed an address); `str.padleft/padright` take the pad as an `i64` byte code (`48` for `'0'`); `str.compare` returns -1/0/1; `json.getval(Json;str) -> str`; `std.homedir` links without importing `std.process`. | 138.11 — `266eb17`; 138.12 — `641b129` | Interpolate the results directly; pass pad as a byte code |
| 6 | **Withdrawn modules and aliases**: `std.string`, `std.fs`, and 13 `str` aliases (`fromu16 fromu32 fromu64 fromint64 fromi32 fromf32 bytelen pad strip trimws wordcount splitfirst splitwhitespace`). The `loke://` scheme and `~/.loke/models/` are removed from the stdlib. | 136.45 — merge `07c267aa` | `std.string`→`std.str`, `std.fs`→`std.file`; `from{i32,u32,u16,int64}`→`str.fromint`; `fromf32`→`str.fromfloat`; `bytelen`→`str.len`; `strip`/`trimws`→`str.trim`; `splitwhitespace`→`str.split(s;" ")`; `splitfirst`→`str.split` then `.get(0)` |
| 7 | **`csv`, `file` and `zip` errors are real error boxes**; their `lasterr`/`lasterrkind` accessors are retired. `FileErr` widens from 3 to 10 variants (`$invalid $isdir $notregular $symlink $toolarge $nomem $badarg` appended; the first three keep their tags); `csverr` gains `kind` and a real `line`. | 127.102a/b/c — `85080971`, `6e5b9868`, `47c5ad55` | Match `$err:e` and read `e`; a `FileErr` match that listed three variants needs the new ones or a catch-all arm |
| 8 | **`mt` binds a result's arms by label**, not by position; `<$err(x)` is an error return. A program whose err arm was written first used to take it for a valid input. | 138.2 — `f4840c41`, `1def6f04` | None needed; drop any "ok arm first" convention |
| 9 | **New rejections for silent-wrong programs**: `==`/`!=` on struct and sum values (E4031; toke has no structural equality); array-literal elements of different types (E4031 — `@(arr;elem)` used to nest); a struct literal of an undeclared type (E3011); a comparison standing as a statement (**E4074**, new); a `$variant(..)` constructor with other than one payload (E4026). | 138.9 — `85570be1`, `c439fea2`, `b88f882b`; 138.10 — `4c44c669`, `17b9a7c4` | Match on the variant or compare a field; append with `x=x+@(v)`; declare the type; `x=$t{a:9;b:x.b}` for a field update |
| 10 | **Lexer/parser diagnostics**: a comment still open at end of file is **E1007** (new), naming the nested `(*` that consumed its `*)` (comments nest, spec §8.10); `x.a=9` is E2002 "a struct field cannot be assigned"; a reserved keyword as an import alias is one E2002. | 138.10 — `093e81d`, `549eb2b`, `ba91e32` | Close the comment; rebuild the struct |
| 11 | **Compiler caps are met at `--check`** and name the count, the limit and the flag (E9010: `--max-funcs`, `--max-structs`, `--max-imports`, or `tkc.toml`). A program over the struct cap that 3.0.0 compiled by silently dropping types is now E9010. **The default `max_funcs` and `max_structs` rise from 128 and 64 to 1024**; `max_imports` stays 32. The locals caps report one located E9010 per function instead of one unlocated record per local. | 138.15 — `6586136f`, merge `fd0a0e88`; 127.250 — `8e2bf425`; 127.252 — `eb24097f` | Most programs need no flag now; past 32 imports, pass the flag the diagnostic names |
| 12 | **One regex engine** for the checker and the runtime. `\s\S\d\D\w\W`, `\b\B`, `(?:…)`, a leading `(?i)` and POSIX classes are supported (on macOS `\s` etc. used to be literal letters); unsupported syntax (lazy quantifiers, backreferences, lookaround, unbalanced brackets, …) is **E4075** at `--check` for a literal pattern and traps **RT008** for a computed one; a step budget traps **RT009**. `str.matches` is whole-string. | 137.9, 121.35 — merge `49d17163` | Rewrite `.*?` as a negated class (`<[^>]*>`) |
| 13 | **Wider capability gates**: `log.openaccess`/`openerror` need `fs.write`; `toml.loadfile`, `toon.load`, `yaml.load`, `md.renderfile`, `crypto.sha256file`/`sha256verify` need `fs.read`; `router.serve`/`chart.serve`/`dashboard.serve` need `net`; `http.downloadfile` needs `fs.write`; `http.servedir`/`servepages` need `fs.read`. | 121.22 — `783e0848`; 121.42 — `9a9447ce`, merge `b88090c3` | Grant the capability in `tkc.toml [capabilities]` or with `--allow-*` |
| 14 | **`std.db`**: every `db.open` returns a fresh handle; handle-less calls use the first still-open database (a second open no longer repoints them); `db.exec` runs every statement, names the failing one, and refuses bind parameters with several statements. | 138.6 — `0deb11d` | Use `db.execon/oneon/manyon/closeon(h;…)` for a second connection |
| 15 | **Advice-only `fix` fields removed** from E2001, E2002 (generic), E2003, E2004, E2005, E4025, E4027 (the did-you-mean fix stays), W1010 and E9005. A repair loop reading `fix` finds it absent where it was never deterministically correct. | 127.233, 127.235, 127.240, 127.243, 127.237 | Read `message`/`got`/`expected` |
| 16 | **`std.llm` needs the `net` capability** for its connection, like `http.client`; RSA key generation accepts 2048 bits only, and encrypt/decrypt/sign/verify refuse larger moduli (they computed wrong results); `infer.load(path;opts)`, `infer.loadstreaming(dir;opts)` and `mdns.advertise(rec)` take their published arguments, and so do `infer.generate(h;prompt;maxtokens)`, `mlx.generate(m;prompt;maxtokens)` and `mdns.browse(type;&callback)` (the 2-argument generate forms are E4026) — none of the stub modules reports false success; `http.stream(httpclient)` takes the client its `.tki` publishes (it read a URL string) and raises `httperr`; `router.ctx`/`$ctx`, `process_set_cwd` and the SIGHUP reload stubs are removed (unreachable before). | 121.42 (2) `ff2ef9d8`; 121.51 (1) `691550fb`; 135.31 (1) `c99ad497`; 121.42 (7) `8226aa6f`, 121.51 (3) `19cd98ae`, 135.31 (4) `bfcaabff`; 137.11 — `5a5f581b`; 121.53 — `c69c18a7` | `--allow-net` / `net = true` for programs that call `llm.*`; 2048-bit RSA keys; pass a token limit to `generate`; `http.stream(http.client(url))` |
| 17 | **A one-sided result match whose value is used is E4010** — including as the value of an expression-`if` branch; `mt` on a nil sum value traps RT005 (it read the tag through a null pointer). | 127.249 (2) `50a0bd83`; 127.252 — `45a0fd28`; 121.51 (2) `d7eb72df` | Write both arms |
| 18 | Behaviour changes in other modules: `soap.fault` escapes its detail; TOON escapes `\ \| \n \r` both ways; `jwtverify` rejects at/after `exp` and before `nbf`; `http.param`/`header` take `$err` when missing; the HTTP/2 `h2c` upgrade is retired (answered as HTTP/1.1); `pkg.toml` has a closed schema, a source allowlist and a strict lock; **`tpl` `layout()` takes a layout name, not a path** (a name holding `/`, `\`, `..` or empty is refused), and template block nesting (64) and render depth (128) are bounded; `toon.str/i64/f64/bool/arr` raise a `ToonErr` payload (`$err:e` bound nil before); `file` errors name `$isdir` and `$symlink` for EISDIR/ELOOP on every call. | 121.16 `7f95de9b`; 121.40 `b1f8c0da`; 121.20 `e53bf845`; 135.28 `2e667e9`; 121.44 `b90ca884`; 121.39 `3256a412`; 121.41 `ed228353`; 127.246 `53d46b3c`, `6e45ebb1` | Pass `layout("base")`, not a path |

### Fixed for loke (the 2026-09-27 handoff, Epic 138)

- **B1 bind address** — `http.serveon(host;port)`, `serveworkerson`,
  `servetlson`, `servetlsworkerson`; `http.listen("h:p";…)` binds its host. The
  port-only forms keep binding every interface (owner-reversible decision,
  recorded in `docs/stdlib/http.md`). 138.1 — `e743005`.
- **B2 `mt` by label** (above, #8). 138.2.
- **B3 client calls** (above, #1); a client DELETE is reachable. 138.3.
- **B4 keychain** — `keychain.laststatus/lasterrkind/lasterr` tell "no item"
  from "locked/denied/failed"; `keychain.create/use/destroy` give a throwaway
  keychain file off the search list. 138.4 — `a1e0b97`.
- **B5 f64 across modules** — the importer declares the defining module's return
  type (double/float and narrow integers, zero-extended when unsigned). 138.5 —
  `ffe9b4f`.
- **B6 `std.db`** (above, #14) and zero values: `row.i64`/`row.f64` of 0, and a
  `COUNT` of 0, are `$ok 0`. 138.6 — `0deb11d`, `9d9544c`.
- **B8 typed `postheaders`** (above, #2). 138.8.
- **T-16** `http.mockreq(method;path;headers;body;params) -> $req` and
  `http.resstatus/resbody/resheader` test a handler in-process. 138.16 —
  `e2fcb3d`, `e08c2fd`.
- **`$req`/`$res` layout** — handler records are laid out in `.tki` order, so
  `req.body`, `req.headers`, `req.params` and a `$res{…}` literal read and
  write the right memory; `make check-tki-layout` gates it. 138.18 — `c662484`.
- **JSON** — `json.str` unescapes correctly; `json.at(Json;str) -> Json!JsonErr`
  reads nested objects. 138.12 — `641b129`.
- **Crashes** — `net.listen`, `str.isdigit`, a struct with a field of its own
  type (`@$self`), a `$internal` struct field. 138.13 — `3af398c`, `266eb17`,
  `5c9bd31e`, `4a62e7e`, `cfa07fa3`.
- **No silent truncation** — string constants of any size; the stdlib source
  list handed to clang is 64 KB and E9010 when it does not fit, so a long
  checkout path builds. 138.14 — `2075fa9`.
- **Diagnostics** — W1020 no longer fires on a binding named like a foreign
  keyword (`let def=…`). 138.10 — `574a491`.
- **T-11 stub modules** — `infer`/`mlx`/`mdns` take the arity their `.tki`
  publishes and never report false success: a missing model takes `$err`,
  `generate` on no model is `$err`, `isavailable` is `false`. 137.11 —
  `5a5f581b`.
- **Default caps** — `max_funcs` and `max_structs` default to 1024, so loke's
  `--max-funcs`/`--max-structs` flags can go (`--max-imports` stays: default
  32). 127.250 — `8e2bf425`.
- **Still open for loke**: T-17 `json.parse` (owner decision, 138.9), T-55
  quadratic `str.charat` (127.170, owner decision), T-59 scoped capability
  grants (not yet filed).

### Security fixes

- **https certificate verification** (above, #3). 135.25.
- **`llm.llmclient`'s `.model` read the API key**; every stdlib record now
  matches its C struct slot for slot, gated by `scripts/check_tki_layout.py`.
  135.30 — `9088da12`.
- **`crypto.sha256` of a `str` returned the digest of the empty string** (and
  two calls corrupted the heap); `str` into a `[byte]` parameter is fixed at the
  checker. 124.2f — merge `e1dcc7ca`.
- **RSA key generation never terminated** (`bi_modinv` seeded the wrong
  coefficient). 121.48 — `0a47c948`. Keys over 2048 bits, which computed wrong
  products, are now refused (121.51 — `691550fb`).
- **Map hash flooding** — the map index is keyed SipHash-1-3. 121.32 — `fc7cbfb1`.
- **HTTP** — one RFC 9112 request reader for plain and TLS (smuggling,
  pipelining, deadlines; 121.17 `32efd7b1`); `servedir` symlink escape (121.18
  `b8bd3b2d`); a single abandoned request no longer kills the server (127.144,
  merge `1ed358e1`); HTTP/2 frame limits and deadlines (121.17, 121.44); a
  lingering close after GOAWAY and after an HTTP/1.1 error response, so the
  peer reads the answer instead of a TCP reset (121.52 — `d742f29b`; its
  C391 test race fixed in `de559637`, `d572ab01`); `tls.listen` no longer
  binds beside a live listener with an unconditional `SO_REUSEADDR` (135.34 —
  `8eb44f6d`).
- **Templates** — a self-including partial or deeply nested blocks blew the
  stack; `layout()` read `templates_dir/../secret.tkt` (121.41 — `ed228353`).
  An unreachable access-log writer that did not escape its fields is deleted
  (121.41 — `00380854`).
- **Injection** — Markdown table-cell `javascript:` links (121.16 `d7875c47`);
  TOON ↔ JSON field and value escaping (121.40 `b1f8c0da`); header CR/LF in
  `postheaders` (138.8).
- **Decoders and crypto** — forbidden PNG variants refused (121.33 `013ee0a3`);
  RNG aborts rather than return zero keys, JWT `exp`/`nbf`, constant-time TOTP,
  key wiping (121.20 `0ee70589`); database TLS required by default for
  MySQL/Postgres and bound parameters for MySQL (121.21 `91691a21`).
- **Ambient authority** — a capability-sink gate follows every callable entry to
  its fs/net/spawn primitives (121.22 `783e0848`); the gaps it found are gated
  (above, #13).

### Everything else

- **Checker**: receiver dispatch — a call to a method that does not exist is a
  diagnostic, not a link failure (127.115, 127.96); an unimported alias is E4033
  (127.135); parameters shadow top-level functions (127.136); E4027 suggests a
  near member (127.122); `m.get` and `json.get*` set the error channel and can be
  matched (127.30 `3f167aea`, 127.241) — the checker does not yet force the
  match (127.244).
- **Codegen**: a later `mt` binder no longer overwrites an outer binding
  (127.234); a string `mt` binds the right arm (127.237); stdlib error payloads
  bind (127.238, 127.241); in-place appends and non-retaining callees
  (135.20, 127.171, 127.229); `as str` on a slice is the string (127.229).
- **Batches Y1–Y3**: a user function named like a std member (`get`,
  `read`) is no longer taken as the std call's error payload (127.252
  `0d086b66`); E4075 follows a pattern named by an immutable `let` of a
  literal (121.45 `6c36fea9`); the unreachable POSIX `regcomp` wrappers are
  deleted (121.45 `fa009569`); `capabilities.md` §2.2 says what the broker
  does with `enforce` and scoped values (121.42 `a035e786`); bounded
  `snprintf` in duration formatting (121.53 `ac955587`).
- **Tooling**: `--lint` exits non-zero on an error (127.137); `--fix` says when
  it stops short of its fixpoint (127.158); `--migrate` keeps v0.4 source and
  comments (127.57); `--emit-tkir` keeps every import (127.235).
- **Gates**: `make diff-codegen` fails on a crashing program instead of
  baselining it (127.139); `make lint` analyses the stdlib (135.3a); `make`
  rebuilds when flags change (136.62); the D-series `fix` checks are live
  (127.239).
- **Batches X1/X2 (merged into this release)**: user `str`/scalar error payloads
  bind, and `args.get`/`env.get` bind their payload (127.249 `cfb13887`); a
  `str` from `tpl.render`/`tpl.escape` interpolates as text (127.242
  `0c265416`); match-arm bindings release their slots, so many-arm matches in
  one function build (127.246 (4) `2b001c27`); E3011's did-you-mean offers only
  a writable name (127.246 (1)); `time.withtz`/`utcoffset` read the zone rules
  themselves and never `setenv TZ` — thread-safe, and an unknown zone is UTC
  (121.42 (6) `2688a0c8`); a db error's `e.msg` owns its text and survives the
  next call or close (121.51 (5) `7124e466`); Postgres survives allocation
  failure and `??` escapes a literal `?` for jsonb operators (121.51 (8)
  `97511f30`).
- **Runtime**: `http.servedir` mounts are a longest-prefix table (127.75); the
  per-IP rate limiter that silently returned 429 is removed (127.60); `ws.recv`
  returns a real message (127.145); `Res` bodies are owned (127.148); HTTP/2
  CONTINUATION, request bodies and response headers (merge `b88090c3`);
  `vecstore.flush` with atomic saves (136.15); `os.getcwd` returns an owned
  string (135.23); failed server starts exit 1 (135.22, 135.24).

### Publishing

Nothing here is tagged or published. For the owner: tag `v3.1.0` on this
branch's tip (the release workflow takes the version from the tag), then bump
ooke's `ooke.toml [requires]` floor to the tag and re-run its packager (ooke
branch `release/ooke-rc2` already declares `mintoke = "3.1.0"`).

---

## 3.0.0 — the v0.4 language boundary

**Status: tagged `v3.0.0` (`0b0bc4e`, 2026-09-23).** The rest of this entry,
"Publishing" included, is the record as written when the version was cut.

### Why this is a major version

`VERSION` read `2.8.0` from `f1e30b3` (2026-06-21) until this change. The
entire v0.4 language break shipped under that one unchanged number, so `2.8.0`
names two mutually incompatible compilers: the one released on 2026-06-21, and
every commit since. Released 2.8.0 **cannot build current ooke at all** — it
rejects `store.tk:148` with a hard E2002 on expression-`if`.

The practical consequence was that no downstream could pin toke by semver.
ooke had to invent three keys — `mintoke`, `mintokedate` and `mintokecommit`
(`ooke.toml [requires]`, `toke-ooke/docs/versioning.md`) — to express a floor
that a version number should have carried. loke and every other consumer are
in the same position. 3.0.0 gives them a semver to pin instead of a commit
hash.

### Breaking changes since 2.8.0 (`f1e30b3`, 2026-06-21)

Each row names the commit that introduced the break, so a consumer can still
bisect if it needs to.

| # | Change | Introduced | Effect on 2.8.0-era source |
|---|---|---|---|
| 1 | `=` is assignment, `==` is equality (Epic 116.3 / A3) | `335113d`, 2026-07-01 | v0.3 source using `=` for equality changes meaning; 2.8.0 warns W2021 on every `==` |
| 2 | Expression-oriented `if` — `if`/`el` yields a value (Epic 116.1 / A1) | `3bc1dbb`, 2026-07-01 | 2.8.0 rejects expression-`if` with E2002; new source is not backward compatible |
| 3 | `@(...)` array and map literals; `[]` removed (Epic 116 syntax decision) | Epic 116 | `[]` literals no longer parse |
| 4 | Capability broker, deny-by-default (ADR-0010) | `8c073a6`, 2026-07-06 | `stdlib/capabilities.c` becomes a link dependency; binaries are subject to `CAP001` |
| 5 | **`tkc.toml [capabilities]` manifest parsed and baked (ADR-0010)** | `b66bd8e`, 2026-07-06 | **the hard floor.** Any earlier tkc aborts before parsing any toke: `tkc: tkc.toml:9: expected key = value` |
| 6 | Escape-by-default template surface (ADR-0011) | Epic 124.3 | stdlib HTML-escape semantics changed |
| 7 | `mut.` mutable bindings | Epic 116 | reassignment now requires a `mut` binding |
| 8 | No-underscore naming for config keys and stdlib call names (113.2a) | Epic 113 | Profile-1 excludes `_`; stdlib call names renamed |
| 9 | `std.fmt` added; lint rules v1 (12 rules, 6 of them the 131.9 pattern rules) | Epic 131.9 | new diagnostics on previously silent source |
| 10 | The Epic 127.x compiler fixes | throughout | several changed observable behaviour, not only failure modes |

**Earliest toke that builds current ooke: `b66bd8e` (2026-07-06)** — verified by
building tkc at that commit and running a full ooke build against it
(`toke-ooke/docs/versioning.md`). That commit is the semantic content of the
3.0.0 floor.

### Pinning guidance for downstreams

- Require `>= 3.0.0` for anything written in v0.4 syntax.
- `2.x` means pre-v0.4 and will not compile v0.4 source. There is no forward
  compatibility and none is planned.
- The commit/date fallbacks (`mintokedate`, `mintokecommit`) can be retired
  once 3.0.0 is tagged and published; until then they remain the only precise
  floor.

### Spec

Spec level is **v0.4** (`docs/spec/toke-spec-v0.4.md`). It does not change with
this version cut; 3.0.0 is the first compiler version number that names v0.4.

### Not changed

Historical measurement records that name `toke 2.8.0` as the compiler under
test are left alone on purpose — `patterns/catalogue.json`, `bench/patterns/`
results, `docs/metrics-baseline.md` measurement rows, `docs/spec/patterns-v0.4.md`
"Measured at" lines, `docs/reference/combinator-status.md` and the reviews under
`docs/about/`. They record what was measured and on what; rewriting them would
make them false. They are re-stamped only by a re-measurement.

### Publishing

**Nothing here is published.** Cutting the number in the repository is
reversible; tagging and publishing is not. Still outstanding, for the owner:

1. Rebuild `tkc` so the binary actually prints `toke 3.0.0` (the checked-in
   binary still reports 2.8.0 until then).
2. Tag `v3.0.0` — `.github/workflows/release.yml` derives the artefact version
   from `GITHUB_REF_NAME`, so the tag name *is* the release version.
3. Fill the real SHA256s into the homebrew formulae and correct their declared
   versions (story 132.27; `homebrew-toke/Formula/tkc.rb` still declares
   `0.3.0`, `ooke.rb` still declares `0.1.0`, and both carry
   `PLACEHOLDER_SHA256_*`). Do not publish the tap before both are real.
4. Bump ooke's `ooke.toml [requires] mintoke` to `3.0.0` and retire
   `mintokedate` / `mintokecommit`.

---

## 2.8.0 — 2026-06-21 (`f1e30b3`)

Stdlib interface fidelity (113.B.1 / .2 / .3). The last release before the v0.4
break. Pre-v0.4 syntax: `=` is equality, `[]` literals, statement-only `if`, no
capability manifest.

## 2.7.0 — 2026-04-30 (`30b132a`)

`sc` / `spawn` keywords; W8001 unsafe diagnostic.

## 2.6.0 — 2026-04-30 (`f7fe774`)

FFI ownership, closure runtime, `sc`/`spawn`, collections (`std.queue`,
`std.set`, `std.stack`).

## 2.5.0 — 2026-04-29 (`f7a9d40`)

`std.task`, array higher-order functions, `extern_c`, closure parser and name
resolution.

## 2.4.0 — 2026-04-29 (`56ea6b1`)

DWARF debug metadata; option type `$none`.

## 2.2.0 — 2026-04-29 (`f159394`)

`&name` function-reference syntax, replacing `f=name`.

## 2.1.0 — 2026-04-29 (`26eadd4`)

Function-reference syntax `f=name` in call arguments.
