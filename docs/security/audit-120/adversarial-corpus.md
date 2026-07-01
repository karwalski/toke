# Adversarial fuzz corpus — Epic 120 / Story 120.23

Companion to 120.22 (fuzzing harnesses). This story seeds the fuzzers with a
handful of hand-crafted **adversarial** inputs — malformed, oversized, and
deeply-nested payloads — that exercise the specific failure modes flagged by
the 120.5 (parsers) and 120.6 (http-core) audits. The existing `corpus/` holds
well-formed `.tk` lexer/parser seeds; these adversarial seeds live separately
so they are easy to review and easy to point a target at with
`./fuzz-<target> corpus-adversarial/`.

Location: `test/fuzz/corpus-adversarial/`

All files are intentionally small (max ~9 KB) but representative — each is a
minimal reproduction of the *class* of attack, scaled down so the seed stays
under the harnesses' size caps and stays cheap for the fuzzer to mutate.

## Seeds

| File | Target(s) | Attack class | Why it's here |
|------|-----------|--------------|---------------|
| `json_deep_nest_arrays.json` | `fuzz-json` | Deep nesting (2000× `[`) | PAR-04 — unbounded recursion in `skip_array` → stack-overflow DoS. |
| `json_deep_nest_objects.json` | `fuzz-json` | Deep nesting (1500× `{"a":`) | PAR-04 — same recursion path through `skip_object`. |
| `json_trailing_backslash.json` | `fuzz-json` | Malformed string (`"value\` unterminated, trailing `\`) | PAR-03 — `skip_string` OOB read when a backslash sits at end-of-buffer. |
| `xml_entity_expansion.xml` | `fuzz-xml` | Entity expansion (billion-laughs, scaled to 4 levels ×10) | Classic entity-blow-up DoS; probes whether the XML/SOAP surface (PAR-11) expands internal entities unboundedly. |
| `xml_deep_unclosed.xml` | `fuzz-xml` | Deep nesting + unbalanced tags (1000 unclosed elements) | Recursion / unbalanced-tag handling on the flat/dotted-path element parser. |
| `multipart_oversized_boundary.txt` | `fuzz-multipart` | Oversized boundary token (~4 KB) + body with no closing boundary | http-core body handling (120.6); stresses `mp_find` boundary scanning and the boundary length assumptions on the unauth file-upload path. First line = boundary per the harness split convention. |
| `http_chunked_malformed.txt` | `fuzz-http-parse` | Malformed chunked body — chunk-size line `FFFFFFFFFFFFFFF0` far larger than the bytes supplied, plus a non-hex `ZZ` size | Chunk-size vs. actual-length mismatch and hex-parse handling in the request body path. |
| `ws_frame_overflow.bin` | `fuzz-ws-frame` | Crafted WS frame: FIN+binary, MASK set, 127 (64-bit ext len) = `0xFFFFFFFFFFFFFFFF`, no payload | HTT-04 — `ws_decode_frame` length/mask integer overflow → heap overflow via the 64-bit extended-length path. |
| `ws_frame_short_payload.bin` | `fuzz-ws-frame` | WS frame claiming 16-bit ext length `0xFFF0` but only 2 payload bytes present | Truncated-frame handling: decoder must not read past the supplied buffer when the advertised length exceeds available bytes. |

## Notes

- **Binary seeds** (`*.bin`) are raw frame bytes fed straight into
  `ws_decode_frame()` — the same bytes an attacker puts on the wire. Inspect
  with `xxd`. `ws_frame_overflow.bin` is `82 ff ff*8 de ad be ef`
  (mask key `deadbeef`); `ws_frame_short_payload.bin` is
  `81 fe ff f0 00 00 00 00 41 42`.
- **Entity expansion** is deliberately capped at 4 nesting levels (×10 fan-out)
  so the seed itself stays under 400 bytes; a mutating fuzzer will happily grow
  the fan-out if the parser proves vulnerable.
- The oversized-multipart seed follows the `fuzz_multipart` split convention:
  the first line (before the first `\n`) is used as the boundary token, so the
  ~4 KB boundary is on line 1 and the malformed part follows.
- These are **seeds**, not regression fixtures — none is expected to crash a
  fixed build. Their job is to hand libFuzzer a starting point deep inside each
  vulnerable code path so coverage-guided mutation finds neighbouring bugs fast.

## Usage

```sh
# Seed a target's in-memory corpus with the adversarial inputs:
./fuzz-json      corpus-adversarial/ -max_total_time=120
./fuzz-xml       corpus-adversarial/ -max_total_time=120
./fuzz-multipart corpus-adversarial/ -max_total_time=120
./fuzz-ws-frame  corpus-adversarial/ -max_total_time=120
./fuzz-http-parse corpus-adversarial/ -max_total_time=120
```

Each target ignores inputs that don't match its format, so pointing several
targets at the shared directory is safe.
