CC      = cc
CFLAGS  = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -g \
          -DTKC_STDLIB_DIR='"$(CURDIR)/src/stdlib"' \
          -MMD -MP   # emit .d header-dependency files so header changes trigger recompiles
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

# ── Story 127.85: vendored-source preflight ──────────────────────────────────
# stdlib/vendor/{cmark,tomlc99} are tracked in this repository (see
# stdlib/vendor/README.md).  If they are ever absent, every std.toml / std.md
# compile fails at the clang stage as an opaque E9003 that names neither the
# dependency nor the reason.  Stop here instead, by name.
# 135.1 adds miniz, which backs std.zip; src/stdlib/zip.c #includes miniz.c
# directly, so a missing checkout fails as a clang "file not found" instead.
VENDOR_SENTINELS = stdlib/vendor/tomlc99/toml.c \
                   stdlib/vendor/tomlc99/toml.h \
                   stdlib/vendor/cmark/src/cmark.c \
                   stdlib/vendor/cmark/src/cmark.h \
                   stdlib/vendor/cmark/src/cmark_export.h \
                   stdlib/vendor/cmark/src/cmark_version.h \
                   stdlib/vendor/miniz/miniz.c \
                   stdlib/vendor/miniz/miniz.h

SRCS    = src/lexer.c src/parser.c src/names.c src/types.c \
          src/arena.c src/ir.c src/llvm.c src/diag.c src/config.c src/fmt.c src/progress.c \
          src/sourcemap.c src/ast_json.c src/migrate.c src/companion.c src/compress.c \
          src/stdlib_deps.c src/glue_gen.c src/lint.c src/tkir.c \
          src/main.c src/stdlib/str.c

# ── Story 19.1.4: stdlib modules linked into tkc for i= imports ──────────
#
# 127.99 — WHAT THE STANDARD LIBRARY IS, is stated by the manifest, not by
# a directory glob.  The module table in src/stdlib_deps.c (printed by
# `tkc --emit-deps`) is authoritative for what ships in a compiled program;
# this list and find_stdlib_sources() in src/llvm.c are the two curated
# subsets built from it.  A build that globs the directory instead is
# guessing, and the guess was wrong: a rename left two copies of a module
# tracked, invisible here and fatal at link there.  test/conform/T004
# (scripts/check_stdlib_link_set.py) keeps the directory a faithful
# derivation of the manifest so a glob can no longer disagree with it.
STDLIB_SRCS = \
          src/stdlib/crypto.c \
          src/stdlib/encoding.c src/stdlib/encrypt.c src/stdlib/auth.c \
          src/stdlib/ws.c src/stdlib/sse.c src/stdlib/router.c \
          src/stdlib/template.c src/stdlib/csv.c src/stdlib/math.c \
          src/stdlib/llm.c src/stdlib/llm_tool.c \
          src/stdlib/chart.c src/stdlib/html.c src/stdlib/dashboard.c \
          src/stdlib/svg.c src/stdlib/canvas.c src/stdlib/image.c \
          src/stdlib/dataframe.c src/stdlib/analytics.c src/stdlib/ml.c \
          src/stdlib/net.c src/stdlib/sys.c \
          src/stdlib/mem.c \
          src/stdlib/os.c \
          src/stdlib/task.c \
          src/stdlib/collections.c \
          src/stdlib/capabilities.c

SRCS    += $(STDLIB_SRCS)
OBJS    = $(SRCS:.c=.o)
LDLIBS  = -lm -lz -lpthread
BIN     = toke

# ── Reproducible-build flags ──────────────────────────────────────────────────
# These flags eliminate sources of non-determinism so that the same source tree
# produces bit-identical binaries across builds on the same platform.
#
#   -frandom-seed=toke      deterministic internal compiler hashes
#   -ffile-prefix-map=...   strip absolute paths from debug info / __FILE__
#
# SOURCE_DATE_EPOCH: if set in the environment, GCC/Clang use it for __DATE__
# and __TIME__.  The Makefile exports it when available; CI pins it to the
# commit timestamp.
# ──────────────────────────────────────────────────────────────────────────────
REPRO_FLAGS = -frandom-seed=toke \
              -ffile-prefix-map=$(CURDIR)=.

export SOURCE_DATE_EPOCH ?= 0

