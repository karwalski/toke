## 120.3 — Compiler front-end memory safety

Scope: C memory safety of the toke compiler front-end on untrusted `.tk` input —
`src/lexer.c`, `src/parser.c`, `src/names.c`, `src/types.c`, `src/fmt.c`,
`src/migrate.c`, `src/diag.c`, `src/lint.c`. Method: manual static review of buffer
and bounds handling, integer overflow on lengths/offsets, unbounded recursion /
stack exhaustion, UTF-8 edge cases, arena misuse, and use-after-free. No builds or
fuzzers were run (shared working tree); findings are grounded in the source as read.

The lexer, diagnostics emitter, name resolver, and formatter are generally careful
(bounds-checked token copies, growing buffers, clamped `TOKSTR`). The two real
memory-safety defects are a heap buffer overflow in the `--migrate` text prepass and
the total absence of a recursion-depth guard in the recursive-descent parser (and the
AST-walking passes downstream of it).

---

### COM-01 — Heap buffer overflow in migrate prepass (`o[w++]` past `slen*2+256`)

- Severity: High
- Reachability: local (`toke --migrate <file>` on attacker-controlled source; also
  build-time if a migration step runs in CI)
- File:line: `src/migrate.c:93` (allocation) and `src/migrate.c:752-758` (expanding
  transform); the whole prepass loop `src/migrate.c:145-926` writes via unchecked
  `o[w++]`.

Description: `prepass()` allocates its output buffer as
`char *o = malloc((size_t)(slen * 2 + 256));` (line 93) on the stated assumption that
"transforms may grow output slightly." The main rewrite loop then performs hundreds of
`o[w++] = …` writes and `memcpy(o+w, …)` appends **with no check that `w` stays within
the allocation**. Several transforms expand input by far more than 2×. The clearest is
the `loop` → three-clause infinite-loop rewrite at lines 752-758:

```c
const char *inf = "lp(let lv=0;true;lv=lv)";   /* 23 bytes */
int il = (int)strlen(inf);
memcpy(o+w, inf, (size_t)il); w += il;
i += 3; continue;
```

The 4-byte input keyword `loop` becomes 23 output bytes (≈5.75×). A source consisting
of repeated `loop;` units (5 input bytes → `lp(let lv=0;true;lv=lv);` = 24 output bytes,
4.8×) overflows the buffer: with `n` units, `slen = 5n`, buffer = `10n + 256`, output =
`24n`; the write index exceeds the allocation once `14n ≥ 256`, i.e. `n ≥ 19` — roughly
a **95-byte crafted file**. Other transforms compound the same class:
`[expr]` → `.get(expr)` (lines 252-254, 274-277), `[]` → `@()` (line 214),
`:void` → `:i64` (line 498), `f=name()` → `f=name():i64` (line 844).

Impact: Controlled heap out-of-bounds write from a small untrusted input. This is
memory corruption (adjacent heap metadata / allocations), a crash at minimum and a
plausible path to arbitrary code execution in the compiler process, which runs with the
developer's full ambient OS authority (no sandbox). The recursive re-invocation of
`tkc_migrate()` (line 1027) re-runs the prepass, widening the reachable surface.

Recommended fix: Track remaining capacity and bounds-check every write, or (better)
replace the fixed `slen*2+256` heuristic with a growing buffer that reallocs on demand
(the `Buf` abstraction already used in `fmt.c` is a ready model). Add a hard maximum
expansion cap and fail with a diagnostic rather than overrun. Also guard the size
computation against integer overflow (see COM-03).

---

### COM-02 — Unbounded recursion in the parser and AST-walking passes (stack exhaustion)

- Severity: Medium
- Reachability: local (compiler/`fmt`/`lint` on untrusted `.tk`); remote-auth if a
  hosted service compiles/formats user-submitted toke (e.g. a playground / sample
  validator)
- File:line: `src/parser.c:407` (`parse_type_expr`, self-recursive on `*`, `@`, `@(`),
  `src/parser.c:506`/`607`/`1022` (`parse_primary`→`parse_expr` on `(`, struct/array/map
  literals), `src/parser.c:796` (`parse_unary`, right-recursive on `-`/`!`/`~`). The
  `Parser` struct (`src/parser.c:82`) has no depth field; `MAX_PARSE_ERRORS` caps only
  the error count, not nesting depth.

Description: The recursive-descent expression and type parsers have no recursion-depth
limit. Deeply nested but well-formed input drives C-stack recursion proportional to
nesting depth: e.g. `((((((…))))))` (parenthesised expr, line 607), a long run of prefix
unary operators `------…x` (line 796), pointer/array type chains `****…i64` or
`@@@@…i64` (lines 408/419/432), or nested struct/array/map literals. There is no
per-parse ceiling, so a modest input (tens to low-hundreds of KB of nesting) exhausts
the stack and crashes the process with SIGSEGV.

The same unbounded depth propagates to every pass that walks the resulting AST
recursively without its own guard — `infer()` in `src/types.c:598` (and `ty_eq` /
`contains_ptr`, lines 121/396), name resolution in `src/names.c`, and the formatter
`fmt_expr`/`pfmt_expr` in `src/fmt.c:195`/`952`. The parser typically faults first, but
any tool that consumes a pre-built AST (or a deeper stack frame) is equally exposed.

