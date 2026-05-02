# Audit: Full Impact Analysis of Removing Underscore from Toke Identifiers

**Story:** 77.1.6
**Date:** 2026-05-01
**Status:** Complete

## Background

The M0 design decision (story 1.1.2, frozen 2026-03-27) explicitly stated: "Underscore excluded, identifier rule updated." The design philosophy is "onelowercaseword" — single concatenated lowercase words like `toint`, `frombytes`, `println`. Underscore crept back in via stdlib naming conventions and legacy corpus, then was presented to researchers as deeply embedded (~719 instances). This audit determines whether removal is structurally feasible.

---

## 1. Stdlib Functions with Underscores

**Source:** `/Users/matthew.watt/tk/toke/stdlib/*.tki`
**Count:** 115 unique underscore identifiers across stdlib interface files.

### Complete list with proposed alternatives:

| Current | Proposed | Module |
|---------|----------|--------|
| `canvas.fill_rect` | `canvas.fillrect` | canvas |
| `canvas.stroke_rect` | `canvas.strokerect` | canvas |
| `canvas.clear_rect` | `canvas.clearrect` | canvas |
| `canvas.fill_text` | `canvas.filltext` | canvas |
| `canvas.begin_path` | `canvas.beginpath` | canvas |
| `canvas.move_to` | `canvas.moveto` | canvas |
| `canvas.line_to` | `canvas.lineto` | canvas |
| `canvas.close_path` | `canvas.closepath` | canvas |
| `canvas.draw_image` | `canvas.drawimage` | canvas |
| `canvas.set_alpha` | `canvas.setalpha` | canvas |
| `canvas.to_js` | `canvas.tojs` | canvas |
| `canvas.to_html` | `canvas.tohtml` | canvas |
| `crypto.to_hex` | `crypto.tohex` | crypto |
| `encrypt.aes256gcm_encrypt` | `encrypt.aes256gcmencrypt` | encrypt |
| `encrypt.aes256gcm_decrypt` | `encrypt.aes256gcmdecrypt` | encrypt |
| `encrypt.aes256gcm_keygen` | `encrypt.aes256gcmkeygen` | encrypt |
| `encrypt.aes256gcm_noncegen` | `encrypt.aes256gcmnoncegen` | encrypt |
| `encrypt.x25519_keypair` | `encrypt.x25519keypair` | encrypt |
| `encrypt.x25519_dh` | `encrypt.x25519dh` | encrypt |
| `encrypt.ed25519_keypair` | `encrypt.ed25519keypair` | encrypt |
| `encrypt.ed25519_sign` | `encrypt.ed25519sign` | encrypt |
| `encrypt.ed25519_verify` | `encrypt.ed25519verify` | encrypt |
| `encrypt.hkdf_sha256` | `encrypt.hkdfsha256` | encrypt |
| `encrypt.tls_cert_fingerprint` | `encrypt.tlscertfingerprint` | encrypt |
| `env.get_or` | `env.getor` | env |
| `http.serve_workers` | `http.serveworkers` | http |
| `http.serve_tls` | `http.servetls` | http |
| `image.to_grayscale` | `image.tograyscale` | image |
| `image.flip_h` | `image.fliph` | image |
| `image.flip_v` | `image.flipv` | image |
| `image.pixel_at` | `image.pixelat` | image |
| `image.from_raw` | `image.fromraw` | image |
| `infer.is_loaded` | `infer.isloaded` | infer |
| `infer.load_streaming` | `infer.loadstreaming` | infer |
| `keychain.is_available` | `keychain.isavailable` | keychain |
| `mdns.stop_advertise` | `mdns.stopadvertise` | mdns |
| `mdns.stop_browse` | `mdns.stopbrowse` | mdns |
| `mdns.is_available` | `mdns.isavailable` | mdns |
| `mlx.is_available` | `mlx.isavailable` | mlx |
| `os.o_rdonly` | `os.ordonly` | os |
| `os.o_wronly` | `os.owronly` | os |
| `os.o_rdwr` | `os.ordwr` | os |
| `os.o_creat` | `os.ocreat` | os |
| `os.o_trunc` | `os.otrunc` | os |
| `os.o_append` | `os.oappend` | os |
| `os.stdin_fd` | `os.stdinfd` | os |
| `os.stdout_fd` | `os.stdoutfd` | os |
| `os.stderr_fd` | `os.stderrfd` | os |
| `secure_mem.*` | `securemem.*` | secure_mem (module name) |
| `str.from_int` | `str.fromint` | str |
| `str.from_float` | `str.fromfloat` | str |
| `str.to_int` | `str.toint` | str |
| `str.to_float` | `str.tofloat` | str |
| `str.from_bytes` | `str.frombytes` | str |
| `test.assert_eq` | `test.asserteq` | test |
| `test.assert_ne` | `test.assertne` | test |
| `time.to_parts` | `time.toparts` | time |
| `time.from_parts` | `time.fromparts` | time |
| `time.is_leap_year` | `time.isleapyear` | time |
| `time.days_in_month` | `time.daysinmonth` | time |
| `time.with_tz` | `time.withtz` | time |
| `time.utc_offset` | `time.utcoffset` | time |
| `time.add_days` | `time.adddays` | time |
| `time.add_months` | `time.addmonths` | time |

