# toke (tk) — Machine-Native Language for LLM Code Generation
## Project Proposal

**Language name:** toke  
**Written shorthand:** tk  
**Compiler binary:** tkc  
**File extension:** .tk  
**Package registry name:** tokelang  
**Status:** Pre-specification  
**Hardware platform:** Mac Studio M4 Max (primary), cloud burst (overflow)

---

## 1. Vision and Core Thesis

toke is a compiled, machine-code-targeting programming language designed from first principles for LLM generation rather than human authorship. Every decision — character set, grammar, error protocol, file structure, type system, compilation pipeline — is optimised for the generation loop: generate, compile, receive structured error, correct, repeat.

The core thesis, which this project will validate empirically before committing to full investment:

> A sufficiently constrained, unambiguous, token-efficient language will reduce end-to-end LLM code generation cost (tokens × iterations × error rate) by a material margin — sufficient to justify building and training a purpose-native model.

The research framework defines seven validation workstreams and a four-phase go/no-go plan. That framework is integrated with the practical build plan, hardware strategy, and cost model throughout this document.

---

## 2. Name Rationale

**toke** is the spoken identity. **tk** is the written shorthand. They are used as Go/go — the language is toke, the files are `.tk`, the compiler is `tkc`.

The name is self-referential: a language designed to minimise token usage has a name that is itself a single token in cl100k_base. "Toke" is also the shortened form of "token", embodying the compression principle in the name itself.

**Conflict analysis:**
- Tcl/Tk (1991): always written `Tk` with capital T, GUI toolkit domain, no conflict
- `toke-runner` on crates.io: small hobby project, no trademark, registered as `tokelang` sidesteps it
- `tokei` (Rust code counter): different spelling, different domain, phonetically close but distinct
- `.tk` TLD (Tokelau): domain registered as `tokelang.dev`
- No registered trademark on "toke" as software

**Verdict:** Clear to proceed. Register `tokelang` across npm, crates.io, PyPI, and Homebrew immediately.

---

## 3. Character Sets

### 3.1 Phase 1 — 80 Characters

80 characters. Round in binary (7 bits with one spare). Compatible with cl100k_base and all current major LLM tokenizers. Used for all corpus generation, prototype compiler, and initial model training.

```
CLASS         CHARACTERS                                        COUNT
──────────────────────────────────────────────────────────────────────
Lowercase     a b c d e f g h i j k l m n o p q r s t u v w x y z   26
Uppercase     A B C D E F G H I J K L M N O P Q R S T U V W X Y Z   26
Digits        0 1 2 3 4 5 6 7 8 9                                     10
Symbols       ( ) { } [ ] = : . ; + - * / < > ! |                     18
──────────────────────────────────────────────────────────────────────
TOTAL                                                                  80
```

**Excluded from structural source:**
- No whitespace (space, tab, newline) — `;` is the universal separator
- No comments — the language is machine-generated; comments are metadata stored outside source files
- No `@`, `#`, `$`, `%`, `^`, `&`, `~`, `` ` ``, `\`, `'`, `,`, `?`

String literals use `"` as delimiter. `"` occupies one symbol slot in the final 80-character table. The exact symbol allocation is locked at M0.

### 3.2 Phase 2 — 56 Characters

Activated when the purpose-built tokenizer is trained on the Phase 1 corpus. Requires retraining.

**Differences from Phase 1:**
- Drop all 26 uppercase letters. Replace with `$` sigil prefix for type names: `User` → `$user`
- Replace `[]` (2 chars) with `@` sigil for array literals: `[1;2;3]` → `@(1;2;3)`, index via `.n`
- Net: −26 −2 +1(`$`) +1(`@`) = −26, rounded to 56 by reserving `^` and `~` for future use

```
CLASS         CHARACTERS                                        COUNT
──────────────────────────────────────────────────────────────────────
Lowercase     a-z                                                      26
Digits        0-9                                                      10
Symbols       ( ) { } = : . ; + - * / < > ! | $ @ " ^ ~              20
──────────────────────────────────────────────────────────────────────
TOTAL                                                                  56
```

**Why the sigil approach wins with a purpose-built tokenizer:** `$` only ever precedes a type name. `@` only ever precedes `(`. BPE merge rules absorb these into single tokens after sufficient training data. `$user`, `$str`, `$err`, `@(` each become one token. Uppercase letters by contrast appear in arbitrary positions and resist clean merging.

### 3.3 Token Count Estimates

Illustrative token counts for a typical HTTP handler across character set and tokenizer combinations. Evaluation baselines are included for benchmark context; actual measured values are determined during Phase 1 validation.

```
tk Phase 1, cl100k_base:       ~38 tokens
tk Phase 2, cl100k_base:       ~43 tokens  (+5, sigil overhead)
tk Phase 1, purpose tokenizer: ~26 tokens  (common patterns merge)
tk Phase 2, purpose tokenizer: ~22 tokens  (sigils merge, further reduction)
Python baseline:               ~85 tokens
TypeScript baseline:           ~92 tokens
```

The Phase 2 + purpose tokenizer combination is expected to achieve approximately 4× token density versus the Python baseline for equivalent logic. These estimates will be validated empirically at Gate 1.

---

## 4. Symbol Assignment

Every symbol has exactly one meaning. No overloading that requires context to resolve.

