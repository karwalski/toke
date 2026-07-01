# Fuzzing harnesses — Epic 120 / Story 120.22

Extends the existing libFuzzer corpus (`fuzz_lexer`, `fuzz_parser`,
`fuzz_http_parse`, `fuzz_url_route`) with coverage-guided targets over the
untrusted-input parsers surfaced by the 120.5 (parsers) and 120.6 (http-core)
audits. Targets are prioritised by remote reachability: the JSON/YAML/TOON
converters, WebSocket frame decoder, and multipart body parser all consume
attacker-controlled bytes with no authentication.

All harnesses use the same `int LLVMFuzzerTestOneInput(const uint8_t *data,
size_t size)` entry-point style as the existing targets, cap input length,
NUL-terminate the buffer for the C-string APIs, and free everything the target
owns so the process stays leak-clean across libFuzzer's in-process reuse.

## New targets

| Target | Harness file | Entry point(s) driven | Prioritised findings |
|--------|--------------|-----------------------|----------------------|
| `fuzz-multipart` | `test/fuzz/fuzz_multipart.c` | `http_multipart_boundary()`, `http_multipart_parse()` (`src/stdlib/http.c`) | 120.6 http-core body handling; remote-unauth file-upload path |
| `fuzz-ws-frame` | `test/fuzz/fuzz_ws_frame.c` | `ws_decode_frame()` (`src/stdlib/ws.c`) | 120.6 HTT-04 (ws length/mask integer overflow → heap overflow) |
| `fuzz-json` | `test/fuzz/fuzz_json.c` | `json_dec()` (`src/stdlib/json.c`) | 120.5 PAR-03 (skip_string OOB), PAR-04 (skipper recursion) |
| `fuzz-yaml` | `test/fuzz/fuzz_yaml.c` | `yaml_dec()`, `yaml_from_json()`, `yaml_to_json()` (`src/stdlib/yaml.c`) | 120.5 PAR-01 (snprintf-accum heap write past buffer) |
| `fuzz-toon` | `test/fuzz/fuzz_toon.c` | `toon_dec()`, `toon_from_json()`, `toon_to_json()` (`src/stdlib/toon.c`) | 120.5 PAR-02 (fixed 4096 buffer), PAR-06, PAR-07 |
| `fuzz-toml` | `test/fuzz/fuzz_toml.c` | `toml_load()` (`src/stdlib/toml.c` → vendored tomlc99) | untrusted config parsing surface |
| `fuzz-xml` | `test/fuzz/fuzz_xml.c` | `xml_parse()` (`src/stdlib/xml.c`) | 120.5 PAR-11 (SOAP/XML surface) |
| `fuzz-template` | `test/fuzz/fuzz_template.c` | `tmpl_compile()`, `tmpl_render()`, `tmpl_renderhtml()` (`src/stdlib/template.c`) | 120.5 PAR-08/PAR-09 (in-memory render/escape paths) |

### Per-target notes

- **fuzz-multipart** — splits the input at the first `\n`: line 1 becomes the
  boundary token, the remainder is parsed as the body (fixed fallback boundary
  when there is no newline). The OpenSSL/TLS paths in `http.c` are macro-guarded
  (`TK_HAVE_OPENSSL`) and stay compiled out, so the link needs only
  `encoding.c`, `str.c`, `log.c` and `-lz` — no OpenSSL. This mirrors the
  existing `test-stdlib-http-multipart` recipe plus `log.c`/`-lz` for the
  request-logging and gzip symbols that `http.c` references.
- **fuzz-ws-frame** — feeds raw bytes straight into `ws_decode_frame()`, the
  same function that consumes client socket bytes; `ws.c` is self-contained
  (its base64 helper is `static`).
- **fuzz-yaml / fuzz-toon** — the `*_from_json` / `*_to_json` converters return
  **either** a `malloc`'d buffer **or** a static string literal (`"{}"`,
  `"[]"`, `"null"`, `"\"\""`) on error paths. The returned pointer therefore
  cannot be safely `free`d, so these two targets intentionally leak the
  successful results. Run them with `ASAN_OPTIONS=detect_leaks=0` so
  LeakSanitizer does not mask the heap-overflow crashes we are hunting (the
  inconsistent ownership is itself the 120.5 PAR-01/PAR-02 finding). All other
  targets are leak-clean.
