---
title: ADR-0016 — Releasing memory, first target: the explicit {arena} block, with S1 sealed strings
slug: ADR-0016-arena-block
section: decisions
order: 23
---

**Date:** 2026-11-24
**Status:** **Proposed.** Written for the owner to accept, amend or reject. No arena code ships with this ADR, and none may land before it is accepted (owner answer 1.13). Where this text says "decision" it means the proposed decision. The questions in the last section are the owner's and are not decided here.
**Deciders:** Matthew Watt (pending)
**Story:** 127.525 (owner answer 1.13 (a), `docs/owner-decisions-2026-11-12.md`, revised 2026-11-13). Design input: `docs/decisions/127.299-function-arena.md` (the 127.299 note: the problem, the measurements, options A–E). Related: 127.524 (the `memory-model.md` §1 amendment, answer (b)), 127.170 / 127.295 (S1 sealed strings), ADR-0006 (array copy-on-write, monotonic `rc`), 124.0c (closure-environment release), 127.236 (return slot), 127.343 (router strings as sealed copies per request). Follow-ups filed with this ADR: 127.528 (the E5001 documentation drift found while writing it) and 127.529 (a committed `TK_MEM_STATS` report, the measurement this ADR gates on).

## Context

Generated toke code releases almost nothing it allocates. The 127.299 note §1 measures it: on ooke's template lexer, one lex-and-render of a 64 KB page allocates 255.7 KB of sealed strings and 217.7 KB of `malloc`'d blocks, about 9 of which are freed, and **no byte allocated in an iteration is live after it**. A long-running server's resident set grows by the total bytes it has ever allocated. The 127.299 slice (C2200) frees only the number conversions inside one interpolation, which no toke value can reach.

The normative `docs/spec/memory-model.md` §1.1–§1.3 promises a per-function arena released in bulk at every scope exit, plus `{arena ...}` sub-arenas. Neither exists. Owner answer 1.13 splits the repair:

- **(b) now:** amend §1.1, §1.3 and §2.3 to describe what ships (127.524), saying what `{arena}` and E5001 mean afterwards.
- **(a) as an ADR before any code:** this document. Its first target is the **explicit `{arena}` block** — request-scoped release for servers — designed **together with S1's never-freed sealed strings**, because S1's one rule ("a sealed string is never written, freed or reallocated", `docs/runtime-abi.md` §3.1) is exactly what a region release breaks.

### What `{arena}` is today (measured on 42fafa61)

- **Grammar.** `ArenaStmt = '{' 'arena' StmtList '}' ;` already exists in `docs/spec/grammar.ebnf`, with the bounded-lookahead note (E2): `arena` is an **identifier** recognised after `{`, not a keyword. It is in `toke.gbnf` / `toke.lark` (no `;` after the closing brace) and has gbnf fixtures (`test/gbnf/extra/x38`–`x40`).
- **Checker.** `NODE_ARENA_STMT` in `src/types.c` raises `arena_depth` and reports E5001 when a **direct** child statement assigns to a **module-scope** name. Module constants cannot be assigned at all, and E4070 reports first: `m=test;n=5:i64;f=main():i64{{arena n=1;}<0};` gives E4070 and nothing else. An assignment to an outer local (`let s=mut."";{arena let t="a\(1)"; s=t;}`) is accepted and runs, as are `<` and outer appends inside the block. No conformance case asserts E5001 (D049–D051 assert its absence). The documentation of E5001 (`docs/spec/errors.md`, `docs/reference/errors.md`, `docs/reference/statements.md`, `docs/spec/semantics.md` §7.3) describes an outer-variable check the compiler does not make, with an example that does not parse; filed as 127.528.
- **Codegen.** `NODE_ARENA_STMT` lowers to its body between `; arena begin` / `; arena end` comments (`src/llvm.c`). It frees nothing.
- **Use.** No `{arena` in ooke main, the website, or the 3,573 toke-test-programs files; none in the v0.4 regen records; 4 records of the v0.2-era `toke-corpus/corpus/phase2_deduplicated/DOC-EXP` set. Giving the block real semantics therefore breaks no measured program.

### The allocations a region would have to own

From the 127.299 note's table, re-read on this base:

