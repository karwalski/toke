---
title: ADR-0017 — Structured concurrency: sc is the one construct, with typed task handles
slug: ADR-0017-structured-concurrency
section: decisions
order: 24
---

**Date:** 2026-11-28
**Status:** **Accepted 2026-11-28** (owner rule 2026-11-28: decide by toke's design principles in docs/about/design.md over prior decisions, existing code or the easier path; the seven questions are resolved in the last section, "Owner resolutions"). Where the body says "proposed", read "decided", except where a resolution below changes it.
**Deciders:** Matthew Watt (pending)
**Stories:** 127.553 (the `sc` / `spawn` construct) and 127.554 (typed task results), under the owner decision of 2026-11-28 recorded in both tracker rows, which supersedes D4=B (story 10.3.11, 2026-04-04) for concurrency. Related: 127.548 (the C-ABI adapters for function values), 127.549 (task.spawn and the keyword through those adapters; E4081), 138.47 (scope handles looked up, not read), 127.101 / 127.109 (per-thread error slot and error box), 124.0c (closure environment counting), ADR-0006 (array copy-on-write), ADR-0016 (the `{arena}` block, proposed), `docs/spec/memory-model.md` §3.

## Context

### The owner decision

`sc{}` is toke's single canonical concurrency construct. The compiler enforces that every task started in a scope is joined at the scope's exit, so the model cannot forget the join, as it cannot forget an error. It is a language construct and needs no import. The user-facing `std.task` surface (`task.scope`, `task.spawn`, `task.awaitall`, `task.result`, `task.cancel`) is withdrawn as a synonym: it becomes runtime-internal, and diagnostics drive the migration. Starting a task yields a checker-typed handle that carries the started function's return type, and reading the result is typed: a `str`, array or record result comes back as its value. There is no untyped `i64` accessor and there are no per-type accessor functions. The decision rests on `docs/about/design.md` (one canonical form, compiler-verifiable, explicit typing, no undefined behaviour), not on the existing code.

### What ships today (measured on main 1c897eaa, tkc built from that tree)

- **Lexer.** `sc` is one of the 14 keywords (`src/lexer.c`, `docs/metrics-baseline.md` § Project facts). `spawn` is not a keyword: the parser treats an identifier spelled `spawn` at the start of an expression as the spawn form, but only while it is inside an `sc` body (`p->in_sc`, `src/parser.c`).
- **Parser.** `sc{ StmtList }` is `NODE_SCOPE_STMT`; `spawn <expr>` is `NODE_SPAWN_EXPR`, accepted as an expression or as the initialiser of a `let`. `docs/spec/grammar.ebnf` has no production for either. `docs/spec/toke.gbnf` has `scope ::= "sc" ws block` and accepts `"spawn" expr-aw` as a unary expression anywhere.
- **Checker.** `NODE_SCOPE_STMT` checks its body and is `void`. `NODE_SPAWN_EXPR` is typed `i64` and its operand must be a `():T` function value, else E4081 (127.549). `task.spawn(s;x)` gets the same E4081 check through `check_task_spawn_call`.
- **Lowering.** `sc` calls `tk_task_scope()` at entry and `tk_task_awaitall()` after its last statement. `spawn` and `task.spawn` call `tk_task_spawn_fv(scope, adapter, value)`: the 127.548 C-ABI adapter returns the result widened to a defined `i64`, and the task owns one reference to a closure.
- **Runtime.** `src/stdlib/task.c`: one process-wide pool (one worker per online CPU, at most 64), at most 256 tasks per scope, at most 4096 task handles in the life of the process (`task.spawn` returns `-1` past either), `awaitall` busy-waits with `sched_yield`, scopes and tasks are never freed, and `task.cancel` skips tasks that have not started.

Probes in scratch on that build:

| Program | Result |
|---|---|
| `sc{ let h=spawn &w; if(n>0){<task.result(h);}; };` | The `<` returns **before** `tk_task_awaitall` (the IR has `ret` ahead of the join), and `task.result` of the unfinished task gives `-1`: five runs out of five printed `-1`. The join is not guaranteed on an early exit. |
| `sc{ let h=spawn &w; };` without `i=task:std.task;` | Does not link: undefined `tk_task_awaitall` (127.553 item 1). |
| `let spawn=3; sc{ let x=spawn+1; };` | E2002 at the `+`: inside `sc` a binding named `spawn` cannot be read. |
| `sc{ let g=fn():i64{let k=spawn &w;<0}; };` | Invalid IR (`use of undefined value '%t0'`): the closure's lifted body refers to the enclosing function's scope temporary. |
| `let fs=mut.@(():i64);` | E2002: a function type cannot be written in a typed-empty literal. |

### Who uses it

| Where | `std.task` calls | `sc` / `spawn` form |
|---|---|---|
| toke `test/` | `task.scope` 12, `task.spawn` 51, `task.awaitall` 13, `task.result` 35, `task.cancel` 3, in `test/standalone/test_127_549.tk` and the shell tests C020, C105, C736, C2063, C2305, C2306, C2307, C2309; 12 `std.task` imports | `test_127_549.tk`, C736, C2307, C2309, `test/conform/fixtures/python_view/edges.tk`, `test/gbnf/fragments.json` and the gbnf extras x41, x71, x84, x88, x90, x93 |
| toke `src/` | `src/stdlib/task.c` / `task.h`, the `tk_task_*_w` wrappers in `src/stdlib/tk_web_glue.c`, the `task` naming fallback and the `tk_task_*` declarations in `src/stdlib_tables.c`, `check_task_spawn_call` in `src/types.c`, the `tk_task_spawn` receiver path in `src/llvm.c` | `NODE_SCOPE_STMT` / `NODE_SPAWN_EXPR` in `parser.c`, `names.c`, `types.c`, `llvm.c`, `fmt.c`, `pyview.c`, `lint.c`, `ast_json.c`, `tkir_ast.c` |
| toke `docs/` | `docs/stdlib/task.md`, `docs/stdlib/index.md`, `docs/spec/stdlib-signatures.md`, `docs/spec/semantics.md` §1.3 / §2.6 / §3.1 / §4.3 / §7.1 / Appendix A, `docs/spec/errors.md`, `docs/reference/errors.md` (E4081), `docs/reference/stdlib-internal.md`, `docs/runtime-abi.md`, `docs/known-limitations.md` §6, `CHANGELOG.md` (history, not migrated) | `docs/about/design.md` (the `sc` keyword row), `docs/reference/grammar.md` |
| ooke main (`tmpwork/ooke_main2`) | none | none |
| toke-website | the mirrored copies of the five toke doc pages above | the mirrored `semantics.md`, `errors.md` and the 96.7 decision page |
| toke-corpus | no v0.4 regen record and no library program; only the frozen `data/tokenizer_training_pre131.txt` (the `std.task` interface line, five times) and its manifest | none |
| toke-test-programs | `infra/worker-docs/stdlib-signatures.json`: the five `std.task` entries | none |
| toke-mcp | none | the `sc` row of `skills/toke-language.md` and its plugin copy |

toke-eval and every `hidden_tests/` directory were not read.

The construct is therefore used almost only by the compiler's own tests. Fixing it now costs a test migration, not a user migration.

## Decision (proposed)

### D1. Syntax: `sc` is the block, the task and the handle type

One keyword spells all three parts of the construct. No keyword is added, so the keyword count stays 14, and `spawn` stops being special.

```text
ScopeStmt  = 'sc' '{' StmtList '}' ;          (* statement: opens a scope   *)
ScTaskExpr = 'sc' CallExpr ;                  (* expression: starts a task  *)
ScType     = 'sc' '(' ')' ':' Type ;          (* the handle's type          *)
```

1. **The block.** `sc{ ... }` is a statement, as today. It has no value (D2.4).
2. **Starting a task.** `sc` followed by a **call** starts that call as a task in the innermost enclosing `sc` block of the same function body: `sc work(x)`, `sc w()`, `sc g()` for a function value `g`, `sc fn():i64{<a+b}()` for an inline body. The callee and the arguments are evaluated on the starting thread, left to right, exactly as for an ordinary call. Only the call itself runs on the task. The parser tells the two forms apart by the token after `sc`: `{` opens a block, anything else is a task. That is one token of lookahead, and the grammar stays backtrack-free.
3. **The handle.** `let h=sc work(x);` binds the handle. A task started only for its effect may stand as a statement, `sc log(x);`, and is still joined at the scope's exit (D2). A fallible one may not (D3.2).
4. **Reading the result.** A handle is read by calling it: `h()`. The type of `h()` is the type the call would have had. Reading joins that one task (D2.3). Reading it again gives the same value. The task is not run again.
5. **After the scope.** A handle cannot be used after its scope ends. Names bound in the block go out of scope at its `}`, and every other way out (returned, stored outside, passed, captured) is a compile error (D4.3, E4083). There is no run-time "used after its scope" state to define.

**Example** (a sketch of the proposed form; it does not compile on today's tkc):

```text
m=demo;
i=io:std.io;
f=count(path:str):i64{<path.len};
f=main():i64{
  let paths=@("a.txt";"bb.txt";"ccc.txt");
  let total=mut.0;
  sc{
    let hs=mut.@(sc():i64);
    lp(let i=0;i<paths.len;i=i+1){hs=hs.append(sc count(paths.get(i)))};
    lp(let i=0;i<hs.len;i=i+1){total=total+hs.get(i)()};
  };
  io.println("\(total)");
  <0;
};
```

**Why the call form, `sc f(x)`, over the function-value form `spawn &f`:**

- *One form, and it is the form models already write.* With call form, every task start is `sc <call>`: a named function, a function value and an inline closure all have one spelling. `spawn w()` is what 127.549 found written for "run `w` as a task", and E4081 had to reject it because the old lowering ran `w` at once. Under this decision that instinct is the canonical form, and the trap disappears. A model's priors from Go (`go f(x)`), Python (`create_task(f(x))`) and Kotlin (`async { f(x) }`) all put the call in the operand.
- *Arguments without a closure.* A task with arguments is `sc work(x)`, not `spawn fn():i64{<work(x)}`. That is several tokens fewer for every parameterised task, which is the common case in a fan-out. No measured token figure is claimed here; the difference is visible in the spellings.
- *Defined argument timing.* "Arguments are evaluated now, the call runs on the task" is Go's rule and the same as a closure's capture by copy (memory-model §2.5). Nothing is evaluated lazily except the call.
- *Rejected: both forms.* Accepting `sc &f` and `sc f()` would be two spellings of one thing, a synonym that `design.md` prohibits.
- *Rejected: the function-value form alone* (`sc &f`, `sc g`, `sc fn():T{..}`, 127.549's operand rule). It keeps E4081's rule but needs a closure for every argument and keeps the trap that the natural spelling is an error. It is open question Q2.

**Why `sc` and not `spawn` for the task:**

- *Reserving `spawn` costs a 15th reserved word.* An identifier that is a form at the start of an expression must be reserved, or code that uses it as a name breaks. The probe above shows the break: `let spawn=3; sc{let x=spawn+1;};` is E2002. Reserving `spawn` everywhere is a keyword in all but name, and `design.md` states that exactly 14 identifiers are reserved.
- *A context-sensitive word is the alternative,* and it is what ships: `spawn` means one thing inside `sc` and another outside. That is the "context-sensitive semantic change" `design.md` rules out, and toke.gbnf already cannot express it (it accepts `spawn` everywhere).
- *`sc` is already the structured-concurrency keyword* (`design.md`: "Scope block (structured concurrency)"). Using it for the block, the task and the type makes the construct one word. A task expression outside a block is a checker error with a message that names the block (E4082), so the two roles cannot be confused silently.
- *Cost.* Both `sc` and `spawn` are short words. No token-count difference is claimed between them.
- *Human reading.* `let a=sc fetch(u);` reads as "a is fetch(u) in the scope". It is less familiar than `spawn`, which is the price of keeping the keyword set closed. This is open question Q1.

**Why read by calling, `h()`:**

- *No new word or operator.* A keyword (`await`) would be a 15th keyword. A postfix operator would have to come from the 59-character set, where `!` is propagation and `.` is field access. A property (`h.v`, `h.result`) is a name to learn that reads like a field but blocks.
- *The type falls out of what is already there.* A handle's type is written `sc():T`, and calling it gives `T`, the same as calling a `():T` function value. A reader who knows function types knows the read.
- *Errors read exactly like a call.* For a fallible task the read returns the error union, so `h()!` propagates and `mt h(){$ok:v ..;$err:e ..}` matches, under the existing rules (semantics.md §5.1-§5.2, D3).
- *The join point is visible.* `h()` is in the source where the program waits. The implicit-future alternative (`let x=sc f();` with `x` typed `T`, joined on first use, like Swift's `async let`) costs no tokens but hides a blocking point and makes a value's type lie about whether reading it blocks. It is rejected under "explicit everything".
- *Cost.* `()` is two characters, and `task.result(h)` needed a module alias, a call and an argument.

### D2. Join semantics

1. **Every exit joins.** Leaving an `sc` block on any path joins every task started in it: falling off the end, `<` / `rt` (after the returned value is evaluated, so the value may read handles), `<` in an `mt` arm, `br` out of an enclosing `lp`, and `!` propagation (an early return). The lowering emits the join on **every exit edge** of the block, with the exit-edge machinery that already releases closure bindings (124.0c), not only after the block's last statement. A conformance test checks the IR: every `ret` and every `br` that leaves an `sc` block is preceded by the join. This repairs the early-return defect in the probe table.
2. **Order.** The join waits for all of the block's tasks. Tasks share no mutable state (D5), so completion order cannot change a value. It can change the interleaving of side effects (two tasks printing). That is an unspecified order among defined outcomes, not undefined behaviour, and `docs/spec/semantics.md` will say so (Q7).
3. **Reading joins one.** `h()` before the block's end waits for that task only. If the task is still queued, the reading thread takes it off the queue and runs it itself. If it is running, the reader waits on the task's completion signal. It does not spin.
4. **No value.** The block is a statement. A result leaves it as a plain value: `total=total+h();` assigns an outer `mut`, or `<h()` returns it. A block with a value would need a yield form, and `if`'s value form (ADR-0007) relies on the last expression, which a block of statements does not have.
5. **A trap ends the program.** A trap (RT002-RT010) in a task aborts the process, as it does on the main thread. No join is attempted. Nothing partial is observable after an abort.
6. **Nesting.** An `sc` block may contain another `sc` block, directly or inside a function a task calls. The inner block joins before the outer one can. A task expression belongs to the innermost block of its own function body (E4082). A closure's body is a function body, so a task expression in a closure needs a block in that closure. Today's code instead emits the enclosing function's scope temporary there and produces invalid IR, as the probe shows.

### D3. Errors from tasks

1. **A fallible task's error is its result.** For `f=load(p:str):$doc!$ioerr`, `sc load(p)` has type `sc():$doc!$ioerr`, and `h()` is a `$doc!$ioerr`. It is unwrapped like any union: `h()!`, `mt h(){..}`, or a `let r=h();` that is later matched. Using it as a `$doc` is E4031, the existing rule (semantics.md §5.1). The error reaches the reader with its payload, as if the reader had made the call.
2. **A fallible task's result must be read.** It is E4084 when `sc` starts a call whose type is an error union and the handle is not read in the block: the task stands as a statement, or it is bound and no `h()` of that binding (or of an element of the collection it was put in) appears in the block. *Superseded by owner resolution 5: the handle must be read on every path through the block.* (As first proposed: the rule was flow-insensitive, like the union-consumption rule of 127.323 (semantics.md §5.1), and a read on one branch satisfied it.) No `fix`: whether the read should be `!` or `mt` is the program's choice.
3. **The scope does not fail by itself.** An error that is never read is not raised at the block's exit. E4084 makes "never read" a compile error instead, so an error cannot vanish without the source saying so. An implicit raise at `}` would be hidden control flow and would pick one error among several by timing.
4. **`!` inside the block** returns the error after the join (D2.1). Tasks still running when it fires run to completion first.
5. **Cancellation: none.** Every task started runs to completion exactly once. There is no cancellation on the first error, on early exit or by request.
   - toke has no cancellation points (no `await`, no yields), so cancelling could only skip tasks that have not started. Whether a task started depends on scheduling, so skipping makes a program's side effects depend on timing. That is the non-determinism the corpus cannot have (memory-model §6.6, rationale).
   - `task.cancel` had exactly that semantics: a skipped task's result read as `-1`, which was indistinguishable from a returned `-1`.
   - A later ADR may add explicit cancellation. It must make it observable in the type: the read of a cancelled task returns an error variant, so the existing `mt` / `!` rules force the program to handle it. This ADR reserves nothing for it.
   - The cost: on an early exit or an error, the block still waits for every task it started. A program that wants to stop early has to structure its tasks to finish early.

### D4. Typing

1. **The handle type** is written `sc():T`: the `sc` keyword before the function type of a no-parameter function returning `T`. The parser's type production gains one alternative. `T` is any type, including an error union (`sc():$doc!$ioerr`) and `void`. Two handle types are equal when their `T`s are equal. A handle is not a `():T` function value, and neither converts to the other.
2. **Rules.**
   - `sc E` requires `E` to be a call expression (`NODE_CALL_EXPR`, a receiver method call, or a call of an immediately applied closure). Otherwise it is **E4085** (operand of an `sc` task is not a call). No `fix`: `sc &w` is fixed by `sc w()`, but a bare value could have been meant as a call with arguments.
   - `sc E` is `sc():T` where `T` is the call's type.
   - `h()` requires `h : sc():T` and gives `T`. `h(x)` is E4026 (wrong argument count), which has that meaning already.
   - `sc E` outside an `sc` block of the same function body is **E4082**. It is checked by the checker, not the parser, so the grammar stays context-free.
   - A handle used as an operand of an arithmetic or comparison operator, in an interpolation, or with `as` is E4031 (type mismatch). A handle has no value but its result.
3. **Confinement (E4083).** A value whose type contains a handle type (a handle, an array or map holding handles) may not outlive the innermost `sc` block that started the task. Every route out is **E4083** (a task handle escapes its scope), with no `fix`:
   - writing `sc():T` in a function parameter, a return type, a record field, a function type, a map key, or a module-level binding. It may be written only in a local `let` annotation and in a typed-empty literal (`@(sc():i64)`, `@($str:sc():i64)`);
   - `<` returning it;
   - assigning it, or a collection updated with it (`xs=xs.append(h)`), to a binding declared outside the innermost `sc` block that started the task;
   - passing it as an argument to any call: a user function, a closure, a stdlib function, or a task's call. The receiver of a collection method (`hs.append(h)`, `hs.get(i)`, `hs.len`) is not an argument;
   - capturing it in a closure.

   Handles are therefore lexically confined. That lets the runtime free task descriptors at the block's exit (D6) and removes the process-wide handle cap. A task that depends on another's result reads it first and passes the value: `let a=h(); let k=sc next(a);`. Pipelining one task on another inside a task (Q4) is given up for a rule the checker decides lexically.
4. **Diagnostic codes.** New codes go at the end of their series (AGENTS.md §6): the highest code in the E2xxx range is E2037, in E4xxx it is E4081, in E5xxx it is E5002, and the highest runtime trap is RT010 (`src/types.h`, `docs/reference/errors.md`, `docs/runtime-abi.md` §9).

   | Code | Stage | Meaning | `fix` |
   |---|---|---|---|
   | E2038 | parse (emitted by the parser and by name resolution) | Use of the withdrawn concurrency surface: an `i=..:std.task` import, a call through such an alias, or the `spawn` form. The construct is `sc`. | Only for `spawn X` where `X` is `&name` or a value the checker types `():T`: the rewrite `sc name()` / `sc X()`, which is equivalent in every such case. Absent for an import or a `task.*` call, whose rewrite is structural. |
   | E4082 | type_check | A task expression `sc E` is not inside an `sc` block of its own function body. | none |
   | E4083 | type_check | A task handle escapes the `sc` block that started it (D4.3). | none |
   | E4084 | type_check | A fallible task's result is never read in its block (D3.2). | none |
   | E4085 | type_check | The operand of an `sc` task is not a call (D4.2). | none |
   | E5003 | arena_check | A task started inside an `{arena}` block belongs to an `sc` block outside it, so it could outlive the region (D5.4). | none |
   | RT011 | run time | A task's descriptor could not be allocated. Today `task.spawn` returns `-1` silently, and the result reads as `-1`. | n/a |

5. **Retired codes stay retired.** E4050 (spawn argument not callable), E4051 (await argument not a Task) and E4052 (spawned function has parameters) were removed under D4=B (story 10.3.11). None is reused. E4050 and E4052 were rules about a function-value operand, which this decision does not have. E4051 belonged to `await`, which does not come back. Reusing one for a call-form or confinement rule would change its meaning, which AGENTS.md §6 forbids. `docs/spec/semantics.md` Appendix A lists them as retired.
6. **E4081 retires with the surface it judged.** Its meaning, that the operand of `task.spawn` / `spawn` is not a function value taking no parameters, applies to two forms this decision withdraws. After the withdrawal it is never emitted. Its `docs/reference/errors.md` entry stays as a retired code (number kept, meaning unchanged), and its tests move to E2038 and E4085. If the owner keeps the function-value form (Q2), E4081 stays live, keeps its meaning on `sc`'s operand, and E4085 is not added.

### D5. Memory and ownership

1. **What crosses threads.** The arguments of a task's call are evaluated on the starting thread and copied into a compiler-built environment, as a closure's captures are (memory-model §2.5). The environment is `malloc`'d and owned by the task (+1), and it is released when the call returns. This is 127.549's closure ownership, unchanged. The result goes the other way, by the return rule (memory-model §2.3, §3.4): a scalar by value, a heap value by reference to the same allocation.
2. **Why sharing by reference is race-free, and what that needs.**
   - Strings are sealed and never written (S1).
   - Arrays and maps are copy-on-write with a monotonic count (ADR-0006). The in-place fast paths (`tk_array_append_inplace_w`, `tk_map_set_inplace_w`) run only on a binding the codegen has proven linear. **Proof obligation O1:** an array or map passed to a task's call, or returned from one, must count as an alias in `la_*` (`src/llvm.c`), so neither side ever mutates it in place. Conformance tests: the parent appends to, and `.set`s, a map it passed to a task while the task reads it, run under ThreadSanitizer.
   - **O2:** `tk_arr_retain` increments `rc` with a plain store. Two threads that duplicate one array race on it. That is a C data race even though a lost increment leaves `rc >= 2` and copy-on-write still holds. Make the increment an atomic relaxed add. Closure counts are already atomic (127.549).
3. **Errors cross explicitly.** A task's error box lives in its thread's per-thread error buffer (127.109), which the next task on that worker reuses. **O3:** the adapter copies the error box and the error status out of the worker's thread-local slots into the task descriptor before the worker takes another job. A read restores them into the reader's slots, so `!` and `mt` see the convention a direct call leaves (127.101, 135.16).
4. **ADR-0016 (`{arena}`, proposed).** Regions are per thread, and a task allocates on its own thread outside any region. Three rules make the two ADRs compose:
   - **E5003.** A task expression inside an `{arena}` block must belong to an `sc` block that is also inside that `{arena}` block. Otherwise the region is released at the `{arena}` exit while the task may still read values captured from it.
   - **Inline runs suspend the region.** A reader or joiner that runs a queued task itself (D2.3) masks its thread's region stack for the duration (O4), so the task allocates exactly as it would on a worker. Without the mask, a result made inside a region could be read later through the handle after the region is gone.
   - **ADR-0016's D4 E5 and D9 need amending.** They rest on "task functions take no arguments and return `i64`", which this ADR ends. Results are typed heap values, made outside any region, and E5001 still rejects storing one out of an `{arena}` block by its type. A task that wants bounded memory opens its own `{arena}`.
5. **memory-model §3 is corrected, not just extended.** §3.2 says values "must be copied at the point of task creation". What ships, and what this ADR keeps, is a copy of the bindings with heap values shared under the immutability and copy-on-write argument of D5.2. §3.3 (lifetime containment) becomes a checked property: D2.1 joins on every edge, and D4.3 confines handles. §1.3 gains one release: task descriptors are freed at the block's exit, when no toke value can reach them (D4.3). Task results are not freed.

### D6. Runtime mapping

The pool stays in `src/stdlib/task.c` (renaming the file is the implementation story's call). Its entry points become runtime-internal names that the `tk_<mod>_<method>` naming fallback cannot produce. That is how 127.549 hid `tk_task_spawn_fv`.

| Today | Becomes | Change |
|---|---|---|
| `tk_task_scope()`, toke `task.scope` | `tk_sc_open()`, emitted at `sc{` | A scope owns a growable task list. The 256-task limit goes. |
| `tk_task_spawn_fv(scope, adapter, fv)`, toke `task.spawn` | `tk_sc_start(scope, adapter, env)`, emitted at `sc E` | `env` is the compiler-built call thunk (D5.1). It returns a task pointer local to the scope, not a process-wide number, so the 4096 cap goes. A descriptor that cannot be allocated traps RT011. |
| `tk_task_result(handle)` → widened `i64`, toke `task.result` | `tk_sc_read(task)` → `i64` slot plus error state, emitted at `h()` | Joins that task: runs it inline if it is queued, otherwise waits on a per-task condition variable. The read site narrows the slot back to `T`, the inverse of the 127.548 adapter (`icmp ne 0` for `bool`, `bitcast` for `f64`, `trunc` for narrow ints, `inttoptr` for `str`, arrays and records), and restores the error state (D5.3). |
| `tk_task_awaitall(scope)`, busy-wait, toke `task.awaitall` | `tk_sc_close(scope)`, emitted on every exit edge (D2.1) | Helping join: while tasks of this scope are queued, the closing thread runs them itself, then waits on the scope's condition variable for running ones. Then it frees the descriptors. |
| `tk_task_cancel`, toke `task.cancel` | removed | No cancellation (D3.5). |
| `tk_task_spawn` (legacy C entry), `tk_task_*_w` in `tk_web_glue.c`, the `task` fallback in `stdlib_tables.c`, `stdlib/task.tki` | removed | `std.task` leaves the stdlib (D7). |

- **Helping join is a correctness property, not a tuning.** With a fixed pool, a task that opens a nested `sc` and blocks on it holds a worker. If every worker does that, the queued children never run and the program hangs. When a joiner runs queued tasks of the scope it waits on, every join makes progress even with zero free workers. By induction on nesting depth, a program cannot deadlock on its own join. Handles cannot be captured or passed (D4.3), so a task cannot wait on a sibling started after it, and there are no cycles.
- **Fork.** The http server forks workers, and pool threads do not survive a fork. A `pthread_atfork` child handler resets the pool to "not started". Helping join means a child that never restarts it still completes every scope, sequentially.
- **Linking.** A program that uses `sc` links the pool without any import. The stdlib dependency resolver (`src/stdlib_deps.c`, `resolve_stdlib_deps_imports_only`) and `--emit-deps` (`src/main.c`) add the pool's C file and `-lpthread` when the AST holds an `sc` block. This closes 127.553 item 1. Linking it into every program was considered and rejected: it adds a pthread dependency and static size to every program that has no concurrency.
- **Adapters.** The 127.548 adapter machinery (`@__tk_fvc_<key>`, emitted once per signature) is reused for the thunk. The thunk takes the environment and makes the call with the copied arguments, so its adapter key is the call's return type.

### D7. Migration

1. **Order.** The new surface lands first (parser, checker, lowering, runtime), next to `std.task`. Then one story withdraws `std.task` and migrates every user in the same commit series, so `make conform` never sees a gap.
2. **Diagnostics that drive it.** After the withdrawal:
   - `i=task:std.task;` is E2038, with the message naming `sc`.
   - Each `task.scope` / `task.spawn` / `task.awaitall` / `task.result` / `task.cancel` call through such an alias is E2038, with a message giving that function's replacement: the block, `sc f()`, nothing (the join is implicit), `h()`, and no replacement. No `fix`: the rewrite moves statements into a block.
   - `spawn X` inside an `sc` block is E2038, with the `fix` of D4.4 when `X` is `&name` or a `():T` value.
   - The parser keeps recognising `spawn` only where it is followed by `&` or `fn`, and only to emit E2038. An identifier followed by either token is never valid otherwise, so the recogniser needs no context and cannot misread a binding named `spawn`.
3. **Counts to migrate** (the Context table): in toke, 114 `task.*` calls in the test files listed and the `sc`/`spawn` forms in the test and fixture files listed. Docs as in D8. Outside toke: no ooke, website-source, corpus-record or toke-test-programs program uses either surface. The website mirrors docs pages, toke-test-programs mirrors the signature list, and toke-mcp mirrors the keyword row. Those follow the docs.
4. **Frozen data is not rewritten.** The frozen `data/tokenizer_training_pre131.txt` in toke-corpus is a dated pre-131 artefact. The tokenizer retrain (116.9) must take its input from the current docs and corpus, and the corpus story records that the `std.task` interface line is excluded there.
5. **Downstream.** Withdrawing a `.tki` module is a stdlib change, so ooke (`make test`) and the website build are rebuilt against the new toke before the merge is called done.

### D8. Grammar, spec and canonical facts

- **Keywords** stay 14 (`docs/metrics-baseline.md`, `keywords`). `spawn` stops being a contextual word.
- **`docs/spec/grammar.ebnf`** gains `ScopeStmt` (missing today), the task expression and the handle type. Each new production raises the `grammar_productions` fact, which is 53 today (`docs/metrics-baseline.md`). The fact sheet, `make check-facts` and every surface quoting the count are updated in the same commit. ADR-0016's "stays 53" is a statement about that ADR's own change and is not altered.
- **`docs/spec/toke.gbnf` / `toke-xgr.gbnf`**: `"spawn" expr-aw` becomes `"sc" expr-aw` restricted to a call. The type rule gains `"sc" ws "(" ws ")" ws ":" ws type`. `docs/spec/toke.lark` is regenerated (`scripts/gbnf_to_lark.py --write`). The fixtures `test/gbnf/fragments.json` and the extras x41, x71, x84, x88, x90, x93 are rewritten. `make check-gbnf` and `make check-grammar-artefacts` gate them.
- **`stdlib_modules`** drops by one when `stdlib/task.tki` is deleted. `diagnostic_codes_documented` / `_in_src` rise by the new codes in D4.4.
- **toke-spec's tree-sitter grammar** is stale at v0.3 (AGENTS.md §10) and is not touched. Syncing it needs its own story.
- **Rewrites, listed and not made here:**
  - `docs/spec/semantics.md` §7.1: replace `spawn(f)` / `Task<T>` / `await` and the 127.549 note with D1-D4. Also §1.3 (the `Task<T>` row and paragraph become `sc():T`), §2.6 (the special-call-forms pointer), §3.1 item 7 (handle type equality), §4.3 (drop `spawn`, `await`, `Task` from the predefined identifiers), and Appendix A (E4050-E4052 and E4081 retired; E2038, E4082-E4085 and E5003 added).
  - `docs/known-limitations.md` §6: becomes "No channels, mutexes or atomics; no cancellation (ADR-0017 D3.5)". The `std.task` workaround and the 127.549 "as implemented" paragraph are removed.
  - `docs/spec/memory-model.md` §1.3, §3.1-§3.4, §6.5, as D5.5 says.
  - `docs/about/design.md`: the `sc` keyword row ("lands in v0.5") becomes the construct. `docs/reference/grammar.md`: the `sc` row.
  - `docs/stdlib/task.md` is replaced by a language page for `sc` (under `docs/reference/`). `docs/stdlib/index.md`, `docs/spec/stdlib-signatures.md`, `docs/reference/stdlib-internal.md` and `docs/runtime-abi.md` drop `std.task`.
  - ADR-0016 D4 E5 and D9, as D5.4 says.
- **Canonical block.** `docs/about/canonical.md` does not mention concurrency, so no canonical surface changes.

## Prior art

| System | What it does | What toke takes | What toke rejects, and why |
|---|---|---|---|
| Trio nurseries | `async with open_nursery() as n: n.start_soon(f, x)`. The block cannot exit until every child finishes. A child's exception cancels the siblings and propagates. | The lexical block as the only place tasks live, and the join at its exit. Trio's argument that unscoped spawning is the concurrency `goto` is the owner's argument. | Cancel-on-exception: Trio has checkpoints to cancel at, and toke has none (D3.5). `start_soon` returns no result: toke needs typed results (127.554). |
| Kotlin `coroutineScope` | `async { f(x) }` returns `Deferred<T>`, and `.await()` gives `T`. The scope waits for its children. A child's failure cancels the scope. | A typed handle per task, read explicitly. | The read spelled `.await()` (toke's `h()` is shorter and needs no name). Failure-driven cancellation, as above. A `Deferred` can escape its scope, and toke confines it (E4083). |
| Swift `TaskGroup` / `async let` | `async let x = f()` starts a child, `await x` reads it, and unread children are cancelled and awaited at scope exit. `withTaskGroup` collects results of a dynamic number of children. | The handle bound by a `let`, and a scope exit that awaits whatever was not read. Dynamic fan-out without a second construct: an array of handles is toke's task group. | Implicit cancellation of unread children (it makes effects depend on timing). `await` as a keyword. `async let`'s implicit future type: toke's handle type is spelled. |
| Java `StructuredTaskScope` (JEP 453 era) | `scope.fork(callable)` returns a `Subtask`. `join()` waits for all. `Subtask.get()` before `join()` throws. `ShutdownOnFailure` / `ShutdownOnSuccess` policies cancel the rest. | The scope as an explicit object of the language rather than a convention. The checked lifetime of subtasks. | `get()` legal only after `join()`, which is a run-time state error that toke would have to define. In toke a read joins that one task, so there is no illegal-read state. Pluggable policies are several forms of one construct. |
| Go `errgroup` | `g.Go(func() error {...})`, `g.Wait()` returns the first error, and a derived context is cancelled on it. Results travel through captured variables. | The call form: Go's `go f(x)` evaluates `f` and `x` now and runs the call later (D1.2). | Results through shared captured variables: toke has no shared mutable state (memory-model §3.2). A forgettable `Wait()`: the join is not optional in toke. First-error cancellation, as above. |

## Implementation plan

Stories for the main thread to file. Every one is blocked on the acceptance of this ADR. "conform" means `make conform` at 100%, `make lint` with no new warning, and the tracker updated (AGENTS.md §6).

| # | Story | Acceptance criteria | Tests | conform |
|---|---|---|---|---|
| S1 | Spec amendment: structured concurrency per ADR-0017 | `semantics.md` §1.3, §2.6, §3.1, §4.3, §7.1 and Appendix A, `memory-model.md` §1.3 / §3 / §6.5, `known-limitations.md` §6, the `design.md` and `grammar.md` `sc` rows, and ADR-0016 D4 E5 / D9 rewritten as D8 lists. Each new rule is marked "implemented by S2-S5" until those land. `grammar.ebnf` and the gbnf files move with S2, not here. | `make check-docs`, `check_metrics_claims.py` and `check_canonical.py` on every touched page | no |
| S2 | Parser: `sc` task expression and `sc():T` type | `sc` followed by `{` is the block and anything else is a task expression. `sc():T` parses in a `let` annotation and in typed-empty literals. `spawn` is an ordinary identifier, except for the E2038 recogniser (D7.2). `grammar.ebnf`, `toke.gbnf`, `toke-xgr.gbnf`, `toke.lark` and the gbnf fixtures are updated. The AST and `.tkir` gain the handle type node (version bump in `tkir_ast.c`). `fmt`, `pyview`, `ast_json` and `lint` print the new forms. | G-series: block vs task by lookahead, the type in both positions, `let spawn=3; sc{let x=spawn+1;}` parses, gbnf fixtures for each form | yes |
| S3 | Checker: handle type and the ADR-0017 rules | Handle type kind with element `T`. `sc E` typed per D4.2, and `h()` typed `T`. E4082, E4083 (every route in D4.3), E4084, E4085 and E5003 emitted, each with no `fix`. E4031 / E4026 on handle misuse. `check-error-codes` agrees with the docs. | D-series per code, with `expected_fix_absent` on each; clean cases for a read inside a nested block, an array or map of handles, and a handle in a `let` annotation | yes |
| S4 | Lowering and runtime: `tk_sc_*`, joins on every exit edge, typed reads | Runtime per D6: growable scopes, condition-variable waits, helping join, descriptors freed at close, RT011, `pthread_atfork`. Lowering: open, start (thunk plus adapter), read (narrow plus error restore, D5.3), and a close on every exit edge (D2.1). Implicit link dependency for `sc` (no import). O1, O2 and O3 discharged; O4 recorded for ADR-0016. | C-series: IR shows the close before every `ret` / `br` leaving a block (with `<`, `br`, `!` and `mt`-arm `<`); results of every kind (`bool`, narrow ints, `f64`, `str`, array, record, map, error union with payload); a nested `sc` under a pool forced to one worker (no hang); more than 256 tasks in one scope and more than 4096 in a process; ThreadSanitizer runs of O1 and O2; a forked child that uses `sc`; linux/amd64 docker run as in 127.548; `make diff-codegen` | yes |
| S5 | Withdraw `std.task` and migrate its users | `stdlib/task.tki`, the `task` fallback, the `tk_task_*` table rows and `_w` wrappers, `check_task_spawn_call` and the `llvm.c` receiver path are removed. E2038 is emitted per D7.2 (`fix` only for `spawn &name` / `spawn <():T value>`). E4081 is documented as retired. Every user in the Context table is migrated. ooke `make test` and the website build pass against the branch. | D-series for E2038 with the exact `fix` value for `spawn &w` → `sc w()` and its absence for the import and calls; the migrated C2305-C2309, C2063, C020, C105, C736 and `test_127_549.tk` pass; `check-facts` updated (stdlib modules, codes) | yes |
| S6 | Docs: the `sc` page and the withdrawn module | A `docs/reference/` page for `sc` (examples that build under `check_doc_examples.py`), `docs/stdlib/task.md` removed or replaced by a withdrawal note, `stdlib/index.md`, `stdlib-signatures.md`, `stdlib-internal.md`, `runtime-abi.md`, `errors.md` (both), `CHANGELOG.md`. The website, toke-mcp skill rows and the toke-test-programs signature list are synced. | `make check-docs`, `check-claims-all` | no |
| S7 | Corpus and patterns: concurrency in the v0.4 regen | A pattern-catalogue entry for fan-out with `sc` (call form, array of handles, read in the block). The regen generator prompt names `sc` and never `std.task`. The 116.9 tokenizer input excludes the frozen pre-131 `std.task` lines. No record enters without the full validation pipeline. | the regen harness tests; the corpus/benchmark task-ID dedup check before any batch | no |

## Consequences

- Concurrency has one spelling, needs no import, and cannot leak a task past its block. The join is guaranteed on every path, which the shipped lowering does not do.
- Results are typed. 127.554 closes without accessor functions, and the `-1` sentinel of `task.result` disappears with the module.
- The keyword set stays closed. `spawn` stops being a word with two meanings. The price is that `sc` has three roles, separated by position and checked by E4082 and E4085.
- Handles cannot be passed or captured. Dependent tasks read first and pass values. A program that wants a pipeline between tasks has to express it as stages of blocks.
- No cancellation: an early exit waits for the work it started.
- The runtime loses its fixed limits (256 per scope, 4096 per process) and its busy-wait. It gains a helping join that also makes nested scopes and forked children safe.
- ADR-0016 has to be amended in two places before either ADR is implemented.

## Alternatives considered

- **Keep both surfaces** (`std.task` as the library and `sc` as sugar). The owner rejected this on 2026-11-28 as a synonym.
- **Keep `std.task` only and drop the keyword** (127.553's other option). The join is then a call the model can forget, and the probe shows the early-return case already forgets it in generated code.
- **`spawn` as the task word,** as a reserved word or as today's contextual identifier (D1, Q1).
- **The function-value operand,** `sc &f` with E4081 (D1, Q2).
- **The implicit future,** `let x=sc f();` with `x : T` (D1). It hides the blocking point.
- **`await` / a property / an operator for the read** (D1).
- **Handles that outlive their scope** (read after the join, as Java allows after `join()`). This needs descriptors kept for the process, which is today's 4096-handle cap problem, and makes "used after its scope" a runtime state.
- **Cancellation on first error** (Trio, Kotlin, errgroup) or on early exit (D3.5).
- **Per-type accessors** (`task.resultstr` and so on), rejected by the owner decision on 127.554.

## Owner resolutions (2026-11-28)

Each question is resolved by the design principle that decides it (docs/about/design.md: "where any implementation decision conflicts with a design principle, the principle takes precedence over convenience").

1. **The task word: `sc`.** `sc f(x)` starts a task and `sc():T` is the handle type. `spawn` is not kept: a context-sensitive word breaks "no production needs semantic context" (Backtrack-free Grammar), and a 15th reserved word would cost tokens and change a canonical fact. The keyword count stays 14.
2. **The operand: the call form `sc f(x)`.** One form, no closure needed to pass arguments, and the arguments are evaluated visibly on the starting thread. E4081 retires and E4085 is added, as proposed.
3. **Cancellation: none.** Cancel-on-first-error would make results depend on timing; every operation has a defined result (Explicit Everything).
4. **Confinement: confirmed.** Handles can be neither passed nor captured, so the compiler can verify every join lexically.
5. **Unread errors: a read on every path (changes D3.2).** design.md (Structured Error Handling): "The compiler enforces that every error path is handled explicitly." E4084 therefore requires the handle of a fallible task to be read on every path through its block, not merely somewhere in it; the checker gains the path analysis this needs (stage S3). The flow-insensitive union-consumption rule of 127.323 that D3.2 cites falls short of the same principle; aligning it is a separate story, not a reason to weaken E4084.
6. **Rollout: an error at once.** `std.task` is withdrawn; E2038 makes its use an error immediately and drives the migration (toke is young: break and fix forward).
7. **Interleaved effects: accepted, and stated.** The side effects of concurrent tasks interleave in an unspecified order; each single output write is atomic (no torn line), and the spec amendment (S1) states both. No lint on output from tasks.