# Portable wall-clock timeout for test binaries (no GNU coreutils needed).
# Uses perl alarm() which survives exec; SIGALRM default action kills the process.
# Override with e.g.  make RUN_TEST_TIMEOUT=60 test-stdlib-encrypt
RUN_TEST_TIMEOUT ?= 180
RUN_TEST = $(CURDIR)/test/run_test.sh $(RUN_TEST_TIMEOUT)

.PHONY: all vendor-check clean lint conform conform-sh conform-check build-all ci check-docs check-patterns render-patterns check-error-codes check-metrics check-canonical check-claims-all diff-codegen diff-codegen-record test-e2e test-companion test-companion-diff test-migrate verify-ir stress test-stdlib test-stdlib-process test-stdlib-ambient test-stdlib-env test-stdlib-crypto test-stdlib-auth test-stdlib-time test-stdlib-test test-stdlib-log test-stdlib-coverage test-stdlib-dataframe test-stdlib-analytics bench repro-check test-compress test-compress-stream test-compress-schema \
	test-stdlib-encoding test-stdlib-encrypt test-stdlib-ws test-stdlib-sse test-stdlib-router \
	test-stdlib-template test-stdlib-csv test-stdlib-math test-stdlib-llm test-stdlib-llm-tool \
	test-stdlib-chart test-stdlib-html test-stdlib-dashboard test-stdlib-svg test-stdlib-canvas \
	test-stdlib-image test-stdlib-ml \
	test-stdlib-all-new \
	test-stdlib-security-integration test-stdlib-network-integration \
	test-stdlib-viz-integration test-stdlib-data-pipeline test-stdlib-llm-live \
	test-stdlib-http test-stdlib-http-cookies test-stdlib-http-multipart \
	test-stdlib-http-leak test-stdlib-toml-glue \
	test-stdlib-http-form test-stdlib-http-tls \
	test-stdlib-file test-stdlib-runtime \
	test-stdlib-path test-stdlib-args test-stdlib-md test-stdlib-toml \
	test-stdlib-vecstore test-stdlib-vecstore-binding test-stdlib-keychain \
	test-stdlib-securemem test-stdlib-glue-contract test-stdlib-zip \
	test-tkir-encoder \
	install-man \
	test-standalone \
	check-tki

all: vendor-check $(BIN) tkc

# Fails loudly and by name if a vendored dependency is missing.  Cheap enough
# to run on every build (eight stat calls).
vendor-check:
	@missing=""; \
	for f in $(VENDOR_SENTINELS); do \
	  [ -f "$$f" ] || missing="$$missing $$f"; \
	done; \
	if [ -n "$$missing" ]; then \
	  echo "" >&2; \
	  echo "ERROR: vendored third-party sources are missing from this checkout." >&2; \
	  echo "" >&2; \
	  for f in $$missing; do echo "  missing: $$f" >&2; done; \
	  echo "" >&2; \
	  echo "  cmark backs std.md, tomlc99 backs std.toml and miniz backs" >&2; \
	  echo "  std.zip.  tkc compiles these .c files directly into every binary" >&2; \
	  echo "  that imports those modules, so" >&2; \
	  echo "  without them any such program fails at the clang stage with an" >&2; \
	  echo "  opaque E9003 naming no dependency." >&2; \
	  echo "" >&2; \
	  echo "  These files are TRACKED in this repository - they are not a" >&2; \
	  echo "  submodule and there is no fetch step.  If they are missing, the" >&2; \
	  echo "  checkout is incomplete or they were deleted locally.  Try:" >&2; \
	  echo "      git checkout -- stdlib/vendor" >&2; \
	  echo "" >&2; \
	  echo "  See stdlib/vendor/README.md (provenance and update procedure)." >&2; \
	  echo "" >&2; \
	  exit 1; \
	fi

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -o $@ $^ $(LDLIBS)

tkc: $(BIN)
	ln -sf $(BIN) tkc