| Symbol | Primary meaning          | Secondary meaning         |
|--------|--------------------------|---------------------------|
| `=`    | Bind / define            | Equality test in match    |
| `:`    | Type annotation          | Key separator in struct   |
| `.`    | Member access            | Module path separator     |
| `;`    | Statement terminator     | Argument / field separator|
| `(`    | Call open / group open   | —                         |
| `)`    | Call close / group close | —                         |
| `{`    | Block open               | Struct literal open       |
| `}`    | Block close              | Struct literal close      |
| `[`    | Array open (Phase 1)     | Index open (Phase 1)      |
| `]`    | Array close (Phase 1)    | Index close (Phase 1)     |
| `+`    | Add                      | String concat             |
| `-`    | Subtract                 | Negate                    |
| `*`    | Multiply                 | Pointer deref (FFI only)  |
| `/`    | Divide                   | —                         |
| `<`    | Return                   | Less-than in expressions  |
| `>`    | Greater-than             | —                         |
| `!`    | Error propagate          | Logical not               |
| `\|`   | Match arm separator      | Union type separator      |
| `"`    | String literal delimiter | —                         |

### 4.1 Keywords (12, all lowercase, 1–3 chars)

```
F    function definition
T    type definition
I    import
M    module declaration
if   conditional branch
el   else branch
lp   loop (single loop construct)
br   break
let  immutable binding
mut  mutable binding
as   type cast
rt   return (long-form alternative to <)
```

---

## 5. Language Design

### 5.1 Module and Imports

```
M=api.user;
I=http:std.http;
I=db:std.db;
I=json:std.json;
```

One module per file. All imports explicit with local alias. Circular imports rejected at import resolution before type checking.

### 5.2 Type Definitions

```
T=User{id:u64;name:Str;email:Str};

T=UserErr{
  NotFound:u64;
  BadInput:Str;
  DbErr:Str
};
```

Struct types and sum types use identical syntax. The type checker distinguishes them by whether field names begin uppercase (sum variant) or lowercase (struct field).

### 5.3 Function Definition

```
F=get_user(id:u64):User!UserErr{
  r=db.one("SELECT id,name,email FROM users WHERE id=?";[id])!UserErr.DbErr;
  <User{id:r.u64(id);name:r.str(name);email:r.str(email)}
};
```

`F=name(params):Return!Error{body}` — the `!ErrorType` suffix is mandatory for fallible functions. `<` returns. `!` after a call propagates error with explicit variant mapping.

### 5.4 Match Expression

```
get_user(id)|{
  Ok:u   <Res.ok(u);
  Err:e  <Res.err(e)
};
```

`|` begins a match block. Exhaustiveness enforced at compile time. New variants added to a sum type cause all non-exhaustive matches to fail compilation immediately.

### 5.5 A Complete Example

HTTP user API module, Phase 1:

```
M=api.user;
I=http:std.http;
I=db:std.db;
I=json:std.json;

T=User{id:u64;name:Str;email:Str};
T=UserErr{NotFound:u64;DbErr:Str};

F=fetch(id:u64):User!UserErr{
  r=db.one("SELECT id,name,email FROM users WHERE id=?",[id])!UserErr.DbErr;
  <User{id:r.u64(id);name:r.str(name);email:r.str(email)}
};

http.GET("/users/:id";F=handle(req:http.Req):http.Res{
  id=req.param(id).u64|{<http.Res.bad("id must be number")};
  fetch(id)|{
    Ok:u  <http.Res.ok(json.enc(u));
    Err:e <http.Res.err(json.enc(e))
  }
});
```

Character count: ~290. The route handler, type definitions, database query, and error handling are all verified at compile time. No runtime surprises from unhandled error paths.

---

## 6. Memory Model

No GC. No reference counting. No borrow checker. **Lexical arena allocation.**

Every function body is an implicit arena. All heap allocations within a function are freed on return. Explicit `{arena...}` blocks handle sub-function lifetimes.

```
F=handle(req:http.Req):http.Res{
  {arena
    parsed=json.dec(req.body)!Res.bad("invalid json");
    result=process(parsed);
    <http.Res.ok(json.enc(result))
  }
};
```

Long-lived state declared at module level with static lifetime. This model covers the vast majority of request-handler, CLI, and worker patterns without requiring model reasoning about memory lifetimes. Complex allocation patterns available via C FFI.

---

## 7. Structured Error Protocol

Every compiler error and runtime panic emits a consistent struct. Designed to be consumed directly by the generation loop without parsing English prose.

```
stage:Str;
error:Str;
file:Str;
pos:u32;
context:[Str];
expected:Str;
got:Str;
fix:Str
```

Example — type mismatch:

```
stage:type_check;error:field_type_mismatch;file:api.user.tk;
pos:247;context:["r=db.one(sql;[id])","<User{id:r.str(id)}"];
expected:u64;got:Str;fix:r.u64(id)
```

The `fix` field is populated when the error is mechanically correctable. For these cases the generation loop applies the fix directly without invoking the model — a corrected program in one round trip at near-zero inference cost.

---

## 8. Compilation Pipeline

