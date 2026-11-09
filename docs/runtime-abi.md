# Toke Runtime ABI v0

Binary interface between compiled toke programs and the C runtime
(`tk_runtime.h` / `tk_runtime.c`).

Story: 10.4.9

---

## 1. Calling Convention

All functions use the **C calling convention** (`ccc` in LLVM IR).

The toke `main` function is renamed to `tk_main` during codegen.
The compiler emits a C-compatible `main()` wrapper:

```llvm
define i32 @main(i32 %argc, ptr %argv) {
  call void @tk_runtime_init(i32 %argc, ptr %argv)
  %r = call i64 @tk_main()
  %rc = trunc i64 %r to i32
  ret i32 %rc
}
```

If `tk_main` returns `void`, the wrapper calls it without capturing the
return value and returns `i32 0`.

**Exit code**: the i64 return value of `tk_main` is truncated to i32 for
the process exit code.

---

## 2. Scalar Types

| Toke type | LLVM IR type | Size    | Representation                |
|-----------|-------------|---------|-------------------------------|
| `i64`     | `i64`       | 8 bytes | Signed two's complement       |
| `f64`     | `double`    | 8 bytes | IEEE 754 double precision     |
| `bool`    | `i1`        | 1 bit   | 0 = false, 1 = true           |
| `void`    | `void`      | 0 bytes | No value                      |

**Bool widening**: when `i1` values cross a boundary that expects `i64`
(arithmetic, function call arguments, return statements), the compiler
emits `zext i1 %val to i64`. This is a zero-extension (false = 0,
true = 1).

---

## 3. String Layout

Strings are **C strings**: null-terminated `ptr` (pointer to `i8`).

- String literals are emitted as LLVM global constants
  (`@.str.N = private constant [K x i8] c"...\\00"`).
- Runtime-created strings are never freed: the runtime does not track
  them, and nothing may `free()` one that reached toke code.  Most are
  `malloc`'d; since 127.170 the core producers return **sealed strings**
  from the string heap instead, which carry their length (§3.1).  A sealed
  string is still a NUL-terminated C string, so nothing that reads one with
  `strlen` changes.

**IR access pattern** for string literals:
```llvm
%t0 = getelementptr inbounds [K x i8], ptr @.str.0, i32 0, i32 0
```

### 3.1 Strings carry no length (story 127.170) — S1 landed

**Status: option S1 (sealed string heap + side bitmap) is implemented,
following the owner decision of 2026-10-01 (§2.1).** Landing steps 1–4 are
in, and step 5 has begun (see "As landed" at the end of this section).  The
subsections from "What the premise check found" to "The prototype" are the
design record as written before the decision; "As landed" says where the
implementation differs from it.

#### What the premise check found

Measured on this host (macOS 26.6.2 arm64, Apple clang 21.0.0) at `f4010f3`
(first taken at `c8575da`; `str.c`, `str_glue.c`, `tk_runtime.c` and `llvm.c`
are unchanged between the two and the figures agree within 1%). Figures are
the median of five runs of `/usr/bin/time -l`, spread in the prototype table
below. **Instructions retired are the figure to read**: four other batches
were building and testing throughout, the load average ran 23–86, and wall
times are inflated two- to eightfold (the `.len` loop's wall median is 25.5 s
here against 6.19 s at load 11–18). Programs: `test/perf/strlen/*.tk` on the
prototype branch.

1. **The row is right about `str.indexfrom`, and the cost is the bounds check
   alone.** The 127.91 comma scanner over `"tok<i>,"` × 64,000 (a 564,890-byte
   string) retires 7,929,676,396 instructions (0.41 s at load 11). Deleting
   only the `memchr` that proves `start` lies inside the string takes it to
   119,138,127 (0.01 s). The remaining time is the check.
2. **`indexfrom` is the smallest of the cases.** Every per-call operation that
   needs the length rescans the whole string, so the ordinary character loop is
   quadratic twice over:
   - `.len` on a `$str` lowers to a call to `tk_str_len_w`, which is `strlen`.
     The call is declared with no attributes (`llvm.c` decl table), so LLVM
     cannot hoist it out of a loop condition. `lp(let k=0;k<t.len;k=k+1){…}`
     over the same string: **80,401,982,998 instructions (6.19 s at load 11).**
   - `t.charat(k)` (`tk_str_charat_w`) calls `strlen` to bounds-check `k`, and
     so does `slice`/`substr` (`str_slice`). The loop above with
     `if(t.charat(k)==44)` in its body: **160,663,111,667 instructions
     (12.36 s at load 11)** — the lexer loop every tokenizer in the language is
     written as.
   - Scaling 16,000 → 64,000 tokens (4×) multiplies instructions by 16.0
     (scanner), 17.9 (`.len`) and 18.0 (`.len` + `charat`). Quadratic.
3. **A pointer-keyed length cache is refuted by the tree.** Caching `strlen`
   by address needs a string's bytes never to change under the same pointer.
   They do: `os.getcwd` returns `static char buf[4096]` (`os.c:104`) while its
   `.tki` declares `"ownership": "caller"`; `ocr` returns `g_lang_list` and
   `pdf` returns `s_errmsg`, both rewritten in place (12 `static char` buffers
   in `src/stdlib`). (`os.getenv` was listed here as borrowed from `environ`;
   since 121.75 it returns a caller-owned copy, and `env.get`/`env.getor`
   since 121.78.) A stale entry is the over-read the check exists to prevent.
4. **The type system cannot help here, unlike 135.18.** Every `$str` is the
   same type, so the choice cannot be made statically; a length has to travel
   with the bytes or be recomputed.
5. **Strings are never freed today**, in the runtime or in any generated code
   (§3: "the runtime does not track or free them"). `docs/spec/memory-model.md`
   §1 describes per-scope arenas with bulk release; the implementation has
   never had them. Every option below that allocates differently inherits that
   gap rather than causing it.

#### Consumer inventory

Counted on `c8575da` with `/usr/bin/grep -a` (the `grep` shim skips files with
a NUL byte), and recounted on `f4010f3`: the core three files and `llvm.c` are
unchanged; Batch FA's strbuf migration removed 15 `strlen(` lines elsewhere,
and `tls.group` added one `str`-returning export.

| Class | Count | What each needs |
|---|---|---|
| Codegen `.len` on `$str` | 3 sites in `src/llvm.c` (method map, receiver table, the 127.8 emitter), one runtime call each | nothing for correctness; see S0 for hoisting |
| Codegen string literals | 1 emission site (`@.str.N`, `llvm.c` ~2578) | nothing: a literal's length is bounded by the source text, not the input |
| Runtime symbols `tk_str_*` that `llvm.c` calls | 55 distinct | nothing |
| Core string readers: `strlen(` in `str.c` / `str_glue.c` / `tk_runtime.c` | 60 lines (41 / 14 / 5) | the per-call O(len) ones — `.len`, `charat`, `char_at`, the `indexfrom` bound, `slice`/`substr`, `setat`, `endswith`, `trimsuffix`, concat and interpolation parts, builder `add` — convert first |
| All other stdlib C readers | 399 more `strlen(` lines; 459 in 77 of 200 `src/stdlib` C/H files (474 in 78 at `c8575da`) | correct unconverted; convert by profile |
| Producers visible to toke | 98 of 934 exported functions return `str`; 54 more return a type containing `str` | correct unconverted; convert by profile |
| Producer allocation sites in C | 314 lines calling `strdup`/`strndup` | each an unconverted producer until converted |
| Strings that must never carry a header | 86 `(intptr_t)"…"` literal returns, 12 `static char` buffers, argv (`os.getenv` left this row in 121.75: it returns an owned copy, an ordinary producer) | excluded by construction (below) |
| C that `free()`s a string that reached toke | 0 in `src/stdlib` for the core producers; `test/stdlib/test_tk_runtime.c` frees three `tk_str_concat` results | must stop — see ABI impact |
| C that writes into a string after returning it | 0 found statically (the three `strtok_r` calls work on private copies) | forbidden; enforced dynamically (the gate) |
| Native C in `toke-ooke`, `toke-mcp` | 0 files | nothing |

#### Options

- **S0 — codegen attributes only.** Declare `tk_str_len_w` (and the other
  pure readers) `nounwind willreturn memory(read)` so LLVM hoists `.len` out of
  a loop whose body writes no memory. Measured by editing the declarations in
  the emitted `.ll` (median of three, `f4010f3`): the `.len` loop goes from
  80,428,518,339 to 112,921,012 instructions — but the `charat` loop only
  halves (160,778,085,611 → 80,404,708,329), because
  `charat` rescans inside the body, and `indexfrom` is untouched. Any loop that
  appends or builds (every real scanner) blocks the hoist. **A cheap
  complement, not a fix.**
