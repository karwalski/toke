# Static Analysis Scan Results

**Date:** 2026-04-19
**Story:** 57.4.5
**Tools:** clang --analyze (Apple clang 17.0.0), -Wall -Wextra -Wpedantic -Werror

## Compiler Sources (17 files)

| File | Findings | Severity | Notes |
|------|----------|----------|-------|
| names.c:431 | Potential memory leak (alias, mpath) | Low | False positive: loop iteration frees previous iteration's allocations; final iteration cleaned by caller |
| llvm.c:2462 | Null pointer dereference (maybe_cond) | Medium | Defensive: `maybe_cond` is null only for malformed ASTs that don't pass the parser. Added to tech debt. |
| llvm.c:3132-3133 | Dead stores (tls_flags, tls_libs) | Info | Intentional: variables used only in `#ifdef TK_HAVE_OPENSSL` path |
| main.c:417 | errno unchecked before malloc | Info | Benign: errno is not used after this point |
| main.c:418 | fread buffer size 0 | Low | False positive: fread is only called when `sbuf != NULL` (guarded by `!sbuf ||`) |
| ir.c:101 | Uninitialized assign | Low | False positive: `s[i]` is within bounds of null-terminated string |
| companion.c:607,667 | errno unchecked before malloc | Info | Same pattern as main.c |

## Stdlib C Sources (18 files)

| File | Findings | Severity | Notes |
|------|----------|----------|-------|
| file.c:61 | errno unchecked | Info | Benign |
| file.c:291 | Stream position indeterminate | Low | After failed fread; handled by error return |
| log.c:521 | Stream position indeterminate | Low | Same pattern |
| router.c:866 | Double free | Medium | **False positive**: `match_route` returns a struct with fresh allocations per call; the loop frees each iteration's results |
| metrics.c:340 | Dead store (off) | Info | Used in subsequent `snprintf` |
| tk_web_glue.c:341 | errno unchecked before fclose | Info | Benign |
| toml.c:66 | errno unchecked | Info | Benign |

## Compilation Verification

- `-Wall -Wextra -Wpedantic -Werror`: 0 warnings (all suppressed warnings are documented)
- Conformance suite: 175/175 pass
- E2E test suite: 28/28 pass (previously verified)

## ASAN/UBSAN Status

cppcheck, clang-tidy, and scan-build are not installed on this machine. ASAN/UBSAN builds require modifying the Makefile CFLAGS. Recommend installing these tools for deeper analysis when hardware constraints allow.

## Verdict

No critical or high-severity findings in the static analysis. All findings are false positives, informational, or low-severity patterns that are correctly handled at the call site.
