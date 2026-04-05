CC      = cc
CFLAGS  = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -g \
          -DTKC_STDLIB_DIR='"$(CURDIR)/src/stdlib"'
SRCS    = src/lexer.c src/parser.c src/names.c src/types.c \
          src/arena.c src/ir.c src/llvm.c src/diag.c src/config.c src/fmt.c src/progress.c \
          src/sourcemap.c src/ast_json.c src/migrate.c src/companion.c src/compress.c \
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
          src/stdlib/dataframe.c src/stdlib/analytics.c src/stdlib/ml.c

SRCS    += $(STDLIB_SRCS)
OBJS    = $(SRCS:.c=.o)
LDLIBS  = -lm
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

.PHONY: all clean lint conform conform-check build-all ci test-e2e test-companion test-companion-diff test-migrate verify-ir stress test-stdlib test-stdlib-process test-stdlib-env test-stdlib-crypto test-stdlib-auth test-stdlib-time test-stdlib-test test-stdlib-log test-stdlib-coverage test-stdlib-dataframe test-stdlib-analytics bench repro-check test-compress test-compress-stream test-compress-schema \
	test-stdlib-encoding test-stdlib-encrypt test-stdlib-ws test-stdlib-sse test-stdlib-router \
	test-stdlib-template test-stdlib-csv test-stdlib-math test-stdlib-llm test-stdlib-llm-tool \
	test-stdlib-chart test-stdlib-html test-stdlib-dashboard test-stdlib-svg test-stdlib-canvas \
	test-stdlib-image test-stdlib-ml \
	test-stdlib-all-new \
	test-stdlib-security-integration test-stdlib-network-integration \
	test-stdlib-viz-integration test-stdlib-data-pipeline test-stdlib-llm-live \
	test-stdlib-http test-stdlib-http-cookies test-stdlib-http-multipart \
	test-stdlib-http-form

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -c -o $@ $<

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
	./test/stdlib/test_str

test-stdlib-db:
	$(CC) $(CFLAGS) -o test/stdlib/test_db \
	    test/stdlib/test_db.c src/stdlib/db.c -lsqlite3
	./test/stdlib/test_db

test-stdlib-http:
	$(CC) $(CFLAGS) -o test/stdlib/test_http \
	    test/stdlib/test_http.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	./test/stdlib/test_http

test-stdlib-http-cookies:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_cookies \
	    test/stdlib/test_http_cookies.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	./test/stdlib/test_http_cookies

test-stdlib-http-multipart:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_multipart \
	    test/stdlib/test_http_multipart.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	./test/stdlib/test_http_multipart

test-stdlib-http-form:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_form \
	    test/stdlib/test_http_form.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	./test/stdlib/test_http_form

test-stdlib-process:
	$(CC) $(CFLAGS) -o test/stdlib/test_process \
	    test/stdlib/test_process.c src/stdlib/process.c
	./test/stdlib/test_process

test-stdlib-env:
	$(CC) $(CFLAGS) -o test/stdlib/test_env \
	    test/stdlib/test_env.c src/stdlib/env.c
	./test/stdlib/test_env

test-stdlib-crypto:
	$(CC) $(CFLAGS) -o test/stdlib/test_crypto \
	    test/stdlib/test_crypto.c src/stdlib/crypto.c src/stdlib/str.c
	./test/stdlib/test_crypto

test-stdlib-auth:
	$(CC) $(CFLAGS) -o test/stdlib/test_auth \
	    test/stdlib/test_auth.c src/stdlib/auth.c src/stdlib/encoding.c src/stdlib/crypto.c src/stdlib/str.c
	./test/stdlib/test_auth

test-stdlib-time:
	$(CC) $(CFLAGS) -o test/stdlib/test_time \
	    test/stdlib/test_time.c src/stdlib/tk_time.c
	./test/stdlib/test_time

test-stdlib-test:
	$(CC) $(CFLAGS) -o test/stdlib/test_tktest \
	    test/stdlib/test_tktest.c src/stdlib/tk_test.c
	./test/stdlib/test_tktest

test-stdlib-log:
	$(CC) $(CFLAGS) -o test/stdlib/test_log \
	    test/stdlib/test_log.c src/stdlib/log.c src/stdlib/tk_time.c
	./test/stdlib/test_log

test-stdlib-toon:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_toon \
	    test/stdlib/test_toon.c src/stdlib/toon.c
	./test/stdlib/test_toon

test-stdlib-yaml:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_yaml \
	    test/stdlib/test_yaml.c src/stdlib/yaml.c
	./test/stdlib/test_yaml

test-stdlib-i18n:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_i18n \
	    test/stdlib/test_i18n.c src/stdlib/i18n.c
	./test/stdlib/test_i18n

test-stdlib-dataframe:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_dataframe \
	    test/stdlib/test_dataframe.c src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/str.c
	./test/stdlib/test_dataframe

test-stdlib-analytics:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_analytics \
	    test/stdlib/test_analytics.c src/stdlib/analytics.c src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/str.c src/stdlib/math.c -lm
	./test/stdlib/test_analytics

