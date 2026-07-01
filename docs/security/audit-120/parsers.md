## 120.5 — Data-format parser family

**Scope:** `src/stdlib/{json,yaml,toml,xml,csv,toon,html,md,template,soap}.c` — the hand-rolled C parsers/encoders that toke/ooke expose to programs and that (in a web-framework deployment) are fed untrusted request bytes. **Method:** manual static review of every file, focused on OOB read/write, integer/length handling, unbounded recursion, XML entity expansion, and alloc/quadratic DoS. No build or dynamic tooling was run (parallel audits share this tree); dynamic proof items are listed at the end. Because these modules run with full ambient OS authority (no language sandbox), a memory-safety break here is a direct RCE/DoS surface.

Findings are ranked most-severe first. Reachability is tagged `remote-unauth` where the defect triggers on parser input that a network client typically controls; note this assumes ooke (separate repo) routes request data into these APIs — confirm the call sites during 120.22 fuzz-target wiring.

---

### PAR-01 — `yaml_from_json` / `yaml_to_json` write past a fixed/insufficient heap buffer
- **Severity:** High (potentially Critical) **Reachability:** remote-unauth **Confidence:** confirmed
- **File:** `src/stdlib/yaml.c:449` (alloc), `:377-461` (`json_to_yaml_r`), `:467-577` (`yaml_to_json`)

**Description.** Both converters size their output once: `int cap = (int)strlen(json) * 2 + 256; char *out = malloc(cap);` and then emit with `snprintf(out + pos, (size_t)(cap - pos), ...)`, accumulating `pos += snprintf(...)`. `snprintf` returns the number of bytes it *would* have written, so `pos` can advance beyond `cap` even though only `cap` bytes exist. Once `pos > cap`, `(cap - pos)` (both `int`) is negative and casts to a gigantic `size_t`, and `out + pos` already points past the allocation — the next `snprintf` performs an unbounded write off the end of the heap block. The trailing `out[pos] = '\0'` (yaml.c:459, :575) is itself an OOB write once `pos > cap`.

The 2× estimate is easy to exceed. `json_to_yaml_r` indents each nesting level with `%*s` where `indent` grows by 2 per level, so nested-object output grows ~quadratically (≈depth²) while input grows linearly (≈6·depth); object nesting of ~25 already exceeds `2*len+256`. `yaml_to_json` emits `"key":""` (~7 bytes) for each `key:`-only line (~3 input bytes), so ≈260 empty-value lines overflows `6N+256`.

**Impact.** Attacker-controlled heap buffer overflow (write) from a single conversion call on hostile JSON/YAML → memory corruption, likely RCE, at minimum crash.

**Fix.** Replace the fixed `snprintf`-accumulator with a growable buffer (as `csv.c`/`html.c` do), or before every append check `pos < cap` and grow with `realloc`. Never add a raw `snprintf` return to an offset without clamping to remaining capacity.

---

### PAR-02 — `toon_from_json` single-object path has a fixed 4096-byte buffer with no growth
- **Severity:** High (potentially Critical) **Reachability:** remote-unauth **Confidence:** confirmed
- **File:** `src/stdlib/toon.c:356-418`

**Description.** The single-object branch does `size_t cap = 4096; char *out = malloc(cap); int pos = 0;` and then emits keys and values entirely via `snprintf(out + pos, cap - (size_t)pos, ...)` with `pos += ...` — there is **no** `realloc` anywhere in this branch (unlike the array branch, which at least grows). A JSON object whose combined key+value text exceeds ~4096 bytes drives `pos` past `cap`; `cap - (size_t)pos` then underflows to a huge value and `out + pos` is past the block, so subsequent `snprintf`s and the final `out[pos] = '\0'` (toon.c:417) write out of bounds. The schema-line emission in the array branch (`toon.c:469-474`) has the same shape for a single very long key.

**Impact.** Attacker JSON of modest size (a few KB of keys/values) yields a heap overflow write during `toon.fromJson`. Memory corruption / crash.

**Fix.** Use the same grow-on-demand buffer pattern for the single-object path (and the schema line); check capacity before each `snprintf`.

---

### PAR-03 — JSON `skip_string` reads out of bounds on a trailing backslash
- **Severity:** High **Reachability:** remote-unauth **Confidence:** confirmed
- **File:** `src/stdlib/json.c:32-41` (`skip_string`); duplicated at `src/stdlib/yaml.c:308-317` (`json_skip_string`)

