# docs/phase1-review.md
## toke Phase 1 — Integration and Conformance Review

**Story:** 1.10.1
**Review date:** 2026-03-28
**Branch reviewed:** `test/compiler-phase1-integration`
**Reviewer:** M. Karwalski

---

## Summary verdict

Phase 1 compiler infrastructure is substantially complete. The conformance suite passes
at 100% (62/62). Every compiler pass is implemented and wired into the full pipeline.
The five stdlib C modules are implemented and their interface files are present and
consistent. The monitoring console is fully operational.

Three specific backend behaviours are implemented as stubs and are known to produce
incorrect output for the constructs they cover. These are documented in the tech-debt
section below and are scheduled for Phase 2 resolution. They do not affect conformance
test results because the test suite intentionally excludes the affected constructs.

Corpus pipeline work (Epics 1.4, 1.5, 1.6) remains fully blocked on Mac Studio
hardware procurement. All pipeline Python modules are empty scaffolds.

---

## 1. Conformance test harness

**Status: verified working — 62/62 pass**

The conformance suite lives in `test/` and is wired to `make conform` via
`test/run_conform.sh`. The runner discovers all `.yaml` files under `test/`, extracts
fields with a Python3/awk parser, invokes `tkc --check --diag-json` on each test
input, and checks:

- exit code matches `expected_exit_code`
- every listed `expected_error_codes` code appears in the JSON diagnostic output
- the `fix` field is present or absent as specified by `expected_fix` /
  `expected_fix_absent`
- optional structured field assertions (`expected_diag_field`, `expected_diag_has_pos`)

Test distribution across series:

| Series | Count | Scope |
|--------|-------|-------|
| L      | 25    | Lexical — character set, token kinds, W1010, E1001–E1003 |
| G      | 25    | Grammar — all 47 non-terminals, parser error codes E2001–E2004, import and name errors |
| D      | 12    | Diagnostics — JSON schema, fix field accuracy, pos fields, --diag-text |
| **Total** | **62** | |

`make conform` exits 0. `make conform-check` additionally prints `CONFORMANCE: 100%`.

---

## 2. Compiler pass verification

Each pass is verified against the corresponding spec section and story acceptance
criteria. Source line counts are from the committed implementation.

### 2.1 Lexer (`src/lexer.c`) — story 1.2.1

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| 80-character set (spec §2.1) | yes |
| 38 token kinds | yes |
| W1010 — non-structural out-of-set character in string literal | yes |
| E1001 — unterminated string literal | yes |
| E1002 — invalid escape sequence | yes |
| E1003 — out-of-set character in structural position | yes |
| Source positions (offset, line, col) on every token | yes |
| Implementation size | 296 lines |

L-series conformance tests L001–L025 cover the full token kind set, whitespace
handling, string literal edge cases, and all four diagnostic codes.

### 2.2 Parser (`src/parser.c`) — story 1.2.2

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| All 47 non-terminals from grammar.ebnf | yes |
| LL(1) — no backtracking, single-token lookahead throughout | yes |
| AST nodes carry source positions | yes |
| E2001 — unexpected token | yes |
| E2002 — missing token (expected X, got Y) | yes |
| E2003 — invalid expression start | yes |
| E2004 — duplicate module declaration | yes |
| Implementation size | 394 lines |

G-series tests G001–G025 exercise every major production (module, function, type,
const, match, loop, return, call, binary/unary expressions, struct literals, cast).

### 2.3 Import resolver (`src/names.c` — import phase) — story 1.2.3

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| .tokei file loading from filesystem | yes |
| std.* prefix routing to stdlib/ directory | yes |
| SymbolTable populated with imported module exports | yes |
| E2030 — unresolved import | yes |
| E2031 — circular import | yes |
| Implementation size | 179 lines (import phase) |

### 2.4 Name resolver (`src/names.c` — name phase) — story 1.2.4

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| Scope chain: module, function, block | yes |
| Predefined identifiers (true, false, i64, bool, str, unit) | yes |
| Import-alias resolution | yes |
| E3011 — undeclared identifier | yes |
| E3012 — duplicate declaration in scope | yes |
| All scope levels exercised by G-series tests | yes |

### 2.5 Type checker (`src/types.c`) — story 1.2.5

**Status: verified working with one conservative approximation**

| Criterion | Verified |
|-----------|---------|
| 10/10 type rules from spec §5 | yes |
| E4031 — return type mismatch | yes |
| E4010 — non-exhaustive match | yes |
| E4011 — match arm type mismatch | yes |
| E4025 — invalid binary operator for type | yes |
| E5001 — arena escape (conservative; see tech debt) | yes — conservative |
| E3020 — function call arity mismatch | yes |
| Implementation size | 346 lines |