```
Source (.tk)
    │
    ▼
Lexer (C, ~300 lines)        — token stream, no whitespace, no comments
    │                           each token unambiguously typed from first character
    ▼
Parser (C, ~400 lines)       — AST, structured error on syntax failure, LL(1)
    │
    ▼
Import resolver              — validates all imports, fails fast with available list
    │
    ▼
Type checker                 — exhaustiveness, coercions, arena validity
    │                           structured error with fix suggestion
    ▼
IR lowering                  — AST to tk IR (SSA, explicit types, no sugar)
    │
    ▼
LLVM IR backend              — tk IR to LLVM IR
    │
    ▼
LLVM                         — native x86-64 or ARM64 binary
    │
    ▼
Binary                       — no runtime dependency, ~40KB overhead
```

Compiler frontend target: under 5,000 lines of C, zero external dependencies. Compilation of a 50-line single-function `.tk` file: under 50ms cold, under 5ms incremental. Fast enough to run synchronously in the generation loop.

---

## 9. Hardware Strategy — Mac Studio M4 Max

### 9.1 The Capital Investment Case

A **Mac Studio M4 Max with 128GB unified memory** ($7,199) is the hardware anchor for this project. It is not a compute node — it is the development machine, local inference server, corpus pre/post-processor, and training platform simultaneously.

Justification:
- Runs Qwen 2.5 Coder 32B in 4-bit quantisation locally at ~25 tok/s inference — sufficient for pre/post-processing in the corpus pipeline without any API cost
- QLoRA fine-tune of 7B model via MLX: ~18–24 hours per run
- QLoRA fine-tune of 34B model via MLX: ~48–72 hours per run
- Holds the full corpus (500K files × ~500 chars average ≈ ~2GB) in memory during processing
- Runs the tkc compiler continuously in the validation loop — no cloud round trips for compilation
- Serves as the local toke inference endpoint for testing generated programs
- Zero marginal cost per training iteration after purchase — cloud A100s at $1.50/hr are rented for burst overflow only

Break-even vs renting cloud A100s: approximately 4,800 GPU-hours of equivalent work, or roughly 18 months of intensive use. For a multi-year language project, buying dominates.

**The M4 Max 128GB (not 96GB):** The extra 32GB headroom means the 34B model fits comfortably with working memory to spare. The 96GB config is tight for 34B QLoRA with a full batch and optimizer state. The $800 delta is worth it.

### 9.2 Qwen as Local Pre/Post Processor

The corpus pipeline uses a two-tier generation strategy that minimises expensive Claude API calls:

```
Task description
      │
      ▼
Qwen 2.5 Coder 32B (local, Mac Studio)
  — pre-filter: is this task well-specified?
  — pre-generate: attempt tk code locally
  — validate: does it compile? does it pass basic tests?
      │
      ├── Yes → save directly, zero API cost
      │
      └── No / ambiguous → escalate to Claude Haiku 4.5 (batch API)
                │
                ▼
           Claude Haiku 4.5 generates / fixes
                │
                ▼
           Compile + test (local tkc)
                │
                ├── Pass → save to corpus
                └── Fail → structured error → Haiku retry (max 3)
```

Expected API escalation rate by phase:
- Phase A (single functions): ~25% escalated to API — Qwen handles 75% locally
- Phase B (data structures): ~40% escalated
- Phase C (system interaction): ~60% escalated — network/socket patterns need stronger model
- Phase D (applications): ~80% escalated — Sonnet 4.6 for multi-module

This reduces API costs by roughly 50–65% versus API-only generation.

### 9.3 Post-Generation Processing (Qwen Local)

After each corpus entry is validated by the compiler, Qwen runs post-generation quality checks locally at zero API cost:

- Structural review: are module boundaries sensible?
- Redundancy check: embedding similarity against existing corpus entries
- Style consistency: does this match established tk idioms?
- Test coverage assessment: are edge cases exercised?

Entries failing post-generation checks are flagged for regeneration or discarded. This replaces the majority of human review in Phases A–C.

---

## 10. Parallel Differential Testing

Every corpus generation task produces implementations in four languages simultaneously. This is **differential testing** — the parallel implementations serve as mutual oracles.

```python
async def generate_and_validate(task):
    # Five parallel calls — four languages + test inputs
    results = await asyncio.gather(
        generate(task, "tk"),
        generate(task, "python"),
        generate(task, "c"),
        generate(task, "java"),
        generate_test_inputs(task)
    )

    binaries = compile_all(results)
    outputs  = run_all(binaries, results.test_inputs)
    verdict  = majority_vote(outputs)

    match verdict:
        Ok:   corpus.add(task, results.tk, build_metadata(results))
        TkBug:  correction_loop(task, results.tk, verdict.error)
        OtherBug: discard_bad_language(results, verdict.outlier)
        Ambiguous: discard_task("task description unclear")
```

### 10.1 What Differential Testing Provides

**Correctness oracle without human test authorship.** Established languages generate correctly on the first attempt at high rates — LLMs have deep training on them. tk that disagrees with all three reference implementations is wrong, almost certainly in tk.

**Performance benchmark built in for free.** Every corpus entry already has four native implementations. Binary size, startup time, execution time, and memory usage across all four — 50,000 data points on tk runtime characteristics emerge automatically.

**Token efficiency data at scale.** The token density claim gets empirical validation across the full distribution of programs, not selected examples.

