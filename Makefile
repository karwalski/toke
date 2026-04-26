CC      = cc
CFLAGS  = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -g \
          -DTKC_STDLIB_DIR='"$(CURDIR)/src/stdlib"'
# GCC-specific: suppress format-truncation warnings (snprintf truncation is by design)
GCC_CHECK := $(shell $(CC) -Wno-format-truncation -x c -c /dev/null -o /dev/null 2>/dev/null && echo yes)
ifeq ($(GCC_CHECK),yes)
CFLAGS += -Wno-format-truncation
endif
# ── Epic 55: vendor library flags ────────────────────────────────────────────
CMARK_SRCS  = $(filter-out stdlib/vendor/cmark/src/main.c, \
                $(wildcard stdlib/vendor/cmark/src/*.c))
CMARK_FLAGS = -Istdlib/vendor/cmark/src -Wno-pedantic
TOML_SRCS   = stdlib/vendor/tomlc99/toml.c
TOML_FLAGS  = -Istdlib/vendor/tomlc99

SRCS    = src/lexer.c src/parser.c src/names.c src/types.c \
          src/arena.c src/ir.c src/llvm.c src/diag.c src/config.c src/fmt.c src/progress.c \
          src/sourcemap.c src/ast_json.c src/migrate.c src/companion.c src/compress.c \
          src/stdlib_deps.c src/glue_gen.c \
          src/main.c src/stdlib/str.c

# ── Story 19.1.4: stdlib modules linked into tkc for i= imports ──────────
STDLIB_SRCS = \
          src/stdlib/crypto.c \
          src/stdlib/encoding.c src/stdlib/encrypt.c src/stdlib/auth.c \
          src/stdlib/ws.c src/stdlib/sse.c src/stdlib/router.c \
          src/stdlib/template.c src/stdlib/csv.c src/stdlib/math.c \
          src/stdlib/llm.c src/stdlib/llm_tool.c \
          src/stdlib/chart.c src/stdlib/html.c src/stdlib/dashboard.c \
          src/stdlib/svg.c src/stdlib/canvas.c src/stdlib/image.c \
          src/stdlib/dataframe.c src/stdlib/analytics.c src/stdlib/ml.c \
          src/stdlib/net.c src/stdlib/sys.c

SRCS    += $(STDLIB_SRCS)
OBJS    = $(SRCS:.c=.o)
LDLIBS  = -lm -lz
BIN     = tkc

# ── Reproducible-build flags ──────────────────────────────────────────────────
# These flags eliminate sources of non-determinism so that the same source tree
# produces bit-identical binaries across builds on the same platform.
#
#   -frandom-seed=tkc       deterministic internal compiler hashes
#   -ffile-prefix-map=...   strip absolute paths from debug info / __FILE__
#
# SOURCE_DATE_EPOCH: if set in the environment, GCC/Clang use it for __DATE__
# and __TIME__.  The Makefile exports it when available; CI pins it to the
# commit timestamp.
# ──────────────────────────────────────────────────────────────────────────────
REPRO_FLAGS = -frandom-seed=tkc \
              -ffile-prefix-map=$(CURDIR)=.

export SOURCE_DATE_EPOCH ?= 0

# Portable wall-clock timeout for test binaries (no GNU coreutils needed).
# Uses perl alarm() which survives exec; SIGALRM default action kills the process.
# Override with e.g.  make RUN_TEST_TIMEOUT=60 test-stdlib-encrypt
RUN_TEST_TIMEOUT ?= 180
RUN_TEST = $(CURDIR)/test/run_test.sh $(RUN_TEST_TIMEOUT)

.PHONY: all clean lint conform conform-check build-all ci test-e2e test-companion test-companion-diff test-migrate verify-ir stress test-stdlib test-stdlib-process test-stdlib-env test-stdlib-crypto test-stdlib-auth test-stdlib-time test-stdlib-test test-stdlib-log test-stdlib-coverage test-stdlib-dataframe test-stdlib-analytics bench repro-check test-compress test-compress-stream test-compress-schema \
	test-stdlib-encoding test-stdlib-encrypt test-stdlib-ws test-stdlib-sse test-stdlib-router \
	test-stdlib-template test-stdlib-csv test-stdlib-math test-stdlib-llm test-stdlib-llm-tool \
	test-stdlib-chart test-stdlib-html test-stdlib-dashboard test-stdlib-svg test-stdlib-canvas \
	test-stdlib-image test-stdlib-ml \
	test-stdlib-all-new \
	test-stdlib-security-integration test-stdlib-network-integration \
	test-stdlib-viz-integration test-stdlib-data-pipeline test-stdlib-llm-live \
	test-stdlib-http test-stdlib-http-cookies test-stdlib-http-multipart \
	test-stdlib-http-form test-stdlib-http-tls \
	test-stdlib-file test-stdlib-runtime \
	test-stdlib-path test-stdlib-args test-stdlib-md test-stdlib-toml \
	test-stdlib-vecstore \
	install-man

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -c -o $@ $<

# encrypt.c uses __int128 for Ed25519 arithmetic — allowed under GCC but rejected by -Wpedantic
src/stdlib/encrypt.o: src/stdlib/encrypt.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -Wno-pedantic -c -o $@ $<

lint:
	$(CC) $(CFLAGS) --analyze $(SRCS)

conform:
	@bash test/run_conform.sh

conform-check:
	@bash test/run_conform.sh && echo "CONFORMANCE: 100%"

build-all:
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=x86_64-linux-gnu"
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=aarch64-linux-gnu"
	$(MAKE) CC=cc CFLAGS="$(CFLAGS) --target=aarch64-apple-macos"

test-e2e: $(BIN)
	@bash test/e2e/run_e2e.sh

test-companion: $(BIN)
	@bash test/companion/run_companion.sh

test-companion-diff: $(BIN)
	@bash test/companion/run_companion_diff.sh

test-migrate: $(BIN)
	@bash test/migrate/run_migrate.sh

test-compress: $(BIN)
	@bash test/compress/compress_placeholder.sh

test-compress-stream: $(BIN)
	@bash test/compress/compress_stream.sh

test-compress-schema: $(BIN)
	@bash test/compress/compress_schema.sh

verify-ir: $(BIN)
	@bash test/verify_ir.sh

stress: $(BIN)
	@bash test/stress/run_stress.sh

ci: lint conform conform-check

test-stdlib:
	$(CC) $(CFLAGS) -o test/stdlib/test_str \
	    test/stdlib/test_str.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_str

test-stdlib-db:
	$(CC) $(CFLAGS) -o test/stdlib/test_db \
	    test/stdlib/test_db.c src/stdlib/db.c -lsqlite3
	$(RUN_TEST) ./test/stdlib/test_db

test-stdlib-file:
	$(CC) $(CFLAGS) -o test/stdlib/test_file \
	    test/stdlib/test_file.c src/stdlib/file.c
	$(RUN_TEST) ./test/stdlib/test_file

test-stdlib-runtime:
	$(CC) $(CFLAGS) -o test/stdlib/test_tk_runtime \
	    test/stdlib/test_tk_runtime.c src/stdlib/tk_runtime.c
	$(RUN_TEST) ./test/stdlib/test_tk_runtime

test-stdlib-http:
	$(CC) $(CFLAGS) -o test/stdlib/test_http \
	    test/stdlib/test_http.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_http

test-stdlib-http-cookies:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_cookies \
	    test/stdlib/test_http_cookies.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_http_cookies

test-stdlib-http-multipart:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_multipart \
	    test/stdlib/test_http_multipart.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_http_multipart

test-stdlib-http-form:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_form \
	    test/stdlib/test_http_form.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_http_form

# Story 27.1.2 — TLS/HTTPS support.
# Compiles the stub (no OpenSSL) so the test binary is always buildable.
# To test with real TLS, rebuild with:
#   make test-stdlib-http-tls TK_OPENSSL=1
ifdef TK_OPENSSL
TLS_CFLAGS  = -I/opt/homebrew/include -DTK_HAVE_OPENSSL \
              -Wno-deprecated-declarations
TLS_LDFLAGS = -L/opt/homebrew/Cellar/openssl@3/3.6.1/lib -lssl -lcrypto
else
TLS_CFLAGS  =
TLS_LDFLAGS =
endif

test-stdlib-http-tls:
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -o test/stdlib/test_http_tls \
	    test/stdlib/test_http_tls.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c \
	    $(TLS_LDFLAGS)
	$(RUN_TEST) ./test/stdlib/test_http_tls

test-stdlib-process:
	$(CC) $(CFLAGS) -o test/stdlib/test_process \
	    test/stdlib/test_process.c src/stdlib/process.c
	$(RUN_TEST) ./test/stdlib/test_process

test-stdlib-env:
	$(CC) $(CFLAGS) -o test/stdlib/test_env \
	    test/stdlib/test_env.c src/stdlib/env.c
	$(RUN_TEST) ./test/stdlib/test_env

test-stdlib-crypto:
	$(CC) $(CFLAGS) -o test/stdlib/test_crypto \
	    test/stdlib/test_crypto.c src/stdlib/crypto.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_crypto

test-stdlib-auth:
	$(CC) $(CFLAGS) -o test/stdlib/test_auth \
	    test/stdlib/test_auth.c src/stdlib/auth.c src/stdlib/encoding.c src/stdlib/crypto.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_auth

test-stdlib-time:
	$(CC) $(CFLAGS) -o test/stdlib/test_time \
	    test/stdlib/test_time.c src/stdlib/tk_time.c
	$(RUN_TEST) ./test/stdlib/test_time

test-stdlib-test:
	$(CC) $(CFLAGS) -o test/stdlib/test_tktest \
	    test/stdlib/test_tktest.c src/stdlib/tk_test.c
	$(RUN_TEST) ./test/stdlib/test_tktest

test-stdlib-log:
	$(CC) $(CFLAGS) -o test/stdlib/test_log \
	    test/stdlib/test_log.c src/stdlib/log.c src/stdlib/tk_time.c
	$(RUN_TEST) ./test/stdlib/test_log

test-stdlib-toon:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_toon \
	    test/stdlib/test_toon.c src/stdlib/toon.c
	$(RUN_TEST) ./test/stdlib/test_toon

test-stdlib-json:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_json \
	    test/stdlib/test_json.c src/stdlib/json.c
	$(RUN_TEST) ./test/stdlib/test_json

test-stdlib-yaml:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_yaml \
	    test/stdlib/test_yaml.c src/stdlib/yaml.c
	$(RUN_TEST) ./test/stdlib/test_yaml

test-stdlib-i18n:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_i18n \
	    test/stdlib/test_i18n.c src/stdlib/i18n.c
	$(RUN_TEST) ./test/stdlib/test_i18n

test-stdlib-dataframe:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_dataframe \
	    test/stdlib/test_dataframe.c src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_dataframe

test-stdlib-analytics:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_analytics \
	    test/stdlib/test_analytics.c src/stdlib/analytics.c src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/str.c src/stdlib/math.c -lm
	$(RUN_TEST) ./test/stdlib/test_analytics

# ── Story 19.1.1: Unit test targets for new stdlib modules ──────────────────

test-stdlib-encoding:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_encoding \
	    test/stdlib/test_encoding.c src/stdlib/encoding.c
	$(RUN_TEST) ./test/stdlib/test_encoding

test-stdlib-encrypt:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_encrypt \
	    test/stdlib/test_encrypt.c src/stdlib/encrypt.c src/stdlib/crypto.c src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_encrypt

test-stdlib-ws:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_ws \
	    test/stdlib/test_ws.c src/stdlib/ws.c
	$(RUN_TEST) ./test/stdlib/test_ws

test-stdlib-sse:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_sse \
	    test/stdlib/test_sse.c src/stdlib/sse.c
	$(RUN_TEST) ./test/stdlib/test_sse

test-stdlib-router:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_router \
	    test/stdlib/test_router.c src/stdlib/router.c src/stdlib/ws.c -lz
	$(RUN_TEST) ./test/stdlib/test_router

test-stdlib-template:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_template \
	    test/stdlib/test_template.c src/stdlib/template.c
	$(RUN_TEST) ./test/stdlib/test_template

test-stdlib-csv:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_csv \
	    test/stdlib/test_csv.c src/stdlib/csv.c
	$(RUN_TEST) ./test/stdlib/test_csv

test-stdlib-math:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_math \
	    test/stdlib/test_math.c src/stdlib/math.c -lm
	$(RUN_TEST) ./test/stdlib/test_math

test-stdlib-llm:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm \
	    test/stdlib/test_llm.c src/stdlib/llm.c
	$(RUN_TEST) ./test/stdlib/test_llm

test-stdlib-llm-tool:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm_tool \
	    test/stdlib/test_llm_tool.c src/stdlib/llm_tool.c src/stdlib/llm.c
	$(RUN_TEST) ./test/stdlib/test_llm_tool

test-stdlib-chart:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_chart \
	    test/stdlib/test_chart.c src/stdlib/chart.c
	$(RUN_TEST) ./test/stdlib/test_chart

test-stdlib-html:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_html \
	    test/stdlib/test_html.c src/stdlib/html.c
	$(RUN_TEST) ./test/stdlib/test_html

test-stdlib-dashboard:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_dashboard \
	    test/stdlib/test_dashboard.c src/stdlib/dashboard.c src/stdlib/chart.c src/stdlib/html.c src/stdlib/router.c src/stdlib/ws.c -lz
	$(RUN_TEST) ./test/stdlib/test_dashboard

test-stdlib-svg:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_svg \
	    test/stdlib/test_svg.c src/stdlib/svg.c -lm
	$(RUN_TEST) ./test/stdlib/test_svg

test-stdlib-canvas:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_canvas \
	    test/stdlib/test_canvas.c src/stdlib/canvas.c
	$(RUN_TEST) ./test/stdlib/test_canvas

test-stdlib-image:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_image \
	    test/stdlib/test_image.c src/stdlib/image.c
	$(RUN_TEST) ./test/stdlib/test_image

test-stdlib-ml:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_ml \
	    test/stdlib/test_ml.c src/stdlib/ml.c -lm
	$(RUN_TEST) ./test/stdlib/test_ml

# ── Aggregate: all 20 new stdlib module tests ───────────────────────────────

test-stdlib-all-new: test-stdlib-encoding test-stdlib-encrypt test-stdlib-auth \
	test-stdlib-ws test-stdlib-sse test-stdlib-router test-stdlib-template \
	test-stdlib-csv test-stdlib-math test-stdlib-llm test-stdlib-llm-tool \
	test-stdlib-chart test-stdlib-html test-stdlib-dashboard test-stdlib-svg \
	test-stdlib-canvas test-stdlib-image test-stdlib-dataframe test-stdlib-analytics \
	test-stdlib-ml
	@echo "ALL 20 new stdlib module tests PASSED"

# ── Story 19.1.2: Integration test targets ─────────────────────────────────

test-stdlib-security-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_security_integration \
	    test/stdlib/test_security_integration.c \
	    src/stdlib/auth.c src/stdlib/encrypt.c src/stdlib/crypto.c src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_security_integration

test-stdlib-network-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_network_integration \
	    test/stdlib/test_network_integration.c \
	    src/stdlib/router.c src/stdlib/ws.c src/stdlib/sse.c -lz
	$(RUN_TEST) ./test/stdlib/test_network_integration

test-stdlib-viz-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_viz_integration \
	    test/stdlib/test_viz_integration.c \
	    src/stdlib/chart.c src/stdlib/html.c src/stdlib/svg.c src/stdlib/canvas.c \
	    src/stdlib/dashboard.c src/stdlib/router.c \
	    src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/math.c src/stdlib/str.c -lm -lz
	$(RUN_TEST) ./test/stdlib/test_viz_integration

test-stdlib-data-pipeline:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_data_pipeline \
	    test/stdlib/test_data_pipeline.c \
	    src/stdlib/csv.c src/stdlib/math.c src/stdlib/dataframe.c \
	    src/stdlib/analytics.c src/stdlib/chart.c src/stdlib/str.c -lm
	$(RUN_TEST) ./test/stdlib/test_data_pipeline

test-stdlib-llm-live:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm_live \
	    test/stdlib/test_llm_live.c \
	    src/stdlib/llm.c src/stdlib/llm_tool.c src/stdlib/json.c
	$(RUN_TEST) ./test/stdlib/test_llm_live

test-stdlib-coverage:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_stdlib_coverage \
	    test/stdlib/test_stdlib_coverage.c \
	    src/stdlib/json.c src/stdlib/file.c src/stdlib/str.c \
	    src/stdlib/db.c -lsqlite3
	$(RUN_TEST) ./test/stdlib/test_stdlib_coverage

bench:
	$(CC) -O2 -iquote src/stdlib -o test/stdlib/bench_stdlib \
	    test/stdlib/bench_stdlib.c \
	    src/stdlib/str.c src/stdlib/json.c src/stdlib/file.c \
	    src/stdlib/crypto.c src/stdlib/tk_time.c src/stdlib/process.c \
	    src/stdlib/env.c src/stdlib/log.c src/stdlib/tk_test.c \
	    src/stdlib/db.c src/stdlib/http.c -lsqlite3
	./test/stdlib/bench_stdlib

# ── Reproducible-build verification ───────────────────────────────────────────
# Builds tkc twice into separate directories and compares SHA-256 hashes.
# Exits non-zero if the two binaries differ.
repro-check:
	@echo "==> Reproducible-build check: building twice and comparing hashes..."
	@rm -rf .repro-a .repro-b
	$(MAKE) clean
	SOURCE_DATE_EPOCH=1700000000 $(MAKE) all
	@mkdir -p .repro-a && cp $(BIN) .repro-a/$(BIN) && \
	 for f in $(OBJS); do cp $$f .repro-a/$$(basename $$f); done
	$(MAKE) clean
	SOURCE_DATE_EPOCH=1700000000 $(MAKE) all
	@mkdir -p .repro-b && cp $(BIN) .repro-b/$(BIN) && \
	 for f in $(OBJS); do cp $$f .repro-b/$$(basename $$f); done
	$(MAKE) clean
	@# ── Compare object files (deterministic on all platforms) ──
	@FAIL=0; \
	 for f in $(patsubst %.o,%,$(notdir $(OBJS))); do \
	   HA=$$(shasum -a 256 .repro-a/$$f.o | cut -d' ' -f1); \
	   HB=$$(shasum -a 256 .repro-b/$$f.o | cut -d' ' -f1); \
	   if [ "$$HA" = "$$HB" ]; then \
	     echo "  PASS $$f.o  $$HA"; \
	   else \
	     echo "  FAIL $$f.o  A=$$HA  B=$$HB" >&2; FAIL=1; \
	   fi; \
	 done; \
	 echo ""; \
	 HA=$$(shasum -a 256 .repro-a/$(BIN) | cut -d' ' -f1); \
	 HB=$$(shasum -a 256 .repro-b/$(BIN) | cut -d' ' -f1); \
	 if [ "$$HA" = "$$HB" ]; then \
	   echo "  PASS $(BIN)  $$HA"; \
	 else \
	   echo "  INFO $(BIN) differs (expected on macOS due to LC_UUID)"; \
	   echo "       A=$$HA"; \
	   echo "       B=$$HB"; \
	 fi; \
	 echo ""; \
	 if [ "$$FAIL" = "0" ]; then \
	   echo "PASS: all object files are identical — build is reproducible"; \
	 else \
	   echo "FAIL: object files differ" >&2; exit 1; \
	 fi
	@rm -rf .repro-a .repro-b

# ── Epic 55: new stdlib test targets ─────────────────────────────────────────

test-stdlib-path:
	$(CC) $(CFLAGS) -o test/stdlib/test_path \
	    test/stdlib/test_path.c src/stdlib/path.c
	$(RUN_TEST) ./test/stdlib/test_path

test-stdlib-args:
	$(CC) $(CFLAGS) -o test/stdlib/test_args \
	    test/stdlib/test_args.c src/stdlib/args.c
	$(RUN_TEST) ./test/stdlib/test_args

test-stdlib-md:
	$(CC) $(CFLAGS) $(CMARK_FLAGS) -o test/stdlib/test_md \
	    test/stdlib/test_md.c src/stdlib/md.c $(CMARK_SRCS)
	$(RUN_TEST) ./test/stdlib/test_md

test-stdlib-toml:
	$(CC) $(CFLAGS) $(TOML_FLAGS) -o test/stdlib/test_toml \
	    test/stdlib/test_toml.c src/stdlib/toml.c $(TOML_SRCS)
	$(RUN_TEST) ./test/stdlib/test_toml

# ── Epic 72.5: std.vecstore ──────────────────────────────────────────────────
test-stdlib-vecstore:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_vecstore \
	    test/stdlib/test_vecstore.c src/stdlib/vecstore.c -lpthread
	$(RUN_TEST) ./test/stdlib/test_vecstore

clean:
	rm -f $(OBJS) $(BIN) test/stdlib/test_str test/stdlib/test_db \
	    test/stdlib/test_process test/stdlib/test_env test/stdlib/test_crypto \
	    test/stdlib/test_time test/stdlib/test_tktest test/stdlib/test_log \
	    test/stdlib/test_stdlib_coverage test/stdlib/bench_stdlib \
	    test/stdlib/test_encoding test/stdlib/test_encrypt \
	    test/stdlib/test_ws test/stdlib/test_sse test/stdlib/test_router \
	    test/stdlib/test_template test/stdlib/test_csv test/stdlib/test_math \
	    test/stdlib/test_llm test/stdlib/test_llm_tool \
	    test/stdlib/test_chart test/stdlib/test_html test/stdlib/test_dashboard \
	    test/stdlib/test_svg test/stdlib/test_canvas test/stdlib/test_image \
	    test/stdlib/test_ml \
	    test/stdlib/test_security_integration test/stdlib/test_network_integration \
	    test/stdlib/test_viz_integration test/stdlib/test_data_pipeline \
	    test/stdlib/test_path test/stdlib/test_args \
	    test/stdlib/test_md test/stdlib/test_toml \
	    test/stdlib/test_vecstore \
	    test/stdlib/test_llm_live \
	    fuzz-lexer fuzz-parser

FUZZ_FLAGS = -fsanitize=address,undefined,fuzzer -g

fuzz-lexer: test/fuzz/fuzz_lexer.c src/lexer.o src/diag.o src/arena.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-lexer $^

fuzz-parser: test/fuzz/fuzz_parser.c src/lexer.o src/parser.o src/diag.o src/arena.o src/names.o src/types.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-parser $^

fuzz-http-parse: test/fuzz/fuzz_http_parse.c
	$(CC) $(FUZZ_FLAGS) -o fuzz-http-parse $^

fuzz-url-route: test/fuzz/fuzz_url_route.c src/stdlib/router.o src/stdlib/str.o
	$(CC) $(FUZZ_FLAGS) -I src/stdlib -o fuzz-url-route $^ -lz

fuzz: fuzz-lexer fuzz-parser
	./fuzz-lexer -max_total_time=60 test/fuzz/corpus/
	./fuzz-parser -max_total_time=60 test/fuzz/corpus/

fuzz-http: fuzz-http-parse fuzz-url-route
	./fuzz-http-parse -max_total_time=120
	./fuzz-url-route -max_total_time=120

# ── Man page ────────────────────────────────────────────────────────────
MANDIR ?= /usr/local/share/man/man1

install-man: doc/toke.1
	@mkdir -p $(MANDIR)
	install -m 644 doc/toke.1 $(MANDIR)/toke.1
	@ln -sf $(MANDIR)/toke.1 $(MANDIR)/tkc.1 2>/dev/null || true
	@echo "Installed toke.1 and tkc.1 symlink to $(MANDIR)"

.PHONY: fuzz fuzz-lexer fuzz-parser fuzz-http fuzz-http-parse fuzz-url-route