# ── Story 19.1.1: Unit test targets for new stdlib modules ──────────────────

test-stdlib-encoding:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_encoding \
	    test/stdlib/test_encoding.c src/stdlib/encoding.c
	./test/stdlib/test_encoding

test-stdlib-encrypt:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_encrypt \
	    test/stdlib/test_encrypt.c src/stdlib/encrypt.c src/stdlib/crypto.c src/stdlib/encoding.c src/stdlib/str.c
	./test/stdlib/test_encrypt

test-stdlib-ws:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_ws \
	    test/stdlib/test_ws.c src/stdlib/ws.c
	./test/stdlib/test_ws

test-stdlib-sse:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_sse \
	    test/stdlib/test_sse.c src/stdlib/sse.c
	./test/stdlib/test_sse

test-stdlib-router:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_router \
	    test/stdlib/test_router.c src/stdlib/router.c
	./test/stdlib/test_router

test-stdlib-template:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_template \
	    test/stdlib/test_template.c src/stdlib/template.c
	./test/stdlib/test_template

test-stdlib-csv:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_csv \
	    test/stdlib/test_csv.c src/stdlib/csv.c
	./test/stdlib/test_csv

test-stdlib-math:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_math \
	    test/stdlib/test_math.c src/stdlib/math.c -lm
	./test/stdlib/test_math

test-stdlib-llm:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm \
	    test/stdlib/test_llm.c src/stdlib/llm.c
	./test/stdlib/test_llm

test-stdlib-llm-tool:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm_tool \
	    test/stdlib/test_llm_tool.c src/stdlib/llm_tool.c src/stdlib/llm.c
	./test/stdlib/test_llm_tool

test-stdlib-chart:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_chart \
	    test/stdlib/test_chart.c src/stdlib/chart.c
	./test/stdlib/test_chart

test-stdlib-html:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_html \
	    test/stdlib/test_html.c src/stdlib/html.c
	./test/stdlib/test_html

test-stdlib-dashboard:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_dashboard \
	    test/stdlib/test_dashboard.c src/stdlib/dashboard.c src/stdlib/chart.c src/stdlib/html.c src/stdlib/router.c
	./test/stdlib/test_dashboard

test-stdlib-svg:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_svg \
	    test/stdlib/test_svg.c src/stdlib/svg.c -lm
	./test/stdlib/test_svg

test-stdlib-canvas:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_canvas \
	    test/stdlib/test_canvas.c src/stdlib/canvas.c
	./test/stdlib/test_canvas

test-stdlib-image:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_image \
	    test/stdlib/test_image.c src/stdlib/image.c
	./test/stdlib/test_image

test-stdlib-ml:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_ml \
	    test/stdlib/test_ml.c src/stdlib/ml.c -lm
	./test/stdlib/test_ml

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
	./test/stdlib/test_security_integration

test-stdlib-network-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_network_integration \
	    test/stdlib/test_network_integration.c \
	    src/stdlib/router.c src/stdlib/ws.c src/stdlib/sse.c
	./test/stdlib/test_network_integration

test-stdlib-viz-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_viz_integration \
	    test/stdlib/test_viz_integration.c \
	    src/stdlib/chart.c src/stdlib/html.c src/stdlib/svg.c src/stdlib/canvas.c \
	    src/stdlib/dashboard.c src/stdlib/router.c \
	    src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/math.c src/stdlib/str.c -lm
	./test/stdlib/test_viz_integration

test-stdlib-data-pipeline:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_data_pipeline \
	    test/stdlib/test_data_pipeline.c \
	    src/stdlib/csv.c src/stdlib/math.c src/stdlib/dataframe.c \
	    src/stdlib/analytics.c src/stdlib/chart.c src/stdlib/str.c -lm
	./test/stdlib/test_data_pipeline

test-stdlib-llm-live:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm_live \
	    test/stdlib/test_llm_live.c \
	    src/stdlib/llm.c src/stdlib/llm_tool.c src/stdlib/json.c
	./test/stdlib/test_llm_live

test-stdlib-coverage:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_stdlib_coverage \
	    test/stdlib/test_stdlib_coverage.c \
	    src/stdlib/json.c src/stdlib/file.c src/stdlib/str.c \
	    src/stdlib/db.c -lsqlite3
	./test/stdlib/test_stdlib_coverage

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
	    test/stdlib/test_llm_live \
	    fuzz-lexer fuzz-parser

FUZZ_FLAGS = -fsanitize=address,undefined,fuzzer -g

fuzz-lexer: test/fuzz/fuzz_lexer.c src/lexer.o src/diag.o src/arena.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-lexer $^

fuzz-parser: test/fuzz/fuzz_parser.c src/lexer.o src/parser.o src/diag.o src/arena.o src/names.o src/types.o
	$(CC) $(FUZZ_FLAGS) -o fuzz-parser $^

fuzz: fuzz-lexer fuzz-parser
	./fuzz-lexer -max_total_time=60 test/fuzz/corpus/
	./fuzz-parser -max_total_time=60 test/fuzz/corpus/

.PHONY: fuzz fuzz-lexer fuzz-parser