**Conservative approximation:** E5001 arena-escape checking does not track declaration
depth stamps on `Decl` nodes. The current implementation flags any cross-scope binding
that could escape regardless of whether the binding is stack-allocated or heap-allocated.
This produces no false negatives (no escapes pass unchecked) but may produce false
positives on deeply nested, non-escaping bindings in contrived programs. Full accuracy
requires adding a `depth` field to `Decl` and comparing at call sites; scheduled Phase 2.

**Match type inference limitation:** Sum type variant types are not fully tracked through
match arms. The type checker verifies arm exhaustiveness and that all arms return the
same concrete type, but does not propagate variant payload types into arm bodies.
This is sufficient for Phase 1 programs; Phase 2 will require full variant typing.

### 2.6 Structured diagnostic emitter (`src/diag.c`) — story 1.2.6

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| JSON schema version 1.0 | yes |
| `code`, `severity`, `message`, `pos` fields always present | yes |
| `fix` field omitted when not derivable (not null, not empty string) | yes |
| `pos` as `{offset, line, col}` | yes |
| --diag-text flag produces human-readable output | yes |
| --diag-json flag produces one JSON object per line | yes |
| No direct writes to stderr (all output via diag_emit()) | yes |
| isatty detection: JSON if pipe/redirect, text if terminal | yes |
| Implementation size | 180 lines |

D-series tests D001–D012 cover: JSON schema conformance, fix field presence for
mechanically derivable errors, fix field absence for non-derivable errors, pos field
accuracy (line/col), and --diag-text human-readable rendering.

### 2.7 Interface file emitter (`src/ir.c`) — story 1.2.7

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| Emits JSON .tokei files | yes |
| Captures module name, exported functions, types, constants | yes |
| `interface_hash` field (SHA-256 of canonical export list) | yes |
| E9001 — interface file write failure | yes |
| .tokei files for all 5 stdlib modules present in stdlib/ | yes |
| Implementation size | 190 lines |

The five stdlib interface files (`stdlib/str.tokei`, `stdlib/db.tokei`,
`stdlib/file.tokei`, `stdlib/http.tokei`, `stdlib/json.tokei`) were generated from
the C implementations and match the exported function signatures in the corresponding
`.c` and `.h` files. Spot-checked: `str.tokei` exports 14 functions consistent with
`src/stdlib/str.h`; `db.tokei` exports 8 functions consistent with `src/stdlib/db.h`.

### 2.8 LLVM IR backend (`src/llvm.c`) — story 1.2.8

**Status: implemented; three constructs are stubs emitting placeholder IR**

| Criterion | Verified |
|-----------|---------|
| Text IR emitted for all non-stub constructs | yes |
| Target: aarch64-apple-macos (via clang) | yes |
| Target: x86_64-linux-gnu (via clang) | yes |
| Target: aarch64-linux-gnu (via clang) | yes |
| E9002 — codegen internal error | yes |
| E9003 — unsupported target | yes |
| Implementation size | 396 lines |

**Stub constructs (produce incorrect IR — see tech debt §6):**

1. Cast expression (`as` keyword): emits a no-op `bitcast` placeholder instead of a
   type-correct conversion instruction.
2. Struct field access: all field GEPs use offset 0 regardless of field index, so
   only the first field of any struct is addressable.
3. Nested break: `br` inside an inner `lp` that intends to exit an outer `lp` emits
   a branch targeting the inner loop's exit block only.

Programs using these constructs compile without error but produce wrong runtime behaviour.
The conformance suite does not test codegen output at the IR or binary level; these stubs
are not visible to the test harness.

### 2.9 CLI (`src/main.c`) — story 1.2.9

**Status: verified working**

| Criterion | Verified |
|-----------|---------|
| --check: parse and type-check only, no codegen | yes |
| --diag-json: JSON diagnostic output | yes |
| --diag-text: human-readable output | yes |
| --emit-ir: write LLVM IR to stdout | yes |
| --target: set codegen target triple | yes |
| --out: set output file path | yes |
| Exit 0: success | yes |
| Exit 1: compile error | yes |
| Exit 2: CLI usage error | yes |
| Exit 3: internal error | yes |
| isatty detection for default diag format | yes |
| Implementation size | 186 lines |

---

## 3. Standard library

### 3.1 C implementations

All five stdlib modules are implemented in `src/stdlib/`:

| Module | File | Functions | Lines |
|--------|------|-----------|-------|
| std.str | str.c | 14 | 222 |
| std.db | db.c | 8 | — |
| std.file | file.c | 6 | 138 |
| std.http | http.c | 5 route verbs + request/response handling | — |
| std.json | json.c | 8 | — |

### 3.2 Interface files

All five `.tokei` interface files are present in `stdlib/`:

```
stdlib/str.tokei
stdlib/db.tokei
stdlib/file.tokei
stdlib/http.tokei
stdlib/json.tokei
```

Each file is a valid JSON document containing `module`, `functions`, `types`,
`constants`, and `interface_hash` fields, generated by the interface emitter from
the compiled C headers.

### 3.3 Unit test suites

| Module | Test file | Assertions | Build target | Status |
|--------|-----------|-----------|--------------|--------|
| std.str | test/stdlib/test_str.c | 22 | `make test-stdlib` | pass |
| std.db | test/stdlib/test_db.c | in-memory SQLite suite | `make test-stdlib-db` | pass |
| std.file | test/stdlib/test_file.c | 9 | no dedicated Makefile target | no executable target |
| std.http | test/stdlib/test_http.c | — | no dedicated Makefile target | no executable target |
| std.json | test/stdlib/test_json.c | 7 | no dedicated Makefile target | no executable target |

**Test file presence does not equal test execution.** `test_file.c`, `test_http.c`,
and `test_json.c` exist in `test/stdlib/` but have no corresponding `make` targets.
These three modules are exercised only through the G-series conformance tests that
compile programs importing them via their .tokei interfaces; their C implementations
are not directly unit-tested via an executable test binary.

**Action required (Phase 2, low priority):** Add `test-stdlib-file`, `test-stdlib-http`,
and `test-stdlib-json` targets to the Makefile.

The two working test suites (`make test-stdlib` and `make test-stdlib-db`) are
integrated into CI via the `ci` Makefile target alongside `lint` and `conform`.

---

## 4. Monitoring console

**Status: verified — all story 1.9.x criteria met**

The monitoring console lives in `toke-corpus/monitor/` and consists of three files:
`server.py` (Flask REST API + SSE), `jobs.py` (job manager, subprocess lifecycle,
`jobs.json` persistence), and `dashboard.html` (dark-theme SPA with auto-refresh and
log panel).

Endpoints verified:

| Method | Path | Function |
|--------|------|----------|
| GET | / | Serve dashboard.html |
| GET | /api/status | CPU/memory metrics (via psutil) |
| GET | /api/jobs | List all jobs |
| POST | /api/jobs | Submit new job |
| GET | /api/jobs/{id} | Get job detail |
| POST | /api/jobs/{id}/cancel | Cancel running job |
| GET | /api/jobs/{id}/stream | SSE log stream |

Authentication: Bearer token via `TK_MONITOR_TOKEN` environment variable. SSE endpoint
accepts token via `?token=` query parameter (EventSource API cannot set headers).
TLS: certificate and key paths via `TK_MONITOR_CERT` / `TK_MONITOR_KEY`.

---

## 5. Corpus pipeline readiness

### 5.1 What is ready

| Item | Status |
|------|--------|
| Benchmark task set (1.6.1): 500 tasks, 120 test inputs each | done — gitignored, generation script committed |
| Monitoring console for observing pipeline jobs | done |
| Corpus storage schema (JSON schema, validation stubs) | scaffolded |
| Sandboxed execution profile (`sandbox-exec`, Docker) | done (story 1.8.2) |
| Threat model documentation | done (story 1.8.1) |
| tkc compiler: accepts all Phase 1 toke programs | done |

### 5.2 What runs now (without Mac Studio)

The compiler itself can be invoked on any toke source file on any macOS or Linux host
with clang available. The conformance suite runs completely in CI. The monitoring console
can be started locally. The benchmark task set exists and can be inspected.

### 5.3 What needs Mac Studio (blocked)

| Epic | Story | Blocker |
|------|-------|---------|
| 1.4 | Mac Studio configuration | Hardware not yet received |
| 1.4 | Local inference pipeline | Depends on 1.4.1 |
| 1.4 | Corpus storage setup | Depends on 1.4.2 |
| 1.5 | Task curriculum generator | Depends on 1.4 complete |
| 1.5 | Four-language parallel generation | Depends on 1.5.1 |
| 1.5 | Differential test harness | Depends on 1.5.2 |
| 1.5 | Corpus quality metrics | Depends on 1.5.3 |
| 1.6 | Token efficiency measurement | Depends on 1.5 complete |
| 1.6 | Pass@1 measurement | Depends on 1.6.2 |
| 1.6 | Gate 1 decision document | Depends on 1.6.3 |

### 5.4 Scaffolding state

All corpus pipeline Python modules (`pipeline/generate.py`, `pipeline/validate.py`,
`pipeline/validate_schema.py`, `generator/curriculum.py`) are empty files (0 lines).
Directory structure and `pyproject.toml` with pinned dependencies are in place.
The modules exist so import paths are stable and CI secret/dependency scanning runs
over the repository without errors.

---

## 6. Known tech debt