**Task ambiguity detection.** When all four languages disagree, the task description is underspecified. These are discarded, keeping the training corpus clean of programs that teach the model to hallucinate solutions to ambiguous prompts.

**LLM correctness progression curve.** Track first-attempt compilation success rates across all four languages per training iteration. Reference languages start high and stay high. tk starts lower and rises with each fine-tuning cycle. This curve is the primary evidence that training is working.

### 10.2 Metadata Stored Per Corpus Entry

```
task_id:           unique identifier
tk_tokens:         token count for tk generation
baseline_tokens:   token count for Python reference implementation
c_tokens:          token count for C reference implementation
java_tokens:       token count for Java reference implementation
tk_ratio:          tk_tokens / baseline_tokens  (density metric)
attempts:          number of correction rounds needed
error_trace:       [broken_code, error_struct, fixed_code] if applicable
perf_ratio:        tk_binary_time / c_binary_time
binary_size:       tk binary bytes
phase:             A|B|C|D|E
```

---

## 11. Corpus Generation Pipeline

### 11.1 Generation Architecture

```
Mac Studio (local)                    Claude API (burst)
──────────────────                    ─────────────────
Curriculum task generator             Haiku 4.5 batch (Phases A-C)
Qwen 2.5 Coder 32B inference          Sonnet 4.6 (Phase D)
tkc compiler + test runner            Sonnet 4.6 + correction loop
Post-generation Qwen judge
Corpus storage (local NVMe)
```

### 11.2 Curriculum

**Phase A — Primitives (target: 50,000 programs)**
Single functions. Tasks generated programmatically from templates.
- Arithmetic across all type combinations
- String operations: concat, split, slice, format
- Array operations: map, filter, fold, sort
- Conditional logic: all boolean expression patterns
- Recursive algorithms: factorial, fibonacci, tree traversal
- Error propagation: chains of fallible operations

**Phase B — Data Structures (target: 30,000 programs)**
2–4 files. Type definitions separate from function implementations.
- Linked list, stack, queue in tk's arena model
- Hash map and set implementations
- Binary search tree and heap
- Graph representations and traversal
- Serialisation / deserialisation patterns

**Phase C — System Interaction (target: 20,000 programs)**
Interact with stdlib network, file, and process modules.
- HTTP client and server handlers
- TCP socket clients and servers
- File read/write/watch patterns
- Process spawn and communicate
- Database query patterns (SQL and key-value)

**Phase D — Applications (target: 5,000 programs, partial agent review)**
Multi-module programs that accomplish a complete task.
- REST API with database backend (5–8 files)
- CLI tool with subcommands and config
- Background worker with queue processing
- Static file server with routing and caching
- Proxy and middleware patterns

**Phase E — Complex Systems (target: 500 programs, human-reviewed)**
Architectural templates requiring coordinated multi-module design.
- Full web application (frontend serving, API, database, sessions)
- Distributed key-value store
- Simple shell with pipes and redirection
- Build system
- Interpreter for a minimal language

Phase E programs are generated by the model, reviewed for architectural soundness by humans, corrected if needed, then added to the training set. These represent the aspirational capability ceiling.

### 11.3 Grammar-Based Generation (Supplement)

In addition to LLM generation, the corpus is supplemented with:
- **Random valid AST generation**: programmatically sample valid tk programs subject to grammar and type rules, guaranteeing full syntax coverage
- **Transpilation**: convert a subset of Python/C functions to tk equivalents, providing realistic code shapes with known-correct semantics
- **Adversarial injection**: deliberately include tricky constructs (deep nesting, long identifier chains, generics examples) and subtle bugs for repair task training
- **Corpus deduplication**: embedding similarity check before adding any entry — no near-duplicates

---

## 12. Agent-Based Review Pipeline

Three review agents run in sequence, replacing the majority of human review across corpus phases A through D:

**Agent 1 — Compiler Oracle (automated, zero cost)**
The tkc compiler itself. Syntax, type safety, import validity, exhaustive match, error propagation coverage. Pass/fail, structured error output. No human, no API call.

**Agent 2 — Qwen Judge (local, zero marginal cost)**
Runs on Mac Studio after compilation passes. Evaluates:
- Does the program match the task description semantically?
- Are module boundaries idiomatic?
- Is error propagation complete and appropriate?
- Does it duplicate an existing corpus entry?
- Would the output on test inputs actually be correct?

Outputs pass/flag/reject with structured reasoning.

**Agent 3 — Claude Sonnet Architectural Review (API, selective)**
Invoked only for Phase D and E programs that passed Agents 1 and 2. Evaluates:
- Is the multi-module design coherent?
- Are the abstraction boundaries in the right places?
- Would a senior developer recognise this as idiomatic for the domain?
- Does the error handling strategy hold across the full program?

Applied to ~20% of Phase D entries, ~60% of Phase E entries.

**Human review** reserved for Phase E programs only, focused on architectural soundness of novel complex systems. Target: ~80 hours total across the project.

---

## 13. Model Training

### 13.1 Base Model Selection

**Qwen 2.5 Coder 7B Instruct** — primary fine-tune target.

Rationale:
- Trained on 5.5T tokens with heavy code weighting
- Achieves 88.4% HumanEval, outperforming many larger models on code tasks
- MLX-compatible — fine-tunes directly on Mac Studio via Apple's framework
- Open weights, commercial use permitted
- Small enough to iterate rapidly on Mac Studio hardware