- **S1 — sealed string heap (recommended).** A string stays a NUL-terminated
  `char *`; nothing that reads one today changes. Converted producers allocate
  from one reserved address range and write the byte length in the 8 bytes
  before the first byte:
  ```text
  [ len:i64 | b0 b1 … b(len-1) \0 | pad to 16 ]
              ^ the pointer toke sees: 16-aligned + 8
  ```
  A side bitmap holds one bit per 16-byte granule, set by `tks_seal()` after
  the bytes are final. A reader trusts `p[-8..-1]` **only** if `p` lies in the
  range, sits at a granule's +8 and its bit is set; otherwise it calls
  `strlen`. So:
  - an **unconverted producer** is slow and correct (no bit, `strlen`);
  - an **unconverted reader** is slow and correct (the bytes are still a C
    string);
  - literals, argv, static buffers and **interior pointers** (a
    pointer into the middle of a sealed string has no bit) are excluded by
    construction, with no list to maintain.

  The migration can therefore stop at any commit and be right. That is the
  property 135.18 did not have, and the reason S1 is recommended over S2.
  Its one new rule: **a sealed string is never written, freed or reallocated.**
  Writing is caught by the gate below; `free`/`realloc` of an address malloc
  did not return aborts loudly on macOS and glibc, never silently.
- **S2 — a header on every string, trusted unconditionally.** The array
  model: `.len` is one load. But the reader cannot tell a header from the
  8 bytes before a literal, an argv string or a static buffer, so **every**
  producer — 313 allocation sites, 86 literal returns, 12 static buffers,
  argv, codegen literals (a layout change to `{i64, [K x i8]}`) —
  must convert before the first reader flips, and a missed one reads malloc
  metadata as a length: the silent wrong answer, at a larger surface than
  135.18. Not recommended; S1 can later specialise to it where codegen knows
  the producer.
- **S3 — a header marked by a magic word.** Probabilistic, and reading 8 bytes
  before a literal at the start of a page can fault. Rejected.
- **S4 — a fat `{ptr, len}` string.** Changes the signature of every function
  that takes or returns `$str` and every glue wrapper; rejected for the reason
  §7.6 rejected the two-word error return.
- **S5 — a pointer-keyed length cache.** Refuted by premise 3.
- **S6 — a cursor/scanner type in `std.str`.** Fixes `indexfrom` only, and is
  a stdlib addition needing a spec amendment. Not recommended alone.

#### ABI impact (S1)

- **Glue:** no signature changes. One header, `tk_strhdr.h`: `tks_alloc(n)`,
  `tks_seal(p, n)`, `tks_dup(s, n)` for producers, `tk_strlen(p)` and
  `tks_hdrlen(p)` for readers. The header's length **must equal `strlen(p)`**:
  a producer whose output can contain a 0 byte (`str.addbyte(b; 0)`,
  `str.setat(s; i; 0)`, `str.frombytes` of a buffer with a 0) seals with the
  C-string length, not the computed one. The prototype's `tk_str_done_w` does
  exactly this.
- **Codegen:** nothing required. Optional, separately: S0's attributes; an
  inline fast path for `.len` (range, bit, load) in place of the call; constant
  `.len` for a literal.
- **Tests:** `test/stdlib/test_tk_runtime.c` must stop `free()`ing
  `tk_str_concat` results once `tk_str_concat` is converted.
- **Memory:** 16 GiB of address space reserved at the first sealed string, plus
  a 128 MiB bitmap, both committed only as touched. A program that makes one
  string pays +32 KiB peak RSS. Bump allocation drops malloc's per-block
  overhead, which pays for the 8-byte header on small strings (below).
- **Threads:** allocation is an atomic bump; `tks_seal` publishes the bit with
  release ordering and readers load it with acquire, so a reader on another
  thread sees either the header or `strlen`, never half of one.
- **Targets without `mmap`** (`Makefile.wasm`): `tks_init` fails and
  `tks_alloc` falls back to `malloc` — every string unsealed, behaviour
  identical to today.
- **Future freeing:** if strings ever get the arenas `memory-model.md` §1
  describes (or ADR-0006's release), the region must become per-arena and
  release must clear its bits. S1 does not make that harder; it makes the
  current never-free behaviour explicit.
- **135.18 (`fix/ea-bytes`, held):** independent — arrays and strings share no
  code path in this plan.

#### Landing order

1. Add `tk_strhdr.h`, `TK_STR_VERIFY`, the scaling gate and the census, with
   **no producer converted**. Behaviour identical; `make diff-codegen`
   0 changes.
2. Convert the core producers (interpolation, concat, `slice`, builder
   `build`, then `replace`, `join`, `split` elements, `file.read`, the readers
   of stdin and sockets) and the core readers listed above. Each commit is
   independently correct.
3. Stop the three test frees (done on the prototype; unfixed,
   `test_tk_runtime` aborts in `free()` right after its `str_concat` case);
   run `make conform`, the stdlib tests and a corpus sample with
   `TK_STR_VERIFY=1`.
4. S0's attributes and the inline `.len`, as their own codegen change with
   their own conformance case.
5. The remaining 399 readers and 314 producer lines by profile, under the
   ratchet.

Steps 1–3 touch only `src/stdlib/` and its tests, never the compiler frontend
or codegen; step 4 touches only the `llvm.c` declaration table and the `.len`
emitter.

#### The gates

Under S1 an unconverted reader is correct, so "every reader converted" is a
performance claim, not a safety one; the safety gate is on writers. Three:

1. **`TK_STR_VERIFY` (safety).** With `TK_STR_VERIFY=1` in the running
   program's environment (read once; `-DTK_STR_VERIFY=1` forces it at compile
   time), every trusted read asserts `header == strlen(p)` and aborts on a
   mismatch. Off, it costs one predictable branch. Run over `make conform`,
   the stdlib tests and a corpus sample. Shown on the prototype: a planted
   write into a sealed `slice` result (`out[n/2] = 0`) makes
   `s.slice("abcdef";0;4)` print `len=4 text=ab` with the check off — the
   silent wrong answer — and abort with
   `TK_STR_VERIFY: header 4 != strlen 2` with it on; without the plant it
   prints `len=4 text=abcd`.
   On the prototype, `TK_STR_VERIFY=1 make conform` passes in full (271 YAML
   cases; 172 scripts, the two named SKIPs) with no abort, and so does
   `test/stdlib/test_str.c`. Its limit: the switch fires only when a sealed
   string's length is *read*. With the same plant, conform goes red through
   ordinary value assertions (C035, C049 print a slice), not through the
   switch — no conform case takes `.len` of a slice result. Landing step 3
   should add one that does.
2. **Scaling (performance).** `test/perf/strlen/scaling.sh` runs the scanner,
   the `.len` loop and the `.len`+`charat` loop at N and 4N and fails if
   instructions retired grow more than 6× (linear ≈ 4, quadratic ≈ 16).
   Instructions, not wall time, so load cannot flip it. Shown at N=4000:
   against `f4010f3` it fails all three (ratios 9.85, 16.91, 17.62; the
   scanner is below 16 at this N because the build loop is linear); against
   the prototype it passes all three (1.83, 1.89, 1.88 — below 4 because
   start-up is a fixed cost at this size).
3. **Reader ratchet.** A `check-str-strlen` script: every `strlen(` in
   `src/stdlib` is on a dated skiplist that only shrinks, and the core readers
   above may not be on it. It says which readers are left, not whether the
   tree is correct — gate 1 says that.

#### The prototype (branch `proto/fe-strlen`, not for merge)

Implements `tk_strhdr.h` and converts four producers (`tk_str_join_n` —
interpolation —, `tk_str_concat`/`str_concat`, `str_slice`, `tk_str_done_w`)
and the readers `str_len`, `tk_str_len`, `tk_str_charat_w`,
`tk_str_char_at`, the `indexfrom` bound, `str_slice`, the concat and
interpolation part lengths and the builder's `add`. Nothing else.

Median of five runs, `[min..max]` for instructions; RSS is the median of
maximum resident set size. `main` is `f4010f3`, `proto` is `proto/fe-strlen`
merged with it. Wall times are omitted as unreadable under load 23–86 (the
`main` wall medians for the 64k rows were 1.84 s, 25.5 s and 48.5 s; every
`proto` wall median was at most 0.18 s).