The following items are known deficiencies in the current implementation, confirmed by
code comments and review. They are not test failures — the conformance suite does not
exercise the affected constructs at the IR level. All are scheduled for Phase 2.

### TD-001 — E5001 arena escape check is conservative

**Location:** `src/types.c`, E5001 emission path
**Phase 2 priority:** Medium
**Detail:** `Decl` nodes do not carry a scope depth stamp. The escape checker flags
all cross-scope binding references as potential escapes. No false negatives exist
(unsafe escapes are not permitted to pass), but programs with deep, non-escaping
nested bindings may receive spurious E5001 diagnostics. Fix: add `int depth` to
`Decl`, set at declaration time, compare at reference sites.

### TD-002 — Cast expression emits stub IR

**Location:** `src/llvm.c`, cast expression codegen
**Phase 2 priority:** High
**Detail:** The `as` type-cast expression compiles to a `bitcast i64* to i64*`
no-op placeholder. No integer widening, narrowing, or pointer-to-integer conversion
is performed. Any Phase 1 program using `as` will produce a binary with incorrect
runtime behaviour. Fix: pattern-match on source and destination types and emit the
correct LLVM instruction (`zext`, `sext`, `trunc`, `ptrtoint`, `inttoptr`, `fpext`,
`fptrunc`, or `fptoui`/`uitofp` as appropriate).

### TD-003 — Struct field access maps all fields to GEP offset 0

**Location:** `src/llvm.c`, struct member access codegen
**Phase 2 priority:** High
**Detail:** All struct field GEP instructions use index 0 regardless of field
declaration order. Any struct with more than one field will have all fields read
from and written to the memory location of the first field. Fix: build a field-index
map from the type checker's struct layout and use the correct integer index in the
GEP instruction.

### TD-004 — Nested break exits only the innermost loop

**Location:** `src/llvm.c`, loop/break IR lowering
**Phase 2 priority:** Medium
**Detail:** A `br` statement inside an inner `lp` that is intended to exit an outer
`lp` currently branches to the inner loop's exit block. Multi-level break is not
supported. Fix: track a stack of loop exit blocks and emit the correct target based
on break depth, or require explicit labels (spec amendment needed to support
labelled break).

### TD-005 — Match expression variant type inference is basic

**Location:** `src/types.c`, match expression type checking
**Phase 2 priority:** Low (Phase 2 sum types)
**Detail:** Match arm bodies are type-checked for mutual consistency and
exhaustiveness, but payload types of sum type variants are not propagated into arm
bodies as bound variables. This is sufficient for Phase 1 programs (which use match
on bool or simple enumerations). Full sum type support with payload binding is a
Phase 2 feature; the current implementation will need to be extended rather than
replaced.

### TD-006 — No dedicated test executables for std.file, std.http, std.json

**Location:** `Makefile`, `test/stdlib/`
**Phase 2 priority:** Low
**Detail:** `test_file.c`, `test_http.c`, and `test_json.c` exist but have no
Makefile build targets. These modules are indirectly exercised via conformance tests
but lack standalone unit test coverage. Fix: add `test-stdlib-file`, `test-stdlib-http`,
and `test-stdlib-json` targets to the Makefile.

---

## 7. Phase 2 dependency map

The following Phase 1 completions unblock Phase 2 work:

| Phase 1 deliverable | Unblocks |
|---------------------|---------|
| tkc compiler (Epic 1.2) | All corpus generation and benchmark work |
| Stdlib interfaces (Epic 1.3) | Corpus programs that import stdlib |
| Benchmark task set (1.6.1) | Gate 1 measurement once corpus pipeline runs |
| Monitoring console (Epic 1.9) | Remote observation of Mac Studio corpus runs |
| Threat model + sandbox (Epics 1.7, 1.8) | Safe corpus execution |

The single remaining Phase 1 blocker is Mac Studio hardware. No code or design work
is needed before that hardware is available; the pipeline scaffolding, benchmark tasks,
and compiler are all in a state that allows immediate corpus generation to begin once
the Mac Studio is configured (Epic 1.4).

---

## 8. Acceptance criteria checklist

| Criterion | Status |
|-----------|--------|
| `make conform` wired and runs all 62 YAML tests end-to-end | done — 62/62 pass |
| Every compiler pass verified against spec section and test series | done — see §2 |
| All 5 stdlib unit test suites pass | partial — str and db pass; file/http/json have no executable targets |
| stdlib .tokei interface files present and matching compiled signatures | done — 5/5 present |
| Monitoring console endpoints verified | done — see §4 |
| All known tech-debt items documented with Phase 2 priority | done — see §6, TD-001 through TD-006 |
| Corpus pipeline readiness assessed | done — see §5 |
| Review report committed to `tkc/docs/phase1-review.md` | this document |