Secondary target: **Qwen 2.5 Coder 32B** for the development-phase model.

### 13.2 Training Method

**Phase 1 fine-tuning:** QLoRA via MLX on Mac Studio M4 Max.

```
base_model:     Qwen2.5-Coder-7B-Instruct
quantization:   4-bit NF4
lora_rank:      64
lora_alpha:     128
lora_dropout:   0.05
target_modules: all linear layers
learning_rate:  2e-4 cosine decay
batch_size:     8 (gradient accumulation × 4 = effective 32)
epochs:         3
warmup_steps:   100
hardware:       Mac Studio M4 Max 128GB, MLX framework
```

**Phase 2 fine-tuning (larger model):** Qwen 2.5 Coder 32B QLoRA on Mac Studio, or burst to Lambda Labs A100 for time-sensitive runs.

**Training data format:**

Direct generation examples:
```
[INST] Generate a tk function that fetches a user by ID from PostgreSQL.
Available: db.$conn. Types: $user{id:u64;name:$str}, $db_err{query_err:$str}
[/INST]
F=get_user(id:u64):$user!$db_err{
  r=db.one("SELECT id,name FROM users WHERE id=?";@(id))!$db_err.query_err;
  <$user{id:r.u64(id);name:r.str(name)}
}
```

Correction examples (most valuable signal):
```
[INST] Fix this tk program. Error: stage:type_check;error:field_type_mismatch;
pos:89;expected:u64;got:Str;fix:r.u64(id)
[BROKEN]
F=get_user(id:u64):$user!$db_err{
  r=db.one(sql;@(id))!$db_err.query_err;
  <$user{id:r.str(id);name:r.str(name)}
}
[/INST]
F=get_user(id:u64):$user!$db_err{
  r=db.one(sql;@(id))!$db_err.query_err;
  <$user{id:r.u64(id);name:r.str(name)}
}
```

Multi-language comparison examples (Phase 2 tokenizer training):
```
[TASK] Sum all integers in an array.
[PYTHON 85 tokens] def sum_arr(a): return sum(a)
[C 62 tokens] int sum_arr(int*a,int n){int s=0;for(int i=0;i<n;i++)s+=a[i];return s;}
[TK 28 tokens] F=sum_arr(a:[i64]):i64{let s=mut.0;lp(let i=0;i<a.len;i=i+1){s=s+a[i]};lt s}
[RATIO] 0.33× Python tokens. All four implementations agree on 1,000 test inputs.
```

### 13.3 Purpose-Built Tokenizer (Phase 2)

Trained exclusively on the Phase 1 corpus. Vocabulary: 32,768 tokens (2^15).

Training process:
1. Collect all Phase 1 validated `.tk` files (~500,000 files)
2. Strip string literal contents (tokenize separately as natural language)
3. Run BPE training targeting 32,768 vocabulary
4. Verify top 100 most common tk constructs are single tokens
5. Manually inspect and correct pathological merges
6. Freeze vocabulary — no changes after Phase 2 begins

Expected improvement: 2.5–4× fewer tokens per tk program vs cl100k_base, based on purpose-built tokenizer advantages for highly repetitive structured languages.

---

## 14. Seven Validation Workstreams

Adopted from the research framework. These run in parallel with development, not after it.

### WS1 — Representation Validity
**Core question:** Does tk genuinely reduce tokens and improve generation quality vs Python/TS/JSON-AST/WASM-IR?

Metrics: token count per task, Pass@1, repair iterations, end-to-end cost.  
Go/no-go criterion: if tk's end-to-end cost is not >10% better than constrained TypeScript within Phase 1, the language thesis is weak and the project pivots to IR-level approach only.

### WS2 — Semantic Design Adequacy
**Core question:** Can tk express real-world needs (concurrency, generics, FFI, modules) without undermining generation efficiency?

Tests: generic container types, async task model, C FFI, module versioning. Success if LLMs can generate correct code for these constructs and they remain token-compact.

### WS3 — Compiler Formalism
**Core question:** Is the grammar fully specified, unambiguous, and formally complete?

Deliverable: EBNF grammar with no ambiguities (validated by parser generator), formal type rules, memory model specification, 100% conformance test coverage of all language features.

### WS4 — Corpus Generation Science
**Core question:** Is the training corpus high-quality, non-contaminated, and sufficient for generalisation?

Methods: grammar-based generation, transpilation, adversarial injection, deduplication, hidden test separation. Success: >80% of benchmark tasks solvable within 3 iterations by the trained model.

### WS5 — Benchmark and Evaluation Design
**Core question:** Are the benchmarks rigorous, with hidden tests that catch semantic errors?

Suite: algorithmic (D2C), system-level multi-file (C2C), maintenance/bug-fix (C2C), multi-language interop (C2C). Multi-trial metrics (pass@k), iterative attempts, adversarial cases.

### WS6 — Ecosystem and Tooling
**Core question:** Can tk be used in practice without custom infrastructure?

Deliverables: language server protocol stub, package manifest format, IDE syntax highlighting, error-driven repair API, CI integration.

### WS7 — Standardisation Pathway
**Core question:** What does a standardisable, interoperable tk look like?