| Program | N | main instructions | proto instructions | main RSS | proto RSS |
|---|---:|---:|---:|---:|---:|
| `scan_from` (127.91 scanner) | 4,000 | 49,410,838 [48.5M..50.6M] | 24,347,477 [23.9M..25.7M] | 1.85 MB | 1.92 MB |
| | 16,000 | 495,713,986 [494.5M..496.5M] | 44,358,629 [43.7M..45.5M] | 2.59 MB | 2.93 MB |
| | 64,000 | 7,929,676,396 [7.929G..7.939G] | 120,119,384 [119.6M..121.3M] | 5.32 MB | 6.88 MB |
| `len_only` (`.len` loop) | 4,000 | 263,094,906 [261.9M..266.1M] | 24,345,322 [24.2M..26.0M] | 1.84 MB | 1.90 MB |
| | 16,000 | 4,490,502,143 [4.486G..4.500G] | 47,822,418 [46.6M..48.6M] | 2.59 MB | 2.93 MB |
| | 64,000 | 80,401,982,998 [80.39G..80.49G] | 133,382,171 [132.7M..134.2M] | 5.32 MB | 6.88 MB |
| `len_loop` (`.len` + `charat`) | 4,000 | 503,317,059 [503.1M..505.3M] | 27,275,179 [25.7M..27.7M] | 1.79 MB | 1.92 MB |
| | 16,000 | 8,937,070,574 [8.933G..8.941G] | 53,552,332 [52.9M..54.0M] | 2.51 MB | 2.93 MB |
| | 64,000 | 160,663,111,667 [160.53G..161.02G] | 161,673,028 [160.1M..162.9M] | 5.31 MB | 6.90 MB |
| `small_strs` (10⁶ interpolated strings kept in an array, `.len` each) | 1,000,000 | 1,380,691,249 | 1,308,898,810 | 66.40 MB | 66.45 MB |
| `lit_len` (`.len` of a literal, 10⁶×) | 1,000,000 | 51,010,888 | 63,127,445 | 1.46 MB | 1.51 MB |
| `short_len` (`.len` of a 12-byte sealed string, 10⁶×) | 1,000,000 | 47,708,239 | 59,714,471 | 1.47 MB | 1.49 MB |

For scale, the scanner with only its `memchr` bound deleted (no header, no
check) retires 119,138,127 at 64,000: the prototype is within 0.8% of that
ideal. Scaling 16,000 → 64,000 on the prototype multiplies instructions by
2.71, 2.79 and 3.02 (main: 16.0, 17.9, 18.0).

The prototype's `allocs` figure (from `bench/patterns/liballoccount.dylib`)
halves — 128,211 → 64,210 malloc calls at 64k — only because interpolated
strings now come from the string heap, which the interposer does not see; it
is not a saving in bytes.

What it proves and what it does not:

- **Proves** that a length carried with the bytes makes the three loops
  linear without changing any signature, any codegen or any unconverted
  caller: 127.91's scanner drops 66×, the `.len` loop 603× and the
  `.len` + `charat` loop 994× at 64,000 tokens, and every output is
  byte-identical to `main`.
- **Proves** the "stop at any commit" property on the tree as it is: only
  four producers and a handful of readers are converted, and nothing else
  was touched to keep the rest correct.
- **Proves** that the verify switch catches a writer that breaks the seal
  (gate 1), and that the scaling gate is red on `main` and green here
  (gate 2).
- **Costs, measured.** RSS rises by 0.34 MB at 16k and 1.56 MB at 64k
  (bump-allocated strings are padded to 16-byte granules plus the header,
  and every 2 MiB of region touches one 16 KiB bitmap page); at 10⁶ small strings RSS
  is level with `main`, because malloc's own per-block overhead is gone.
  A `.len` on a short string pays about 12 instructions more per call than
  `strlen` does (literal: 51.0M → 63.1M per 10⁶; 12-byte sealed string:
  47.7M → 59.7M) — the range, alignment and bitmap tests plus the verify
  branch. That is the cost the inline `.len` of landing step 4 removes; it
  is also why S0's attributes stay worth doing.
- **Does not prove** anything about threads under contention, the wasm
  fallback (not built), or any producer not converted; it does not free
  strings, so it says nothing about the arena future beyond the note above.
  It leaves `str.setat` writing into its argument's copy only because
  `setat` already copies — a producer that wrote in place into a sealed
  string would be caught by gate 1, not prevented.
- **Not for merge.** It defaults the region to 16 GiB of reserved address
  space with no configuration and carries a weak-symbol trick so that the
  compiler binary, which links `str.o` without `tk_runtime.o`, still links;
  landing step 1 should put the globals in one translation unit instead.

#### As landed (branch `feature/z63-s1-strings`, 2026-10-08)

**The API** is `src/stdlib/tk_strhdr.h`; its state and out-of-line half are
at the end of `src/stdlib/str.c`, the one translation unit every compiled
program and `tkc` itself link (no weak symbols).  Producers: `tks_alloc(n)`,
`tks_seal(p, n)`, `tks_dup(s, n)`, `tks_strdup(s)`.  Readers: `tk_strlen(p)`,
`tks_hdrlen(p)`.  Layout, trust rule and the one new rule (a sealed string
is never written, freed or reallocated) are as designed above.

**Where it differs from the prototype:**

- *Commit as touched, for real.* The 16 GiB region and the 128 MiB bitmap
  are mapped `PROT_NONE` (plus `MAP_NORESERVE`) at the first `tks_alloc`, and
  made read/write by `mprotect` in 64 MiB steps (512 KiB of bitmap per step)
  as the bump pointer reaches them.  The prototype mapped both read/write up
  front, which Linux's default heuristic overcommit refuses outright on a
  host with less than ~16 GiB of RAM + swap; a refused mapping silently
  meant the malloc fallback everywhere.
- *Initialisation under a lock.* The prototype stored the bitmap pointer
  before its compare-and-swap of the base, so a thread that lost the race
  could leave `tks_bits` pointing at the bitmap it then unmapped.  The
  reservation and each chunk commit now run under one spin lock; the bump
  itself stays a lock-free `fetch_add`.  The lock is held across `fork()`
  (`pthread_atfork`) because the HTTP server forks workers: a fork during a
  commit would otherwise leave the child's copy locked.
- *Verify switch read once, before publication.* `TK_STR_VERIFY` is read from
  the environment when the region is reserved, before the base pointer is
  published, so a reader that sees a sealed string sees the switch too.
- *ASan.* Under AddressSanitizer every block's bytes past the NUL plus a
  16-byte redzone are poisoned, so an over-read of a sealed string is
  reported as it would be for a malloc'd one ("use-after-poison").
- *Fallback.* No `mmap` (wasm), a 32-bit `size_t`, `-DTK_STR_NO_HEAP`, an
  `RLIMIT_AS` too small for the reservation, or an `mprotect` the OS refuses:
  `tks_alloc` returns malloc'd memory and `tks_seal` does nothing — every
  string unsealed, behaviour as before.

**Converted.** Readers (all 20 named in the consumer inventory, checked by
`scripts/check_str_strlen.py`): `.len` (`str_len`, `tk_str_len`), `charat`
(`tk_str_charat_w`, `tk_str_char_at`), `slice`/`substr`, `setat`, `endswith`,
`trimsuffix`, concat, interpolation parts (now measured once, not twice),
builder `add`, and the `indexfrom` bound (header when present, else the
`memchr` of the prefix).  Producers: interpolation (`tk_str_join_n`),
`tk_str_concat`/`str_concat`, `slice`, the builder's `build` (one copy out
of the builder; sealed with the C-string length because `addbyte(b;0)` can
store a 0), `replace`, `join`, every `split` element, `file.read` (new
`file_read_into(path, alloc, seal)`: a regular file is read straight into
the heap; `file_read` keeps its malloc contract because `toml_load_file`
frees it), `io.readln`, `net.read`; and, as step 5's first tranche, `trim`,
`upper`, `lower`, `repeat`, `padleft`, `padright`, `reverse`, `trimprefix`,
`trimsuffix`.  Producers whose bytes can hold a 0 (file, line, packet, a 0
pad) seal with the C-string length.  Not converted: `str.frombytes` (its
wrapper frees the `str_from_bytes` result), `json` strings, every other
module — census below.

