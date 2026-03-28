# docs/conventions.md
## toke — Coding Conventions

These conventions apply across all components of the tokelang repository. When in doubt, follow the existing code's style rather than inventing a new one.

---

## C conventions (tkc compiler)

### Naming

| Thing | Convention | Example |
|-------|------------|---------|
| Functions | snake_case | `type_check_expr` |
| Structs | PascalCase | `TypeNode` |
| Typedef aliases | snake_case_t | `token_t` |
| Constants / macros | UPPER_SNAKE | `MAX_ARENA_DEPTH` |
| Local variables | snake_case | `current_scope` |
| Source files | lowercase, one word or underscore | `lexer.c`, `type_check.c` |
| Header files | same as source | `lexer.h` |

### Structure

- One struct definition per header file
- One primary responsibility per source file
- Declarations in header files, definitions in source files
- Include guards on all headers: `#ifndef TK_LEXER_H` / `#define TK_LEXER_H`
- System includes before project includes, separated by a blank line

### Function discipline

- Maximum 50 lines per function. Split anything longer
- Maximum 5 parameters. Group related parameters into a struct if more are needed
- Return type on its own line for readability in longer signatures
- Every public function has a brief comment above it if its name is not self-describing

```c
/*
 * type_check_match — verify all match arms are exhaustive and
 * return the same type. Emits E4010 on non-exhaustion, E4011
 * on type mismatch. Returns the common arm type or NULL on error.
 */
TypeNode *
type_check_match(MatchExpr *expr, Scope *scope, Arena *arena);
```

### Error paths

```c
/* Return NULL and emit diagnostic — do not return NULL silently */
if (!node) {
    diag_emit(DIAG_ERROR, E4010, expr->pos,
              "non-exhaustive match",
              "got", variant_name,
              "fix", NULL);  /* no mechanical fix for this one */
    return NULL;
}
```

### Diagnostic emission

Always use `diag_emit()`. Never write to stderr directly.

```c
/* Good */
diag_emit(DIAG_ERROR, E4020, call->pos,
          "argument type mismatch",
          "expected", type_name(param_type),
          "got", type_name(arg_type),
          "fix", fix_string_if_derivable);

/* Bad */
fprintf(stderr, "type error at line %d\n", call->line);
```

---

## Python conventions (corpus, tokenizer, models)

### Naming

| Thing | Convention | Example |
|-------|------------|---------|
| Functions | snake_case | `generate_task` |
| Classes | PascalCase | `CorpusEntry` |
| Constants | UPPER_SNAKE | `MAX_RETRY_ATTEMPTS` |
| Modules | snake_case | `corpus_pipeline.py` |
| Type aliases | PascalCase | `TaskId = str` |

### Type annotations

All function signatures must be fully annotated. Use `from __future__ import annotations` for forward references.

```python
# Good
def escalate_to_api(
    task: Task,
    error: DiagnosticRecord,
    max_retries: int = 3,
) -> CorpusEntry | None:
    ...

# Bad
def escalate(task, error, retries=3):
    ...
```

### Error handling

```python
# Good — specific exception, useful context
try:
    result = tkc.compile(source, target="arm64-apple-macos")
except CompilationError as e:
    logger.error("compilation failed for task %s: %s", task.id, e.error_code)
    return None

# Bad — bare except, silent failure
try:
    result = tkc.compile(source)
except:
    pass
```

### Logging

```python
import logging
logger = logging.getLogger(__name__)

# Use structured log messages with consistent field names
logger.info("corpus entry accepted", extra={
    "task_id": task.id,
    "phase": task.phase,
    "tk_tokens": entry.tk_tokens,
    "attempts": entry.attempts,
})
```

### Configuration

No hardcoded paths or values. All configurable values come from:
- CLI arguments (argparse)
- Environment variables (via `os.environ.get`)
- Config files loaded at startup

```python
# Good
output_dir = Path(args.output_dir)

# Bad
output_dir = Path("/Users/matt/tokelang/corpus/data")
```

---

## toke source conventions (stdlib)

### File structure

Every `.toke` file follows this order with no exceptions:
1. Module declaration
2. Import declarations
3. Type declarations (structs first, then sum types)
4. Constant declarations
5. Function declarations (exported functions first, helpers after)

### Naming

| Thing | Convention | Example |
|-------|------------|---------|
| Types (Phase 1) | PascalCase | `HttpReq` |
| Types (Phase 2) | $lowercase | `$user` |
| Functions | snake_case | `parse_route_param` |
| Parameters | snake_case | `req_body` |
| Local bindings | snake_case | `row_count` |

### Function discipline

- Maximum 30 statements per function
- Error type explicitly declared on every partial function
- One primary exported function per file where possible

### Comments in toke source

toke has no comment syntax. Use a companion `.md` file in the same directory for documentation:

```
stdlib/
├── std.http.toke
├── std.http.md    ← documentation for std.http
```

---

## Commit message examples

Good:

```
feat(typechecker): enforce exhaustive match arms E4010

Adds exhaustiveness checking to the match expression type checker.
Non-exhaustive match emits E4010 with variant name and source span.
All T-series conformance tests for match pass.
```

```
fix(lexer): emit E1003 for out-of-set chars in structural position

Characters outside the 80-char Phase 1 set in structural positions
now emit E1003 with byte offset and span. Previously the lexer
produced an internal error instead of a user-facing diagnostic.
```

```
test(diagnostics): add D-series fix field accuracy tests for E4020

Adds 12 D-042 through D-053 tests verifying that E4020 (argument
type mismatch) populates the fix field correctly for all supported
accessor substitution patterns.
```

Bad:

```
update stuff
```

```
fixes
```

```
WIP
```

---

## What not to do

These are the most common mistakes. Avoid them:

- Writing to stderr directly instead of using `diag_emit()` in the compiler
- Populating the `fix` field with a fix that is not always correct
- Changing an existing error code number or meaning
- Adding an `#include` for a non-standard library header in the compiler frontend
- Hardcoding file paths in Python scripts
- Skipping type annotations on Python functions
- Committing stdlib `.toke` files that do not compile with `tkc --check`
- Adding a new stdlib function that is not in the RFC spec without creating a spec amendment issue first