Deliverables: AST/IR exchange format, formal error schema, model-compiler protocol spec, package registry prototype, draft governance document.

---

## 15. Four-Phase Project Plan

Adopted from the research framework, updated with toke name and current timeline.

### Phase 1 — Falsification (Months 1–6)
**Objective:** Test the core thesis with minimal implementation. If tk shows no advantage, stop here.

**Deliverables:**
- Minimal compiler: functions, records, imports, errors
- 50,000 Phase A corpus programs (differential-tested)
- Multi-language benchmark suite (tk vs Python vs C vs Java)
- Token efficiency report
- Pass@1 comparison across languages

**Go/No-Go gate:** tk shows >10% token reduction AND equal or better Pass@1 vs Python on held-out tasks. If not, pivot to typed-IR approach without full language build.

**Key hardware/cost:**
- Mac Studio purchase at project start ($7,199 — capital)
- Corpus API costs ~$85 (Haiku batch + prompt caching)
- No model training in Phase 1 — use general models with spec in context

### Phase 2 — Language Viability (Months 6–14)
**Objective:** Extend tk with essential features (generics, concurrency, FFI, versioned modules) and validate expressiveness.

**Deliverables:**
- Extended compiler: generics, async model, C FFI, module versioning
- Phase B–C corpus (50,000 additional programs)
- Phase 2 purpose-built tokenizer
- First fine-tuned model: Qwen 2.5 Coder 7B on Phase A corpus
- Benchmark results for system-level tasks (HTTP servers, database clients)

**Go/No-Go gate:** tk with full features still offers token advantages AND the trained 7B model outperforms general models on tk-specific tasks by a measurable margin.

### Phase 3 — Ecosystem Proof (Months 14–26)
**Objective:** Build the end-to-end ecosystem. Multiple models, full benchmark suite, working toolchain.

**Deliverables:**
- Production-quality compiler with full stdlib
- Full corpus A–E (~105,500 programs)
- Fine-tuned models: 7B and 32B on full corpus
- Language server stub, package registry prototype
- Self-improvement loop running autonomously
- Multi-model evaluation (Qwen + Llama families)

**Go/No-Go gate:** Multiple model families generate correct tk at >70% Pass@1 on held-out Phase C tasks. Self-improvement loop demonstrably improves corpus quality over time.

### Phase 4 — Standard Pathway (Months 26–32)
**Objective:** Formalise into a standard. Draft specification. Propose to open consortium.

**Deliverables:**
- Formal language specification (grammar + semantics, EBNF)
- Published conformance suite
- Standard error schema and AST exchange format
- Package registry — initial public release
- Proposal document for open consortium or standards body
- Self-redesign pilot: model proposes language construct revisions based on error pattern analysis

---

## 16. Cost Model

### 16.1 Capital Investment

| Item | Cost | Amortisation |
|------|------|-------------|
| Mac Studio M4 Max 128GB / 8TB | $7,199 | Project infrastructure, daily dev machine, training platform, local inference |

The Mac Studio is not an operating cost — it is the project's compute platform. All training runs, local inference, corpus validation, and compiler development run on it.

### 16.2 Phase 1 — Falsification (~$850 total)

| Item | Detail | Cost |
|------|--------|------|
| Compiler build (Claude Code) | ~20hrs assisted dev | $40 |
| Test harness + tooling | ~10hrs assisted dev | $20 |
| Phase A corpus generation | 50K programs, Haiku 4.5 batch + caching, 75% Qwen local, 25% API | $60 |
| Parallel generation (Python/C/Java) | Same 50K tasks, Haiku batch | $25 |
| Post-processing (Qwen local) | 0 API cost | $0 |
| Benchmark suite build | ~15hrs Claude Code | $30 |
| API infrastructure setup | | $25 |
| Mac Studio pro-rata (6 months) | $7,199 / 36 months × 6 | $1,200 |
| **Phase 1 total (excl. hardware)** | | **~$200** |
| **Phase 1 total (incl. hardware pro-rata)** | | **~$1,400** |

No model training in Phase 1. General models with tk spec in context validate the thesis cheaply.

### 16.3 Phase 2 — Language Viability (~$1,800 total)

| Item | Detail | Cost |
|------|--------|------|
| Extended compiler development | ~80hrs Claude Code | $160 |
| Phase B–C corpus generation | 50K programs, mixed Haiku/Sonnet + Qwen local | $280 |
| Parallel generation (all 4 languages) | Adds ~$120 | $120 |
| Qwen judge agent runs | Local, zero API cost | $0 |
| Agent 3 (Sonnet) review, Phase D | ~20% of 5K Phase D entries | $80 |
| Phase 2 tokenizer training | CPU job ~12hrs local | $0 |
| 7B QLoRA fine-tune (3 runs) | Mac Studio local, MLX, ~20hrs each | $0 |
| 32B QLoRA fine-tune (2 runs) | Mac Studio local, MLX, ~60hrs each | $0 |
| Cloud A100 burst (if needed) | 40hrs Lambda Labs overflow | $60 |
| Evaluation runs and iteration | | $80 |
| Infrastructure (CI, storage, tools) | | $120 |
| Mac Studio pro-rata (8 months) | | $1,600 |
| **Phase 2 total (excl. hardware)** | | **~$900** |
| **Phase 2 total (incl. hardware pro-rata)** | | **~$2,500** |