**Step 5, second tranche (story 127.295, Batch Z96).**  Converted whole, by
module: `std.path` (every result sealed; `test_path.c` no longer frees),
`std.xml` (builder results, `get`, `attr`; the parser's `XmlNodeArray`
strings stay malloc'd, `xml_free` owns them) and `std.fmt` (`arr`, `strs`,
`pad`).  Readers only, because C callers free the results: `std.encoding`
(`auth.c`, `acme.c`, `http.c` free the encoders' strings), `std.html` and
`std.svg` (the node trees and documents own theirs).  `html.render`,
`html.docr` and `svg.render` now return a sealed copy from the glue, which
also fixes a dangling result (127.316: the next render freed the cached
buffer toke held).  The rest of `str.c`/`str_glue.c`: every reader but the
verify switch's own `strlen` (`tks_verify_fail`), and `fmt.f64`,
`fmt.sprintf`, `str.sha256prefix` seal.  Gate: C911 (each converted
producer's header equals `strlen`, and a program reads them under
`TK_STR_VERIFY=1`).  `encoding.jsonfield` followed once its over-read was
fixed (127.317, Batch Z97: a value ending in a backslash at the end of the
input stepped over the NUL); its result is sealed (`tks_dup` of the scanned
value, which can no longer hold a 0 byte).  Gate: C920 (ASan C driver and
an ASan-built toke program under `TK_STR_VERIFY=1`).

**Step 5, third tranche (story 127.295, Batch Z105).**  The C callers that
held `std.encoding` back were changed with it: every encoding string result
(`base32`, `b64encode`, `b64urlencode`, `hexencode`, `urlencode`,
`urldecode` — which drops `%00`, so its count is the C-string length) is
sealed, and `auth.c`, `acme.c` and `http.c`'s form parser no longer free
them (`http_form_free` frees only the field array).  `std.auth` converted
whole: every reader uses `tk_strlen`, and the token, `jwtverify`/
`jwtdecode` claim strings, API key, bearer token, OAuth2 authorize URL and
TOTP URI/secret are sealed (the glue now hands `jwtverify`'s claims to toke
as they are instead of copying them); `JwtDecodedClaims.raw_payload` (bytes
that may hold a 0) and `auth_password_hash` (std.crypto's bcrypt producer)
stay malloc'd.  `acme.c`: every reader uses `tk_strlen`; its client strings
are internal storage that never reaches toke.  Gate: C990 (ASan C drivers
over encoding, auth, the form parser and `acme_jws`, plus a toke program
under `TK_STR_VERIFY=1`).

**Step 5, fourth tranche (story 127.295, Batch Z115).**  `std.llm` converted
whole: every reader in `llm.c` uses `tk_strlen` or the length a buffer or
`json_escape` already holds, and every string that reaches toke is sealed —
a response's content (`llm_chat`, `llm_chat_extra`, `llm_complete`,
`llm_json_mode`, `llm_vision`), every `llm_parse_sse_chunk` result and so
every stream chunk, and the client's `provider`/`base_url`/`api_key`/`model`
(`llm_client_free` frees only the struct; `llmtool.withtools` copies the
struct and shares the fields).  `err_msg` strings, request bodies, raw
response bodies and embedding arrays stay malloc'd: they never reach toke.
`llm_tool.c`'s callers stop freeing the content; `llm_agentic_loop` hands
its C caller a malloc'd copy of the final reply (its own strings are the
next tranche).  The JSON string decoder no longer steps over the NUL after a
trailing backslash (story 127.331), so its result cannot hold a 0 byte.
`std.ws` (`ws.c`, `ws_server.c`) reads every length with `tk_strlen` or from
the literal or buffer that holds it; it hands no string to toke (`ws.recv`
returns a `[byte]`), so nothing there is sealed.  Gate: C1050 (an ASan C
driver against a local mock endpoint, plus a toke program under
`TK_STR_VERIFY=1`).

**Step 5, fifth tranche (story 127.295, Batch Z118).**  `std.vecstore`
converted whole: every reader in `vecstore.c` / `vecstore_glue.c` uses
`tk_strlen`, and `vecstore.search` hands toke sealed `.id` / `.payload`
copies (the core's own entries, names and result arrays stay malloc'd:
`collection_free` / `vecstore_free_results` own them).  `std.template`
(`template.c`), `std.router` (`router.c`) and the std.http web glue
(`tk_web_glue.c`) are converted as readers only: every length is read with
`tk_strlen`; their producers (rendered templates, route and page tables,
`strdup`'d header values) are not sealed yet, because C callers in the
same files free them.  Gate: C1061 (an ASan C driver under
`TK_STR_VERIFY=1` over sealed inputs, plus a toke program).

**Codegen (step 4, S0).** `tk_str_len`, `tk_str_len_w`, `tk_str_char_at` and
`tk_str_charat_w` are declared `nounwind willreturn readonly` in
`src/stdlib_tables.c` (`readonly`, not `memory(read)`: older LLVM parses
only the former, LLVM ≥ 16 upgrades it).  Only declaration text changes;
`make diff-codegen` shows no behaviour change.  The inline `.len` fast path
was **not** landed: an inline header read needs acquire loads, which LICM
does not hoist, so it would trade away S0's hoisting where that applies, to
save about 5 instructions per call where it does not.  Constant `.len` of a
literal was not landed either; a hoisted `.len` of a literal already costs
nothing per iteration.

**The gates, red and green.**

1. *`TK_STR_VERIFY` (safety)* — conform case C736: `.len` of a slice,
   concat, interpolation and build results, a build holding a 0 byte,
   charat/indexfrom/endswith/trimsuffix, eight `spawn`ed tasks producing
   strings concurrently, all under `TK_STR_VERIFY=1`; then the switch in C
   against the shipped `str.c`.  Red: `str_slice` with a planted
   `out[n/2] = 0` after sealing aborts with `TK_STR_VERIFY: header 4 !=
   strlen 2`; main fails the switch case (no header).  The full conformance
   suite was run with `TK_STR_VERIFY=1` at landing step 3 (`85b2b1ea`):
   every YAML case and every script passed except C735, whose instruction
   counts the switch's per-read `strlen` makes quadratic (the case now
   unsets it); no verify abort anywhere.  The stdlib CI-green tests under
   the switch: all pass but `test-stdlib-auth-flow-integration`, which fails
   identically on main.  Blast radius: 840 corpus programs
   with a `main` (60 from each of the 14 categories of
   `toke-corpus/corpus/regen_v04`), built with main's compiler and run
   plainly, and with the branch's compiler and run under `TK_STR_VERIFY=1`:
   exit status and stdout identical for all 840 (828 ran, 12 fail to compile
   on both), no verify abort, no `free()` abort.  ASan/UBSan: the C736
   programs, the scaling programs and the producer probes built with an
   instrumented stdlib run clean, as do the stdlib unit tests
   (`make test-asan-stdlib`, plus file, runtime, process and toml).
2. *Scaling (performance)* — conform case C735 runs
   `test/perf/strlen/scaling.sh` (N = 4,000 vs 16,000, fail above 6×).
   Main: 9.72 / 17.02 / 17.75, FAIL.  Branch: 1.78 / 1.77 / 1.92, PASS.  An
   in-script control runs the `.len` + charat loop over a string from an
   unconverted producer (`str.frombytes`) and must still FAIL (16.99 here),
   so the gate is seen to fire on the host that runs it.  (Not the `.len`
   loop alone: since step 4 a loop-invariant `.len` is hoisted, and is
   linear even on an unsealed string.)  The measured runs unset
   `TK_STR_VERIFY`, which by design adds a `strlen` to every trusted read.
3. *Reader ratchet* — `make check-str-strlen` (also in `make lint`):
   per-file counts of bare `strlen(` lines in `scripts/check_str_strlen_skiplist.txt`
   may only fall, and the core readers may not call `strlen` at all.  Shown
   red on a planted `strlen` in `tk_str_charat_w` (CORE + NEW) and on a
   removed one (STALE).  Census at landing: 420 bare `strlen(` lines in 79
   files (458 before), 40 sealed-producer lines in 5 files, 228
   `strdup`/`strndup` lines in 36 files still unconverted.

**Measured** on this host (macOS 26.6.2 arm64, Apple clang 21.0.0), main
`fb0592a1` against the branch head; instructions retired, median of five
runs (`[min..max]`), `-O2`; RSS is the median maximum resident set size.
Programs in `test/perf/strlen/`: the three scaling programs, the
prototype's three cost probes, and two more whose loop writes memory, so
`.len` cannot be hoisted.

| Program | N | main instructions | branch instructions | main RSS | branch RSS |
|---|---:|---:|---:|---:|---:|
| `scan_from` (127.91 scanner) | 4,000 | 48,446,655 [48.00M..49.60M] | 21,860,998 [21.64M..23.43M] | 1.80 MB | 1.89 MB |
| | 16,000 | 492,067,144 [491.8M..493.4M] | 41,542,178 [40.56M..42.07M] | 2.47 MB | 2.83 MB |
| | 64,000 | 7,909,248,772 [7.905G..7.915G] | 113,758,916 [113.4M..114.5M] | 5.06 MB | 6.61 MB |
| `len_only` (`.len` loop) | 4,000 | 261,087,221 [260.7M..261.7M] | 21,221,498 [20.94M..22.50M] | 1.81 MB | 1.88 MB |
| | 16,000 | 4,471,371,162 [4.465G..4.474G] | 39,440,508 [38.35M..39.84M] | 2.48 MB | 2.83 MB |
| | 64,000 | 80,075,912,776 [80.07G..80.11G] | 103,489,463 [102.7M..104.6M] | 5.08 MB | 6.61 MB |
| `len_loop` (`.len` + `charat`) | 4,000 | 501,867,922 [501.4M..503.6M] | 22,834,316 [22.58M..25.01M] | 1.80 MB | 1.86 MB |
| | 16,000 | 8,894,363,766 [8.892G..8.903G] | 44,675,494 [44.28M..45.49M] | 2.47 MB | 2.83 MB |
| | 64,000 | 160,056,233,395 [160.01G..160.16G] | 131,460,316 [130.5M..131.7M] | 5.08 MB | 6.62 MB |
| `small_strs` (10⁶ interpolated strings kept, `.len` each) | 1,000,000 | 1,464,136,049 | 1,354,707,484 | 63.36 MB | 77.23 MB |
| `lit_len` (`.len` of a literal, hoistable) | 1,000,000 | 51,058,870 | 18,067,834 | 1.44 MB | 1.48 MB |
| `short_len` (`.len` of a 12-byte sealed string, hoistable) | 1,000,000 | 46,760,917 | 18,521,841 | 1.44 MB | 1.47 MB |
| `lit_len_nh` (literal, loop writes memory) | 1,000,000 | 70,966,532 | 79,520,122 | 1.44 MB | 1.50 MB |
| `short_len_nh` (sealed, loop writes memory) | 1,000,000 | 66,007,558 | 72,360,333 | 1.44 MB | 1.48 MB |

