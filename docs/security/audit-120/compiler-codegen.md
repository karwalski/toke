## 120.4 — Compiler IR / codegen memory safety

Scope: memory-safety review of the toke compiler's IR/codegen and adjacent text-processing
modules — `src/tkir.c` (binary .tkir reader/writer), `src/ir.c` (.tki interface emitter),
`src/llvm.c` (codegen string handling only; the LLVM toolchain interface is covered by 120.2),
`src/glue_gen.c` (stdlib wrapper generation from .tki), `src/companion.c` (.tkc.md companion
files) and `src/compress.c` (prose/JSON/CSV compress + decompress). Method: manual static read
of the full in-scope source plus targeted grep for buffer-growth, integer-overflow, and
codegen string patterns, with call-site tracing through `src/main.c`. No build/compile was run
(shared working tree). Findings below are grounded in specific lines that were read.

The headline defect is a heap buffer overflow in `decompress_text()`: the documented and
implemented output-buffer sizing rule (`len*4+64`) is provably too small because dictionary
back-references can expand a compressed stream by up to ~42x. The binary .tkir reader
(`tkir.c`), by contrast, is well bounds-checked and is currently reachable only from tests.

---

### COM-01 — `decompress_text()` output can exceed the caller's `len*4` buffer (heap overflow)

- Severity: High
- Reachability: local (`tkc --decompress` on attacker-supplied stdin / .tkc content; would be remote if the streaming/loke pipeline that the module documents is wired to network input)
- File:line: `src/compress.c:355` (function), `src/compress.c:400-405` (back-ref emit), `src/compress.c:443-455` (dictionary rebuild); caller `src/main.c:550` and `src/main.c:565`; contract `src/compress.h:71-77`
- Confidence: confirmed (deterministic size arithmetic)

Description. `decompress_text()` writes into a caller-supplied `out_buf` with **no size
parameter and no bounds checks on any write**. The API contract (compress.h:71-77) tells callers
`out_buf` "must be at least (len * 4 + 64) bytes to accommodate worst-case expansion", and the
only production caller sizes the buffer exactly that way:

```
src/main.c:550   size_t out_size = ilen * 4 + 128;
src/main.c:565   written = decompress_text(ibuf ? ibuf : "", ilen, obuf);
```

The `len*4` assumption is false. During decompression a verbatim word is appended to a rebuilt
dictionary (compress.c:443-455) with a stored length up to `MAX_WORD_LEN` (128). A subsequent
back-reference token `@Rn` — as few as **3 input bytes** (`@R1`) — is expanded to the full
dictionary word, up to **128 output bytes** (compress.c:400-405), an expansion of ~42x, far
beyond 4x. Every expansion is an unchecked `memcpy(out_buf + out, w, wlen)`.

Concrete overflow. Feed `tkc --decompress` the bytes:
`"TK:P"` + one 128-byte alphabetic word + N repetitions of `"@R1"`.
- input length `ilen = 4 + 128 + 3N`; buffer allocated `= ilen*4 + 128 = 656 + 12N`.
- output produced `= 128 + 128N (+1 NUL)`.
For N ≥ 5 the output exceeds the allocation, and the overrun grows by ~116 bytes per extra
`@R1`. The overflowed bytes are attacker-controlled (the dictionary word content), and the
overflow length is attacker-controlled — a classic, shapeable heap overflow.

Impact. Heap buffer overflow past a `malloc`'d allocation with attacker-controlled content and
size, in an AOT binary that runs with full ambient OS authority (no language sandbox).
Corrupts adjacent heap metadata/allocations; realistically escalates to control-flow hijack /
RCE, or at minimum a crash (DoS). The same unbounded-write pattern also applies to the
abbreviation (`~t`→`the`) and placeholder paths, but back-references are the amplifier that
breaks the 4x contract.