Human review: ~40 hours Phase D architectural review at $25/hr = $1,000. Included separately — this is the only irreducible human cost in Phase 2.

### 16.4 Phase 3 — Ecosystem Proof (~$6,000 total)

| Item | Detail | Cost |
|------|--------|------|
| Full compiler + stdlib | ~200hrs Claude Code | $400 |
| Phase D–E corpus | 5,500 programs | $320 |
| Phase E human review | ~80hrs expert architectural review | $2,000 |
| DPO preference data (compiler-derived) | Automated from compiler metrics | $0 |
| DPO preference data (human) | ~100hrs labelling | $2,500 |
| 7B fine-tune iterations (5 runs) | Mac Studio | $0 |
| 32B fine-tune iterations (3 runs) | Mac Studio or cloud burst | $120 |
| Cloud burst for speed | 100hrs A100 total | $150 |
| Evaluation and benchmarking | | $200 |
| Language server + tooling | ~40hrs Claude Code | $80 |
| Infrastructure (12 months) | | $300 |
| Mac Studio pro-rata (12 months) | | $2,400 |
| **Phase 3 total (excl. hardware)** | | **~$6,070** |
| **Phase 3 total (incl. hardware pro-rata)** | | **~$8,470** |

### 16.5 Phase 4 — Standard Pathway (~$3,000 total)

| Item | Detail | Cost |
|------|--------|------|
| Specification writing | ~100hrs assisted | $200 |
| Conformance suite | ~60hrs Claude Code | $120 |
| Self-redesign pilot | Model proposes language revisions, evaluate empirically | $100 |
| Community and outreach | | $500 |
| Infrastructure (6 months) | | $150 |
| Mac Studio pro-rata (6 months) | | $1,200 |
| **Phase 4 total (excl. hardware)** | | **~$1,070** |

### 16.6 Total Cost Summary

| Phase | Duration | Operating Cost | Hardware Pro-rata | Total |
|-------|----------|---------------|-------------------|-------|
| Phase 1 — Falsification | 6 months | ~$200 | $1,200 | ~$1,400 |
| Phase 2 — Language Viability | 8 months | ~$900 + $1,000 human | $1,600 | ~$3,500 |
| Phase 3 — Ecosystem Proof | 12 months | ~$3,670 + $2,000 human | $2,400 | ~$8,070 |
| Phase 4 — Standard Pathway | 6 months | ~$1,070 | $1,200 | ~$2,270 |
| **Mac Studio purchase** | **upfront** | — | $7,199 | **$7,199** |
| **Total all phases** | **32 months** | | | **~$22,440** |

**The dominant cost is human review and DPO labelling (~$5,500 total), not compute.** The Mac Studio eliminates essentially all cloud training costs for the 7B and 32B models. Cloud GPU costs across the entire project total under $400.

---

## 17. Self-Improvement Loop

Once a working toke model exists, the pipeline becomes a continuous improvement engine.

**Automatic corpus expansion:**
The model generates tk programs. Programs passing compilation and test are added to the corpus. Programs failing are added as correction examples. The corpus grows without human intervention for Phase A–C complexity.

**Error pattern analysis:**
The compiler logs all structured errors with frequencies. The top error patterns become targeted training examples. If 12% of errors are "missing error propagation on db call", 1,000 new examples demonstrating correct propagation are generated and added. This analysis runs weekly, automatically.

**Language redesign proposals (Phase E capability):**
Once the model reliably generates Phase C programs, it can be prompted:

```
You have generated 100,000 tk programs. These constructs appear most
frequently in correction traces: [top 10 error-prone patterns].

These constructs have >50% first-attempt error rate: [list].

Propose modifications to the tk construct set that would:
1. Eliminate the top 3 error patterns
2. Reduce average file token count by 15%
3. Maintain backward compatibility where possible

For each proposal: before/after example, expected error rate impact.
```

Proposals are evaluated empirically: implement in the compiler, translate a corpus subset, measure error rate change. Adopted changes are incorporated into the next language revision. The model fine-tunes on the updated corpus.

This is the path toward a language that partially designs itself — grounded in empirical measurement, not intuition.

---

## 18. Benchmark Targets

These are illustrative estimates based on language and runtime characteristics. All values are hypotheses to be tested during Phase 1 validation. The evaluation baselines exist to ground the benchmark methodology, not to make pre-validated performance claims.

| Metric | toke | Python | TypeScript | C | JSON-AST | WASM-IR |
|--------|------|--------|------------|---|----------|---------|
| Token efficiency (↓ better) | Target: Very High | Baseline | Baseline | Baseline | Low | Medium |
| First-pass correctness | Target: High | High | High | Medium | Medium | Low |
| Repair iterations (↓ better) | Target: Low | Medium | Medium | Medium | Medium | High |
| End-to-end generation cost | Target: Low | Medium | Medium | Medium | High | High |
| Binary performance | Native | Interpreted | JIT | Native | N/A | Near-native |
| Startup time | Target: <1ms | ~100ms | ~200ms | <1ms | N/A | ~5ms |
| Memory (request handler) | Arena, no GC | GC | GC/JIT | Manual | N/A | Linear |
| Model training data available | Built by project | Vast | Vast | Vast | Some | Some |