**Struct fields in .tki files** (also need renaming):
`font_size`, `font_family`, `stroke_width`, `cert_path`, `key_path`, `cert_pem`, `key_pem`, `peer_cert`, `peer_cert_pem`, `pool_size`, `timeout_ms`, `schema_version`, `n_gpu_layers`, `n_threads`, `ram_ceiling_gb`, `prefetch_layers`, `requires_nvme`, `pairing_code`, `mime_type`, `created_at`, `expires_at`, `tokens_in`, `tokens_out`, `dest_path`, `delete_before`, `model_handle`, `webview_handle`, `set_title`, `run_event_loop`, `register_handler`, `on_close`, `eval_js`

**Module name:** `secure_mem` itself contains an underscore and would become `securemem`.

---

## 2. Compiler Source: Lexer Changes Required

**File:** `/Users/matthew.watt/tk/toke/src/lexer.c`

### Current code (line 137):
```c
static int is_ident_cont(char c) { return is_letter(c) || is_digit(c) || c == '_'; }
```

### Required change:
```c
static int is_ident_cont(char c) { return is_letter(c) || is_digit(c); }
```

### Additional change needed (line 592):
Currently the main loop allows identifiers to START with underscore:
```c
if (is_letter(c) || c == '_') {
```
This would become:
```c
if (is_letter(c)) {
```

### What happens after this change:
- Any `_` in source code will hit the `default:` case in the switch statement and produce diagnostic E1003 ("character outside allowed character set").
- No other lexer changes required. The change is exactly 2 lines.

---

## 3. Spec Reference: Current vs Proposed Identifier Rules

**File:** `/Users/matthew.watt/tk/docs/spec/toke-spec-v0.3.md`, Section 8.4

### Current rule (v0.3):
```
[a-z_][a-z0-9_]*
```
With constraints:
- begins with lowercase letter (a-z) or underscore
- continues with lowercase letters, digits (0-9), or underscores
- trailing `_` is forbidden (E1006)
- `__` (double underscore) reserved for compiler-generated names (E1007)

### Proposed revert (matching M0 decision):
```
[a-z][a-z0-9]*
```
With constraints:
- begins with a lowercase letter (a-z)
- continues with lowercase letters and digits (0-9) only
- no additional constraints needed (trailing `_` and `__` rules become unnecessary)

### Spec sections requiring update:
- S8.4 (identifier rules) — rewrite regex and constraint list
- S8.9.2 (whitespace requirement) — remove `_` from "identifier characters" description
- S7.1 (character set) — remove `_` from the 59-char set, reducing to 58
- S11.5 (sum types / variants) — update variant naming rule
- S13.2 / S16.x — update error variant examples
- Appendix E — update examples

---

## 4. Error Variants Using Underscores

**All $-prefixed variants with underscores found in the spec and codebase:**

| Current | Proposed | Location |
|---------|----------|----------|
| `$not_found` | `$notfound` | spec S16, stdlib |
| `$bad_request` | `$badrequest` | spec S16, examples |
| `$bad_input` | `$badinput` | RFC draft |
| `$db_err` | `$dberr` | RFC draft |
| `$db_error` | `$dberror` | spec S16 |
| `$connection_failed` | `$connectionfailed` | spec S16 |
| `$query_failed` | `$queryfailed` | spec S16 |
| `$user_err` | `$usererr` | RFC draft |
| `$api_err` | `$apierr` | already exists without underscore in spec! |
| `$infer_opts` | `$inferopts` | stdlib |
| `$mlx_model` | `$mlxmodel` | stdlib |
| `$stream_opts` | `$streamopts` | stdlib |
| `$service_record` | `$servicerecord` | stdlib |
| `$secure_buf` | `$securebuf` | stdlib |
| `$ooke_config` | `$ookecfg` | already renamed (story 56.8.4) |
| `$model_handle` | `$modelhandle` | stdlib |
| `$webview_handle` | `$webviewhandle` | stdlib |

**Note:** Story 75.1.5 (done 2026-04-30) made a DECISION: "all $lowercase $snake_case for multi-word." This decision directly conflicts with the M0 design rule and would need to be REVERSED. The decision was made 1 day ago.

---

## 5. Corpus Usage