**Description.**
```c
static const char *skip_string(const char *p) {
    if (*p != '"') return NULL;
    p++;
    while (*p) {
        if (*p == '\\') { p += 2; continue; }   /* line 36 */
        if (*p == '"')  { return p + 1; }
        p++;
    }
    return NULL;
}
```
If a `\` is the last byte before the NUL terminator, `p += 2` skips over the `\` *and* the `\0`, landing one byte past the buffer; `while (*p)` then dereferences unallocated memory and keeps scanning until it happens to hit a `"` or `\0` in adjacent heap (or walks into an unmapped page → SIGSEGV). This helper backs `find_json_key`, `skip_object`, `json_keys`, `json_len`, `json_dec`, so it is reached by essentially every non-streaming JSON entry point. Trigger: `json_dec("{\"a\":\"x\\")` (object whose last string ends in a lone backslash). The `yaml.c` clone is reached by `yaml_from_json` on untrusted JSON.

**Impact.** Out-of-bounds read → crash/DoS, and the scanned bytes influence parse control flow. (The well-designed *streaming* parser `stream_parse_string`, json.c:993, bounds every step on `s->len` and is not affected — see Positive observations.)

**Fix.** Bound the escape skip: `if (*p == '\\') { p++; if (*p) p++; continue; }` so a trailing backslash cannot step past the NUL. Apply the identical fix to the yaml.c copy.

---

### PAR-04 — Unbounded recursion in the non-streaming JSON and YAML value skippers (stack-overflow DoS)
- **Severity:** High **Reachability:** remote-unauth **Confidence:** confirmed
- **File:** `src/stdlib/json.c:44-104` (`skip_value`→`skip_object`/`skip_array`); `src/stdlib/yaml.c:319-374` (`json_skip_*`) and `:377-442` (`json_to_yaml_r`)

**Description.** `skip_value` → `skip_object`/`skip_array` → `skip_value` recurse once per nesting level with no depth cap. Deeply nested input such as `[[[[…]]]]` (or `{"a":{"a":…}}`) drives C-stack depth proportional to nesting and overflows the stack (SIGSEGV). `json_dec` on untrusted JSON reaches this directly; `json_keys`, `json_len`, `json_at`, `json_arr` all call the same skippers. The yaml.c `json_skip_*` and the recursive `json_to_yaml_r` are the same class, reachable via `yaml_from_json`. Notably the JSON *streaming* parser already enforces `JSON_STREAM_MAX_DEPTH` (json.c:1163, :1177) — the non-streaming path simply lacks the equivalent guard.

**Impact.** A short hostile payload (a few KB of nested brackets) crashes the process. Trivial remote DoS.

**Fix.** Thread a depth counter through `skip_value`/`skip_object`/`skip_array` (and the yaml equivalents and `json_to_yaml_r`) and fail past a fixed bound (reuse `JSON_STREAM_MAX_DEPTH`).

---

### PAR-05 — `md_render` forces `CMARK_OPT_UNSAFE` and emits unsanitized link/href output (XSS)
- **Severity:** High **Reachability:** remote-unauth **Confidence:** confirmed
- **File:** `src/stdlib/md.c:318-319` (`CMARK_OPT_UNSAFE`), `:120-136` (`md_inline_cell` link handling)

**Description.** `md_render` always calls cmark with `int options = CMARK_OPT_UNSAFE;`, which disables cmark's raw-HTML and dangerous-URL filtering — so any `<script>…</script>` or `<img onerror=…>` in the Markdown is passed straight through into the returned HTML. Separately, the table-cell link renderer builds `<a href="…">` by escaping only `< > & "` (`mdb_esc`); it does **not** validate the URL scheme, so `[x](javascript:alert(1))` becomes `<a href="javascript:alert(1)">`. There is no post-render sanitizer in the module.

**Impact.** If ooke renders user-supplied Markdown (comments, profiles, wiki content) and serves the result, this is stored/reflected XSS in every consumer.

**Fix.** Default to safe cmark options (drop `CMARK_OPT_UNSAFE`; consider `CMARK_OPT_SAFE`/URL filtering) and expose "unsafe" only as an explicit opt-in; reject/relativize `javascript:`, `data:`, `vbscript:` schemes in the table link builder.

---

### PAR-06 — `toon_arr` trusts the declared `[count]` and returns uninitialized element pointers
- **Severity:** Medium **Reachability:** remote-unauth **Confidence:** confirmed
- **File:** `src/stdlib/toon.c:302-334`