| Allocation | Allocator today | Freed today |
|---|---|---|
| String literal | static `@.str.N` | never (static) |
| Converted string producer | S1 sealed heap: one 16 GiB reservation, `__atomic_fetch_add` on a process-wide `tks_top`, side bitmap one bit per 16-byte granule (`src/stdlib/str.c`) | never, by S1's rule |
| Unconverted string producer, builder buffer | `malloc` / `realloc` | never, except C2200's conversions |
| Array, `[byte]` | `tk_arr_alloc` (`tk_array.h`) and codegen's inline `malloc` (array literal, spread); header `rc \| cap \| len` | never; `rc` only decides copy-on-write (ADR-0006 D2) |
| Map, struct/sum box | `malloc` | never |
| Error box | per-thread buffer (127.109) | reused |
| Closure environment | `malloc` (codegen) | at the binding's exits when its count reaches zero (124.0c) |
| Glue results handed to toke | `malloc` / `tks_dup` across about 640 `malloc(` call sites in `src/stdlib/*.c` | never |

## Decision (proposed)

### D1. Scope: the explicit block only

`{arena ... }` becomes a real region. Per-function arenas (`memory-model.md` §1.1, option A of the 127.299 note) are **not** part of this ADR; whether they stay in the spec, pair with 127.236's return slot, or leave the spec is open question Q4. Outside any `{arena}` block nothing changes: allocation and (non-)release stay exactly as shipped and as 127.524 will describe them.

### D2. Syntax: no grammar change

The block keeps its existing production. **No production is added or changed, so the published production count stays 53** (`docs/metrics-baseline.md`, `grammar_productions`), and `arena` stays a contextual identifier, so the keyword count stays 14. `toke.gbnf`, `toke.lark` and the tree-sitter grammar need no edit. The block stays a statement: it has no value. A result leaves it through an outer `mut` binding (scalars, D4 rule E1) or through `<` (copy-out, D4 rule E2). No `{arena}` form with a value, no annotation and no `std.arena` module is proposed; each would be a spec amendment of its own.

### D3. Semantics: a dynamic, per-thread region stack

1. **Entry** pushes a fresh region onto the **calling thread's** region stack. **Every exit** pops and releases it: falling off the end, `br` out of an enclosing loop, `<` (after copy-out, D4 E2), and `!` propagation (also a return). A trap (RT00x) or process exit releases nothing; the process is ending.
2. **Extent is dynamic, not lexical.** While the block runs, every allocation by a *converted* allocator (D5) on that thread goes to the top region, including allocations made by functions the block calls, at any depth. That is what makes one block around a request handler or a render loop's body recover everything (127.299 §1: the Z77 loop keeps only an integer).
3. **Nesting** is LIFO and strict: an inner block's region is released before the outer one, and releasing one never touches another. A value made in an outer region may be read freely inside an inner one.
4. **Reading in, never writing out.** Values from outside the block (parameters, outer bindings, module constants, literals) are read normally; the outer region outlives the block. What leaves the block is governed by D4.
5. **Allocation outside the top region.** Only a converted allocator uses the region. Every unconverted one keeps calling `malloc` and is never freed, exactly as today. So the migration has S1's property: **it can stop at any commit and be correct**; an unconverted producer only leaks, as it does now.

### D4. Escape rules

The checker (the E5001 walk in `src/types.c`, extended to the whole block body, recursively) classifies every way a region value can outlive the block. A value is **region-bound** when its static type is a heap kind: `$str`, an array (including `[byte]`), a map, a struct or sum, an error union with a heap payload, a closure. Scalars (`i64`, `u8`…`u64`, `f32`/`f64`, `bool`) are never region-bound.