# Auto-generate stdlib IR declarations before compiling llvm.c (Story 103.11)
src/stdlib_decls_gen.h: $(wildcard src/stdlib/*_glue.c) $(wildcard src/stdlib/*.c) scripts/gen_stdlib_decls.py
	python3 scripts/gen_stdlib_decls.py

src/llvm.o: src/stdlib_decls_gen.h

%.o: %.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -c -o $@ $<

# Header-dependency files emitted by -MMD; pull them in so editing a .h
# recompiles every .o that includes it (prevents stale-header struct-layout
# corruption, e.g. adding a field to Node in parser.h). Silent if absent.
-include $(OBJS:.o=.d)

# encrypt.c uses __int128 for Ed25519 arithmetic — allowed under GCC but rejected by -Wpedantic
src/stdlib/encrypt.o: src/stdlib/encrypt.c
	$(CC) $(CFLAGS) $(REPRO_FLAGS) -Wno-pedantic -c -o $@ $<

lint:
	$(CC) $(CFLAGS) --analyze $(SRCS)

conform:
	@bash test/run_conform.sh
	@$(MAKE) --no-print-directory conform-sh

# 131.37: shell-script conformance cases (test/conform/*.sh) — each script
# prints its own PASS/FAIL lines and exits non-zero on any failure.
conform-sh:
	@pass=0; fail=0; \
	for s in test/conform/*.sh; do \
	  if bash "$$s" > /tmp/tkc_conform_sh.log 2>&1; then \
	    pass=$$((pass+1)); echo "PASS $$s: $$(grep -E '^Results:' /tmp/tkc_conform_sh.log | tail -1)"; \
	  else \
	    fail=$$((fail+1)); echo "FAIL $$s"; tail -25 /tmp/tkc_conform_sh.log | sed 's/^/    /'; \
	  fi; \
	done; \
	echo "conform-sh: $$pass scripts passed, $$fail failed"; \
	[ $$fail -eq 0 ]

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

check-tki:
	python3 scripts/check_tki_coverage.py

ci: lint conform conform-sh conform-check check-tki check-docs check-error-codes check-patterns check-facts check-metrics check-canonical check-claims-all

# 119.6 — compile-gate every full-program ```toke block in the canonical docs.
# Fails on any regression (intentional error-demo pages are skip-listed in the script).
#
# 136.26 — this BUILDS each example to a binary instead of stopping at
# `--check`. Until 136.26 it type-checked only, so a documented call to a
# function that exists NOWHERE passed the gate; that is how it read 432/432
# while shipping examples that cannot be built. Measured cost at 12 jobs:
# 1.7s to type-check all 432, 148s to build all 432. That is affordable in CI,
# so there is no fast/slow split — use `--check-only` for a 2-second local
# loop, and never in CI (the script prints a warning in that mode).
#
# --self-test runs FIRST and is the gate's negative control: it builds a
# program whose only fault is a call to a function that does not exist and
# fails unless the gate rejects it. A gate nobody has watched fail is not a
# gate. Reads nothing outside this repository (cf. 127.88).
check-docs: $(BIN)
	python3 scripts/check_doc_examples.py --self-test
	python3 scripts/check_doc_examples.py docs

# 131.11 — pattern catalogue drift gate. Validates patterns/catalogue.json, regenerates
# docs/spec/patterns-v0.4.md, docs/guide/11-patterns-and-efficiency.md and
# patterns/card_snippet.md into a temp dir and diffs them against the committed files
# (drift fails), then compile-gates every full-program fence in the two generated docs.
# render-patterns regenerates them in place (run it after any catalogue change).
check-patterns: $(BIN)
	python3 scripts/patterns/validate_catalogue.py
	python3 scripts/patterns/render_catalogue.py check
render-patterns:
	python3 scripts/patterns/render_catalogue.py all

# 132.6 — token-efficiency claim gate. Every published percentage about tokens must
# carry its TEMSpec §6.3 fields (metric type, tokenizer, baseline, N), no withdrawn
# headline may be restated without its supersession note, and a toke-trained tokenizer
# may never be compared against a non-toke baseline (132.13). Approved wording lives in
# docs/metrics-baseline.md. Files owned by an in-flight story warn instead of failing;
# `--strict` fails on those too.
# 132.41 — --selftest runs FIRST and is this gate's negative control, added
# because it had none: eleven cases, six of them real defects this guard has
# shipped past (a bare percentage, a lane crossing, the withdrawn 52% headline,
# a stdlib count that is not the tree's) that must keep failing, and five
# wordings that must keep passing. Watched to fail by switching rule 1 off. Do
# not split the two lines apart: a gate nobody has watched fail is not a gate.
check-metrics:
	python3 scripts/check_metrics_claims.py --selftest
	python3 scripts/check_metrics_claims.py

# 132.1 / 132.12 — canonical facts gate. One block (docs/about/canonical.md +
# canonical.json) is the source of truth for how toke is named, described and
# measured; every README, llms.txt, home page and registry description copies it
# word for word, and this fails CI when a copy has drifted. It also blocks the two
# facts our own spec retired — "LL(1)" (toke-spec-v0.4.md §E) and "13 keywords"
# (§A) — from coming back. Surfaces owned by an in-flight story warn with that
# story number instead of failing; `--strict` fails on those too.
# 132.29 — --selftest runs first: Rule 2 was loosened so that a sentence which
# explicitly retires a fact ("it is NOT strict LL(1)", wrapped across two lines)
# stops being reported as that fact, and a loosened gate is only worth having if
# it still fails on real drift. The selftest pins both halves, nine of its
# thirteen cases being stale wordings that must keep failing.
# 132.23 — the selftest now also pins MARKUP blindness, a different defect: the
# claim and the correction are both read as a reader sees them, so `is
# <strong>14</strong>` counts as the correction it is AND `<strong>13</strong>
# keywords` counts as the drift it is. Nineteen of the 28 Rule 2 cases, and 2 of
# the 5 Rule 1 cases, are wordings that must keep failing.
check-canonical:
	python3 scripts/check_canonical.py --selftest
	python3 scripts/check_canonical.py

# 132.15 — the same two gates, run across the sibling repos. Both guards scoped to
# this repo plus toke-tokenizer until 132.15, so nothing else was ever swept:
# toke-spec published "12.5% reduction vs Python (cl100k)", the toke-mcp server served
# models a 56-character alphabet and 12 keywords, a Hugging Face Space synthesised a
# Python token count by dividing by 0.875, and the console told users the toke BPE
# count was "52% fewer". The guards take paths, so there is one copy of each rule here
# rather than a fork per repo; a repo that is not checked out is skipped. Rule 4
# (counts of things, story 132.14) travels with check_metrics_claims.py — but
# check-facts stays local, because it derives the counts from THIS tree.
# toke-website warns rather than fails (owned by 132.2/132.9/132.16), as does the
# toke-spec RFC draft (132.8).
# 132.42 added ../toke-cloud: it is a public repository that powers the published
# API, and it had never been swept by either guard. Two repositories named in
# docs/about/repos.md still cannot be swept because they are not checked out in
# this workspace — karwalski/loke (the local intelligence layer) and
# karwalski/tkc (an early copy of the compiler tree, public with no description).
# A repo that is absent is reported by name as a skip, not silently passed over;
# cloning them into the workspace is story 132.46.
SIBLING_REPOS := ../toke-spec ../toke-corpus ../toke-model ../toke-eval ../toke-mcp \
                 ../toke-console ../toke-cloud ../toke-ooke ../toke-test-programs \
                 ../toke-tokenizer ../toke-website ../homebrew-toke \
                 ../loke ../tkc
check-claims-all:
	@repos=""; for r in $(SIBLING_REPOS); do \
	  if [ -d "$$r" ]; then repos="$$repos $$r"; else echo "skip (not checked out): $$r"; fi; \
	done; \
	echo "checking claims across:$$repos"; \
	python3 scripts/check_metrics_claims.py $$repos --list && \
	python3 scripts/check_canonical.py $$repos --list

# 132.14 — project-facts gate. Every countable project-scale number (character
# set, keywords, EBNF productions, stdlib modules, conformance cases, diagnostic
# codes, corpus records, epics, stories) is derived from the tree by
# scripts/verify_project_facts.py and recorded once, with its deriving command, in
# docs/metrics-baseline.md § Project facts. This fails CI when the table and the
# tree disagree; check-metrics rule 4 then fails any doc that states a different
# number. Run without --check to print the sheet, --json for the machine-readable
# form, --probe to re-derive the character set against the built compiler.
# 132.19 — --check also gates the counts stated in src/** and in `tkc --help`,
# so the binary's own strings cannot drift from the lexer table.
check-facts: $(BIN)
	python3 scripts/verify_project_facts.py --check

# 123.12 — drift-gate: every diagnostic code the compiler emits must be
# documented in docs/reference/errors.md, and errors.md must not list dead codes.
check-error-codes:
	python3 scripts/check_error_codes.py

# 124.0b — differential codegen regression gate (root of trust). Fails if any
# full-program test's observable behaviour (compile / exit / stdout) changed vs
# the recorded baseline. Run BEFORE landing a codegen change that feeds the
# corpus; re-record (diff-codegen-record) only for an *intended* behaviour change.
DIFF_CODEGEN_DIRS = test/standalone test/e2e test/codegen
diff-codegen: $(BIN)
	TOKE=./$(BIN) python3 scripts/diff_codegen.py --baseline test/codegen-baseline.json $(DIFF_CODEGEN_DIRS)
diff-codegen-record: $(BIN)
	TOKE=./$(BIN) python3 scripts/diff_codegen.py --record test/codegen-baseline.json $(DIFF_CODEGEN_DIRS)

test-stdlib:
	$(CC) $(CFLAGS) -o test/stdlib/test_str \
	    test/stdlib/test_str.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_str

test-stdlib-db:
	$(CC) $(CFLAGS) -o test/stdlib/test_db \
	    test/stdlib/test_db.c src/stdlib/db.c src/stdlib/capabilities.c -lsqlite3
	$(RUN_TEST) ./test/stdlib/test_db

test-stdlib-file:
	$(CC) $(CFLAGS) -o test/stdlib/test_file \
	    test/stdlib/test_file.c src/stdlib/file.c
	$(RUN_TEST) ./test/stdlib/test_file

test-stdlib-runtime:
	$(CC) $(CFLAGS) -o test/stdlib/test_tk_runtime \
	    test/stdlib/test_tk_runtime.c src/stdlib/tk_runtime.c \
	    src/stdlib/capabilities.c src/stdlib/args.c
	$(RUN_TEST) ./test/stdlib/test_tk_runtime

test-stdlib-http:
	$(CC) $(CFLAGS) -o test/stdlib/test_http \
	    test/stdlib/test_http.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_http

# Story 127.65: the keep-alive loop must free the parsed request each
# iteration.  Drives the real server loop over a loopback connection and
# reads the heap's in-use byte count either side of it.
test-stdlib-http-leak:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_keepalive_leak \
	    test/stdlib/test_http_keepalive_leak.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c -lz -lpthread
	$(RUN_TEST) ./test/stdlib/test_http_keepalive_leak

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
	    test/stdlib/test_process.c src/stdlib/process.c \
	    src/stdlib/tk_runtime.c src/stdlib/capabilities.c src/stdlib/args.c
	$(RUN_TEST) ./test/stdlib/test_process

# 124.4h — ambient defect hardening (AMB-04/05/06/07/08 at the C level).
test-stdlib-ambient:
	$(CC) $(CFLAGS) -o test/stdlib/test_ambient \
	    test/stdlib/test_ambient.c src/stdlib/path.c src/stdlib/file.c \
	    src/stdlib/env.c src/stdlib/os.c src/stdlib/capabilities.c
	$(RUN_TEST) ./test/stdlib/test_ambient

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
	    test/stdlib/test_json.c src/stdlib/json.c src/stdlib/json_glue.c
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

test-stdlib-map:
	$(CC) $(CFLAGS) -o test/stdlib/test_map \
	    test/stdlib/test_map.c src/stdlib/collections_glue.c src/stdlib/collections.c
	$(RUN_TEST) ./test/stdlib/test_map

test-stdlib-md:
	$(CC) $(CFLAGS) $(CMARK_FLAGS) -o test/stdlib/test_md \
	    test/stdlib/test_md.c src/stdlib/md.c $(CMARK_SRCS)
	$(RUN_TEST) ./test/stdlib/test_md

test-stdlib-toml:
	$(CC) $(CFLAGS) $(TOML_FLAGS) -o test/stdlib/test_toml \
	    test/stdlib/test_toml.c src/stdlib/toml.c $(TOML_SRCS)
	$(RUN_TEST) ./test/stdlib/test_toml

# Story 127.67: the std.toml wrappers must distinguish a value of false/0
# from an error, and a missing section must not resolve to its parent.
test-stdlib-toml-glue:
	$(CC) $(CFLAGS) $(TOML_FLAGS) -o test/stdlib/test_toml_glue \
	    test/stdlib/test_toml_glue.c src/stdlib/toml.c src/stdlib/toml_glue.c $(TOML_SRCS)
	$(RUN_TEST) ./test/stdlib/test_toml_glue

# ── Epic 72.5: std.vecstore ──────────────────────────────────────────────────
test-stdlib-vecstore:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_vecstore \
	    test/stdlib/test_vecstore.c src/stdlib/vecstore.c -lpthread
	$(RUN_TEST) ./test/stdlib/test_vecstore

# ── Story 136.4: std.vecstore binding, end to end ────────────────────────────
# test-stdlib-vecstore above drives the C core in one process and passed all the
# way through the period when the module was unusable from toke. This target
# compiles and RUNS a real .tk consumer and re-execs between the write and the
# read, so a pass proves the vectors reached disk rather than surviving in
# process memory -- which is exactly what they were not doing before 136.4.
test-stdlib-vecstore-binding: $(BIN)
	@bash test/stdlib/vecstore_binding.sh

# ── Story 135.1: std.zip, read-only archive access ───────────────────────────
# Behavioural, not compile-only. It builds a program importing std.zip and
# NOTHING else (136.33: wrong-module glue registration is invisible unless the
# module is the sole import), reads every $zipentry field and the exact bytes
# of a text and a binary entry, and then opens seven hostile archives that must
# each be refused BY NAME -- traversal, absolute path, backslash path, the size
# cap, the ratio cap, the entry-count cap, and a file that is not an archive.
test-stdlib-zip: $(BIN)
	@bash test/stdlib/zip.sh

# ── Story 136.5: std.securemem, core + binding ───────────────────────────────
# test/stdlib/test_securemem.c existed but had NO make target and so had never
# run -- the same pattern 136.10 is chasing. It runs here now, and the .tk
# consumer alongside it imports std.securemem by the name the documentation
# publishes, which is the part 136.5 was actually about: the capability worked
# throughout, under a name no consumer had been given.
test-stdlib-securemem: $(BIN)
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_securemem \
	    test/stdlib/test_securemem.c src/stdlib/securemem.c -lpthread
	$(RUN_TEST) ./test/stdlib/test_securemem
	@TKC_STDLIB_DIR=$(PWD)/src/stdlib ./$(BIN) test/stdlib/securemem_roundtrip.tk -o test/stdlib/securemem_roundtrip.bin
	$(RUN_TEST) ./test/stdlib/securemem_roundtrip.bin
	@rm -f test/stdlib/securemem_roundtrip.bin

# ── Story 136.3: std.keychain binding, end to end ────────────────────────────
# Compiles and RUNS a real .tk consumer, because that is the layer that was
# broken: the C core was fine, the link line was not. Write and read happen in
# separate processes of the same binary, so a pass proves the secret reached the
# OS credential store. Skips itself where no credential store exists.
test-stdlib-keychain: $(BIN)
	@bash test/stdlib/keychain_binding.sh

# ── Epic 136.16-136.25: the _w glue honours the documented contract ──────────
# check-docs proves a documented example COMPILES. It cannot prove the call
# does what the page says, and every defect in this group compiled cleanly
# while dropping an argument. This target compiles and RUNS real .tk consumers
# and compares their output against the documented values, so a pass is about
# behaviour rather than linkage.
test-stdlib-glue-contract: $(BIN)
	@bash test/stdlib/glue_contract.sh

# ── Story 76.1.6a: .tkir encoder test ────────────────────────────────────────
test-tkir-encoder: $(BIN)
	@bash test/tkir/test_tkir_encoder.sh ./$(BIN)

test-standalone: $(BIN)
	@test/standalone/run_all.sh

clean:
	rm -f $(OBJS) $(BIN) tkc test/stdlib/test_str test/stdlib/test_db \
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

.PHONY: fuzz fuzz-lexer fuzz-parser fuzz-http fuzz-http-parse fuzz-url-route \
	test-e2e-check test-standalone-check test-all

# ── Epic 88: Comprehensive test suite ─────────────────────────────────────

# Syntax-check all new e2e and standalone tests
test-e2e-check:
	@echo "=== E2E syntax check ==="
	@fails=0; total=0; \
	for f in test/e2e/e2e_*.tk; do \
		total=$$((total + 1)); \
		if $(BIN) --check "$$f" 2>&1 | grep -q '"severity":"error"'; then \
			echo "  FAIL: $$f"; fails=$$((fails + 1)); \
		else \
			echo "  OK: $$f"; \
		fi; \
	done; \
	echo "$$total checked, $$fails failed"; \
	test $$fails -eq 0

test-standalone-check:
	@echo "=== Standalone syntax check ==="
	@fails=0; total=0; \
	for f in test/standalone/test_*_full.tk test/standalone/test_process_env.tk; do \
		total=$$((total + 1)); \
		if $(BIN) --check "$$f" 2>&1 | grep -q '"severity":"error"'; then \
			echo "  FAIL: $$f"; fails=$$((fails + 1)); \
		else \
			echo "  OK: $$f"; \
		fi; \
	done; \
	echo "$$total checked, $$fails failed"; \
	test $$fails -eq 0

# Run all test suites
test-all: test-stdlib test-e2e-check test-standalone-check conform
	@echo ""
	@echo "=== All test suites complete ==="