Recommended fix. Give `decompress_text()` (and the sibling `compress_*` emitters, which share
the no-size-parameter design) an explicit `size_t out_cap` argument and bounds-check **every**
write, returning -1 on overflow. Do not rely on a fixed expansion factor: back-reference
expansion relative to compressed size is effectively unbounded, so a correct fixed pre-size is
not derivable — the buffer must be checked at each append (or grown dynamically). As an
immediate mitigation in `main.c`, size the decompress buffer at `MAX_WORD_LEN * ilen + 64`
(worst case 3 input bytes → 128 output bytes ⇒ factor ~43), but the real fix is bounds-checked
writes.

---

### COM-02 — `compress_stream_feed()` infinite loop on a token larger than the pending buffer (DoS)

- Severity: Medium
- Reachability: local (`tkc --compress-stream` on stdin containing a >4094-byte run with no whitespace/newline)
- File:line: `src/compress.c:857-917` (loop), specifically the "hold incomplete token" breaks at `src/compress.c:876`, `src/compress.c:898`; fill guard `src/compress.c:859`; caller `src/main.c:532-544`
- Confidence: confirmed

Description. The streaming compressor accumulates input into a fixed `pending[COMPRESS_STREAM_BUFSZ]`
(4096) buffer. The fill loop only tops up while `pending_len < COMPRESS_STREAM_BUFSZ - 1`
(compress.c:859). When a single word (`isalpha` run) or placeholder spans the entire buffer and
more input remains, the token processor deliberately holds the incomplete token and breaks:

```
src/compress.c:898   if (consumed + (int)wl >= plen && ci < len) break;   /* word */
src/compress.c:876   if (pos + pl > (size_t)plen && ci < len) break;      /* placeholder */
```

With `pending_len == 4095`, `consumed` stays 0 (nothing is emitted), `remaining == plen`, and
the fill loop cannot add bytes (buffer full). The outer `while (ci < len || pending_len > 0)`
condition remains true because `ci < len`, so the function spins forever making no progress —
never emitting, never consuming, never terminating.

`main.c` reaches this by splitting stdin on `'\n'` and feeding each line as one chunk
(main.c:537-543); a single line longer than ~4094 bytes with no interior whitespace (e.g. a long
URL, base64 blob, or JWT) triggers the hang.

Impact. Unbounded CPU spin / denial of service; the process must be killed. No memory
corruption, but it is a hard hang rather than merely slow.

Recommended fix. When the pending buffer is full and no token was consumed in a pass, force
progress: flush the buffered bytes as a verbatim (uncompressible) token instead of holding them,
or cap the maximum token length and emit an oversize token verbatim. Ensure the loop cannot
iterate without either consuming input or emitting output.

---

### COM-03 — Unvalidated `ftell()` result before `malloc`/`fread` in companion file readers

- Severity: Low
- Reachability: local (`tkc --verify-companion <f>` / `--companion-diff <f>`)
- File:line: `src/companion.c:606-620` (verify_companion) and `src/companion.c:903-914` (companion diff)
- Confidence: likely (edge-case dependent)

Description. Both readers do `fseek(SEEK_END); long clen = ftell(cf); rewind(); malloc(clen+1);
fread(cbuf,1,clen,cf)` with no check that `clen > 0`. If `ftell()` returns -1 (e.g. the path is
a directory that `fopen(...,"rb")` accepted, or a non-seekable/erroring stream), `(size_t)clen`
becomes `SIZE_MAX`, `malloc((size_t)clen + 1)` becomes `malloc(0)`, and the subsequent
`fread(cbuf, 1, (size_t)clen, cf)` requests up to `SIZE_MAX` bytes into a zero-length buffer.
The comparable loader in `glue_gen.c:243` already guards this (`if (sz <= 0 || sz > 1000000)`),
so the fix pattern exists in-tree.

Impact. Under the error/edge conditions above, a bogus length feeds `malloc`/`fread`; worst case
is an out-of-bounds write from `fread` into a 0-byte buffer, more commonly a benign failure. Low
severity because the trigger requires an unusual file object and the reader is a local,
operator-invoked verification path.

Recommended fix. After `ftell`, reject `clen < 0` (and optionally impose an upper bound as
glue_gen.c does) before allocating; treat as a read error.