| # | Escape route | Rule |
|---|---|---|
| E1 | Assignment to a name declared outside the block (`s=t`, `xs=xs.append(v)`, a loop's outer `mut`) | **E5001** when the assigned value is region-bound; allowed when it is a scalar (matches `docs/reference/statements.md`: "computed results (non-pointer values) may be written to outer mutable bindings") |
| E2 | `<` (and `!` propagation) inside the block | **Copy-out**: the returned value is deep-copied into the region below the outermost `{arena}` of this function's activation (or the global heap when there is none) before the release. This is `memory-model.md` §2.3's existing return rule, applied to the block |
| E3 | A closure created in the block that escapes (E1 or E2) | **E5001**. Its environment is `malloc`'d (§2.5) but its captured strings point into the region; copying a closure out is not proposed |
| E4 | A region-bound argument to a std/FFI parameter that **retains** it (router tables, `http` handler state, `vecstore` entries, `llm`/`llmtool` client structs, html/svg trees, `std.mem`) | **E5001**, driven by a load-bearing `.tki` field (D7). An unaudited module counts as retaining every heap argument |
| E5 | An `sc` task inside the block (amended 2026-11-28, story 127.568, ADR-0017 D5.4) | allowed when the task's `sc` block is also inside this `{arena}` block, so the task joins before the region is released; **E5003** when the task belongs to an `sc` block outside it, because the region would be released while the task could still read values captured from it. A task allocates on its own thread outside any region (none, unless the task opens its own block), so its typed result is not region-bound and E5001 still rejects storing a region-bound value out of the block by its type. A thread that runs a queued task inline masks its region stack for the duration (ADR-0017 O4). *Superseded basis:* this row first said tasks are allowed because "task functions take no arguments and return `i64`", which ADR-0017 ends (tasks take arguments and return typed heap values). |
| E6 | A value flowing in through a parameter or outer binding | always allowed (D3.4) |
| E7 | Module constants and declarations | cannot be assigned (E4070); not an escape |

E2 versus rejection is open question Q2 for every route other than `<`: this ADR proposes **rejection for stores (E1, E3, E4) and copy-out only where the spec already says "copied" (E2)**, the conservative reading of §2.3–§2.4. A diagnostic's `fix` stays absent: no single rewrite is correct in every case (AGENTS §6).

### D5. Runtime ABI

New runtime entry points (C, in `tk_runtime.c` / `str.c`; names indicative):

```text
void   *tk_rgn_enter(void);              /* push; returns an opaque mark       */
void    tk_rgn_leave(void *mark);        /* pop + release everything since mark */
void   *tk_rgn_alloc(size_t n);          /* top region, or malloc when none    */
void   *tk_rgn_realloc(void *p, size_t o, size_t n); /* never realloc() a region block */
int64_t tk_rgn_copyout_<kind>(int64_t v);/* deep copy for D4 E2, per heap kind  */
```

- **Codegen** emits `tk_rgn_enter` at block entry and `tk_rgn_leave` on every exit edge (D3.1), and the copy-out call before the leave on a `<`. The region stack is thread-local, declared the way the stdlib already declares per-thread state (`_Thread_local` or `__thread` behind a feature check, as `db.c`'s error buffer is).
- **Malloc'd kinds.** A region is a list of chunks (say 64 KiB, larger blocks get their own chunk); release returns its chunks to a per-thread free list. Converted in order: `tk_arr_alloc` and codegen's inline array/spread `malloc`, map and struct/sum boxes, then glue producers module by module, with a census like S1's (`runtime-abi.md` §3.1 "Consumer inventory").
- **Never `free()` a region block.** Two existing frees must stay on plain `malloc`: C2200's interpolation conversions (`str_from_int` / `str_from_float` carry a comment saying so) and 124.0c's closure environments. A converted producer whose result some path `free()`s is a bug the ASan run in each slice reports ("attempting free on address which was not malloc()-ed").
- **Fork.** The http server forks workers; each child inherits a copy of the parent's region state. Requests run in the children, so per-process stacks are correct; the `pthread_atfork` handler S1 already installs must also cover the region free-list lock.

### D6. S1 sealed strings: per-thread sealed chunks, and the amended rule

The sealed heap stays one 16 GiB reservation with its bitmap, and S1's reader-side trust check is unchanged (range, granule +8, bit). The **allocation** side gains a region path:

1. With no region active, `tks_alloc` is unchanged: a global atomic bump, never freed.
2. With a region active, `tks_alloc` bumps inside a **sealed chunk** that the region took from the reservation (a chunk is claimed with one `fetch_add` on `tks_top`, then filled without atomics by its one owning thread). Interleaving with other threads therefore cannot break release, which a plain mark/reset of the global `tks_top` would.
3. **Release** of a region, for each of its sealed chunks: clear the chunk's bitmap bits (so a stale pointer reads as unsealed and falls back to `strlen`, never a stale header), re-poison it under ASan, and return it to a free-chunk list for reuse by later regions. `madvise(MADV_DONTNEED)` or its macOS equivalent above a high-water mark is a tuning choice for the slice, not part of the ABI.
4. **S1's rule becomes:** *"a sealed string is never written, freed or reallocated, **except that the release of the `{arena}` region whose chunk holds it clears its bit and returns the chunk for reuse**."* Sealed strings made outside any region keep the old rule unchanged.
5. **Why not strings outside the sealed heap inside a block** (the alternative in Q1): it gives up S1's O(1) `.len` for every string a request makes — the scanners S1 was built for — and doubles the producer paths. Recommended against, but it is the owner's call.
6. **Dangling reads.** A pointer kept past release (a missed escape) into a reused chunk would read another string's bytes: a silent wrong answer, the failure S1 was designed never to have. Mitigation: under `TK_STR_VERIFY=1` and ASan builds, released chunks are quarantined (poisoned, never reused), so the conformance runs turn every missed escape into an ASan report. Production builds reuse.

### D7. Interaction with ADR-0006 (refcount / CoW)

- `rc` stays monotonic and keeps deciding copy-on-write only; region release does not read it.
- The in-place fast path (`x = x.append(v)` with `rc == 1`, the `la_*` linear-array analysis in `src/llvm.c`) is safe inside a block because D4 E1 rejects storing a region-bound value into an outer name: an array that grows in place inside the block is region-local, and growth uses `tk_rgn_realloc` into the same region. An outer array cannot be appended to inside the block without assigning to the outer name, which E1 rejects — the price of rejection over copy-out (Q2).
- A region-local array whose block was copied out (D4 E2) gets a fresh `rc == 1` header in the destination.

### D8. The `.tki` ownership field becomes load-bearing

`memory-model.md` §4.4 and the `.tki` `"ownership"` field (rare today, `"caller"` where present, and unenforced) gain one checked meaning: a parameter marked **retained** may not receive a region-bound value inside `{arena}` (D4 E4). Each module is audited (S1-census style) and marked `retained` / `borrowed` per heap parameter; an unaudited module is treated as retaining. Whether this is the mechanism is open question Q3; the alternative — glue that always copies what it keeps, as 127.343 does for router strings — moves the cost to every call, inside a block or not.

### D9. Threads and workers

- Region stacks are per thread (D3); a region is never shared, and no lock is taken on the allocation fast path.
- `sc` tasks (pool threads, or a joining thread that runs a queued task inline with its own region stack masked, ADR-0017 O4), the `tls.c` accept thread, `mdns` browse and `infer_stream` prefetch threads allocate on their own threads, outside any region of the starting thread; a block on the starting thread neither covers nor releases their memory. A task's result is a typed heap value made outside any region; a task that wants bounded memory opens its own `{arena}`. A task inside an `{arena}` block must belong to an `sc` block inside it (E5003, D4 E5). (Amended 2026-11-28, story 127.568, ADR-0017 D5.4: this bullet first rested on tasks taking no arguments and returning `i64`.)
- Forked http workers each have their own heap and stacks (D5). A request handler that opens a block releases that request's memory in the worker before the next request.

## Migration

1. **No source migration.** No measured program uses `{arena}` (Context), and the block's meaning changes only from "no-op" to "release". The checker change (E5001 over the whole body, D4) can reject previously-accepted code only inside `{arena}`; it ships first, under a warning release (W5001-style) if the owner wants one, and is measured over the toke tree, ooke, the website, toke-test-programs and the corpus before it lands.
2. **127.524 first.** The §1 amendment states the shipped model and that `{arena}` is reserved for the region this ADR defines; the docs listed under 127.528 are corrected in the same pass.
3. **Runtime slices**, each conform-gated and separately mergeable, in this order: (i) region stack + `tk_rgn_*` with no converted allocator (behaviour identical; IR shape only); (ii) arrays and boxes; (iii) sealed chunks + the amended S1 rule; (iv) glue producers module by module with the `.tki` audit; (v) copy-out for `<`.
4. **ooke** wraps `serve`'s per-request handling and the render loop in a block once (ii) and (iii) land, through `make check-toke-candidate`; the website follows.

## Conformance plan

Reserved series per slice (numbers assigned when each slice is a story):

- **D-series (YAML)** for every D4 route: E1 heap (E5001, `expected_fix_absent`), E1 scalar (clean), E2 (clean), E3 (E5001), E4 retained / unaudited (E5001) and borrowed (clean), E5 (clean), nested blocks, a block in a loop with `br`. The existing D049–D051 stay clean.
- **C-series**, each in the C2200 style: (a) IR shape — `tk_rgn_enter` once, `tk_rgn_leave` on every exit edge including `br`, `<` and `!`; (b) a run under ASan+UBSan with `TK_STR_VERIFY=1` (quarantine on) of every escape route, the stdlib built through the same ASan wrapper; (c) a fork case (http worker, 127.0.0.1, a port of the slice's batch) and a threaded case (`sc` tasks allocating while the parent's block releases); (d) **negative controls**: a scratch build that releases one edge early or skips copy-out must fail (b) with `heap-use-after-free` or a poisoned read.
- **Gates.** `make conform` 100%, `make test-asan-compiler`, `make check-gbnf` (unchanged grammar), `make check-facts` (53 productions), ooke `make check-toke-candidate` + `make test`, the website's `make check-toke`.

## Measurement plan

The 127.299 note §7–§8, unchanged in method:

1. **Harness.** The Z77 template harness (`test/z256bench` in a scratch ooke worktree), `lex` and `render` at 20, 200 and 2000 iterations, with and without one `{arena}` around the loop body, against the candidate and the same-tree base.
2. **Primary metric: bytes per iteration** = the slope of (sealed bytes handed out + malloc bytes − freed bytes) between 200 and 20 iterations. **Accept a slice when the slope for code inside the block is zero** (127.299 §1 shows every byte is reclaimable there) and the slope outside is unchanged. Peak RSS is reported, not gated (it varies 2× at 2000 iterations on macOS).
3. **Counters.** A committed `TK_MEM_STATS=1` exit report (sealed bytes, region bytes released, malloc calls/bytes, frees) replaces the scratch interposer: 127.529, a prerequisite of slice (ii).
4. **Cost.** Wall time of the harness and of `ooke` page builds must not regress beyond noise (three to five runs, medians); copy-out cost is measured separately on a handler returning a page-sized string.

## Consequences

- Servers and render loops get bounded memory by writing one block, with the lifetime stated by the programmer and checked at the block's edges, not inferred for every function.
- S1 keeps its reader path and its O(1) `.len`; its rule gains one stated exception, and its allocation path gains a per-thread chunk.
- The `.tki` ownership field stops being documentation, which costs one audit per module.
- Code inside a block cannot append to an outer array or store a string into an outer binding (until Q2 says otherwise).
- A missed escape is a use-after-free in production builds; the quarantine mode makes it visible in tests, not impossible.

## Alternatives considered

The 127.299 note's options: A (per-function arenas with escape analysis — very high risk, deferred to Q4), B (scope-exit frees of temporaries — blocked by freshness/alias proofs and S1 P3), D (release refcounting — ADR-0006 rejected it on risk), E (the shipped model plus a leak budget — that is 127.524, needed regardless). A runtime-only per-request region inside `http.c` (no syntax, the server wraps every handler call) is a variant of this ADR that covers servers but not render loops or batch jobs; listed as Q5.

## Questions for the owner

1. **S1 (127.299 note Q1).** Accept the amended rule and per-thread sealed chunks (D6), or allocate strings outside the sealed heap inside a block?
2. **E5001 versus copy-out (note Q2).** Reject stores of region-bound values to outer names (proposed), or copy them out (permissive; needs region-aware allocation and per-store copies)? Copy-out for `<` is proposed either way, from §2.3.
3. **`.tki` ownership (note Q3).** Make `retained` load-bearing with a per-module audit and "unaudited means retaining" (proposed), or require glue to copy whatever it keeps?
4. **Per-function arenas (note Q4).** Design them later together with 127.236's return slot, or drop §1.1's per-function arena from the spec in 127.524 in favour of `{arena}` plus the amendment?
5. **Servers without syntax.** Should `http.serve` additionally run each handler call inside an implicit region (D5's ABI, no source change), or must a program write the block itself?
6. **Checker rollout.** Should the extended E5001 (D4) ship as a warning for one release first, given that no measured program uses `{arena}`?
7. **Production reuse of released chunks.** Reuse in production builds (proposed; quarantine only under `TK_STR_VERIFY` / ASan), or quarantine always at the cost of address-space growth?