Impact: Denial of service (reliable crash) of the compiler/formatter/linter from a
small untrusted file. Stack overflow is generally a crash rather than corruption here,
but it is trivially triggerable and defeats any batch/CI or hosted-compile use case.

Recommended fix: Add a `depth` counter to `Parser` (and to the type/format walkers),
increment on entry to the recursive productions, and emit a diagnostic + bail once a
configurable maximum (e.g. 256–1024) is exceeded. Register the limit in
`tkc_limits.h` alongside the existing capacity constants.

---

### COM-03 — Integer overflow on length/size arithmetic for very large inputs

- Severity: Low
- Reachability: local / build-time (requires ~1–2 GB input, so mostly a robustness
  rather than a practical-attack concern)
- File:line: `src/migrate.c:93` (`slen * 2 + 256`), `src/fmt.c:50-51`
  (`nc = b->cap * 2; while (…) nc *= 2;`), `src/parser.c:155-156`
  (`nc = par->child_cap*2; arena_alloc(p->a,(int)(nc*(int)sizeof(Node*)))`),
  `src/arena.c:77` (`arena_alloc` takes `int size`). For context (out of scope):
  `src/main.c:286` sizes the token buffer as `(int)(slen + 16) * (int)sizeof(Token)`.

Description: Sizes and offsets are computed in `int` throughout the front-end. For
inputs approaching/exceeding `INT_MAX`, `slen * 2`, `cap * 2`, `child_cap*2 *
sizeof(Node*)`, and the token-buffer product wrap to negative or small values.
`arena_alloc` clamps `size <= 0` to `1` (`src/arena.c:79`), so a wrapped size yields a
1-byte allocation into which the caller then writes many bytes — a heap overflow. In
`fmt.c` the doubling loop can also spin or realloc a wrapped size. Reaching these
requires enormous source (hundreds of MB to >2 GB) or an AST node with ~2^28 children,
so severity is Low, but the pattern is systemic.

Impact: Heap overflow / abort on pathological but not impossible inputs; a hardening gap
in the same class as COM-01.

Recommended fix: Perform size math in `size_t` with explicit overflow checks (or
`__builtin_mul_overflow`), reject inputs above a sane maximum source size with a
diagnostic, and make `arena_alloc` reject (not silently shrink) non-positive sizes.

---

### Dynamic-testing follow-up

The following should be proven at runtime (not run here, per audit rules):

- Build the compiler with ASan/UBSan and run `toke --migrate` on
  `printf 'loop;%.0s' {1..64}` (or a generated file of ≥19 `loop;` units) to confirm the
  COM-01 heap-buffer-overflow write; extend to a corpus mixing `[expr]`, `[]`, `:void`,
  and bare `f=name(){…}` to measure real expansion ratios.
- Fuzz the lexer→parser pipeline (AFL++/libFuzzer over the `lex`+`parse` entry points)
  with a small max input size and low stack limit (`ulimit -s`) to characterise the
  COM-02 stack-exhaustion crash depth for `(`, prefix-`-`, `*`/`@` type chains, and
  nested `@(…)` literals; repeat against `toke fmt` and `toke lint` to confirm the
  downstream walkers.
- Stress the `int` size arithmetic (COM-03) with large generated inputs under ASan to
  observe the wrapped-allocation overflow in `arena_alloc`/prepass/`buf_grow`.
- Fuzz UTF-8 / non-ASCII and truncated-escape inputs through the lexer to confirm the
  (apparently correct) `\x`, `\(`, and `(* *)` boundary handling holds under ASan.

### Positive observations (defenses already correct)

- `src/lexer.c`: `advance()` guards `pos >= len` (line 215); `emit()` checks
  `out_n >= out_cap` before every store (line 223); all scan loops are bounded by
  `pos < len`; `classify_ident` clamps to `buf[32]` via `len >= 32` (lines 199-201);
  string-interpolation `\(` and `(* *)` comment nesting are iterative (depth counters),
  not recursive.
- `src/diag.c`: `extract_source_line` fully bounds line start/end and clamps
  `line_len` to `buf_size-1` (lines 121-144); `buffer_sarif` uses `strncpy` with
  explicit NUL termination (lines 389-396); `json_escape` handles control bytes.
- `src/types.c`: the `TOKSTR` macro clamps every token copy to `sizeof(buf)-1`
  (lines 233-236); fixed collection arrays (`fields[64]`, `arm_tags[64]`) are guarded
  by `<64` checks before writing (lines 493/1436).
- `src/names.c`: `node_path_str` bounds-checks before each separator and `memcpy`
  (lines 80-86); `build_avail_list` sizes its output from a measured total (lines
  134-145) and honours `max_avail`.
- `src/fmt.c`: output uses a growing `Buf` with `buf_grow`/realloc rather than a fixed
  buffer (lines 46-56); `tok_text` allocates `tok_len+1` exactly (lines 88-96).
- Arena model (`src/arena.c`) means AST nodes are never individually freed, which
  structurally avoids use-after-free/double-free in the parser and later passes.