At 64,000 tokens the scanner retires 69× fewer instructions, the `.len`
loop 774× and the `.len` + `charat` loop 1,218× fewer, with byte-identical
output; 16,000 → 64,000 multiplies them by 2.74, 2.62 and 2.94 (main: 16.1,
17.9, 18.0).  The costs: a `.len` that cannot be hoisted pays about 6
instructions more per call on a short sealed string and about 9 more on a
literal (the range test fails, then `strlen`), against about 28 and 33 fewer
where S0 lets it hoist; and small strings take more memory — 10⁶ strings of
3–9 bytes cost 77.2 MB against 63.4 MB, because a block is the 8-byte header
plus the bytes plus the NUL rounded up to 16, so a string of 8–23 bytes takes
32 bytes where macOS malloc's 16-byte quantum gives 16 or 32.  The prototype
measured RSS level at this size with different string lengths; this is the
cost the owner decision accepted with the 16-byte granule.

**Memory, measured on macOS** (an in-process probe reading
`TASK_VM_INFO`): the first sealed string adds 16.13 GiB of virtual size
(the region plus the bitmap; region base `0x300000000` in the probe,
`0x7000000000` in a compiled toke program) and 0.05 MiB resident; 100 MiB of
sealed strings then add 101.8 MiB resident, and 10⁶ five-byte strings
15.4 MiB (16 bytes each).  macOS does not enforce `RLIMIT_AS`, so
`ulimit -v` cannot make the reservation fail there.

**Memory on Linux, from the code** (not run: this host has no Linux
toolchain, and `make build-all` needs a sysroot it lacks).  A `PROT_NONE`
private mapping is not charged against the commit limit, so the 16 GiB
reservation succeeds under `vm.overcommit_memory` 0, 1 and 2; each 64 MiB
`mprotect` to read/write is charged, and under mode 2 a refused one sends
that allocation and every later one past the committed end to malloc.  An
`RLIMIT_AS` below about 16.2 GiB (`ulimit -v`) makes the reservation fail
and every string unsealed.  The read/write prefix and the `PROT_NONE` rest
stay two mappings each, so `vm.max_map_count` is not a concern.