- **fuzz-template** — drives only the in-memory `{{IDENT}}` compile/render/
  escape paths. The file-backed `tmpl_renderfile` / `tmpl_renderpage`
  layout/partial paths (120.5 PAR-09 path-traversal) are **not** fuzzed here
  because they require filesystem fixtures; recommend a separate directory-
  seeded target in a follow-up.

### Targets considered but not created

None skipped for lack of an entry point — every target listed in the story has
a clean, identified parse entry point in the stdlib. The only deliberate scope
cut is the *file-backed* template path noted above (needs FS fixtures, not a
byte-buffer harness).

## Build validation

Each target was compiled and run under `-fsanitize=address,undefined` with a
stub `main` driver (the libFuzzer runtime `libclang_rt.fuzzer_osx.a` is not
installed in the dev sandbox, the same constraint the existing targets have
locally — the `-fsanitize=fuzzer` link happens on the CI/build host). All eight
link cleanly and run without diagnostics on seed input.

## Proposed Makefile diff (NOT applied)

Add the recipes below and extend the `fuzz`/`fuzz-http` aggregate + `.PHONY`
lines. Recipes compile the target `.c` **from source** with `$(FUZZ_FLAGS)` so
the parser code itself is sanitizer-instrumented (matching `fuzz-http-parse`).

```make
# ── Story 120.22: stdlib parser fuzz targets ─────────────────────────────
fuzz-json: test/fuzz/fuzz_json.c src/stdlib/json.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-json $^

fuzz-yaml: test/fuzz/fuzz_yaml.c src/stdlib/yaml.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-yaml $^

fuzz-toon: test/fuzz/fuzz_toon.c src/stdlib/toon.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-toon $^

fuzz-toml: test/fuzz/fuzz_toml.c src/stdlib/toml.c $(TOML_SRCS)
	$(CC) $(FUZZ_FLAGS) $(TOML_FLAGS) -o fuzz-toml $^

fuzz-xml: test/fuzz/fuzz_xml.c src/stdlib/xml.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-xml $^

fuzz-template: test/fuzz/fuzz_template.c src/stdlib/template.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-template $^

fuzz-ws-frame: test/fuzz/fuzz_ws_frame.c src/stdlib/ws.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-ws-frame $^

fuzz-multipart: test/fuzz/fuzz_multipart.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-multipart $^ -lz
```

Extend the aggregate run targets (leak detection disabled only for the two
literal-or-heap converters):

```make
fuzz-stdlib: fuzz-json fuzz-yaml fuzz-toon fuzz-toml fuzz-xml fuzz-template
	./fuzz-json     -max_total_time=60
	ASAN_OPTIONS=detect_leaks=0 ./fuzz-yaml -max_total_time=60
	ASAN_OPTIONS=detect_leaks=0 ./fuzz-toon -max_total_time=60
	./fuzz-toml     -max_total_time=60
	./fuzz-xml      -max_total_time=60
	./fuzz-template -max_total_time=60

fuzz-http: fuzz-http-parse fuzz-url-route fuzz-multipart fuzz-ws-frame
	./fuzz-http-parse -max_total_time=120
	./fuzz-url-route  -max_total_time=120
	./fuzz-multipart  -max_total_time=120
	./fuzz-ws-frame   -max_total_time=120
```

`.PHONY` line — append the new target names:

```make
.PHONY: fuzz fuzz-lexer fuzz-parser fuzz-http fuzz-http-parse fuzz-url-route \
	fuzz-stdlib fuzz-json fuzz-yaml fuzz-toon fuzz-toml fuzz-xml \
	fuzz-template fuzz-ws-frame fuzz-multipart \
	test-e2e-check test-standalone-check test-all
```

### CI wiring

The repo's fuzz CI step (whatever invokes `make fuzz` / `make fuzz-http`) picks
up `fuzz-multipart` and `fuzz-ws-frame` automatically once the `fuzz-http`
aggregate above is used. Add one new line to run the stdlib parser batch:

```yaml
    - name: Fuzz stdlib parsers (smoke)
      run: make fuzz-stdlib
```

Seed corpora can be dropped under `test/fuzz/corpus/` (shared by all targets)
to shorten time-to-coverage; valid `.json` / `.yaml` / `.toon` / `.toml` /
`.xml` samples and a captured multipart body / masked WS frame make good seeds.