---

### COM-04 — Signed-int accumulation and dictionary truncation in `decompress_text` (robustness)

- Severity: Info
- Reachability: local
- File:line: `src/compress.c:397-399` (index accumulation), `src/compress.c:449-451` vs `src/compress.c:336-337` (dictionary truncation asymmetry)
- Confidence: confirmed (behavioral), not independently memory-unsafe

Description. The back-reference index is accumulated as `idx = idx * 10 + (input[k]-'0')`
(compress.c:397-399) with no digit cap; a long digit run causes signed-int overflow (UB),
though the result is subsequently constrained by `idx >= 1 && idx <= dict_count` (≤256), so it
does not itself produce an OOB access. Separately, words at/over `MAX_WORD_LEN` are stored
truncated in the dictionary on both compress (compress.c:336-337) and decompress
(compress.c:449-451) sides, which silently breaks the documented byte-identical round-trip for
long words — a correctness bug that also contributes to the COM-01 expansion surprise.

Recommended fix. Parse the index into an unsigned type with an explicit digit/`value` cap and
reject overflow; document (or lift) the `MAX_WORD_LEN` round-trip limitation. Fold the memory
half into the COM-01 fix.

---

### Positive observations (defenses already correct)

- `src/tkir.c` binary reader is consistently bounds-checked: all cursor reads guard against
  `c->len` (`cur_read_u8/u16/u32/bytes/str` at compress-safe checks, tkir.c:791-839); section
  lengths are validated against the file size (`c.pos + sec_len > len`, tkir.c:1364); per-entry
  sizes are re-checked against section end (data section, tkir.c:1196); `func_idx` and every
  register operand are range-checked (code section, tkir.c:1036, tkir.c:1081-1093); length-prefixed
  strings are clamped to their destination buffers (tkir.c:834). It is also currently reachable
  only from `test/tkir/test_tkir_reader.c`, not from the production CLI, limiting exposure.
- `src/glue_gen.c` clamps every `memcpy` length to the destination buffer size before copying
  (glue_gen.c:280-281, 289-290, 304-305, 327-328), caps input .tki files at 1 MB and rejects
  `sz <= 0` (glue_gen.c:243), and uses `vsnprintf`-sized dynamic growth (glue_gen.c:114-126).
- `src/ir.c` uses `snprintf`/clamped `memcpy` (`tok_copy`, ir.c:154-160) and a proper JSON string
  escaper (`json_str`, ir.c:98-110); it streams output to `FILE*` rather than a fixed buffer.
- `src/companion.c` clamps all name/signature copies to their fixed buffers
  (companion.c:749-771) and range-limits path construction (companion.c:647-654).
- `src/llvm.c` name-mangling `strcpy` calls all copy the constant "tk_main" (8 bytes) into
  ≥128-byte token buffers that were themselves filled via bounds-aware `tok_cp`
  (e.g. llvm.c:559-566, 4699-4702, 4977-4978, 5929-5931, 6319-6321); no overflow.

### Dynamic-testing follow-up

- COM-01: build `tkc` with AddressSanitizer and run `printf 'TK:P%.0saaaa…(128 a)…' ; ` piping
  `TK:P` + 128-byte word + many `@R1` into `tkc --decompress` to observe the heap overflow;
  then fuzz `decompress_text` (libFuzzer/AFL++) with a corpus of `TK:P`-prefixed inputs to
  confirm the crash surface and search for other expansion paths.
- COM-02: run `python -c 'print("A"*100000)' | tkc --compress-stream` under a wall-clock timeout
  to confirm the non-terminating spin.
- COM-03: run `tkc --verify-companion <a-directory>` under ASAN to see whether the platform's
  `fopen("rb")`/`ftell` combination yields the -1 path.
- General: fuzz `compress_json` / `compress_csv` output sizing against their `>= len + 64`
  contract to confirm no analogous under-sizing exists (not found by static review, but the
  no-output-cap design warrants dynamic confirmation).