All entries in the toke column are targets, not measurements. Gate 1 at Month 8 is the first point at which measured values replace estimates.

---

## 19. Repository Structure

```
tokelang/
├── tkc/                    tkc compiler (C, zero dependencies)
│   ├── lexer.c
│   ├── parser.c
│   ├── type_checker.c
│   ├── ir_lower.c
│   ├── llvm_backend.c
│   └── test/               conformance suite
├── stdlib/                 tk standard library
│   ├── std.http.tk
│   ├── std.db.tk
│   ├── std.json.tk
│   ├── std.file.tk
│   └── std.net.tk
├── corpus/
│   ├── generator/          programmatic corpus generation
│   ├── pipeline/           LLM generation + validation harness
│   ├── judge/              Qwen judge agent
│   └── diff_test/          parallel 4-language differential testing
├── tokenizer/
│   ├── train.py            Phase 2 BPE tokenizer training
│   └── eval.py             tokenizer evaluation protocol
├── models/
│   ├── finetune/           QLoRA training scripts (MLX)
│   └── eval/               benchmark evaluation harness
├── benchmark/
│   ├── tasks/              benchmark task definitions
│   ├── hidden_tests/       not in training corpus
│   └── baselines/          Python/C/Java reference implementations
└── spec/
    ├── grammar.ebnf        formal grammar
    ├── semantics.md        type rules and memory model
    └── errors.md           structured error protocol
```

Each subdirectory has a GitHub Actions CI pipeline. The conformance suite runs on every commit to tkc. Corpus validity checks (no syntax errors, token count targets) run nightly.

---

## 20. Milestones

| Milestone | Deliverable | Month |
|-----------|-------------|-------|
| M0 | toke spec locked (80-char set, symbol table, keywords, grammar sketch) | 1 |
| M0.5 | Mac Studio M4 Max purchased and configured | 1 |
| M0.5 | Qwen 2.5 Coder 32B running locally, pipeline tested | 1 |
| M1 | tkc lexer + parser, zero dependencies, LL(1) | 2 |
| M2 | Type checker + structured error output | 3 |
| M3 | LLVM IR backend, hello world compiles to native binary | 4 |
| M4 | stdlib core: http, db, json, file | 6 |
| M5 | Phase A corpus: 50,000 programs, differential-tested | 8 |
| M5.5 | **Phase 1 go/no-go: token efficiency + Pass@1 results** | 8 |
| M6 | Prototype fine-tune: Qwen 7B on Phase A, first accuracy benchmark | 10 |
| M7 | Phase B–C corpus: 50,000 additional programs | 14 |
| M8 | Phase 2 tokenizer trained and validated | 16 |
| M9 | Development models: 7B + 32B on full Phase A–C corpus | 18 |
| M9.5 | **Phase 2 go/no-go: language viability results** | 18 |
| M10 | Phase D corpus + agent review pipeline | 22 |
| M11 | Self-improvement loop running autonomously | 24 |
| M12 | Phase 3 production models + full ecosystem | 26 |
| M12.5 | **Phase 3 go/no-go: multi-model, multi-task benchmark** | 26 |
| M13 | Formal specification document | 28 |
| M14 | Self-redesign pilot: model proposes language revision from error pattern analysis | 30 |
| M15 | Conformance suite + consortium proposal | 32 |

---

## 21. Open Questions

Intentionally deferred to avoid premature commitment:

**Concurrency model** — async/await, goroutine-style, message passing, or structured concurrency? Affects type system and compiler significantly. Deferred until synchronous model is stable and validated.

**Generics depth** — currently only collection types. Full parametric polymorphism requires significant type checker complexity. Deferred until Phase 2 validation.

**FFI surface** — minimal `extern` declarations are sufficient for Phase 1. Full C ABI interop designed in Phase 2.

**The `^` and `~` reserved characters** — not yet assigned. Candidates: bitwise XOR, bitwise NOT. Assigned when concrete need arises.

**Language self-redesign** — the model will propose language revisions in Phase 4 based on error pattern analysis. The spec is deliberately left open to that outcome rather than anticipated by humans now.

---

## 22. Risks

| Risk | Probability | Mitigation |
|------|-------------|------------|
| Token savings marginal (<10%) | Medium | Phase 1 go/no-go gate; pivot to typed-IR approach |
| LLMs can't learn new language reliably | Low | Strong prior: specialisation demonstrated on domain-specific languages, Zig, COBOL |
| Mac Studio insufficient for 32B training | Low | Cloud A100 burst available; M4 Max 128GB has headroom |
| Qwen local quality insufficient for pre-filter | Medium | Easily verified; fall back to API-only if needed |
| Corpus contamination (test leakage) | Medium | Strict holdout, deduplication, hidden test rotation |
| Tcl/Tk confusion | Low | Different domain, different case; `tokelang` package name resolves ambiguity |
| Phase E architectural review under-resourced | Medium | Reduce Phase E scope; increase automated structural checks |

---

*toke is a research and engineering project. Every design decision should be revisited when empirical evidence — compiler error rates, corpus generation failure modes, model accuracy benchmarks — suggests a better alternative. The Phase 1 go/no-go gate at Month 8 is the most important decision point. Do not invest in Phase 2 before that data exists.*
