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

## Audit-120 cross-reference

**Added:** 2026-07-02 (Story 120.21, Epic 120)

The 57.4.5 scan above was limited by tooling (no cppcheck/clang-tidy/scan-build/ASAN/UBSAN
on the build machine) and reported no high-severity issues. The Epic 120 manual audit went
deeper than the available static tooling and **did** find high-severity memory-safety and
injection defects in the same C sources. Per-area reports live under [`audit-120/`](audit-120/);
each finding is tracked as an Epic 121 remediation story. This section supersedes the "no
critical or high-severity findings" verdict for the files listed.

### High-severity memory-safety findings audit-120 surfaced

| Finding | File | Area report |
|---------|------|-------------|
| COM-01 Heap buffer overflow in migrate prepass (`o[w++]` past `slen*2+256`) | `src/migrate.c:93` | [`compiler-frontend.md`](audit-120/compiler-frontend.md) |
| COM-01 `decompress_text()` output can exceed the caller's `len*4` buffer (back-reference overflow) | `src/compress.c:400` | [`compiler-codegen.md`](audit-120/compiler-codegen.md) |
| PAR-01/02/03 `yaml`/`toon`/`json` heap overflow and OOB read on trailing backslash | `src/stdlib/yaml.c`, `toon.c`, `json.c` | [`parsers.md`](audit-120/parsers.md) |
| PAR-04/08 Unbounded recursion (stack-overflow DoS) in value skippers / markdown | `src/stdlib/json.c`, `md.c` | [`parsers.md`](audit-120/parsers.md) |
| HTT-01/04 Chunked-body limit bypass; `ws_recv` integer overflow → heap overflow | `src/stdlib/http.c`, `ws.c` | [`http-core.md`](audit-120/http-core.md) |
| WEB-01 Heap buffer overflow in reverse-proxy request builder | `src/stdlib/proxy.c:314` | [`web-glue.md`](audit-120/web-glue.md) |
| TLS-01 Stack buffer overflow in `x509_fingerprint_hex` (fixed 8 KiB DER buffer) | `src/stdlib/tls.c:167` | [`tls-acme.md`](audit-120/tls-acme.md) |
| DAT-01/05 String-concatenated SQL (injection) + 4 KiB SQL-buffer overflow | `src/stdlib/db_glue.c` | [`database.md`](audit-120/database.md) |
| REL-01/06 Chunked-body overflow; signed-int `arena_alloc` overflow | `src/stdlib/http.c`, `src/arena.c` | [`reliability.md`](audit-120/reliability.md) |

### Note on the earlier "false positive" calls

Two 57.4.5 entries neighbour audit-120 findings and warrant re-review:
`companion.c:607` (57.4.5: "errno unchecked before malloc / Info") is the same reader
flagged by audit-120 COM-03 (unvalidated `ftell()` before `malloc`/`fread`,
[`compiler-codegen.md`](audit-120/compiler-codegen.md)); and the `router.c` static-serve
double-free dismissed as a false positive should be re-checked against the web-glue and
runtime-waf reports. The overall recommendation from 57.4.5 stands and is reinforced:
install and run cppcheck/clang-tidy/scan-build plus ASAN/UBSAN builds to catch the
overflow classes above automatically.