**Threads.** `test-stdlib-strhdr`: eight threads race the one-time
reservation, then allocate, seal and cross-read 160,000 strings; forty
`fork()`s while a second thread commits chunks.  Clean under ASan/UBSan;
TSan reports no data race.  C736 does the same at toke level with `sc` /
`spawn` (std.task's worker pool).

**Not landed / next (step 5, by profile, under the ratchet):** the other 420
`strlen` readers and 228 `strdup`/`strndup` producer lines across the
stdlib; `str.frombytes` (needs its wrapper's `free` reworked first); the
inline `.len` (above).

---

## 4. Array Layout

> Rewritten 2026-09-26 (135.18). The version this replaces described the
> pre-114.18 single-word header and stack-allocated literals; neither has been
> true since ADR-0006. `src/stdlib/tk_array.h` is the normative source.

Every array is a heap block with a three-word header and `i64` element slots:

```
Memory: [ rc | cap | len | data[0] | data[1] | ... | data[len-1] ]
                           ^ handle (the i64 that crosses the toke ABI)
handle[-1] == len    handle[-2] == cap    handle[-3] == rc
```

- `len` and `cap` count elements. `rc` is a monotonic reference count used
  only for copy-on-write (ADR-0006 D2): a mutation writes in place when
  `rc == 1`, otherwise it copies. Blocks are never freed.
- Every element is 8 bytes. `f64` elements are stored as their bit pattern;
  `str`, struct and nested-array elements as a pointer (`ptrtoint`).
- Construction goes through `tk_arr_alloc(cap, len)` in C and an inlined
  `malloc((n + 3) * 8)` with the same header in codegen (array literals,
  spread). There are no stack-allocated or header-less array blocks.

**Length** (`.len`): `load i64, (gep i64, handle, -1)`.
**Element read** (`a[i]`, after the RT003 bounds check against `handle[-1]`):
`load i64, (gep i64, handle, i)`, then a `bitcast` to `double` for `[f64]`.
Everything else (`append`, `set`, `slice`, `+`, `map`/`filter`/`reduce`/`sort`,
`join`, `indexof`) is a runtime call into `collections_glue.c`, `str_glue.c` or
`tk_runtime.c`.

### 4.1 `[byte]` today: one i64 slot per byte

A `[byte]` is an ordinary array whose elements happen to be 0..255
(`src/stdlib/bytes_rt.h`). Glue converts at the boundary with
`tk_bytes_unpack` (block → malloc'd `uint8_t *`) and `tk_bytes_pack`
(`uint8_t *` → block). This is binary-safe and needs no codegen support, and
it costs **eight bytes of memory per byte of data** — which is why
`file.readbytes` and `file.readrange` are capped at 64 MiB
(`TK_FILE_MAX_BYTES`).

Measured on this host (macOS 26.6.2 arm64, Apple clang 21.0.0;
`test/perf/perfmeasure.sh`, peak RSS, 3 runs):

| Program | Peak RSS |
|---|---|
| `file.readbytes` of a 64 MiB file (the cap) | 605,536,256 B (577 MiB) |
| 64 × `file.readrange` 1 MiB windows over the same file | 543,719,424 B (518 MiB) |

The second row is the leak-forever model (ADR-0006) multiplying the first:
each 1 MiB window costs 8 MiB and is never returned.

### 4.2 The compact-buffer plan (story 135.18) — not landed

**Status: plan plus a measured prototype on branch `proto/cb-byte-compact`.
Not merged, and deliberately so.** The row asked for one coherent change or
none; the premise check below found the change is larger than the row
assumed, and two of its prerequisites are open defects in codegen.

#### What the premise check found

1. **The saving is real.** The prototype (below) measures `readbytes` at the
   cap at 135,757,824 B (129 MiB, down 4.5×) and the 64-window `readrange`
   loop at 73,236,480 B (70 MiB, down 7.4×), wall 0.08 s → 0.03 s. The row's
   "~576 MiB → ~128 MiB" is borne out.
2. **`[byte]` is not a distinct type anywhere the layout could key on.** The
   checker accepts every one of these, and all run correctly today:
   ```toke
   f=g(b:@byte):i64{<b.len};
   let a = @(97;0;98);            (* an [i64] literal ... *)
   g(a);                          (* ... passed as [byte] *)
   let y:@i64 = s.bytes("abc");   (* a [byte] bound as [i64] *)
   let x = @(1;2) + s.bytes("zz") (* mixed concatenation *)
   ```
   So codegen cannot choose an element width from the static type: the same
   block reaches `[i64]` and `[byte]` code. The width has to travel **with the
   block**, and every reader has to dispatch on it — or the type system has to
   separate the two (see option B).
3. **The consumer count is larger than recorded.** The row counted 79
   pack/unpack call sites in 10 glue modules. Measured now: **96 in 13**
   (`encrypt` 34, `crypto` 17, `encoding` 12, `image` 8, `csv` 7, `auth`,
   `file`, `ocr`, `str` 3 each, `ws`, `zip` 2, `pdf`, `xlsx` 1). Those are the
   *easy* ones — they go through the helpers. The hard ones read elements
   directly: **88 raw `[-1]`/element reads in 20 stdlib C files** that treat an
   array generically (`json_glue.c`, `csv_glue.c`, `template_glue.c`,
   `db_glue.c`, `vecstore_glue.c`, …), **20 generic array functions** in
   `collections_glue.c`/`str_glue.c`/`tk_runtime.c`, and the inline codegen
   sites in `src/llvm.c` (element read, literal construction, spread copy
   `len * 8`). Any one of them handed a compact block reads bytes as words
   and returns a plausible wrong answer — the prototype shows exactly that for
   `json.print`.
4. **Two prerequisite codegen defects** (found here; both reproduce on main
   with the current layout, so neither is caused by this plan):
   - `let b = s.bytes("AB"); let e = b + b;` lowers `+` as
     `llvm.sadd.with.overflow.i64` on the two **pointers**; `e.len` then reads
     a wild address (garbage length, RT003 with a random length, or SIGBUS
     across runs). With `let b:@byte = …` it concatenates correctly.
   - `b.slice(1;2)` on a `[byte]` lowers to `tk_str_slice_w`, a string slice
     over the array block — SIGSEGV. 127.45's array slice covers typed
     receivers only.
   Both mean codegen does not reliably know a `[byte]` is an array. A width
   dispatch built on top of that inherits every place it guesses wrong.
5. **Two producers declare `[byte]` and return a C string**:
   `chart.tojson` (`chart_glue.c:84`) and `http.streamnext`
   (`tk_web_glue.c:1251`). `.len` on either reads the 8 bytes before a
   `malloc`'d string (0 in the reproduction), and `str.frombytes` of the result
   is empty, so the JSON never reaches a toke caller. They must be fixed to
   return real `[byte]` before any layout change, or they become over-reads
   of a different shape.

#### Option A (recommended): width travels with the block

- **Encoding.** The top byte of the `cap` word is an element-width code:
  `0` = `i64` slots (every block that exists today, bit-for-bit unchanged),
  `1` = `u8`. `len` stays at `handle[-1]` as an element count, so `.len`
  needs no change anywhere. Caps stay below 2^56 elements. A `u8` block keeps
  the 3-word header and rounds its data up to a whole word.
- **Readers.** One accessor pair, `tk_arr_ld(h, i)` / `tk_arr_st(h, i, v)` in
  `tk_array.h`, and a rule: **nothing outside `tk_array.h` and `bytes_rt.h`
  indexes an array block with a raw pointer.** Codegen's inline element read
  becomes a width test and two loads (or a call; see cost).
- **Mixed operations widen.** `u8 + u8` stays `u8`; any other `+`, an
  `append`/`set` of a value outside 0..255, and every generic producer that
  cannot prove its output fits, produce `i64` slots. Values are never
  truncated: a `[byte]` that acquires a 300 simply becomes wide.
- **Literals stay wide.** `@(97;0;98)` remains an `i64` block and is still
  accepted wherever `[byte]` is, because `tk_bytes_unpack` reads both widths.
  Only `tk_bytes_pack` produces compact blocks, which is where every large
  `[byte]` comes from (files, network, crypto, codecs).
- **Cost.** One extra load and branch on every element read of every array.
  That must be measured by `perf-check` before it lands, not assumed.
- **The gate that makes it safe.** A `check-array-access` script that fails on
  any `(int64_t *)` element access or `[-1]`/`[-2]` read of an array handle
  outside `tk_array.h`/`bytes_rt.h`, with a dated skiplist that starts at the
  88 + 20 sites above and must reach zero before the producer flips. This is
  what turns "silent wrong answer" into "red gate", and it is the reason the
  change cannot be done piecemeal.

#### Option B: make `[byte]` a distinct static type

Give `[byte]` its own element width at compile time, with an explicit or
checker-inserted conversion wherever an `[i64]` flows into it and back. Codegen
then emits byte loads directly and no reader dispatches. It is cleaner and
cheaper at run time, but it is a **language change**: every example above
stops type-checking or changes meaning, corpus programs that treat bytes as
`i64` arrays need migration, and it needs a spec amendment story and
`types.c` work. Not recommended as the first step; it becomes cheap once A's
accessor discipline exists, because the dispatch can then be specialised away
where the type is known.

#### Landing order (one branch, merged together)

1. Fix the prerequisites: `[byte]` `+` and `.slice` lowering (item 4) and the
   two string-returning producers (item 5), each with its own conformance
   test. These are independent bug fixes and can merge on their own.
2. Add the accessors and the width code with **every producer still emitting
   width 0**. Behaviour identical, `make diff-codegen` 0 changes in stdout,
   `perf-check` measures the dispatch cost.
3. Convert every reader to the accessors; add `check-array-access`; drive its
   skiplist to zero.
4. Flip `tk_bytes_pack` to width 1. This is the only commit that changes the
   layout of any live block, so it can be reverted alone.
5. Conformance suite (C157 is reserved for it) exercising `.len`, `a[i]`,
   `.get`, `.set`, `append`, `+`, `slice`, `map`/`filter`/`reduce`/`sort`,
   `indexof`, `json.print`, interpolation, `str.bytes`/`str.frombytes` and one
   consumer per glue module, each at lengths 0, 1 and 2^20 and with the values
   0, 1 and 255, on both widths and on mixed pairs; ASan runs of the stdlib
   tests that move bytes; `perfmeasure.sh` before/after on the two programs
   in 4.1. Then, and only then, revisit the 64 MiB cap.

Steps 2–4 touch the element-read and literal paths of `src/llvm.c`, so they
need a window in which no other batch is editing those paths.

#### The prototype (branch `proto/cb-byte-compact`, not for merge)

Implements the encoding, `tk_arr_ld`/`tk_arr_st`/`tk_arr_alloc_u8`/`tk_arr_widen`,
compact `tk_bytes_pack`, width-aware `tk_bytes_unpack`, an out-of-line
`tk_arr_ld_w` for codegen's `a[i]`, and width dispatch in `append`,
`append_inplace`, `set`, `set_inplace`, `get`, `pop`, `slice`, `indexof`,
`map`/`filter`/`reduce`/`sort` and `tk_array_concat`. Nothing else is
converted. What it proves and what it does not:

- `test/standalone/test_bytes_crypto`, `test_encoding`, `test_encoding_full`,
  `test_crypto_full`, `test_127_38` and `test_str_join` all pass on it, as do
  mixed `[i64] + [byte]`, `@i64`-annotated `[byte]`, and an `[i64]` literal
  passed to a `[byte]` parameter.
- Memory as in item 1 above.
- `json.print` of a compact `[byte]` prints `[16961,…]` — two bytes read as
  one word — instead of `[65,66]`. That is the unconverted-reader failure
  mode, observed, and it is why step 3's gate precedes step 4.
- `make conform` was not run on the prototype: the machine-wide gate slots
  were held by the other batches for the whole window, and the slot went to
  this branch's own gate. The unconverted-reader inventory in item 3, not a
  conformance count, is the measure of what is left.

---

## 5. Struct Layout

Structs are emitted as flat `i64` arrays (not LLVM named struct types).
All fields are `i64`-typed, with non-integer values stored via pointer
casts.

**Allocation**: heap-allocated via `malloc`, eight bytes per field. The
local that names the struct is an `alloca` holding the *pointer*, not the
fields:
```llvm
%t0 = call i8* @malloc(i64 24)  ; struct with 3 fields
%p  = alloca i8*                ; the binding, pointing at %t0
```

Struct values are therefore safe to return and to outlive the frame that
built them — which is what lets an error record survive the return that
raised it (§7.3). (Corrected 127.97: this section previously described an
`alloca i64, i32 N` layout that the compiler has never emitted.)

**Field access**: via `getelementptr` with a compile-time field index
determined by `struct_field_index()`:
```llvm
; Write field at index 1:
%t1 = getelementptr inbounds i64, ptr %t0, i32 1
store i64 %val, ptr %t1

; Read field at index 1:
%t2 = getelementptr inbounds i64, ptr %t0, i32 1
%t3 = load i64, ptr %t2
```

The compiler maintains a `StructInfo` registry that maps field names to
indices. Field order matches declaration order in the source.

---

## 6. Map Layout

A map value is an `i8*` to a `TkMapImpl` (`src/stdlib/collections_glue.c`),
created by `tk_map_new` (str keys) or `tk_map_new_int` (i64 keys) and passed
through the i64 ABI like any other handle. Keys and values are i64-ABI words.

| Field | Meaning |
|-------|---------|
| `entries`, `len`, `cap` | dense `{key, val}` pairs in **insertion order** — a layout contract shared with `template_glue.c` and `tk_map_len_w`; do not reorder |
| `kind` | str or int keys, fixed at creation |
| `slots`, `nslots` | open-addressing index into `entries` (linear probing, load ≤ 0.7) |
| `hashes` | the hash of each entry's key |

The index hash is SipHash-1-3 under a per-process random key (121.32), so
colliding key sets cannot be precomputed; iteration walks `entries` and never
depends on it (semantics.md §10.2).

`tk_map_get` (`m.get(k)`) is a slot call (127.30): it stores `0` in
`@tk_current_error` on a hit and `1` (failed, no payload) on a miss, returning
the value or the `0` filler (§7.1–7.2). A match on it decides on the slot, so
a stored `0` is `$ok` (semantics.md §10.1).

---

## 7. Error Union Layout

Error unions (`T!Err`) are **split across two channels**: the ordinary
return register carries the success value, and a separate runtime slot
carries the error. There is no two-word return and no tagged value.

Stories: 114.41, 114.55, 124.0a, 127.56, 127.97, 127.193, 135.5b, 127.182, 127.103.

### 7.1 The two channels

| Channel | Symbol | Type | Carries |
|---------|--------|------|---------|
| Value | the function's own return register | `T` lowered per §2–§6 | the success value, or a zero/null filler on the error path |
| Error | `@tk_current_error` | `i64` | the discriminant **and** the payload |

A `T!Err` function lowers to a plain `T`-returning function. Nothing in
its LLVM signature records that it is fallible; the error type is known
statically at every call site from the declaration, and that is what the
consumer uses to decode the slot.

### 7.2 `@tk_current_error` is tri-valued

This single `i64` is both the discriminant and the payload pointer:

| Value | Meaning |
|-------|---------|
| `0` | **Success.** The value channel holds a real `T`. |
| `1` | **Failure with no payload.** The reason is not recoverable. |
| anything else | **Failure with a payload.** The value is a pointer to an error record (§7.3), cast through `ptrtoint`. |

`1` is reserved and is never a valid payload pointer, because every
payload is `malloc`'d. It is what the C glue stores (`analytics_glue.c`,
`image_glue.c`, …), what a `<$none{}` return stores (124.0a — `$none`
declares no fields, and storing its natural box value of `0` would read
back as success), and what `!` propagation stores when the callee it
propagated from used the older value-sentinel convention (127.56).

**The zero filler is not the discriminant.** An ok return of `0`, `0.0`,
`false`, `""` or an empty array is indistinguishable from the error
filler in the value channel, so consumers must decide on the slot, never
on the value. This is why every ok return from a fallible function
explicitly stores `0` into the slot (114.55) — the clear happens *after*
the return expression is evaluated, so a call inside that expression
cannot leave a stale error behind.

**The slot belongs to one call, so a binding captures it (135.16).** The
slot is live global state: by the time a consumer looks, any later
fallible call has overwritten it. So when a fallible call is bound —
`let r = file.size(p)` — the compiler spills the slot into a local of
that binding's own, taken immediately after the call returns, and a
later `mt r` decides on **that** copy. Without the capture the two
spellings of one call disagree: `mt file.size(p)` read the slot and
`let r = file.size(p); mt r` fell back to the value sentinel, so an
empty file's size of `0` was reported as a failure — silently, for
`str.toint`/`tofloat`, `toml.i64`/`bool`, `file.size` and every user
`T!$err` whose ok value can be `0`. Re-assigning the binding re-points
it at the new call's slot; a shadowing re-bind gets its own.

### 7.3 Error record layout

The payload is a heap record. It must not be an `alloca` — it may
outlive the frame that raised it.

**Ownership (127.109).** It is *not* `malloc`'d per error. Each thread owns
exactly one error-box buffer (`tk_err_box`, `tk_runtime.c`), **reused by every
raise on that thread**. A box is valid until the next raise on the same
thread, and so is the `$err` arm's binding of it — so there is nothing to free
per error. A `let`-bound fallible value is the one place a box must outlive
later raises, and it keeps its own copy (127.103, §7.5 item 1).

The buffer is an inline thread-local array sized for the common box (a 2-slot
sum, or a record of up to 8 fields), so the usual error return performs **no
allocation at all** and cannot fail. Only a wider error type reaches the heap;
that buffer is released at exit for the main thread, and a worker that raised
one leaves a single buffer at thread exit — constant, not
workload-proportional. `tk_err_box` never returns null, because a null box
would be stored into the slot as `0`, and `0` is success: an out-of-memory
error return would read back as a successful one.

Until 127.109 every error return and every `!` propagation called `malloc` and
the emitted code contained **no `free` at all**: 16 bytes per error ever
raised. `patterns/err-default` at N=64e6 allocated 9,142,858 boxes and
146 MB, with the free count constant in N. It is now 0 allocations for the
error channel — 32 for the whole process, the same as a program that raises
nothing — and 1.4 MB peak, constant in N. The idiom is at parity with the
precondition-guard form it used to lose to by five orders of magnitude
(127.49), on both allocations and wall time (0.12 s either way at N=64e6).

**Sum-typed errors** (`t=$err{$bad:$str;$worse:i64}`) are a 2-slot box:

```llvm
%box = call i8* @malloc(i64 16)
; slot 0: i64 tag     — the variant's declaration index
; slot 1: i64 payload — the variant's value, bitcast/ptrtoint to i64
```

The tag `-1` is reserved: it marks an error synthesised by `!`
propagation from a callee that carried no variant of its own, so a
nested `mt e {$variants}` reads a well-formed box and falls through to
its last arm rather than dereferencing a sentinel (127.56).

**Record-typed errors** (`t=$myerr{code:i64;msg:$str}`) are an ordinary
struct per §5 — a `malloc`'d flat `i64` array, one slot per field, in
declaration order.

**Stdlib errors (135.5b, 127.182).** A stdlib wrapper has no toke body, so
its payload is whatever its C glue stores, and the `$err` arm can bind it only
if the compiler knows that the glue stores the declared shape and never leaves
the slot stale. The wrappers for which that holds are listed in `llvm.c`'s
`is_stdlib_slot_wrapper()` (std.json scalars, std.toml, std.image); for them
the payload type is read from the `.tki` declaration and:

| Declared error | Slot on failure | `$err:e` binds |
|---|---|---|
| `T!str` (std.image) | the message pointer itself — never `0` or `1` | `e` as the `str` |
| a record, e.g. `tomlval!tomlerr` with `tomlerr{msg:str}` | a `tk_err_box` of the record's layout | `e` as that record (`e.msg`) |
| a sum not registered from a `.tki` (`jsonerr`, `fileerr`) | a `{tag, msg}` box | nil, as before |

Until 135.5b every stdlib `T!str` failure stored the payload-less flag `1`,
and the payload lookup consulted only user functions' signatures — so the
parser's and the decoder's own messages were reachable from the C tests and
invisible from toke, and `e.msg` on a toml error was E4034 (the record was
declared `"kind":"record"`, which nothing registered) or RT005.

`!` passes a stdlib payload through only into an error type of the same
layout — the same type, or a record whose fields have the same types in the
same order. Anything else is re-raised payload-less exactly as a
sentinel-ABI callee is (127.56): a tag `-1` box into a sum, `1` otherwise.
Passing it through blind would let a `{msg}` box be read as a sum's
`{tag, payload}`, or a `jsonerr {Missing, msg}` box select a user sum's third
variant.

### 7.4 Consuming an error union

`mt call() {$ok:v …;$err:e …}` lowers to a branch on the slot. The
`$ok` arm binds the value channel. The `$err` arm binds the slot, after
normalising the reserved `1` to nil — so an arm that reads a field of a
payload-less error takes a clean RT005 nil trap (§9) rather than
dereferencing the address `1`.

`expr!$Err` propagates: it branches on the same slot, and on the error
path re-raises into the caller's declared error type and returns the
zero filler.

### 7.5 Restrictions (as implemented)

These are limitations of the current lowering, not of the design:

1. ~~**The payload is bound only when the `mt` scrutinee is the call
   itself.**~~ **Fixed, 135.16 and 127.103.** 135.16 spilled the slot into a
   per-binding local at the `let`, so `let r = f(x); mt r {…}` takes the
   right arm. That was not yet a capture of the *payload*: the slot holds a
   pointer into the one per-thread box (127.109), and the next raise on the
   thread rewrote it, so `let a = g(0); let z = g(-1); mt a {…}` bound z's
   payload (`1099` for `77`, measured at `2e932d6`). The `let` now copies the
   box into a frame buffer the binding owns (`tk_err_keep`), and the `$err`
   arm copies it back into the thread box (`tk_err_rebox`), so the bound `e`
   has exactly the direct spelling's lifetime — valid until the next raise —
   and never points into a frame it could outlive. No allocation: the copy is
   an entry-block `alloca`. `test/conform/C084` pins it, including a
   re-assigned binding and one inside a loop.
2. ~~**The slot is process-wide, not thread-local.**~~ **Fixed, 127.101.**
   It is thread-local, as the compiler comments always claimed and this
   document previously denied. `tk_runtime.c` defines it `TK_TLS` and
   `llvm.c` emits `@tk_current_error = external thread_local global i64`.
   The two spellings are load-bearing: a plain-global declaration against a
   TLS definition links with **no diagnostic** and then takes SIGBUS on the
   first access, so every translation unit that declares the slot — the
   twelve glue files included — must use the macro. `TK_TLS`, not C11
   `_Thread_local`, because `tk_runtime.c` is built with `-std=c99
   -Wpedantic -Werror`, under which `_Thread_local` is a hard error.

   This was reachable, not theoretical: `task.c`'s `pool_worker` calls a
   toke function pointer (`t->result = t->fn()`) on a pool thread, so two
   `std.task` tasks raising errors really did overwrite each other's.
3. **Nothing enforces that every error exit sets the slot** (127.59).
4. **The declared error type is not checked against what is
   observable.** `mdnserr` declares five variants and no function returns
   it. (`fileerr` declared three while `file.lasterrkind()` distinguished
   ten; 127.102b widened it to the ten and retired the accessor.)

### 7.6 Why this representation

The obvious alternative is a two-word return — `{i64 tag, i64 payload}`
by value, or an `sret` out-parameter — which needs no global, is
re-entrant, and composes. It was **not** chosen here, because it changes
the LLVM signature of every fallible function: 132 fallible functions
across 33 stdlib modules, every C glue wrapper behind them, all 63
error-union declarations in ooke, and the 1,253 corpus records that
call them.
<!-- facts-exempt: 2026-09-20 the counts in the paragraph above are the tree as
     it stood when this decision was taken (114.41-era: 33 stdlib modules, 1,253
     corpus records). They size a decision already made, so re-deriving them to
     today's numbers would rewrite the record rather than correct it. Story
     132.36. --> That is a coordinated break of the whole tree, and the
compiler is the root of trust for the corpus (AGENTS.md §1).

The split-channel form, by contrast, is **already the ABI** — the return
path has stashed a typed box since 114.41. What was missing was only
that the consumer read it back, and a written definition of the slot's
three states. Both are supplied above.

Restriction 2 was named here as the trigger that would force the question.
127.101 answered it the cheaper way — a thread-local slot — so the trigger
is spent and the two-word return is **not** required for concurrent error
reporting. A caller holding two error payloads live at once — the case
that would otherwise have forced it — is now served by the `let` keeping its
own copy of the box (127.103), still without changing any signature.

---

## 8. Runtime Functions

All functions are declared in `tk_runtime.h` and linked from
`tk_runtime.c`.

| Function | IR Signature | Purpose | Allocation |
|----------|-------------|---------|------------|
| `tk_runtime_init` | `void (i32, ptr)` | Store argc/argv globals for later access | None |
| `tk_str_argv` | `ptr (i64)` | Return `argv[index]` as a C string | None (returns pointer into argv) |
| `tk_json_parse` | `i64 (ptr)` | Parse JSON string into a toke value | malloc for arrays and strings |
| `tk_json_print` | `void (i64)` | Print i64 value as JSON to stdout | None |
| `tk_json_print_i64` | `void (i64)` | Print i64 as decimal integer | None |
| `tk_json_print_bool` | `void (i64)` | Print i64 as `true`/`false` | None |
| `tk_json_print_str` | `void (ptr)` | Print C string as JSON quoted string | None |
| `tk_json_print_arr` | `void (ptr)` | Print length-prefixed i64 array as JSON | None |
| `tk_json_print_arr_bool` | `void (ptr)` | Print length-prefixed bool array as JSON | None |
| `tk_json_print_f64` | `void (double)` | Print f64 as decimal number | None |
| `tk_json_print_arr_str` | `void (ptr, i64)` | Print string array as JSON (takes data ptr + len) | None |
| `tk_array_concat` | `ptr (ptr, ptr)` | Concatenate two length-prefixed i64 arrays | malloc -- caller owns result |
| `tk_str_concat` | `ptr (ptr, ptr)` | Concatenate two C strings | sealed string heap (§3.1) -- never freed, never written |
| `tk_str_len` | `i64 (ptr)` | Return byte length of a C string (`0` for NULL); O(1) for a sealed string (§3.1) | None |
| `tk_str_cmp` | `i64 (ptr, ptr)` | NULL-safe three-way string compare; backs `==` `!=` `<` `>` `<=` `>=` on `$str`. NULL is the `?(T)` miss sentinel, is **not** equal to `""`, and orders before every string | None |
| `tk_str_char_at` | `i64 (ptr, i64)` | Return char code at index (unsigned byte) | None |
| `tk_overflow_trap` | `void (i32)` | Print RT002 diagnostic and exit(1) | None (terminates) |

---

## 9. Runtime Traps

The emitted code fails **loudly at the fault site** rather than corrupting memory
or invoking undefined behaviour. Each trap prints `RTNNN: …` to stderr and
`exit(1)`; the `unreachable` after the call lets `-O2` delete the guard where it
can prove the fault is impossible.

| Code | Condition | Runtime fn | Message |
|------|-----------|-----------|---------|
| RT002 | `i64` add/sub/mul overflow | `tk_overflow_trap` | `RT002: integer overflow in <op>` |
| RT003 | array/collection index out of bounds | `tk_bounds_trap` | `RT003: index N out of bounds for length M` |
| RT004 | divide-by-zero / `INT64_MIN / -1` | `tk_div_trap` | `RT004: <division|remainder> by zero` |
| RT005 | nil struct-field dereference | `tk_nil_trap` | `RT005: nil dereference` |
| RT008 | invalid regex pattern reached `str.containsre`/`countre`/`findall`/`matches`/`replacere` at run time (a literal one is E4075 at compile time; 137.9 — `str.checkre` returns the same error as a `regexerr` value instead, 121.45) | `regex_trap_invalid` (static, `str_glue.c`) | `RT008: invalid regex pattern in str.<fn>: <reason> (at byte N of "<pattern>")` |
| RT009 | a regex call spent its step budget (121.35; see [str.md](stdlib/str.md#regular-expressions)) | `regex_trap_budget` (static, `str_glue.c`) | `RT009: regex step budget exceeded in str.<fn> (pattern "<pattern>", subject N bytes)` |

(See [memory-model.md §6.6](spec/memory-model.md) for the normative spatial/
arithmetic-safety properties, and ADR-0012 for the design.)

### 9.1 Overflow (RT002)

Integer arithmetic on `i64` uses LLVM checked intrinsics:

| Operation | Intrinsic | op_code |
|-----------|-----------|---------|
| Addition  | `@llvm.sadd.with.overflow.i64` | 0 |
| Subtraction | `@llvm.ssub.with.overflow.i64` | 1 |
| Multiplication | `@llvm.smul.with.overflow.i64` | 2 |

**IR pattern**:
```llvm
%r = call {i64, i1} @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
%val = extractvalue {i64, i1} %r, 0
%ov  = extractvalue {i64, i1} %r, 1
br i1 %ov, label %ov_trap, label %ov_ok

ov_trap:
  call void @tk_overflow_trap(i32 0)
  unreachable

ov_ok:
  ; use %val
```

`tk_overflow_trap` prints to stderr and terminates:
```
RT002: integer overflow in addition
```

The process exits with code 1. The `unreachable` after the call allows
LLVM to optimize the non-overflow path.

---

## 10. Initialization

The emitted `main()` wrapper calls `tk_runtime_init(argc, argv)` before
any toke code executes.

`tk_runtime_init` stores `argc` and `argv` into file-scoped globals
(`g_argc`, `g_argv`) so that `tk_str_argv(index)` can access command-line
arguments at any point during execution.

- Out-of-bounds `tk_str_argv` calls return `""` (empty string, not NULL).
- `argv[0]` is the program name, `argv[1]` is typically the JSON input
  for benchmark programs.

`tk_runtime_init` also calls `tk_cap_init(argc, argv)` to initialise the
capability broker (see §12).

---

## 11. Injection Resistance

The runtime neutralises the injection classes closed in ADR-0011; these are
guarantees of the emitted/stdlib behaviour, not advice:

1. **Argv-only process exec.** `process.exec`/`spawn`/`spawndetached` never route
   through `sh -c`. The command string is tokenised on whitespace (quoted segments
   preserved) and `execvp`'d directly, so shell metacharacters (`;`, `|`, `$()`,
   backticks, `&&`) are treated literally — a command-injection payload runs no
   extra process.
2. **Context-aware HTML/template escaping (escape-by-default).** Interpolated
   values are HTML-escaped by default in both `tpl.render` (stdlib) and the ooke
   `{= =}` template engine; attribute values, and JS/CSS/`<title>` raw-text
   contexts, are escaped for their context; `md.render` runs with cmark's safe
   default (raw HTML in markdown source is neutralised). A `|raw` filter is the
   explicit, greppable opt-out for trusted HTML.
3. **URL-scheme neutralisation.** `javascript:`/`vbscript:`/`data:` URLs are dropped
   from `href`/`src`/`action`/… attributes.

(See [memory-model.md §6.7](spec/memory-model.md) and ADR-0011.)

---

## 12. At-Rest Crypto Envelope

`encrypt.seal`/`encrypt.open` wrap AEAD output in a self-describing, versioned,
algorithm-tagged envelope so ciphertext at rest is upgradeable without breaking
format:

```
[ 'T' 'K' 'E' | version(1) | alg(1) | nonce(12) | ciphertext | tag(16) ]
```

`open` dispatches on `(version, alg)` and rejects unknown/tampered values, so an
attacker cannot downgrade or confuse the format. `alg` 1 = AES-256-GCM, 2 =
ChaCha20-Poly1305 (both 256-bit AEADs). New algorithms (incl. PQC) are added as new
`alg` ids. TLS uses hybrid post-quantum key exchange (`X25519MLKEM768`, classical
fallback) and an opt-in ML-DSA-65 self-signed cert path. (See ADR-0013.)

---

## 13. Capability Broker

`tk_cap_init` (called from `tk_runtime_init`) computes the effective grant set from
the compiler-baked `@__tk_cap_baked_{grants,present,enforce}` globals unioned with
runtime `--allow-*` flags. Each fs/net/env/process sink calls `tk_cap_check`; a
denied call raises `CAP001` and exits before the syscall. Default mode is
`ALLOW_ALL` (no-op) until the deny-by-default flip. Full details in
[spec/capabilities.md](spec/capabilities.md) and ADR-0010.