**Description.** `parse_schema` reads the row count from the attacker-controlled `name[count]{…}` header (`toon.c:57`). In `toon_arr`, `if (rowcount == 0) rowcount = actual_rows;` — i.e. when the header declares a non-zero count, that value is trusted over the actual number of data lines. `arr` is `malloc(rowcount * sizeof(Toon))`, the fill loop runs `for (int row = 0; row < rowcount && *rp; row++)` (stops early at end of data), yet the result is returned with `r.ok.len = rowcount`. When the declared count exceeds the real rows (e.g. `data[100]{id}:\n1\n`), `arr[1..99].raw` are never initialized but are handed back as a length-100 array; the caller dereferences uninitialized/garbage pointers.

**Impact.** Use of uninitialized heap pointers → crash or, depending on heap contents, arbitrary read when the toke program iterates the array. Remote-triggerable via crafted TOON.

**Fix.** Ignore/clamp the declared count to the counted `actual_rows`, or zero-initialize `arr` and set `len` to the number actually filled.

---

### PAR-07 — TOON converters grow the buffer by a single doubling that can still be too small
- **Severity:** Medium **Reachability:** remote-unauth **Confidence:** likely
- **File:** `src/stdlib/toon.c:509-512`, `:526-529`, `:598-601`

**Description.** The growth guards use a one-shot `if (pos + need >= cap) { cap *= 2; out = realloc(out, cap); }`. A single `cap *= 2` is not guaranteed to satisfy `pos + need < cap` when a single field is larger than the current capacity (`need > cap`), so the following `snprintf` can still be handed an offset near/over the end of the block — reintroducing the PAR-01/PAR-02 overflow for large individual field values. The `realloc` return is also unchecked (NULL on failure clobbers `out`).

**Impact.** Heap overflow write for a single oversized field; NULL-deref on allocation failure.

**Fix.** Grow in a `while (pos + need + 1 > cap) cap *= 2;` loop, check the `realloc` result, and guard against `cap` overflow.

---

### PAR-08 — `md_inline_cell` recurses without a depth bound (stack-overflow DoS)
- **Severity:** Medium **Reachability:** remote-unauth **Confidence:** likely
- **File:** `src/stdlib/md.c:80-146` (self-calls at :101, :113, :130)

**Description.** The table-cell inline renderer recurses on nested emphasis/links (`**…**`, `*…*`, `[…]`). Depth is bounded only by the length of the cell, so a table cell containing deeply nested `**` emphasis (or bracketed spans) recurses roughly `len/4` deep and can exhaust the C stack on a large cell. Only reachable when the table pre-processor is engaged (a `|`-row followed by a separator row), which itself runs on user Markdown.

**Impact.** Remote DoS via a single crafted Markdown table cell.

**Fix.** Convert to an explicit bound/iteration, or cap recursion depth.

---

### PAR-09 — Recursion and path handling in the template engine (blocks, self-referential partials, layout LFI)
- **Severity:** Medium **Reachability:** remote-auth / local **Confidence:** likely
- **File:** `src/stdlib/template.c:340-495` (`parse_nodes`), `:795-813` (`NODE_PARTIAL`), `:1354-1372` (`tmpl_renderpage` layout path)

**Description.** (a) `parse_nodes` recurses per nested block tag and `render_nodes` recurses per block/partial with no depth limit; a partial that references itself (`{{>self}}` in partial `self`) recurses forever → stack overflow. (b) `tmpl_renderpage` builds the layout path as `templates_dir/layout_name.tkt` where `layout_name` comes from a `{! layout("…") !}` directive in the page file; a value like `../../../etc/passwd%00`-style traversal (`.tkt` suffix limits but does not eliminate this) reads files outside `templates_dir`. These inputs are normally developer-authored templates, hence lower reachability, but `tmpl_renderpage`/partials become dangerous if any template name or source is influenced by request data.

**Impact.** DoS (stack overflow) and local file disclosure if template/partial/layout names are attacker-influenced.