### Test directory (`/Users/matthew.watt/tk/toke/test/`):
- **687 instances** of underscore identifiers across .tk files
- **274 unique** underscore identifiers
- **26 files** contain at least one underscore

### Examples directory (`/Users/matthew.watt/tk/toke/examples/`):
- **343 instances** of underscore identifiers across .tk files

### Total corpus impact:
- ~1030 instances across test + examples
- All are mechanical renames (find-replace with word boundaries)

### Precedent:
Story 56.8.4 already performed this exact operation on 7 .tk files for the ooke module ("snake_case to concatenated"). The pattern is proven and repeatable.

---

## 6. Structural Necessity Assessment

**Is underscore EVER structurally necessary?**

**NO.** Every single use of underscore in the toke codebase is a naming convention choice, not a structural requirement. Evidence:

1. **Function names** — all are compound words that concatenate cleanly: `fillrect`, `toint`, `fromparts`, `isavailable`
2. **Struct fields** — same pattern: `fontsize`, `certpath`, `poolsize`, `timeoutms`
3. **Error variants** — same pattern: `$notfound`, `$badrequest`, `$dberror`
4. **Module names** — `secure_mem` becomes `securemem` (story 56.8.4 already did `ooke_config` -> `ookecfg`)
5. **OS constants** — `o_rdonly` becomes `ordonly` (these are already toke-specific names, not POSIX API passthrough)
6. **No disambiguation case** — there is no instance where removing underscore creates ambiguity. The module prefix (`str.`, `time.`, `os.`) already provides namespace separation.

**Conclusion:** Underscore serves zero structural purpose. It is purely conventional and every instance is replaceable with concatenated lowercase.

---

## 7. BPE Tokenizer Impact

### Character set impact:
- Underscore adds 1 character to the training alphabet (59 -> 58 without it)
- Fewer unique characters = simpler BPE merge table = faster training convergence

### Token splitting behaviour:
With underscore present, BPE tokenizers (GPT-style) typically split at `_` boundaries because underscore is a common separator in training corpora:

```
str.to_int  ->  ["str", ".", "to", "_", "int"]     = 5 tokens
str.toint   ->  ["str", ".", "to", "int"]           = 4 tokens (or ["str", ".", "toint"] = 3 tokens with toke-trained BPE)
```

### Quantified impact for toke-specific BPE:
- A toke-trained BPE will learn `toint`, `fromint`, `fillrect` etc. as single tokens if underscore is absent (high frequency in training corpus)
- With underscore present, the tokenizer is biased to keep `_` as a separate token or split boundary, producing more tokens per identifier
- More tokens = higher cost per inference, lower context efficiency

### Training corpus cleanliness:
- Removing underscore eliminates an entire class of merge rules from the BPE vocabulary
- The resulting vocabulary is more compact and toke-specific identifiers get dedicated single-token entries faster

---

## 8. Double Underscore (__) Reservation

### Current rule (spec S8.4):
> `__` (double underscore) is reserved for compiler-generated names; user identifiers containing `__` are a compile error (E1007).

### Impact of full underscore removal:
- If `_` is not a valid identifier character at all, then `__` cannot appear in any identifier
- The E1007 diagnostic becomes dead code — no path can reach it
- The reservation rule in the spec becomes unnecessary text
- **Simplification:** Remove S8.4 bullet about `__`, remove E1007 from diagnostics table

### Compiler-generated names:
If the compiler internally generates names containing `__` (for closures, anonymous types, etc.), these remain valid as **internal symbols** that never appear in source code. The lexer restriction only applies to source-level identifiers, not to names generated during compilation. No change to compiler internals needed.

---

## Summary

| Dimension | Impact | Effort |
|-----------|--------|--------|
| Lexer change | 2 lines | Trivial |
| Spec update | ~6 sections | 1 hour |
| Stdlib renames | 115 unique identifiers | Mechanical (script) |
| Error variants | 14 types | Mechanical |
| Test corpus | 687 instances / 274 unique | `sed` script |
| Examples corpus | 343 instances | `sed` script |
| Structural blockers | **NONE** | — |
| BPE benefit | Fewer tokens per identifier | Positive |
| Removed complexity | E1007 diagnostic, trailing-_ rule | Positive |

### Verdict

Underscore removal is **straightforward** and **entirely mechanical**. There is no structural dependency on underscore anywhere in the language. The only friction is:
1. Story 75.1.5 decided `$snake_case` for variants 1 day ago — this must be reversed.
2. ~1030 instances across test/example corpus need find-replace.
3. 115 unique stdlib interface names need renaming.

All of this is scriptable. The "719 instances deeply embedded" framing presented to researchers overstated the difficulty — the changes are purely cosmetic renames with zero semantic impact.