**Fix.** Add recursion/partial-expansion depth caps with cycle detection; reject `/`, `\`, and `..` in layout/partial names before path assembly.

---

### PAR-10 — Missing allocation NULL-checks across the parsers
- **Severity:** Low **Reachability:** local **Confidence:** confirmed
- **File:** e.g. `src/stdlib/json.c:304-305` (`malloc`+`memcpy` unchecked), `src/stdlib/csv.c:48-56` (`gbuf_ensure` unchecked `realloc`), `:101`, `:399`, `:407`; `src/stdlib/toon.c:74`, `:82`, `:324`; `src/stdlib/yaml.c:276`

**Description.** Numerous hot-path `malloc`/`realloc` results are used without NULL checks (`json_arr` copies each element with an unchecked `malloc`+`memcpy`; `csv.c`'s `gbuf_ensure` assigns `b->data = realloc(...)` and, on failure, leaves `cap` bumped so the next `gbuf_push`/`memcpy` writes through NULL/undersized). Under memory pressure these become NULL-deref crashes; the `csv.c` case additionally risks a short-buffer write if `realloc` fails after `cap` was already increased.

**Impact.** DoS on allocation failure; borderline corruption in the csv `gbuf_ensure` failure path.

**Fix.** Check every allocation and fail the operation cleanly; in `gbuf_ensure` do not update `cap` until the `realloc` succeeds.

---

### PAR-11 — `soap_fault` embeds the `detail` argument without XML-escaping
- **Severity:** Low **Reachability:** remote-auth **Confidence:** confirmed
- **File:** `src/stdlib/soap.c:170-173`

**Description.** `soap_fault` XML-escapes `faultcode` and `faultstring` (via `xml_escape`) but emits `detail` raw: `snprintf(..., "  <detail>%s</detail>\n", detail)`. If `detail` carries attacker-influenced content it can inject arbitrary XML into the fault envelope (structure break / element injection). This is not a memory-safety issue (the buffer is sized from `strlen(detail)` and `snprintf` is bounded) but it is an injection/correctness gap inconsistent with the other fields.

**Fix.** Escape `detail` with `xml_escape` like the other fields (or document that callers must pass pre-formed XML).

---

### Dynamic-testing follow-up (feeds 120.22)

Priority fuzz targets, roughly in order of expected yield:

1. **`json_dec` + the non-streaming accessors** (`json_str`, `json_keys`, `json_len`, `json_at`, `json_arr`) under ASAN — confirms PAR-03 (trailing-backslash OOB read) and PAR-04 (nesting stack overflow). Compare against the streaming parser, which should survive.
2. **`yaml_from_json` and `yaml_to_json`** under ASAN with nested/duplicated-key and empty-value corpora — confirms PAR-01 heap overflow write; watch for the `pos > cap` transition.
3. **`toon_from_json` (single object, >4 KB) and `toon_to_json`/`toon_arr`** under ASAN — confirms PAR-02, PAR-06 (MSan for the uninitialized-pointer path), PAR-07.
4. **`md_render`** with an HTML/JS injection corpus — confirms PAR-05 (assert output contains no unescaped `<script>`/`javascript:`); table-cell nesting corpus for PAR-08 stack depth.
5. **`xml_parse` / `xml_attr`** under ASAN with deeply nested, malformed, and unterminated CDATA/comment inputs — the review found the bounds guards correct, but fuzz to confirm and to exercise the `MAX_DEPTH`-overflow tag-stack accounting.
6. **`toml_load`** — this module is a thin FFI wrapper over vendored `tomlc99` (`stdlib/vendor/tomlc99`); fuzz the vendored parser separately, as the wrapper adds no parsing of its own.

ASAN + a memory allocator that pages out (or ASAN's `poison`) is needed to make the `pos > cap` write in PAR-01/PAR-02 fault deterministically; plain runs may silently corrupt the heap.

### Positive observations (defenses already correct)

- **JSON streaming parser** (`json.c:993-1318`) bounds every read on `s->len`, enforces `JSON_STREAM_MAX_DEPTH` (:1163, :1177), and caps numeric literals at 64 bytes (:1080-1082). It is the model the non-streaming path should follow.
- **No XML entity/DTD expansion.** `xml.c`'s `xml_unescape` (:200) only resolves the five predefined entities and every branch is length-checked before dereference; DOCTYPE/`<!…>` blocks are skipped wholesale (:344), so there is **no** custom-entity or external-entity machinery — "billion laughs" and classic XXE do not apply. XML depth uses an explicit iterative stack with `MAX_DEPTH` 256 (`xml.c:234-247`), avoiding parser recursion.
- **XML/HTML/SOAP escapers** (`xml_escape`, `html_escape`, `tmpl_escape`, soap `xml_escape`) all two-pass size-then-fill with correct worst-case multipliers, so the escapers themselves do not overflow.
- **CSV parser** is fully iterative with a growable field buffer and correct RFC-4180 quote handling; no recursion, no fixed output buffer (aside from the OOM gap noted in PAR-10).
- **`html.c`** is a builder, not a parser; it explicitly documents (file header) that content is not auto-escaped and callers must use `html_escape`. Its buffer routines grow correctly. (Callers passing untrusted data without escaping remain an XSS surface, but that is a call-site concern, not a defect in this file.)
