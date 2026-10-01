CC      = cc
CFLAGS  = -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -g \
          -DTKC_STDLIB_DIR='"$(CURDIR)/src/stdlib"' \
          -MMD -MP   # emit .d header-dependency files so header changes trigger recompiles
# GCC-specific: suppress format-truncation warnings (snprintf truncation is by design)
GCC_CHECK := $(shell $(CC) -Wno-format-truncation -x c -c /dev/null -o /dev/null 2>/dev/null && echo yes)
ifeq ($(GCC_CHECK),yes)
CFLAGS += -Wno-format-truncation
endif

# ── Story 123.9 (folds 122.1): exploit-mitigation flags ──────────────────────
#
# The shipping build carried NONE of these until 2026-09-24.  Searching this
# Makefile and .github/workflows/ for FORTIFY, fsanitize or stack-protector
# matched exactly one line before this change, and it was the fuzz harness.
#
# READ THIS BEFORE QUOTING ANY OF THESE FLAGS AS PROTECTION.  Two of them are
# macros or defaults rather than switches, and can sit here doing nothing:
#
#   -D_FORTIFY_SOURCE=3   does nothing without optimisation on glibc (the
#                         checks are built from __builtin_object_size(), which
#                         folds to "unknown" at -O0), and does nothing AT ALL
#                         on macOS, whose SDK implements levels 1 and 2, sets
#                         the default to 2 itself, and has no level 3.
#                         MEASURED 2026-09-24 on Apple clang 21 / macOS 15: a
#                         16-byte memcpy into an 8-byte HEAP buffer runs to
#                         completion under `-O2 -D_FORTIFY_SOURCE=3`.
#
#   -fstack-protector-strong  the compiler default already guards SOME
#                         functions, so the presence of __stack_chk_fail in
#                         the binary proves nothing about the level reached.
#
# `make check-hardening` therefore does not grep for these strings.  It
# compiles behavioural probes with the real $(CFLAGS), runs its own negative
# controls first, and reports the fortify LEVEL actually enforced rather than
# the level requested.  Do not report "hardened build" from a host where that
# gate has not run.
#
# -O2 is part of the mitigation set, not a performance choice: without it
# _FORTIFY_SOURCE is inert.  It replaces nothing -- -g stays.
HARDEN_CFLAGS = -O2 -D_FORTIFY_SOURCE=3 -fstack-protector-strong \
                -fno-common -Wformat -Wformat-security -fPIE

# Probe each remaining mitigation rather than assuming the toolchain has it:
# -fstack-clash-protection is GCC/clang-on-Linux, -fcf-protection is x86-only,
# and -Wl,-z,relro is ELF-only (Apple's ld rejects it outright).  A flag the
# toolchain silently ignores is the exact failure mode this block is about, so
# anything not accepted here is simply absent, and check-hardening reports
# which mitigations the binary actually ended up with.
#
# The probe compiles a NON-EMPTY translation unit under -Werror, and both
# halves of that are load-bearing.  The obvious probe -- the one GCC_CHECK
# above uses, `$(CC) $(flag) -x c -c /dev/null` -- accepted
# -fstack-clash-protection on Apple clang 21 and then every real compile died
# with `argument unused during compilation: '-fstack-clash-protection'
# [-Werror,-Wunused-command-line-argument]`.  clang only warns about an unused
# codegen flag when there is code to generate, so an empty TU cannot tell an
# accepted flag from an ignored one.  Caught 2026-09-24 by this probe turning
# 29 working test targets red.
harden_cc_probe = $(shell printf 'int tkp(void);int tkp(void){char a[4];a[0]=1;return a[0];}\n' \
                    | $(CC) -Werror $(1) -x c -c - -o /dev/null >/dev/null 2>&1 && echo $(1))
HARDEN_CFLAGS += $(call harden_cc_probe,-fstack-clash-protection)
HARDEN_CFLAGS += $(call harden_cc_probe,-fcf-protection=full)

# $(call) splits its arguments on commas, so `$(call f,-Wl,-z,relro)` passes
# THREE arguments and $(1) is the string "-Wl".  Measured here before this
# comma variable existed: HARDEN_LDFLAGS came out as `-pie -Wl -Wl`, which
# links fine and hardens nothing.  Every literal comma below goes through
# $(COMMA) for that reason.
COMMA := ,
# -Werror in the probe, for the same reason as harden_cc_probe above and
# caught the same way: Apple's ld makes every binary position-independent
# already, so `-pie` LINKS FINE and then warns `argument unused during
# compilation: '-pie' [-Wunused-command-line-argument]`, which the real build
# promotes to an error. A probe that only checks the exit status accepts a
# flag the build cannot use. Third occurrence of this shape in one afternoon;
# if you add a probe here, put -Werror in it.
#
# 136.41: a private mktemp directory per probe, not the fixed /tmp/tkc_ldprobe.
# This probe runs whenever ANY make in ANY worktree parses this file, so two
# concurrent makes shared one path: one's `rm` could delete the other's source
# mid-probe, the probe failed, and the flag silently dropped out of the build
# (and out of TKC_BUILD_FLAGS, forcing a full rebuild on the next run).
harden_ld_probe = $(shell d=$$(mktemp -d "$${TMPDIR:-/tmp}/tkc_ldprobe.XXXXXX") && \
                    printf 'int main(void){return 0;}\n' > "$$d/p.c" && \
                    $(CC) -Werror $(1) -o "$$d/p" "$$d/p.c" >/dev/null 2>&1 && echo '$(1)'; \
                    [ -n "$$d" ] && rm -rf "$$d")
HARDEN_LDFLAGS = $(call harden_ld_probe,-pie)
HARDEN_LDFLAGS += $(call harden_ld_probe,-Wl$(COMMA)-z$(COMMA)relro)
HARDEN_LDFLAGS += $(call harden_ld_probe,-Wl$(COMMA)-z$(COMMA)now)

# HARDEN=0 turns the set off for one build.  It exists so check-hardening has
# something to be run against as a control, and for bisecting a miscompile at
# -O2.  It is not a supported way to ship.
HARDEN ?= 1
ifeq ($(HARDEN),1)
CFLAGS += $(HARDEN_CFLAGS)
else
HARDEN_LDFLAGS =
endif

# ── How a sub-make changes the build flags, and why not with CFLAGS= ────────
#
# CFLAGS contains -DTKC_STDLIB_DIR='"$(CURDIR)/src/stdlib"' -- single quotes
# around double quotes -- and a `$(MAKE) CFLAGS="$(CFLAGS) ..."` round trip
# loses the inner pair. The sub-make then compiles
# `#define TKC_STDLIB_DIR /path/to/src/stdlib`, which is not a string literal,
# and src/names.c fails with 11 errors that name neither the flag nor the
# quoting. Hit while wiring up `make coverage` on 2026-09-24; it is the same
# hazard the TKC_BUILD_FLAGS comment below describes, arriving from the other
# direction.
#
# So the sanitizer and coverage builds APPEND through EXTRA_CFLAGS and turn
# the hardening set off with HARDEN=0, and CFLAGS is never rewritten by a
# recursive make.
CFLAGS += $(EXTRA_CFLAGS)

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
          src/sourcemap.c src/ast_json.c src/migrate.c src/companion.c src/sha256.c src/compress.c \
          src/stdlib_deps.c src/stdlib_tables.c src/glue_gen.c src/lint.c src/tkir.c \
          src/regex_check.c src/main.c src/stdlib/str.c

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

# ── Story 135.3a: what gets STATICALLY ANALYSED is not what gets LINKED ──────
#
# $(SRCS) is the LINK set for tkc: the compiler plus the 28-module curated
# subset above.  Using it as the ANALYSIS set left the other 104 files in
# src/stdlib/ analysed by nothing and compiled under -Werror by nothing --
# tkc builds them at PROGRAM link time, host-only, with clang's default
# warning level.  Six of them do not survive -Wall -Wextra -Wpedantic -Werror
# even on the host; the #else branch of every platform conditional in them is
# built by no gate on any platform.  (C033 compiles exactly one such file,
# src/stdlib/ocr.c, with -U__APPLE__, and says so in its own comment.)
#
# 127.99's rule -- the module manifest in src/stdlib_deps.c states what the
# standard library IS, never a directory glob -- governs the LINK set and is
# unchanged.  A glob is the right thing HERE and only here: analysis coverage
# should include every file present, including one the manifest has not
# reached yet.  A file this glob picks up that the manifest does not name is
# a file nothing ships, which T004 already catches.
# 136.64: src/wasm_api.c (the browser build's entry point, Makefile.wasm) is
# in neither set otherwise; it is analysed with -DTKC_WASM_BUILD (the script's
# EXTRA_FLAGS), since without it the file is a one-typedef stub.
ANALYZE_SRCS = $(sort $(SRCS) $(wildcard src/stdlib/*.c) src/wasm_api.c)
# 127.250 (3): every header is also analysed as a translation unit of its own
# -- the analyzer reports only on a TU's main file, so a header's inline code
# was checked through no .c file, and a header nothing includes was not even
# compiled (tkc_caps.h's null dereferences were found only that way).  The
# four src/stdlib_*_gen.h files are excluded: they are generated initializer
# FRAGMENTS, #included inside an array's braces (stdlib_tables.c, types.c),
# not headers, and hold data only.  src/glue_gen.h is a header and stays.
ANALYZE_HDRS = $(sort $(filter-out src/stdlib_%_gen.h,$(wildcard src/*.h src/stdlib/*.h)))

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
# and __TIME__.  CI pins it to the commit timestamp.
#
# 136.41: it is passed to the COMPILER BUILD ONLY ($(BUILD_ENV) on the three
# recipes that make $(BIN) and its objects), not exported.  It used to be
# `export SOURCE_DATE_EPOCH ?= 0`, which put 0 into the environment of every
# recipe in this file -- every test target, every conformance suite -- and
# anything else that honours the variable read it as "the time is 1970".
# Python 3.14's zipfile does, and 1970 is below the 1980 DOS epoch a zip
# header can hold, so C007's and C031's fixture builders died under `make`
# and passed by hand (131.79).  Those suites now pin their own stamps; this
# stops the next consumer being exposed the same way.
# ──────────────────────────────────────────────────────────────────────────────
REPRO_FLAGS = -frandom-seed=toke \
              -ffile-prefix-map=$(CURDIR)=.

SOURCE_DATE_EPOCH ?= 0
BUILD_ENV = SOURCE_DATE_EPOCH=$(SOURCE_DATE_EPOCH)

# Portable wall-clock timeout for test binaries (no GNU coreutils needed).
# Uses perl alarm() which survives exec; SIGALRM default action kills the process.
# Override with e.g.  make RUN_TEST_TIMEOUT=60 test-stdlib-encrypt
RUN_TEST_TIMEOUT ?= 180
RUN_TEST = $(CURDIR)/test/run_test.sh $(RUN_TEST_TIMEOUT)

.PHONY: all vendor-check clean lint lint-nonhost lint-file lint-report conform conform-sh conform-check build-all ci check-docs check-doc-patterns render-tki-quarantine render-stdlib-doc-quarantine render-stdlib-internal check-tki-names check-index check-index-all check-patterns render-patterns check-error-codes check-glue-core check-core-reach check-parallel-stdlib check-metrics check-canonical check-claims-all clean-cross diff-codegen diff-codegen-record test-codegen test-e2e test-companion test-companion-diff test-migrate verify-ir stress test-stdlib test-stdlib-process test-stdlib-ambient test-stdlib-env test-stdlib-crypto test-stdlib-auth test-stdlib-time test-stdlib-test test-stdlib-log test-stdlib-coverage test-stdlib-dataframe test-stdlib-analytics bench repro-check test-compress test-compress-stream test-compress-schema \
	test-stdlib-encoding test-stdlib-encrypt test-stdlib-ws test-stdlib-sse test-stdlib-router \
	test-stdlib-template test-stdlib-csv test-stdlib-math test-stdlib-llm test-stdlib-llm-tool \
	test-stdlib-chart test-stdlib-html test-stdlib-dashboard test-stdlib-svg test-stdlib-canvas \
	test-stdlib-image test-stdlib-ml \
	test-stdlib-all-new \
	test-stdlib-security-integration test-stdlib-network-integration \
	test-stdlib-viz-integration test-stdlib-data-pipeline test-stdlib-llm-live \
	test-stdlib-http test-stdlib-http-cookies test-stdlib-http-multipart \
	test-stdlib-http-leak test-stdlib-toml-glue \
	test-stdlib-http-form test-stdlib-http-tls test-stdlib-tls \
	test-stdlib-file test-stdlib-runtime \
	test-stdlib-path test-stdlib-args test-stdlib-md test-stdlib-toml \
	test-stdlib-vecstore test-stdlib-vecstore-binding test-stdlib-keychain \
	test-stdlib-securemem test-stdlib-glue-contract test-stdlib-zip \
	test-tkir-encoder test-tkir-reader \
	install-man \
	test-standalone \
	check-tki \
	sbom cve-scan

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
	$(BUILD_ENV) $(CC) $(CFLAGS) $(REPRO_FLAGS) -o $@ $^ $(HARDEN_LDFLAGS) $(LDLIBS)

tkc: $(BIN)
	ln -sf $(BIN) tkc

# Auto-generate stdlib IR declarations before compiling llvm.c (Story 103.11)
src/stdlib_decls_gen.h src/stdlib_dummyarg_gen.h src/stdlib_paramkind_gen.h: $(wildcard src/stdlib/*_glue.c) $(wildcard src/stdlib/*.c) scripts/gen_stdlib_decls.py
	python3 scripts/gen_stdlib_decls.py

# 135.21: the tables that include them moved to stdlib_tables.c
src/stdlib_tables.o: src/stdlib_decls_gen.h src/stdlib_dummyarg_gen.h
# 127.201: the checker reads each glue wrapper's parameter kinds.
src/types.o: src/stdlib_paramkind_gen.h
# 127.224: and the quarantined exports no C source implements.
src/stdlib_quarantine_gen.h: scripts/check_tki_skiplist.txt $(wildcard src/stdlib/*.c) $(wildcard stdlib/*.tki) scripts/gen_tki_quarantine_h.py scripts/check_tki_coverage.py
	python3 scripts/gen_tki_quarantine_h.py
src/types.o: src/stdlib_quarantine_gen.h

%.o: %.c
	$(BUILD_ENV) $(CC) $(CFLAGS) $(REPRO_FLAGS) -c -o $@ $<

# Header-dependency files emitted by -MMD; pull them in so editing a .h
# recompiles every .o that includes it (prevents stale-header struct-layout
# corruption, e.g. adding a field to Node in parser.h). Silent if absent.
-include $(OBJS:.o=.d)

# ── Story 136.62: an object must remember the flags it was built with ───────
#
# -MMD made a .o depend on its headers.  Nothing made a .o depend on the
# FLAGS, so changing them invalidated nothing and `make` reported a build it
# had not done.  This is the mechanism behind 127.118 and 127.92 ("incremental
# `make` produces a compiler that behaves like neither commit").
#
# Measured before this rule existed:
#
#   * `make CFLAGS="<host flags> -DW3_FLAG_PROBE=1"` on an up-to-date tree ran
#     ZERO compile commands, left every .o byte-identical, and exited 0.
#
#   * `make build-all` -- three recursive makes for three different
#     --target triples -- finished in 0.74s having run ZERO compile commands
#     and exited 0.  Each sub-make found the HOST objects newer than their
#     sources and did nothing.  AGENTS.md §14 asks contributors to report
#     "make build-all: success on x86-64 and ARM64"; that is what "success"
#     has meant.
#
# The fix is the standard one: write the flags to a file, make the objects
# depend on that file.  The stamp is rewritten only when the text CHANGES
# (cmp), so an unchanged build stays incremental.
#
# TKC_BUILD_FLAGS goes through the ENVIRONMENT rather than being interpolated
# into the recipe because CFLAGS contains both single and double quotes
# (-DTKC_STDLIB_DIR='"..."'), and every way of quoting that into a shell
# command line mangles it.  A make variable defined in the makefile beats the
# same name inherited from the environment, so each recursive sub-make of
# build-all recomputes this from ITS OWN CFLAGS, which is the whole point.
BUILD_FLAGS_STAMP = .build-flags
export TKC_BUILD_FLAGS = $(CC)|$(CFLAGS)|$(REPRO_FLAGS)|$(HARDEN_LDFLAGS)|$(LDLIBS)|$(SOURCE_DATE_EPOCH)

.PHONY: FORCE
FORCE:

$(BUILD_FLAGS_STAMP): FORCE
	@printf '%s\n' "$$TKC_BUILD_FLAGS" | cmp -s - $@ 2>/dev/null \
	  || { printf '%s\n' "$$TKC_BUILD_FLAGS" > $@; \
	       echo "build flags changed -- rebuilding all objects"; }

$(OBJS): $(BUILD_FLAGS_STAMP)

# ── Story 131.33: the compiler states the commit it was built from ─────────
#
# Provenance stamps took the repo HEAD at RUN time, so a binary built three
# commits ago was stamped with a commit it does not contain.  The commit and a
# dirty flag are now baked in at build time and `tkc --version` (and
# `--version --json`) print them.
#
#   TKC_GIT_SHA    12 hex digits of HEAD, read only when this directory is
#                  the TOP of a git checkout (a release tarball unpacked
#                  inside some other repository must not take that repo's
#                  commit).  Outside a checkout, or when it is not hex, it is
#                  "unknown".  Pass TKC_GIT_SHA=<sha> to state it by hand.
#   TKC_GIT_DIRTY  1 when a tracked file under src/, the Makefile or VERSION
#                  differs from HEAD -- the inputs of the binary; 0 otherwise.
#
# Reproducible: no time, host or path enters it, so the same tree builds the
# same binary (repro-check).  The header is rewritten only when its text
# changes (cmp, as the 136.62 flags stamp), so an unchanged tree stays
# incremental and a new commit recompiles main.o alone.
BUILD_INFO_H = src/build_info.h
ifeq ($(origin TKC_GIT_SHA),undefined)
TKC_GIT_SHA := $(shell [ "$$(git rev-parse --show-toplevel 2>/dev/null)" = "$$(pwd -P)" ] && git rev-parse --short=12 HEAD 2>/dev/null)
endif
ifeq ($(origin TKC_GIT_DIRTY),undefined)
TKC_GIT_DIRTY := $(if $(TKC_GIT_SHA),$(shell git status --porcelain --untracked-files=no -- src Makefile VERSION 2>/dev/null | grep -q . && echo 1 || echo 0),0)
endif
export TKC_GIT_SHA TKC_GIT_DIRTY

$(BUILD_INFO_H): FORCE
	@sha=$$(printf '%s' "$$TKC_GIT_SHA" | tr -d '[:space:]'); \
	 case "$$sha" in ''|*[!0-9a-f]*) sha=unknown ;; esac; \
	 case "$$sha:$$TKC_GIT_DIRTY" in unknown:*) d=0 ;; *:1) d=1 ;; *) d=0 ;; esac; \
	 { printf '/* generated by make (131.33) -- do not edit, not tracked */\n'; \
	   printf '#define TKC_GIT_SHA "%s"\n' "$$sha"; \
	   printf '#define TKC_GIT_DIRTY %s\n' "$$d"; } > $@.tmp; \
	 if cmp -s $@.tmp $@ 2>/dev/null; then rm -f $@.tmp; else mv $@.tmp $@; fi

src/main.o: $(BUILD_INFO_H)

# encrypt.c uses __int128 for Ed25519 arithmetic — allowed under GCC but rejected by -Wpedantic
src/stdlib/encrypt.o: src/stdlib/encrypt.c $(BUILD_FLAGS_STAMP)
	$(BUILD_ENV) $(CC) $(CFLAGS) $(REPRO_FLAGS) -Wno-pedantic -c -o $@ $<

# ── 135.3a ──────────────────────────────────────────────────────────────────
# `lint` used to be one line:
#
#     $(CC) $(CFLAGS) --analyze $(SRCS)
#
# It could not fail.  clang's --analyze does NOT honour -Werror, so the target
# printed 97 analyzer warnings across 19 files and exited 0, and AGENTS.md
# §14's reporting template has been asking for "make lint: 0 warnings" against
# a command that never reported zero and never returned non-zero.  It also
# analysed $(SRCS) -- the link set -- so 104 of the 132 files in src/stdlib/
# were not analysed at all.  Proven by injecting a null dereference into
# src/stdlib/xlsx.c: clang reported it when xlsx.c was analysed directly;
# `make lint` did not mention the file and still exited 0.
#
# scripts/check_stdlib_analysis.py now runs three passes over $(ANALYZE_SRCS)
# and FAILS on anything not accounted for on a checked-in, self-failing
# skiplist.  See its header for what a pass does and does not prove.
lint:
	@python3 scripts/check_stdlib_analysis.py --compile --analyze $(ANALYZE_SRCS) $(ANALYZE_HDRS)

# The #else branch of every platform conditional, built.  Generalises what
# test/conform/C033_ocr_text_runs.sh does for src/stdlib/ocr.c alone.
lint-nonhost:
	@python3 scripts/check_stdlib_analysis.py --nonhost $(ANALYZE_SRCS) $(ANALYZE_HDRS)

# 122.13 (10): the same three passes over ONE file (or a few), with the
# skiplist entries for every other file out of scope rather than stale.
#   make lint-file F=src/stdlib/http.c
lint-file:
	@if [ -z "$(F)" ]; then echo "usage: make lint-file F=<source.c> [F=\"a.c b.c\"]" >&2; exit 2; fi
	@python3 scripts/check_stdlib_analysis.py --only --compile --analyze $(F)
	@python3 scripts/check_stdlib_analysis.py --only --nonhost $(F)

# Drafting aid: print proposed skiplist lines for everything now failing.
# Writes nothing — paste the lines in and put a real reason and story on each.
lint-report:
	@python3 scripts/check_stdlib_analysis.py --compile --analyze --nonhost \
	    --report $(ANALYZE_SRCS) $(ANALYZE_HDRS)

# 127.156 / 136.65: every conformance entry point goes through
# scripts/conform_isolated.sh, which pins a private copy of the compiler,
# gives the run a private TMPDIR, caps concurrent runs machine-wide
# (TKC_GATE_SLOTS, default 2) and runs the load-sensitive S005/S006 under a
# machine-wide serial lock.  See the script's header for why each is needed.
# One invocation for `conform`, not a recursive make, so a run holds exactly
# one slot.
conform:
	@bash scripts/conform_isolated.sh all

# 131.37: shell-script conformance cases (test/conform/*.sh) — each script
# prints its own PASS/FAIL lines and exits non-zero on any failure.
# 131.79 gave each run its own log; 127.156 its own compiler and TMPDIR.
conform-sh:
	@bash scripts/conform_isolated.sh sh

conform-check:
	@bash scripts/conform_isolated.sh yaml && echo "CONFORMANCE: 100%"

# ── 136.62 exposed this target ──────────────────────────────────────────────
# Three recursive makes for three --target triples, all writing into the SAME
# object files.  Before 136.62 gave objects a record of their flags, nothing
# invalidated them between the three, so sub-makes 2 and 3 found the host
# objects newer than their sources and did nothing.  MEASURED 2026-09-23 on an
# up-to-date tree: `make build-all` finished in 0.74s, ran ZERO compile
# commands, and exited 0.  AGENTS.md §14 asks contributors to report
# "make build-all: success on x86-64 and ARM64"; that is what "success" meant.
#
# With the flag stamp in place it does recompile, and then fails honestly:
# the two Linux triples need a sysroot this macOS host does not have
# (`fatal error: 'stdio.h' file not found`).  The target has never
# cross-compiled anything here.  build-preflight below says so by name rather
# than letting it surface as a missing libc header.  135.3i gave each triple
# its own object tree and a sysroot variable (below); a sysroot itself is
# still not on this host, and nothing here downloads one.
.PHONY: build-preflight
# 135.3i: the preflight is now the probe half of scripts/build_all.sh, with
# the SAME per-triple flags (--target plus TK_SYSROOT_<triple>, and the
# triple's link flags), so it cannot pass a triple the build would skip or
# vice versa.  Fails, naming each triple and why, if any cannot be built.
build-preflight:
	@CC='$(CC)' MAKE='$(MAKE)' CROSS_DIR='$(CROSS_DIR)' \
	  bash scripts/build_all.sh --probe-only $(CROSS_TRIPLES)

# ── 135.3i: per-triple object directories, a sysroot per triple ─────────────
#
# build-all used to be three recursive makes that all wrote the SAME in-tree
# objects -- and, now that objects remember their flags (136.62), a build-all
# that could succeed would have left the host tree holding whichever triple
# ran last.  It also passed CFLAGS="$(CFLAGS) ..." down, which loses the inner
# quotes of -DTKC_STDLIB_DIR (see "How a sub-make changes the build flags").
#
# Now each triple builds into its own tree, build/cross/<triple>/, from rules
# instantiated below -- no recursive CFLAGS round trip, and the in-tree host
# objects are never touched.  scripts/build_all.sh drives it: it probes each
# triple (compile AND link a hello-world with that triple's flags), builds
# the ones that work, checks each binary's format with file(1), and names
# every triple it could not build and why.
#
# PROVIDING A SYSROOT.  Nothing is downloaded.  Per triple, with the triple's
# dashes as underscores:
#
#   TK_SYSROOT_x86_64_linux_gnu=/path/to/sysroot     (--sysroot=)
#   TK_CROSS_LDFLAGS_x86_64_linux_gnu='-fuse-ld=lld' (default for *-linux-gnu)
#
# A Linux sysroot needs usr/include (libc6-dev, zlib1g-dev) and usr/lib with
# crt*.o, libc, libm, libz and libpthread -- e.g. a Debian/Ubuntu tree made by
# `debootstrap --variant=minbase --include=libc6-dev,zlib1g-dev --arch=amd64`
# or copied out of a `docker run --platform linux/amd64 ubuntu` container
# after `apt-get install libc6-dev zlib1g-dev`.  From macOS the link also
# needs ld.lld (`brew install lld`); Apple's ld cannot write ELF.
#
# BUILD_ALL_ALLOW_SKIP=1 makes a skipped triple exit 0.  By default it does
# not: a build-all that built one triple of three is not "build-all: success"
# (AGENTS.md §14.3), and the final line says which triples were SKIPPED.
CROSS_TRIPLES = x86_64-linux-gnu aarch64-linux-gnu aarch64-apple-macos
CROSS_DIR     = build/cross
cross_var       = $(subst -,_,$(1))
cross_sysroot   = $(TK_SYSROOT_$(call cross_var,$(1)))
cross_cflags    = --target=$(1) $(if $(call cross_sysroot,$(1)),--sysroot=$(call cross_sysroot,$(1)))
cross_ldflags   = $(if $(filter undefined,$(origin TK_CROSS_LDFLAGS_$(call cross_var,$(1)))),$(if $(findstring linux,$(1)),-fuse-ld=lld),$(TK_CROSS_LDFLAGS_$(call cross_var,$(1))))

define CROSS_template
CROSS_OBJS_$(1) := $$(addprefix $(CROSS_DIR)/$(1)/,$$(SRCS:.c=.o))
CROSS_FLAGS_TEXT_$(1) = $$(CC)|$$(CFLAGS)|$$(call cross_cflags,$(1))|$$(call cross_ldflags,$(1))|$$(REPRO_FLAGS)|$$(LDLIBS)

$(CROSS_DIR)/$(1)/.build-flags: FORCE
	@mkdir -p $$(@D)
	@printf '%s\n' "$$$$TKC_CROSS_FLAGS" | cmp -s - $$@ 2>/dev/null \
	  || printf '%s\n' "$$$$TKC_CROSS_FLAGS" > $$@
$(CROSS_DIR)/$(1)/.build-flags: export TKC_CROSS_FLAGS = $$(CROSS_FLAGS_TEXT_$(1))

$(CROSS_DIR)/$(1)/%.o: %.c $(CROSS_DIR)/$(1)/.build-flags
	@mkdir -p $$(@D)
	$$(BUILD_ENV) $$(CC) $$(CFLAGS) $$(call cross_cflags,$(1)) $$(REPRO_FLAGS) \
	    $$(if $$(filter src/stdlib/encrypt.c,$$<),-Wno-pedantic) -c -o $$@ $$<

$(CROSS_DIR)/$(1)/src/llvm.o: src/stdlib_decls_gen.h src/stdlib_dummyarg_gen.h
$(CROSS_DIR)/$(1)/src/types.o: src/stdlib_paramkind_gen.h

$(CROSS_DIR)/$(1)/toke: $$(CROSS_OBJS_$(1))
	$$(BUILD_ENV) $$(CC) $$(CFLAGS) $$(call cross_cflags,$(1)) $$(call cross_ldflags,$(1)) \
	    $$(REPRO_FLAGS) -o $$@ $$^ $$(LDLIBS)

.PHONY: build-cross-$(1)
build-cross-$(1): vendor-check $(CROSS_DIR)/$(1)/toke

-include $$(CROSS_OBJS_$(1):.o=.d)
endef
$(foreach t,$(CROSS_TRIPLES),$(eval $(call CROSS_template,$(t))))

# The probe flags for one triple, printed for scripts/build_all.sh so that the
# script and the build rules cannot disagree about what a triple's flags are.
.PHONY: cross-flags-%
cross-flags-%:
	@printf 'CFLAGS=%s\nLDFLAGS=%s\nSYSROOT=%s\n' '$(call cross_cflags,$*)' '$(call cross_ldflags,$*)' '$(call cross_sysroot,$*)'

build-all: vendor-check
	@CC='$(CC)' MAKE='$(MAKE)' CROSS_DIR='$(CROSS_DIR)' \
	  bash scripts/build_all.sh $(CROSS_TRIPLES)

clean-cross:
	rm -rf $(CROSS_DIR)

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

# 138.18: a C struct marked `/* tki-record: mod.type */` in src/stdlib/*.h
# (the records the glue hands to toke code, e.g. http's TkReqRec) must have
# that .tki record's fields, in its order, one 8-byte slot each.
.PHONY: check-tki-layout
check-tki-layout:
	python3 scripts/check_tki_layout.py --list

# 127.266: check-tki-names rides here too.  It was only in `make ci` and the
# CI workflow, which batches do not run locally, so a stale rule in it failed
# on stdlib/csv.tki for two days with nobody seeing it.
check-tki: check-tki-layout check-tki-names
	python3 scripts/check_tki_coverage.py
	python3 scripts/check_tki_coverage.py --quarantine-md-check
	python3 scripts/check_tki_coverage.py --internal-md-check
	python3 scripts/check_stdlib_doc_quarantine.py
	python3 scripts/check_stdlib_doc_exports.py
	@# 135.17a: the generator agrees with every .tki, and every arity
	@# disagreement it declines to fix is recorded with its reason.
	python3 scripts/gen_tki.py --check

# 132.102: 45 documented function entries were not .tki exports (encrypt's
# pre-113.2a underscored spellings, json/http designs no interface declares,
# the withdrawn webview) and math.md typed floor/ceil i64 against math.tki's
# f64. check_stdlib_doc_exports.py (run by check-tki above) fails on any entry
# that is not an export and not marked **Not exported.**, and on a documented
# return type that differs from the .tki's (gen:abi exports exempt).

# 127.230 (6): docs/stdlib/*.md documented quarantined calls (canvas.arc,
# canvas.fill, ...) as ordinary functions; the pages never consulted the
# skiplist. check_stdlib_doc_quarantine.py (run by check-tki above) fails when
# a documented function's [quarantined] marker disagrees with the skiplist, in
# either direction; this target rewrites the markers from it.
render-stdlib-doc-quarantine:
	python3 scripts/check_stdlib_doc_quarantine.py --fix

# 136.7 — the quarantine population had two hand-maintained records (this
# skiplist and a prose list in the tracker) and they diverged: the prose named
# three exports that resolve, and a module with no .tki at all, which cannot be
# in this class. scripts/check_tki_skiplist.txt is now the single authoritative
# record and docs/reference/tki-quarantine.md is generated from it; check-tki
# fails on drift. Never hand-write a second list — link to the generated page.
render-tki-quarantine:
	python3 scripts/check_tki_coverage.py --quarantine-md

# 136.54 / 136.34: docs/reference/stdlib-internal.md -- exports marked
# "internal": true, and every tk_* definition no interface reaches, by class.
# Generated from stdlib/*.tki and src/stdlib/*.c; check-tki fails on drift.
render-stdlib-internal:
	python3 scripts/check_tki_coverage.py --internal-md

# 136.46 — toke's default 59-character profile excludes `_`, so an underscored
# type or field name in a .tki is unwriteable from toke source: E1003 at every
# use site. The function stays reachable; the TYPE does not. .tki content never
# reaches the lexer (a hand-rolled scanner in src/llvm.c reads it), so the
# compiler cannot catch this and it has to be a gate. Nineteen had accumulated
# across six interface files before anything looked.
check-tki-names:
	python3 scripts/check_tki_nameable.py

# 136.43 — three sibling repos were found holding a staged revert of the Gate 1
# correction, each with a worktree identical to HEAD, so the file on disk looked
# untouched and only `git diff --cached` showed what a bare `git commit` would
# land. Three of exactly that shape DID fire on 2026-09-21. §12.4's pathspec
# commit protects the paths you name and not a bare commit at all, and no other
# gate reads the index.
check-index:
	python3 scripts/check_clean_index.py

# The cross-repo form, for a periodic sweep: the three that were found were in
# three DIFFERENT repositories, so checking only this one would have missed all
# of them.
check-index-all:
	python3 scripts/check_clean_index.py --sweep ..

# 136.50 — a *_glue.c that never #includes its own core's header cannot call
# anything that core declares, so whatever it returns it did not compute. This
# is the detector for "the glue returns a plausible value while the C core
# beside it is complete" (136.16 std.toon, 136.44 std.tls, 137.11) — a
# `(void)arg; return 0;` sweep misses the stubs that return an empty array or
# "[... not available]", which is most of them.
check-glue-core:
	python3 scripts/check_glue_core_link.py

# 136.50a — the other direction from check-tki and check-glue-core: a public
# C function in src/stdlib/<m>.h that no .tki exports and nothing calls is
# finished work with no way in (136.44a was one, declared on neither side).
# The findings are classified in scripts/core_reach_triage.txt as gap /
# internal / dup. ASSERTED: every finding is classified and no line is stale,
# so a new unsurfaced function cannot land silently and the list only shrinks.
# REPORTED, NOT ASSERTED: the gap backlog itself (114 at 2026-09-24), since
# each needs a surface decision, not a CI failure.
check-core-reach:
	python3 scripts/report_core_reach.py --check

# 136.56 — every tracked test/**/*.c is named on a non-comment line of the
# Makefile, a workflow, scripts/ or a test script. The Makefile hand-lists
# every test target, so this is what notices the one it forgot: eighteen
# sources, 6,691 lines, had no target when this was written.
check-test-orphans:
	python3 scripts/check_test_orphans.py

# 131.82 — every numbered test case (test/<dir>/<S><NNN>.yaml, <S><NNN>_<subject>.sh)
# has a series-wide unique id, a yaml `id:` that matches its file, and a
# subject.  Three agents once each wrote a C009 on the same day because each
# took "the next free number" from its own tree; new ids come from
#   python3 scripts/check_test_ids.py --claim C <subject>
# which reserves one above every id in this tree, every worktree, every
# unmerged branch and the shared claims registry in the git common dir.
check-test-ids:
	python3 scripts/check_test_ids.py

# 132.43 — the website's published toke programs, compiled by THIS compiler.
# The website checks its own examples, but only on a push to the website: when
# stdlib/io.tki landed here and made `io.printi` a diagnosable error, every
# program the site published that used it broke and nothing on this side
# looked, so it shipped in v3.0.0. This is the other direction of that
# trigger. It runs the site's own verifier in --compile-only mode (the token-
# count half of that gate is red on purpose pending an owner decision and a
# compiler change cannot affect it). SITE_REPO defaults to the sibling
# checkout; CI and the release workflow check the site out and pass it.
SITE_REPO ?= ../toke-website
SITE_VERIFY = $(SITE_REPO)/scripts/verify_site_examples.py

check-site-compile: $(BIN)
	@if [ ! -f "$(SITE_VERIFY)" ]; then \
	  echo "SKIP check-site-compile: $(SITE_VERIFY) not found (set SITE_REPO=)"; \
	elif ! grep -q -- '--compile-only' "$(SITE_VERIFY)"; then \
	  echo "SKIP check-site-compile: $(SITE_REPO) predates --compile-only (132.43)"; \
	else \
	  TKC="$(abspath $(BIN))" python3 "$(SITE_VERIFY)" --check --compile-only; \
	fi

# 135.11 — a stdlib module must have exactly ONE description of each kind:
# stdlib/<m>.tki (interface), src/stdlib/<m>.c (implementation) and
# docs/stdlib/<m>.md (docs). A stdlib/<m>.tk reimplementation or a
# stdlib/<m>.md left behind by the docs move is a SECOND description that no
# gate reads, so it drifts. That is the shape that produced the
# wrapper-versus-core drift in Epic 136 and cost 31 defects. The quarantine
# list only ever shrinks; deleting a file forces deleting its line.
check-parallel-stdlib:
	python3 scripts/check_parallel_stdlib.py

# ── Story 123.9: is the shipping build actually hardened? ───────────────────
# Behavioural, not textual. See the HARDEN_CFLAGS block at the top of this
# file and the header of scripts/check_hardening.py for why grepping the
# flags would prove nothing. Runs its own two negative controls first.
#
# CFLAGS travels by environment because it contains both quote kinds
# (-DTKC_STDLIB_DIR='"..."'), the same reason TKC_BUILD_FLAGS does.
.PHONY: check-hardening
check-hardening: $(BIN)
	@TKC_HARDEN_CC="$(CC)" TKC_HARDEN_CFLAGS="$(CFLAGS)" \
	    python3 scripts/check_hardening.py --binary $(BIN)

# ── Story 123.9: sanitizers ────────────────────────────────────────────────
#
# ZERO sanitizers ran on a pull request before 2026-09-24. `-fsanitize`
# appeared once in this Makefile, in FUZZ_FLAGS, which only the nightly fuzz
# workflow reaches.
#
# These targets recursively re-make with the sanitizer flags in CFLAGS. That
# rebuilds the tree, and it rebuilds it CORRECTLY only because 136.62 gave
# objects a record of the flags they were built with: before the .build-flags
# stamp existed, `make CFLAGS=<different>` on an up-to-date tree ran zero
# compile commands and exited 0, so a "sanitizer build" would have been the
# ordinary objects with a sanitizer runtime linked in and no instrumentation
# in them whatsoever. That is precisely the false-green shape this wave is
# clearing out, and it is worth knowing that these targets depend on that fix.
#
# _FORTIFY_SOURCE is forced OFF here: its interceptors and ASan's fight, and
# the fortify checks mask the very overflows ASan is here to report.
# -O1 rather than the hardened -O2 keeps stack traces readable.
SAN_FLAGS  = -fsanitize=address,undefined -fno-omit-frame-pointer \
             -fno-sanitize-recover=undefined -fno-optimize-sibling-calls -O1

# The hardening set is FILTERED OUT rather than overridden.  Overriding it was
# tried first and every sanitizer build died at the preprocessor:
#
#   error: '_FORTIFY_SOURCE' macro redefined [-Werror,-Wmacro-redefined]
#   <built-in>:464:9: note: previous definition is here
#
# -fsanitize=address makes clang predefine `_FORTIFY_SOURCE 0` ITSELF, as a
# built-in, because the ASan interceptors and the fortify wrappers fight; a
# `-D_FORTIFY_SOURCE=0` of our own then collides with it and -Werror stops the
# build.  A sanitizer job that is red because it did not COMPILE is the "red
# for a reason nobody understands" failure this wave is here to remove, so the
# flags come out of CFLAGS instead of being argued with.  -O1 likewise replaces
# the hardened -O2 by filtering, to keep stack traces readable.
# Used only where the compiler is invoked DIRECTLY from a recipe (the
# self-test); recursive makes go through EXTRA_CFLAGS instead, see above.
SAN_CFLAGS = $(filter-out $(HARDEN_CFLAGS),$(CFLAGS)) $(SAN_FLAGS)
export ASAN_OPTIONS  ?= detect_leaks=0:abort_on_error=0:detect_stack_use_after_return=1
export UBSAN_OPTIONS ?= print_stacktrace=1:halt_on_error=1

.PHONY: asan asan-clean test-asan test-asan-compiler test-asan-stdlib
asan-clean:
	@$(MAKE) --no-print-directory clean >/dev/null

# The compiler itself, instrumented, driven through the conformance suite.
test-asan-compiler: asan-selftest
	@echo "=== ASAN/UBSAN: tkc through the conformance suite ==="
	$(MAKE) HARDEN=0 EXTRA_CFLAGS="$(SAN_FLAGS)" $(BIN)
	$(MAKE) HARDEN=0 EXTRA_CFLAGS="$(SAN_FLAGS)" conform

# The stdlib C unit tests, instrumented. A red result here is a true result:
# do not add suppressions to get a tick.
#
# 135.24 item 5: this comment used to say the target was EXPECTED TO BE RED
# because of json.c:670 (use after free) and json.c:941/949, json_glue.c:718
# (free() of a global).  Those were clang --analyze findings (135.3a) on
# error and OOM paths, fixed by 135.3c (5350fdc, pinned by conform C132's
# counting allocator), and test-stdlib-json never reached them: built under
# this configuration it runs clean at dbb254a, where the comment was written
# (102/102), at 5350fdc~1 (110/110) and now (111/111), 0 sanitizer reports.
ASAN_STDLIB_TARGETS = test-stdlib test-stdlib-json test-stdlib-encoding \
                      test-stdlib-math test-stdlib-template \
                      test-stdlib-encrypt test-stdlib-toon \
                      test-stdlib-yaml test-stdlib-regex \
                      test-stdlib-http-leak test-stdlib-http-route-param-leak \
                      test-stdlib-http test-stdlib-http2 test-stdlib-router \
                      test-stdlib-http-multipart test-stdlib-http-cookies \
                      test-stdlib-http-form test-stdlib-http-json-integration \
                      test-stdlib-web-server-integration \
                      test-stdlib-time
#
# 127.246 (6) / 121.53 (3): test-stdlib-time joins -- tk_time_format_duration
# used sprintf(), which failed -Werror here (see the csv note below); it now
# uses bounded snprintf.
#
# 121.30: the HTTP/router C tests join the sanitizer run -- the request
# framing, HTTP/2, routing, static serving, multipart, cookie and form paths
# had never run under ASan in CI.  Measured 2026-09-29 on this tree, each
# built with SAN_FLAGS: 0 sanitizer reports.  test-stdlib-http-multipart
# needed its test helper moved off sprintf() first (the csv reason below).
#
# test-stdlib-http-leak and test-stdlib-http-route-param-leak were excluded
# until 122.14: they measured retention with malloc_zone_statistics() /
# mallinfo2(), the SYSTEM allocator's count, and under ASan that reads ASan's
# shadow and quarantine bookkeeping (~1500 bytes "retained" per request on a
# tree that retains none).  test/stdlib/heap_in_use.h now asks ASan itself
# (__sanitizer_get_current_allocated_bytes) when built with it, and each run
# prints which counter it used.  Measured 2026-09-27: 0.0 bytes/request both
# ways on this tree; with free_req() removed from the keep-alive loop, 285.6
# bytes/request without ASan and 190.9 with it -- red both ways.
#
# EXCLUDED, with the reason, because "we left it out" and "it is clean" must
# never look the same in a report:
#
#   test-stdlib-csv
#       Does not COMPILE under the sanitizer build, for a real reason worth
#       its own story: test/stdlib/test_csv.c:360 calls sprintf(). Apple's
#       SDK marks sprintf deprecated only when _FORTIFY_SOURCE is 0, and
#       -fsanitize=address sets it to 0, so -Wdeprecated-declarations fires
#       there and -Werror stops the build. The ordinary fortified build never
#       sees it. A sweep on 2026-09-24 found 7 more unbounded sprintf() calls
#       in SHIPPING code -- src/stdlib/tk_time.c (format_duration, now
#       snprintf and in the list above, 127.246 (6)) and
#       src/stdlib/db_postgres.c:37 (no sprintf there at 2026-09-30). Re-add
#       this target when test_csv.c is
#       dealt with.
# The negative control, run FIRST and never behind a flag: a planted
# heap-use-after-free that this build configuration must catch. Without it,
# "0 findings" and "the instrumentation never made it into the binary" are
# the same output.
.PHONY: asan-selftest
asan-selftest:
	@echo "=== ASAN self-test: the detector must catch a planted defect ==="
	@# 136.41: a private directory, not the fixed /tmp/tkc_asan_selftest{,.log}
	@d=$$(mktemp -d "$${TMPDIR:-/tmp}/tkc_asan_selftest.XXXXXX") || exit 1; \
	 trap 'rm -rf "$$d"' EXIT; \
	 $(CC) $(SAN_CFLAGS) -o "$$d/selftest" scripts/asan_selftest.c || exit 1; \
	 "$$d/selftest" > "$$d/selftest.log" 2>&1; rc=$$?; \
	 if [ $$rc -ne 0 ] && \
	    grep -a -q 'ERROR: AddressSanitizer: heap-use-after-free' \
	      "$$d/selftest.log"; then \
	   echo "  ok  planted use-after-free was caught:"; \
	   sed -n '/ERROR: AddressSanitizer/p' "$$d/selftest.log" \
	     | head -1 | sed 's/^/      /'; \
	 else \
	   echo ""; \
	   echo "  exit=$$rc, and the AddressSanitizer banner is $$(grep -a -c \
	     'ERROR: AddressSanitizer' "$$d/selftest.log") line(s) of the output:"; \
	   cat "$$d/selftest.log" | sed 's/^/      /'; \
	   echo ""; \
	   echo "ASAN SELF-TEST DID NOT FIRE -- refusing to report on a build"; \
	   echo "whose sanitizer has not been seen to work. Nothing below ran."; \
	   exit 1; \
	 fi

test-asan-stdlib: asan-selftest
	@echo "=== ASAN/UBSAN: stdlib C unit tests ==="
	@fail=0; total=0; \
	for t in $(ASAN_STDLIB_TARGETS); do \
	  total=$$((total+1)); \
	  log="$$(mktemp "$${TMPDIR:-/tmp}/tkc_asan.XXXXXX")"; \
	  if $(MAKE) --no-print-directory HARDEN=0 EXTRA_CFLAGS="$(SAN_FLAGS)" \
	       RUN_TEST_TIMEOUT=$(RUN_TEST_TIMEOUT) $$t > "$$log" 2>&1; then \
	    echo "clean  $$t"; \
	  else \
	    echo "FINDING  $$t"; \
	    sed -n '/ERROR: AddressSanitizer\|runtime error:\|SUMMARY:/,+12p' "$$log" \
	      | head -40 | sed 's/^/    /'; \
	    tail -6 "$$log" | sed 's/^/    | /'; \
	    fail=$$((fail+1)); \
	  fi; \
	  rm -f "$$log"; \
	done; \
	echo "test-asan-stdlib: $$fail of $$total target(s) reported a finding"; \
	[ $$fail -eq 0 ]

test-asan: test-asan-compiler test-asan-stdlib

# ── Story 123.9: coverage floor ────────────────────────────────────────────
#
# There was no coverage measurement of any kind before 2026-09-24.
#
# COVERAGE_MIN is a FLOOR, and the number below is a MEASUREMENT of this tree,
# rounded down -- not an aspiration. Both other shapes are gates that do not
# work: a floor above the current number is red for a reason nobody can act on
# in the pull request that trips it, and a floor of 0 is a gate that cannot
# fail, which is the thing this wave exists to remove. Raise it when the
# measured number rises, in a change that says what raised it.
# MEASURED 2026-09-24 on this tree, tkc driven through `make conform`:
#
#     regions 34.06%   functions 31.96%   LINES 34.40%   branches 31.12%
#
# The floor is the line figure rounded DOWN to a whole number. Raise it when
# the measured number rises, in a change that says what raised it.
COVERAGE_MIN ?= 34
COV_FLAGS     = -fprofile-instr-generate -fcoverage-mapping -O0

# llvm-profdata/llvm-cov are on PATH on the Linux runner and behind xcrun on
# macOS. Resolve once, and FAIL BY NAME if absent -- a coverage target that
# quietly reports nothing because a tool is missing is the exact shape of the
# seven checks audited on 2026-09-20.
LLVM_PROFDATA := $(shell command -v llvm-profdata 2>/dev/null || xcrun --find llvm-profdata 2>/dev/null)
LLVM_COV      := $(shell command -v llvm-cov 2>/dev/null || xcrun --find llvm-cov 2>/dev/null)

.PHONY: coverage-preflight coverage check-coverage
coverage-preflight:
	@missing=""; \
	[ -x "$(LLVM_PROFDATA)" ] || missing="$$missing llvm-profdata"; \
	[ -x "$(LLVM_COV)" ]      || missing="$$missing llvm-cov"; \
	if [ -n "$$missing" ]; then \
	  echo "ERROR: coverage needs:$$missing" >&2; \
	  echo "  Install the llvm package (Linux) or Xcode command line tools." >&2; \
	  echo "  Refusing to report a coverage number this host cannot measure." >&2; \
	  exit 1; \
	fi

# Rebuilds the tree instrumented, drives it through the conformance suite,
# and prints the summary. Kept separate from the floor check so the number
# can be read without a pass/fail verdict attached.
# %m, not %p.  %p is one .profraw per PROCESS, and `make conform` runs the
# compiler thousands of times: the first attempt at this target left 635
# untracked .profraw files in the repository root and was still going.  %m
# keys the file on the instrumented module's signature instead, so every
# invocation of the same binary merges into ONE file under a lock.
COV_DIR      = $(CURDIR)/.coverage
COV_PROFDATA = $(COV_DIR)/tkc.profdata

coverage: coverage-preflight
	@rm -rf $(COV_DIR) && mkdir -p $(COV_DIR)
	$(MAKE) HARDEN=0 EXTRA_CFLAGS="$(COV_FLAGS)" $(BIN)
	@LLVM_PROFILE_FILE="$(COV_DIR)/tkc-%m.profraw" \
	   $(MAKE) --no-print-directory conform > /dev/null 2>&1 || true
	@ls $(COV_DIR)/*.profraw >/dev/null 2>&1 || { \
	   echo "ERROR: the instrumented build produced no .profraw files." >&2; \
	   echo "  Nothing was measured, so there is no number to report." >&2; \
	   exit 1; }
	@"$(LLVM_PROFDATA)" merge -sparse $(COV_DIR)/*.profraw -o $(COV_PROFDATA)
	@"$(LLVM_COV)" report ./$(BIN) -instr-profile=$(COV_PROFDATA) $(SRCS) \
	    | tail -3
	@rm -f $(COV_DIR)/*.profraw

check-coverage: coverage
	@# llvm-cov's TOTAL row is: TOTAL <reg> <missed> <cover%> <fn> <missed>
	@# <exec%> <lines> <missed> <cover%> <branches> <missed> <cover%>.
	@# Field 10 is LINE coverage. The first version of this scanned backwards
	@# for the last field ending in '%' and silently read field 13 -- BRANCH
	@# coverage, a different and lower number -- while the message beside it
	@# said "line coverage". Take the column by position, and check the row
	@# has the shape this assumes.
	@pct=$$("$(LLVM_COV)" report ./$(BIN) -instr-profile=$(COV_PROFDATA) $(SRCS) \
	          | awk '/^TOTAL/{ if (NF != 13) { exit } gsub(/%/,"",$$10); print $$10; exit }'); \
	if [ -z "$$pct" ]; then \
	  echo "ERROR: could not read a line-coverage percentage from llvm-cov." >&2; \
	  echo "  Either there is no TOTAL row, or it does not have the 13 fields" >&2; \
	  echo "  this parser assumes. Not reporting a pass on an unparsed number." >&2; \
	  exit 1; \
	fi; \
	echo "line coverage: $$pct%   floor: $(COVERAGE_MIN)%"; \
	awk -v p="$$pct" -v m="$(COVERAGE_MIN)" \
	    'BEGIN { if (p+0 < m+0) { printf "BELOW FLOOR: %s%% < %s%%\n", p, m; exit 1 } \
	             printf "at or above floor\n" }'

# ── Story 123.8: the test families PR CI did not run ───────────────────────
#
# Before 2026-09-24 the pull-request workflow ran `make`, `make conform`,
# `make verify-ir` and `make lint` and nothing else. Every target below was a
# local-only `make` target: nothing on a push or a PR invoked one, which is
# why four test-stdlib-http* rules could sit unable to LINK for long enough
# that nobody noticed.
#
# THE LISTS BELOW ARE A MEASUREMENT, NOT A SELECTION.  Every one of the 72
# runnable test targets was built and run on a clean tree at 6fb3b50 on
# 2026-09-24 before any of them was wired in, precisely so that CI would not
# go red on arrival for reasons unrelated to any pull request:
#
#     57 pass   8 fail to BUILD   7 fail at RUN time
#
# Seven of the eight build failures were missing sources or libraries on the
# recipe line and are fixed above in this same change; the eighth
# (test-stdlib-router) is a stale TEST SOURCE, not a recipe, and is in
# TEST_CI_KNOWN_RED with the rest.
#
# CAVEAT, AND IT IS A REAL ONE: that survey ran on macOS/arm64 and this list
# is consumed by an ubuntu-latest runner.  A macOS-green target is EVIDENCE
# ABOUT MACOS.  The three targets known to be macOS-only are excluded by name
# below rather than silently dropped; for the rest, the first Linux CI run is
# itself the Linux measurement, and anything that goes red on it belongs in
# TEST_CI_KNOWN_RED with a story number, not quietly deleted from here.
#
# A target belongs in -green only while it passes.  Moving one out of
# KNOWN_RED is a fix landing; moving one INTO it needs a story number on the
# line saying what is broken.

TEST_CI_GREEN = \
	test-stdlib test-stdlib-ambient test-stdlib-analytics test-stdlib-args \
	test-stdlib-canvas test-stdlib-chart test-stdlib-csv \
	test-stdlib-dashboard test-stdlib-data-pipeline test-stdlib-dataframe \
	test-stdlib-db test-stdlib-encoding test-stdlib-encrypt test-stdlib-env \
	test-stdlib-glue-contract test-stdlib-html test-stdlib-i18n \
	test-stdlib-image test-stdlib-json test-stdlib-llm test-stdlib-llm-tool \
	test-stdlib-map test-stdlib-math test-stdlib-md test-stdlib-ml \
	test-stdlib-network-integration test-stdlib-path test-stdlib-process \
	test-stdlib-regex test-stdlib-runtime test-stdlib-securemem \
	test-stdlib-security-integration test-stdlib-sse test-stdlib-svg \
	test-stdlib-template test-stdlib-test test-stdlib-time \
	test-stdlib-toml test-stdlib-toml-glue test-stdlib-toon \
	test-stdlib-vecstore test-stdlib-vecstore-binding test-stdlib-ws \
	test-stdlib-yaml test-tkir-encoder test-tkir-reader \
	test-stdlib-http-leak test-stdlib-http-route-param-leak \
	test-stdlib-http-tls \
	test-companion test-companion-diff test-migrate \
	test-compress test-compress-stream \
	test-standalone-check \
	test-stdlib-http-cookies test-stdlib-http-multipart \
	test-stdlib-http-form test-stdlib-log test-stdlib-coverage \
	test-stdlib-capabilities test-stdlib-auth-flow-integration \
	test-stdlib-dashboard-integration test-stdlib-http-json-integration \
	test-stdlib-http2 test-stdlib-image-pipeline-integration \
	test-stdlib-infer-stream test-stdlib-ml-pipeline-integration \
	test-stdlib-mlx test-stdlib-svg-diagram-integration \
	test-stdlib-web-server-integration test-stdlib-ws-handshake-integration \
	test-pkg \
	test-stdlib-infer test-stdlib-template-email-integration \
	test-stdlib-http test-stdlib-ws-client-tk \
	test-stdlib-zip test-stdlib-router test-stdlib-file \
	test-codegen

# Green on macOS, not runnable on the Linux runner.  Excluded BY NAME so the
# gap is visible: nothing on any platform covers these in CI today.
#   test-stdlib-tls        links -framework Security -framework CoreFoundation
#                          and -L/opt/homebrew/lib (TLS_TEST_LDFLAGS)
#   test-stdlib-keychain   drives the macOS Keychain via a shell script
#   test-stdlib-webview    Objective-C under __APPLE__, WebKit + Cocoa.
#                          Green on macOS/arm64 2026-09-24 (136.56).
#   test-stdlib-keychain-core
#                          the C core of the above; writes to the login
#                          keychain. NOT RUN by 136.56 (a keychain write can
#                          raise a macOS access prompt), so unmeasured.
TEST_CI_MACOS_ONLY = test-stdlib-tls test-stdlib-keychain \
	test-stdlib-webview test-stdlib-keychain-core

# Wired, and never run anywhere yet. Not green, not known red: UNMEASURED.
# CI runs these in the allowed-to-fail step, which makes the Linux run their
# first measurement; promote or demote from there.
#   test-stdlib-mdns       on macOS it really advertises and browses over
#                          Bonjour, which can raise the Local Network
#                          permission prompt, so 136.56 did not run it here.
#                          On Linux mdns.c is a stub and the test checks the
#                          stub's safe return values.
TEST_CI_UNMEASURED = test-stdlib-mdns

# Red at HEAD on 2026-09-24, measured, with the reason.  CI runs these in a
# job that is ALLOWED TO FAIL and says so on its face -- see
# .github/workflows/ci.yml.  They are listed rather than omitted because a
# target nobody runs is how the four unlinkable http rules survived.
#
#   test-stdlib-viz-integration links now; "FAIL: test3: rendered contains
#                              Chart.js" then SIGTRAP.
#   test-stdlib-crypto         runtime failures
#   test-compress-schema       22/23; CSV compression ratio over threshold
#   test-e2e / test-e2e-check  11 of 48 fixtures still use `=` for equality
#                              and are rejected with E2002. Stale fixtures.
#   test-standalone            558/571
#   test-stdlib-auth           FLAKY, ~19% of runs, and the flake is the
#                              test being wrong rather than auth.c being
#                              broken. test_auth.c:124 "tampers" with a JWT
#                              by doing tampered[tlen-1] ^= 0x01 -- an XOR on
#                              the ASCII BYTE of the last base64url character
#                              of the signature, not on the signature itself.
#                              An HMAC-SHA256 signature is 32 bytes, so its
#                              unpadded base64url is 43 characters and the
#                              43rd carries 4 data bits plus 2 padding bits
#                              that decode to nothing. The encoder always
#                              emits those padding bits as 00, so the last
#                              character is always one of the 16 alphabet
#                              entries whose value ends in 00, and for three
#                              of them -- '0'(52), '4'(56), '8'(60) -- ^0x01
#                              moves to '1', '5', '9', changing ONLY a
#                              padding bit. The decoded 32 bytes are
#                              identical, so auth_jwtverify accepts, and it
#                              is right to. 3 in 16 is 18.75%; measured 5/20
#                              and 5/24. The test does not prove what its
#                              name claims, and would not catch a real
#                              bypass in the final signature byte. Needs a
#                              tamper of a DECODED byte, or of a character
#                              in the middle of the segment.
#
#   test-stdlib-zip was listed here as "runtime failures". By 2026-09-27 its
#   one failure was the script's own inline program, whose body ended in a
#   bare `mt` (E4071, 35bd23e); zip_consumer.tk and all seven hostile
#   archives passed. 136.72 (6) fixed the script (19/19 on macOS/arm64) and
#   promoted it, so zip_consumer.tk is built and run on the gating path.
#   test-stdlib-infer and test-stdlib-template-email-integration were
#   listed here by 136.56 with assertions that were wrong, not the modules;
#   136.66 corrected both assertions and promoted them to -green.
#   test-stdlib-router was listed here: test_router.c called
#   router_static_serve with 3 args where it takes 4, and three assertions
#   predated the HTML 404 page and the charset in the MIME table. 135.28
#   fixed the TEST (6 call sites, 3 assertions); 125 pass on macOS/arm64
#   2026-09-28; promoted.
#   test-stdlib-file was listed here as "runtime failures": its one failure
#   was the test's own fixture -- b.txt and c.txt were written under
#   base/sub/sub/, which did not exist, unchecked, so file_listall rightly
#   found one file where the test expected three. 127.260 fixed the TEST
#   (paths, and the writes are now asserted); promoted.
#   test-stdlib-http was listed here as "exits 1 with no diagnostic inside
#   http_get()". The diagnostic was CAP001 (net not granted; the harness
#   never called tk_cap_init), and behind it sat two stale assertions of
#   behaviour removed on purpose (NULL-router guard, 27096b4; 30 s default
#   timeout, 59.4.12). 127.147 fixed all three in the TEST; promoted.
TEST_CI_KNOWN_RED = test-stdlib-auth \
	test-stdlib-viz-integration test-stdlib-crypto \
	test-compress-schema test-e2e test-e2e-check \
	test-standalone

# Run a list of targets, one log each, never stopping at the first failure --
# "3 of 61 failed, here they are" is actionable; "make: *** Error 1" after the
# first is not.
define run_test_list
	@fail=0; total=0; \
	for t in $(1); do \
	  total=$$((total+1)); \
	  log="$$(mktemp "$${TMPDIR:-/tmp}/tkc_test_ci.XXXXXX")"; \
	  if $(MAKE) --no-print-directory RUN_TEST_TIMEOUT=$(RUN_TEST_TIMEOUT) $$t \
	       > "$$log" 2>&1; then \
	    echo "PASS $$t"; \
	  else \
	    echo "FAIL $$t"; tail -25 "$$log" | sed 's/^/    /'; \
	    fail=$$((fail+1)); \
	  fi; \
	  rm -f "$$log"; \
	done; \
	echo "$(2): $$fail of $$total failed"; \
	[ $$fail -eq 0 ]
endef

.PHONY: test-ci test-ci-green test-ci-known-red test-ci-unmeasured check-test-orphans check-test-ids check-site-compile check-examples check-stdlib-tk
test-ci-green:
	$(call run_test_list,$(TEST_CI_GREEN),test-ci-green)

# Expected to fail. Runs so the failures stay visible and so that a target
# that starts passing can be promoted.
# No `-` prefix: this reports its real exit status, and the CI job that runs
# it carries continue-on-error instead. A target that always exits 0 is a
# target that cannot fail, which is the thing this wave is removing.
test-ci-known-red:
	$(call run_test_list,$(TEST_CI_KNOWN_RED),test-ci-known-red)

test-ci-unmeasured:
	$(call run_test_list,$(TEST_CI_UNMEASURED),test-ci-unmeasured)

test-ci: test-ci-green

# 127.143 — `diff-codegen` is the root-of-trust gate: it is the only thing in
# this repository that notices a codegen change altering what a compiled
# program DOES. It was defined at line 1026 and named by no target above, so it
# had never run in CI. Measured on this host (M4 Max, binary already built):
# 118 programs compiled and executed in 265 s wall — 4.4 minutes against a
# `make ci` that already runs well over an hour, so cost is not a reason to
# leave it out. It is a prerequisite of `ci` as of this line.
#
# `$(BIN)` is already its own prerequisite, so ordering with `conform` is free.
ci: check-index lint lint-nonhost conform conform-sh conform-check check-tki check-tki-names check-glue-core check-core-reach check-test-orphans check-test-ids fuzz-smoke secrets-scan check-site-compile check-parallel-stdlib check-docs check-examples check-stdlib-tk check-error-codes check-patterns check-combinator-status check-probes perf-check check-facts check-metrics check-canonical check-claims-all check-hardening check-progress diff-codegen

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
# 127.262: pass the built compiler, not the script's ./tkc default (a symlink only
# the main checkout has), so check-docs works in a fresh worktree. TKC still wins.
check-docs check-doc-patterns: export TKC ?= $(abspath $(BIN))
check-docs: $(BIN)
	TOKE_WORKSPACE="$(TOKE_WORKSPACE)" python3 scripts/check_doc_site_links.py --self-test  # 132.111
	python3 scripts/check_doc_examples.py --self-test
	python3 scripts/check_doc_examples.py docs

# 132.99 — the idiom half of the docs gate, REPORT mode (132.43): runs the
# compiler's pattern rules (the idiom judge's hard gate) over every documented
# full program and prints what would fail. It exits 0 on hits -- the docs
# predate the pattern catalogue -- and non-zero only if it cannot read its rule
# list. Deliberate "Not this" examples carry
# `<!-- pattern-report: counter-example -->` above their fence and are listed
# as ALLOW, not hidden. The self-test proves the marker and the rule list.
check-doc-patterns: $(BIN)
	python3 scripts/check_doc_examples.py --self-test
	python3 scripts/check_doc_examples.py docs --pattern-report

# 127.87 — the shipped examples/ compile. Nothing read examples/ before this,
# and a multi-module example could not even be type-checked from a fresh clone
# (its interfaces are gitignored build artifacts). Generates them in a private
# dir, --checks every module in dependency order, builds the single-module
# examples to binaries, and fails on any tracked example source it does not
# list. See the script for why multi-module examples are not linked yet.
check-examples: $(BIN)
	@bash scripts/check_examples.sh

# 127.126 — the stdlib's own toke sources type-check. No target compiled
# stdlib/*.tk, which is how two of the five genuine modules broke unnoticed.
# Failures must be on the script's self-failing KNOWN_RED list with a story.
check-stdlib-tk: $(BIN)
	@bash scripts/check_stdlib_tk.sh

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
# models a stale alphabet size and keyword count, a Hugging Face Space synthesised a
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
#
# 132.68 / 136.41 — siblings are resolved from the WORKSPACE, not from `..`.
# `../toke-*` is relative to the current directory, so from a worktree
# (~/tk/tmpwork/wt_x) every sibling printed "skip (not checked out)" and the
# gate passed having checked nothing -- measured: 14 skips, "checking claims
# across:" followed by nothing, exit 0.  The workspace is now the directory
# holding the MAIN checkout, found through `git rev-parse --git-common-dir`
# (from a worktree that is <main>/.git; from the main checkout, `.git`), so a
# worktree sweeps the same repositories the main checkout does.  Override with
# TOKE_WORKSPACE=<dir>.  The same variable is exported to check_canonical.py,
# whose declared `../toke-*` surfaces resolve through it, and to
# verify_project_facts.py, which reads ../toke-corpus.
#
# And a sweep that resolves NO sibling now FAILS instead of passing.  The one
# place that is expected -- a GitHub runner, where no sibling is checked out
# (story 132.46) -- says so explicitly with CLAIMS_ALLOW_NO_SIBLINGS=1, and the
# run still prints that it checked zero repositories.
ifndef TOKE_WORKSPACE
TOKE_WORKSPACE := $(shell d=$$(git rev-parse --git-common-dir 2>/dev/null) && \
                    cd "$$d/../.." 2>/dev/null && pwd)
endif
ifeq ($(strip $(TOKE_WORKSPACE)),)
TOKE_WORKSPACE := $(abspath $(CURDIR)/..)
endif
# 132.108 -- `--self` also scans THIS repository's docs/, README and
# PROJECT_STATUS.  The sweep skips each sibling's mirror of toke's docs
# (toke-website content/docs) as "scanned at the source", but the source was
# never in the list: ecosystem.md stated a stdlib module count that disagreed
# with the fact sheet and this target stayed green (132.108).
SIBLING_NAMES := toke-spec toke-corpus toke-model toke-eval toke-mcp \
                 toke-console toke-cloud toke-ooke toke-test-programs \
                 toke-tokenizer toke-website homebrew-toke \
                 loke tkc
SIBLING_REPOS := $(addprefix $(TOKE_WORKSPACE)/,$(SIBLING_NAMES))
check-canonical check-claims-all check-facts: export TOKE_WORKSPACE := $(TOKE_WORKSPACE)
check-claims-all:
	@echo "workspace: $(TOKE_WORKSPACE)"; \
	repos=""; n=0; for r in $(SIBLING_REPOS); do \
	  if [ -d "$$r" ]; then repos="$$repos $$r"; n=$$((n+1)); else echo "skip (not checked out): $$r"; fi; \
	done; \
	if [ $$n -eq 0 ]; then \
	  if [ "$${CLAIMS_ALLOW_NO_SIBLINGS:-0}" = 1 ]; then \
	    echo "check-claims-all: resolved 0 sibling repositories under $(TOKE_WORKSPACE) -- CLAIMS_ALLOW_NO_SIBLINGS=1, so only this repo is checked"; \
	  else \
	    echo "check-claims-all: FAIL -- resolved 0 of $(words $(SIBLING_NAMES)) sibling repositories under $(TOKE_WORKSPACE)." >&2; \
	    echo "  A sweep that finds nothing is not a pass (132.68). Set TOKE_WORKSPACE, or" >&2; \
	    echo "  CLAIMS_ALLOW_NO_SIBLINGS=1 where no sibling can exist (a CI runner)." >&2; \
	    exit 1; \
	  fi; \
	fi; \
	echo "checking claims across $$n of $(words $(SIBLING_NAMES)) sibling repositories:$$repos"; \
	python3 scripts/check_metrics_claims.py $$repos --self --list && \
	python3 scripts/check_canonical.py $$repos --list

# 121.82 — downstream gate: build ooke (check-toke-candidate) and toke-website
# (website + check-routes) against THIS tree, each in a temp worktree of its
# main. Required before merging a change to src/stdlib/*.c, src/stdlib_deps.c
# or stdlib/*.tki (CONTRIBUTING.md). An absent sibling is skipped, not failed.
.PHONY: check-downstream
check-downstream: $(BIN)
	scripts/check_downstream.sh "$(CURDIR)" "$(TOKE_WORKSPACE)"

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

# ── 136.58 / 136.49 / 136.59: combinator-status.md is rendered, and gated ────
# docs/reference/combinator-status.md was transcribed by hand from
# patterns/probe/probe_results.json; the JSON stayed right and ~20 rows rotted
# (136.53), and run_probes.sh was in no target, so nothing noticed (136.49).
#
#   check-combinator-status  cheap, deterministic: the committed document is
#       exactly what scripts/render_combinator_status.py renders from the
#       committed JSON + template.  Also fails if a probe has no row, a row
#       names a probe with no result, a row's probes disagree, or the JSON
#       does not cover patterns/probe/*.tk.
#   check-probes  re-runs every probe against THIS compiler (pinned copy,
#       private TMPDIR, a gate slot -- scripts/conform_isolated.sh exec) and
#       fails if any probe's verdict differs from the committed JSON.  Slow:
#       it compiles and runs ~136 programs.
#   render-combinator-status  re-render the document; with PROBES=1, re-run
#       the probes into the committed JSON first.
.PHONY: check-combinator-status check-probes render-combinator-status
check-combinator-status:
	python3 scripts/render_combinator_status.py

check-probes: $(BIN)
	@bash scripts/conform_isolated.sh exec -- sh -c '\
	  f="$$TMPDIR/probe_results.json"; \
	  bash patterns/probe/run_probes.sh "$$TKC" "$$f" > "$$TMPDIR/probes.log" 2>&1 \
	    || { echo "check-probes: run_probes.sh failed:"; tail -20 "$$TMPDIR/probes.log"; exit 1; }; \
	  echo "probes: $$(tail -1 "$$TMPDIR/probes.log")"; \
	  python3 scripts/render_combinator_status.py --compare "$$f"'

render-combinator-status:
ifeq ($(PROBES),1)
	@$(MAKE) --no-print-directory $(BIN)
	@bash scripts/conform_isolated.sh exec -- bash patterns/probe/run_probes.sh "" patterns/probe/probe_results.json
endif
	python3 scripts/render_combinator_status.py --write

# 132.58a — structural gate on docs/progress.md, the canonical tracker. Until
# this existed the file that tracks every epic was checked by nothing at all,
# which is how 234 rows with unescaped `|` (whose status column is therefore
# not what it appears), 40 reused story IDs and 13 rows under no table header
# accumulated. Those 287 are baselined in scripts/progress-baseline.txt and
# are all in historical epics; the gate fails on anything NEW, and also on a
# baseline entry that stops being produced, so the file cannot become a
# blanket amnesty. Regenerate the baseline only deliberately: --record.
check-progress:
	python3 scripts/check_progress.py

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

# 123.6 — the C-series codegen cases in test/codegen/ with their PINNED expected
# results: exact stdout, exit code, and the trap line on stderr, at -O0 and -O2,
# plus the multi-module programs diff-codegen cannot build (test/codegen/multimod).
# diff-codegen hashes stdout at -O2 only, so it cannot tell RT003 from RT004.
test-codegen: $(BIN)
	TKC=./$(BIN) bash test/codegen/run_codegen.sh

test-stdlib:
	$(CC) $(CFLAGS) -o test/stdlib/test_str \
	    test/stdlib/test_str.c src/stdlib/str.c
	$(RUN_TEST) ./test/stdlib/test_str

# 137.9 / 121.35: the regex engine (src/stdlib/tk_regex.h) -- supported
# language, rejected syntax with offsets, and the step budget.
test-stdlib-regex:
	$(CC) $(CFLAGS) -o test/stdlib/test_regex test/stdlib/test_regex.c
	$(RUN_TEST) ./test/stdlib/test_regex

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

# ── 123.8: these four could not LINK at all ─────────────────────────────────
# http.c gained calls into log.c (tk_access_log_*, tk_error_log_*) and
# capabilities.c (tk_cap_check/deny) and started using zlib for gzip response
# encoding.  Only test-stdlib-http-leak and test-stdlib-http-tls, written
# afterwards, had their source lists updated (136.57 fixed -tls).  These four
# have been dead since -- `Undefined symbols: _deflate, _tk_cap_check,
# _tk_access_log_write` -- and nobody noticed, because NO CI JOB HAS EVER
# INVOKED A test-stdlib-* TARGET.  That is the whole of story 123.8 in one
# example: the missing gate and the rot it hid arrived together.
# All four pass once linked.
test-stdlib-http:
	$(CC) $(CFLAGS) -o test/stdlib/test_http \
	    test/stdlib/test_http.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c -lz -lpthread
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

# Story 127.72: the route matcher must release the key/value pairs it binds
# for every `:param` segment.  Drives the real server loop over loopback
# keep-alive connections and reads the heap's in-use byte count either side.
test-stdlib-http-route-param-leak:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_route_param_leak \
	    test/stdlib/test_http_route_param_leak.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c -lz -lpthread
	$(RUN_TEST) ./test/stdlib/test_http_route_param_leak

test-stdlib-http-cookies:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_cookies \
	    test/stdlib/test_http_cookies.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c -lz -lpthread
	$(RUN_TEST) ./test/stdlib/test_http_cookies

test-stdlib-http-multipart:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_multipart \
	    test/stdlib/test_http_multipart.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c -lz -lpthread
	$(RUN_TEST) ./test/stdlib/test_http_multipart

test-stdlib-http-form:
	$(CC) $(CFLAGS) -o test/stdlib/test_http_form \
	    test/stdlib/test_http_form.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c -lz -lpthread
	$(RUN_TEST) ./test/stdlib/test_http_form

# Story 27.1.2 — TLS/HTTPS support.
# Compiles the stub (no OpenSSL) so the test binary is always buildable.
# To test with real TLS, rebuild with:
#   make test-stdlib-http-tls TK_OPENSSL=1
ifdef TK_OPENSSL
TLS_CFLAGS  = -I/opt/homebrew/include -DTK_HAVE_OPENSSL \
              -Wno-deprecated-declarations
# 136.57: was pinned to openssl@3/3.6.1, a Cellar version not installed here
# (3.6.2 and 3.6.4 are), so this target could not link at all.  Use the same
# -L/opt/homebrew/lib that src/llvm.c passes when it links a real std.tls
# program, so the two link paths agree and neither pins a point release.
TLS_LDFLAGS = -L/opt/homebrew/lib -lssl -lcrypto
else
TLS_CFLAGS  =
TLS_LDFLAGS =
endif

# 136.57: http.c gained calls into log.c (tk_access_log_write,
# tk_error_log_*) and capabilities.c (tk_cap_check/deny); only
# test-stdlib-http-leak, written afterwards, had its source list updated.
# Without these this rule failed to link in BOTH configurations -- the
# OpenSSL pin was only the first of two independent blockers.
test-stdlib-http-tls:
	$(CC) $(CFLAGS) $(TLS_CFLAGS) -o test/stdlib/test_http_tls \
	    test/stdlib/test_http_tls.c src/stdlib/http.c \
	    src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c \
	    src/stdlib/capabilities.c src/stdlib/http2.c -lz -lpthread \
	    $(TLS_LDFLAGS)
	$(RUN_TEST) ./test/stdlib/test_http_tls

# ── 136.51: std.tls core unit test ───────────────────────────────────────────
# test/stdlib/test_tls.c had no build rule and no caller, so nothing ever
# compiled or ran it — the third instance of that class after test_tkir_reader
# (136.42) and the three files found on 2026-09-21.
#
# Unlike test-stdlib-http-tls above there is no stub build: this file includes
# <openssl/x509.h> directly and tests the real TLS 1.3 core that 136.44 landed,
# so OpenSSL is unconditional. The paths mirror what src/llvm.c passes when it
# links a toke program against std.tls (-L/opt/homebrew/lib plus the two macOS
# frameworks).  TLS_LDFLAGS above now uses the same -L/opt/homebrew/lib;
# it named openssl@3/3.6.1, an uninstalled version, until 136.57.
#
# --allow-net is the grant, not a bypass: tls_connect is behind
# TK_REQUIRE(TK_CAP_NET) and still denies with CAP001 if the flag is dropped.
# Without it the suite passed 24 asserts and exited 1 at the gate.
TLS_TEST_CFLAGS  = -I/opt/homebrew/include -DTK_HAVE_OPENSSL -Wno-deprecated-declarations
TLS_TEST_LDFLAGS = -L/opt/homebrew/lib -lssl -lcrypto \
                   -framework Security -framework CoreFoundation

test-stdlib-tls:
	$(CC) $(CFLAGS) $(TLS_TEST_CFLAGS) -o test/stdlib/test_tls \
	    test/stdlib/test_tls.c src/stdlib/tls.c src/stdlib/capabilities.c \
	    $(TLS_TEST_LDFLAGS)
	$(RUN_TEST) ./test/stdlib/test_tls --allow-net

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

# 123.8: could not LINK -- log.c gzips rotated log files (_gzopen, _gzwrite,
# _gzclose) and -lz was never on this line. Passes once linked.
test-stdlib-log:
	$(CC) $(CFLAGS) -o test/stdlib/test_log \
	    test/stdlib/test_log.c src/stdlib/log.c src/stdlib/tk_time.c -lz
	$(RUN_TEST) ./test/stdlib/test_log
	$(CC) $(CFLAGS) -o test/stdlib/test_log_rotate \
	    test/stdlib/test_log_rotate.c src/stdlib/log.c src/stdlib/tk_time.c -lz
	$(RUN_TEST) ./test/stdlib/test_log_rotate

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
	    test/stdlib/test_router.c src/stdlib/router.c src/stdlib/ws.c src/stdlib/capabilities.c -lz
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

# 121.23: llm.c's tcp_connect calls tk_cap_check since 121.42, and neither
# rule linked capabilities.c -- both failed at link on main (f605aec9).
test-stdlib-llm:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm \
	    test/stdlib/test_llm.c src/stdlib/llm.c src/stdlib/capabilities.c
	$(RUN_TEST) ./test/stdlib/test_llm

test-stdlib-llm-tool:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm_tool \
	    test/stdlib/test_llm_tool.c src/stdlib/llm_tool.c src/stdlib/llm.c \
	    src/stdlib/capabilities.c
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
	    test/stdlib/test_dashboard.c src/stdlib/dashboard.c src/stdlib/chart.c src/stdlib/html.c src/stdlib/router.c src/stdlib/ws.c src/stdlib/capabilities.c -lz
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
	    src/stdlib/router.c src/stdlib/ws.c src/stdlib/sse.c src/stdlib/capabilities.c -lz
	$(RUN_TEST) ./test/stdlib/test_network_integration

# 123.8: could not LINK -- the live-dashboard path reaches ws.c
# (_ws_encode_frame, _ws_decode_frame, _ws_accept_key and two frees) and
# src/stdlib/ws.c was never on this line. Passes once linked.
test-stdlib-viz-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_viz_integration \
	    test/stdlib/test_viz_integration.c \
	    src/stdlib/chart.c src/stdlib/html.c src/stdlib/svg.c src/stdlib/canvas.c \
	    src/stdlib/dashboard.c src/stdlib/router.c src/stdlib/ws.c \
	    src/stdlib/dataframe.c src/stdlib/csv.c src/stdlib/math.c src/stdlib/str.c \
	    src/stdlib/capabilities.c -lm -lz
	$(RUN_TEST) ./test/stdlib/test_viz_integration

test-stdlib-data-pipeline:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_data_pipeline \
	    test/stdlib/test_data_pipeline.c \
	    src/stdlib/csv.c src/stdlib/math.c src/stdlib/dataframe.c \
	    src/stdlib/analytics.c src/stdlib/chart.c src/stdlib/str.c -lm
	$(RUN_TEST) ./test/stdlib/test_data_pipeline

# ── 136.56: eighteen orphaned test sources, now targets ─────────────────────
# Each of these was a tracked test source with no target and no caller --
# 6,691 lines nothing compiled, eight of them the `*_integration.c` files that
# docs/spec/spec-implementation-delta.md cites as evidence. Source lists were
# found by linking and adding the defining module for each undefined symbol,
# not copied from the files' own (stale) header comments. Every target was
# built and run on macOS/arm64 on 2026-09-24 (G2) before being placed in a
# TEST_CI_* list; `make check-test-orphans` fails on the next one.
ORPHAN_LIBS = -lm -lz -lpthread

test-stdlib-capabilities:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_capabilities \
	    test/stdlib/test_capabilities.c src/stdlib/capabilities.c
	$(RUN_TEST) ./test/stdlib/test_capabilities

test-stdlib-auth-flow-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_auth_flow_integration \
	    test/stdlib/test_auth_flow_integration.c \
	    src/stdlib/auth.c src/stdlib/encrypt.c src/stdlib/encoding.c \
	    src/stdlib/crypto.c src/stdlib/json.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_auth_flow_integration

test-stdlib-dashboard-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_dashboard_integration \
	    test/stdlib/test_dashboard_integration.c \
	    src/stdlib/csv.c src/stdlib/dataframe.c src/stdlib/analytics.c \
	    src/stdlib/chart.c src/stdlib/html.c src/stdlib/dashboard.c \
	    src/stdlib/math.c src/stdlib/router.c src/stdlib/ws.c src/stdlib/capabilities.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_dashboard_integration

test-stdlib-http-json-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_http_json_integration \
	    test/stdlib/test_http_json_integration.c \
	    src/stdlib/json.c src/stdlib/llm.c src/stdlib/http.c src/stdlib/str.c \
	    src/stdlib/capabilities.c src/stdlib/encoding.c src/stdlib/log.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_http_json_integration

test-stdlib-http2:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_http2 \
	    test/stdlib/test_http2.c src/stdlib/http2.c src/stdlib/http.c \
	    src/stdlib/capabilities.c src/stdlib/encoding.c src/stdlib/log.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_http2

test-stdlib-image-pipeline-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_image_pipeline_integration \
	    test/stdlib/test_image_pipeline_integration.c src/stdlib/image.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_image_pipeline_integration

test-stdlib-infer:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_infer \
	    test/stdlib/test_infer.c src/stdlib/infer.c src/stdlib/infer_stream.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_infer

test-stdlib-infer-stream:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_infer_stream \
	    test/stdlib/test_infer_stream.c src/stdlib/infer_stream.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_infer_stream

test-stdlib-mdns:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_mdns \
	    test/stdlib/test_mdns.c src/stdlib/mdns.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_mdns

test-stdlib-ml-pipeline-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_ml_pipeline_integration \
	    test/stdlib/test_ml_pipeline_integration.c \
	    src/stdlib/ml.c src/stdlib/csv.c src/stdlib/dataframe.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_ml_pipeline_integration

test-stdlib-mlx:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_mlx \
	    test/stdlib/test_mlx.c src/stdlib/mlx.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_mlx

test-stdlib-svg-diagram-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_svg_diagram_integration \
	    test/stdlib/test_svg_diagram_integration.c \
	    src/stdlib/svg.c src/stdlib/csv.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_svg_diagram_integration

test-stdlib-template-email-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_template_email_integration \
	    test/stdlib/test_template_email_integration.c src/stdlib/template.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_template_email_integration

test-stdlib-web-server-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_web_server_integration \
	    test/stdlib/test_web_server_integration.c \
	    src/stdlib/http.c src/stdlib/router.c src/stdlib/ws.c src/stdlib/sse.c \
	    src/stdlib/capabilities.c src/stdlib/encoding.c src/stdlib/log.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_web_server_integration

test-stdlib-ws-handshake-integration:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_ws_handshake_integration \
	    test/stdlib/test_ws_handshake_integration.c src/stdlib/ws.c $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/stdlib/test_ws_handshake_integration

# src/pkg.c is the package registry, a compiler-side unit, so its test binary
# sits beside its source in test/ rather than in test/stdlib/.
test-pkg:
	$(CC) $(CFLAGS) -iquote src -o test/test_pkg test/test_pkg.c src/pkg.c src/sha256.c \
	    $(ORPHAN_LIBS)
	$(RUN_TEST) ./test/test_pkg

# macOS only. webview.c is Objective-C under __APPLE__ and needs WebKit and
# Cocoa; the test is headless by design (it never opens a real window).
test-stdlib-webview:
	$(CC) $(CFLAGS) -iquote src/stdlib -x objective-c -o test/stdlib/test_webview \
	    src/stdlib/webview.c test/stdlib/test_webview.c \
	    -framework WebKit -framework Cocoa
	$(RUN_TEST) ./test/stdlib/test_webview

# macOS only. The C-core sibling of test-stdlib-keychain (which drives the
# toke binding). 138.4: it works in a throwaway keychain it creates and
# destroys itself, and refuses to run if it cannot -- it no longer writes to
# the user's login keychain.
test-stdlib-keychain-core:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_keychain \
	    test/stdlib/test_keychain.c src/stdlib/keychain.c \
	    -framework Security -framework CoreFoundation
	$(RUN_TEST) ./test/stdlib/test_keychain

test-stdlib-llm-live:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_llm_live \
	    test/stdlib/test_llm_live.c \
	    src/stdlib/llm.c src/stdlib/llm_tool.c src/stdlib/json.c
	$(RUN_TEST) ./test/stdlib/test_llm_live

# 123.8: could not LINK -- db.c puts every connection behind TK_REQUIRE(TK_CAP_DB)
# (_tk_cap_check, _tk_cap_deny) and src/stdlib/capabilities.c was never on this
# line, though test-stdlib-db names it. Passes once linked.
test-stdlib-coverage:
	$(CC) $(CFLAGS) -iquote src/stdlib -o test/stdlib/test_stdlib_coverage \
	    test/stdlib/test_stdlib_coverage.c \
	    src/stdlib/json.c src/stdlib/file.c src/stdlib/str.c \
	    src/stdlib/db.c src/stdlib/capabilities.c -lsqlite3
	$(RUN_TEST) ./test/stdlib/test_stdlib_coverage

bench:
	$(CC) -O2 -iquote src/stdlib -o test/stdlib/bench_stdlib \
	    test/stdlib/bench_stdlib.c \
	    src/stdlib/str.c src/stdlib/json.c src/stdlib/file.c \
	    src/stdlib/crypto.c src/stdlib/tk_time.c src/stdlib/process.c \
	    src/stdlib/env.c src/stdlib/log.c src/stdlib/tk_test.c \
	    src/stdlib/db.c src/stdlib/http.c -lsqlite3
	./test/stdlib/bench_stdlib

# ── 123.11: performance-regression gate ────────────────────────────────────
# Compares COUNTS (instructions retired, malloc-family calls) for tkc
# compiling each bench/programs/*.tk at -O2 and for the binary it produced,
# against bench/perf-baseline.json.  Not wall time: this host runs other
# builds concurrently (load 20-290 measured) and a timing gate would be noise.
# Thresholds and the reasoning are in scripts/perf_gate.py's header.  The
# baseline is host-specific; elsewhere the gate SKIPs by name (bench.yml
# compares base vs head on one runner instead).  Runs under
# scripts/conform_isolated.sh for the pinned compiler and a gate slot.
.PHONY: perf-check perf-record
perf-check: $(BIN)
	@bash scripts/conform_isolated.sh exec -- python3 scripts/perf_gate.py

# Re-record deliberately, in the commit that justifies the new numbers.
perf-record: $(BIN)
	@bash scripts/conform_isolated.sh exec -- python3 scripts/perf_gate.py --record

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
	    test/stdlib/test_toml.c src/stdlib/toml.c src/stdlib/file.c $(TOML_SRCS)
	$(RUN_TEST) ./test/stdlib/test_toml

# Story 127.67: the std.toml wrappers must distinguish a value of false/0
# from an error, and a missing section must not resolve to its parent.
test-stdlib-toml-glue:
	$(CC) $(CFLAGS) $(TOML_FLAGS) -o test/stdlib/test_toml_glue \
	    test/stdlib/test_toml_glue.c src/stdlib/toml.c src/stdlib/file.c src/stdlib/toml_glue.c \
	    src/stdlib/capabilities.c $(TOML_SRCS)
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

# ── Story 127.149: test/stdlib/ws_client.tk, built and run ──────────────────
# The file had never compiled at v0.4 and no target read it, so it implied
# coverage that did not exist. Rewritten to v0.4; this compiles, links and runs
# it. main() needs no server (the client helpers are type-checked and linked,
# not called), so it is safe on a CI runner.
test-stdlib-ws-client-tk: $(BIN)
	@TKC_STDLIB_DIR=$(PWD)/src/stdlib ./$(BIN) test/stdlib/ws_client.tk -o test/stdlib/ws_client.bin
	$(RUN_TEST) ./test/stdlib/ws_client.bin
	@rm -f test/stdlib/ws_client.bin

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

# ── 136.42: .tkir binary reader unit test ────────────────────────────────────
# test/tkir/test_tkir_reader.c had NO build rule and NO caller anywhere in the
# repo, so the only trace of it was a committed 113 KB binary that no target
# had ever produced. It is not a spare test:
# docs/security/audit-120/compiler-codegen.md cites the .tkir reader's bounds
# checks as audited *precisely because* this test reaches them — a citation
# resting on 15 cases nothing ran. Needs src/diag.c: tkir.c calls diag_emit.
# 127.211: it is also given the compiler, so one case emits a real .tkir
# and reads it back; the other cases all decode hand-built buffers.
test-tkir-reader: $(BIN)
	$(CC) $(CFLAGS) -o test/tkir/test_tkir_reader \
	    test/tkir/test_tkir_reader.c src/tkir.c src/diag.c
	$(RUN_TEST) ./test/tkir/test_tkir_reader ./$(BIN)

test-standalone: $(BIN)
	@test/standalone/run_all.sh

# 127.118: `clean` removed every .o but not one .d, so the -MMD dependency
# files -- which are gitignored and therefore survive every checkout --
# were `-include`d on the next build describing a different commit's
# include graph.  `clean` now means clean.
clean:
	rm -f $(OBJS:.o=.d) $(BUILD_FLAGS_STAMP) $(BUILD_INFO_H)
	rm -f $(OBJS) $(BIN) tkc test/stdlib/test_str test/stdlib/test_regex test/stdlib/test_db \
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
	    test/stdlib/test_capabilities test/stdlib/test_auth_flow_integration \
	    test/stdlib/test_dashboard_integration test/stdlib/test_http_json_integration \
	    test/stdlib/test_http2 test/stdlib/test_image_pipeline_integration \
	    test/stdlib/test_infer test/stdlib/test_infer_stream test/stdlib/test_mdns \
	    test/stdlib/test_ml_pipeline_integration test/stdlib/test_mlx \
	    test/stdlib/test_svg_diagram_integration \
	    test/stdlib/test_template_email_integration \
	    test/stdlib/test_web_server_integration \
	    test/stdlib/test_ws_handshake_integration test/stdlib/test_webview \
	    test/stdlib/test_keychain test/test_pkg \
	    test/stdlib/test_security_integration test/stdlib/test_network_integration \
	    test/stdlib/test_viz_integration test/stdlib/test_data_pipeline \
	    test/stdlib/test_path test/stdlib/test_args \
	    test/stdlib/test_md test/stdlib/test_toml \
	    test/stdlib/test_vecstore \
	    test/stdlib/test_llm_live \
	    test/stdlib/test_tls test/tkir/test_tkir_reader \
	    fuzz-lexer fuzz-parser

# ── 121.30: the four original harnesses, folded into the machinery below ──
#
# fuzz-lexer, fuzz-parser, fuzz-http-parse and fuzz-url-route had rules of
# their own, libFuzzer-only, linked from the unsanitised shipping .o files,
# and NONE of the four worked:
#   fuzz-lexer / fuzz-parser  did not compile -- the lexer harness declared a
#       four-argument lex() (lexer.h's takes a Profile too) and the parser
#       harness put the opaque Arena on the stack.  nightly.yml built exactly
#       these two, so the nightly fuzz job was red at its compile step (it
#       is retired; security-nightly.yml fuzzes every FUZZ_TARGET).
#   fuzz-url-route  did not link (router.c needs ws.c), and the harness
#       free()d the string-literal body of every non-200 answer, so the first
#       403 aborted it.
#   fuzz-http-parse  fuzzed its own embedded COPY of an old parse_request;
#       src/stdlib/http.c was not in it at all.
# No workflow and no make ci target ran the last two, and nothing replayed
# the http adversarial input (test/fuzz/corpus-adversarial/http_chunked_
# malformed.txt, now http_parse_chunked_malformed.txt).  All four are now
# FUZZ_TARGETS, built by the same rule under either engine, replayed by
# fuzz-smoke (make ci and CI's ci-gates job) and fuzzed by the security
# nightly; `fuzz` and `fuzz-http` remain as names for the old groupings.
fuzz: fuzz-lexer fuzz-parser
	$(MAKE) --no-print-directory fuzz-120-22-run FUZZ_TARGETS="lexer parser"

fuzz-http: fuzz-http-parse fuzz-url-route fuzz-multipart fuzz-ws-frame
	$(MAKE) --no-print-directory fuzz-120-22-run \
	    FUZZ_TARGETS="http_parse url_route multipart ws_frame"

# ── 123.10: the eight 120.22 stdlib harnesses, wired ─────────────────────────
#
# test/fuzz/fuzz_{json,yaml,toml,toon,xml,template,multipart,ws_frame}.c were
# written for 120.22 and built by NO target (136.56's orphan gate exempted
# them by name, pending this story).  security-nightly.yml called
# `fuzz-120-22` / `fuzz-120-22-run`, which did not exist either, so its fuzz
# job was red by construction and was removed (123.10a).  Both are real now.
#
# TWO ENGINES, chosen by FUZZ_ENGINE (default auto):
#   libfuzzer   -fsanitize=fuzzer.  libFuzzer supplies main() and mutates.
#               This is fuzzing.  CI (Linux clang) builds this.
#   standalone  the harness is linked against test/fuzz/standalone_main.c,
#               which REPLAYS the files it is given -- no mutation.  Apple
#               clang ships no libclang_rt.fuzzer_osx.a, so on a developer Mac
#               this is the only way the harnesses build at all.  It proves
#               they compile, link and run clean on the seed and adversarial
#               corpora under ASAN+UBSAN.  It is a regression replay, NOT
#               fuzzing, and every recipe that runs it says so.
#   auto        libfuzzer if $(CC) can link a -fsanitize=fuzzer program, else
#               standalone.  Probed once per make, and only when a fuzz
#               target is actually built.
#
# The parser sources are compiled FROM SOURCE with the sanitizers, not linked
# from the shipping .o files, so the code under test is itself instrumented.
# UBSAN recovers by default and would print and carry on; it is made fatal.
#
# Seeds are tracked under test/fuzz/seeds/<target>/; the 120.x adversarial
# regressions under test/fuzz/corpus-adversarial/<target>_*.  A libFuzzer run
# never writes into either: it fuzzes a scratch copy under build/fuzz/corpus/
# and merges (minimises) what it found into build/fuzz/corpus-min/, which the
# nightly uploads.  Promoting a minimised input into seeds/ is a reviewed commit.
FUZZ_ENGINE ?= auto
FUZZ_SECONDS ?= 120
FUZZ_OUT = build/fuzz
FUZZ_SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined \
           -fno-omit-frame-pointer -g -O1
FUZZ_STDLIB_CFLAGS = -std=c99 -D_GNU_SOURCE -iquote src/stdlib
FUZZ_TARGETS_120_22 = json yaml toml toon xml template multipart ws_frame
# 122.13 (11): test/fuzz/fuzz_image.c (122.9) had no target -- only C138
# built it, by hand.  It joins the set here, so fuzz-120-22 builds it,
# fuzz-smoke replays test/fuzz/seeds/image/ through it, and the nightly
# fuzz-120-22-run fuzzes it.
FUZZ_TARGETS = $(FUZZ_TARGETS_120_22) image lexer parser http_parse url_route
fuzz_have_libfuzzer = $(shell d=$$(mktemp -d "$${TMPDIR:-/tmp}/tkc_fuzzprobe.XXXXXX") && \
    printf 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long n){(void)d;(void)n;return 0;}\n' > "$$d/p.c" && \
    $(CC) -fsanitize=fuzzer -o "$$d/p" "$$d/p.c" >/dev/null 2>&1 && echo libfuzzer || echo standalone; \
    [ -n "$$d" ] && rm -rf "$$d")
# Memoised: evaluated on first use, then a plain value.
FUZZ_ENGINE_R = $(eval FUZZ_ENGINE_R := $(if $(filter auto,$(FUZZ_ENGINE)),$(fuzz_have_libfuzzer),$(FUZZ_ENGINE)))$(FUZZ_ENGINE_R)
FUZZ_LINK = $(if $(filter libfuzzer,$(FUZZ_ENGINE_R)),-fsanitize=fuzzer,test/fuzz/standalone_main.c)

# $(1)=target  $(2)=harness + sources (the harness path written out, so
# check-test-orphans can see it)  $(3)=extra flags/libs
define fuzz_build_rule
	@case "$(FUZZ_ENGINE_R)" in libfuzzer|standalone) ;; \
	  *) echo "FUZZ_ENGINE=$(FUZZ_ENGINE_R): must be libfuzzer, standalone or auto" >&2; exit 2 ;; esac
	@mkdir -p $(FUZZ_OUT)
	@echo "fuzz: building fuzz-$(subst _,-,$(1)) [$(FUZZ_ENGINE_R)]"
	$(CC) $(FUZZ_SAN) $(FUZZ_STDLIB_CFLAGS) -o $(FUZZ_OUT)/fuzz-$(subst _,-,$(1)) \
	    $(FUZZ_LINK) $(2) $(3)
endef

.PHONY: fuzz-json fuzz-yaml fuzz-toml fuzz-toon fuzz-xml fuzz-template \
	fuzz-multipart fuzz-ws-frame fuzz-image fuzz-120-22 fuzz-120-22-run fuzz-smoke

fuzz-json:
	$(call fuzz_build_rule,json,test/fuzz/fuzz_json.c src/stdlib/json.c,)
fuzz-yaml:
	$(call fuzz_build_rule,yaml,test/fuzz/fuzz_yaml.c src/stdlib/yaml.c,)
fuzz-toml:
	$(call fuzz_build_rule,toml,test/fuzz/fuzz_toml.c src/stdlib/toml.c src/stdlib/file.c $(TOML_SRCS),$(TOML_FLAGS))
fuzz-toon:
	$(call fuzz_build_rule,toon,test/fuzz/fuzz_toon.c src/stdlib/toon.c,)
fuzz-xml:
	$(call fuzz_build_rule,xml,test/fuzz/fuzz_xml.c src/stdlib/xml.c,)
fuzz-template:
	$(call fuzz_build_rule,template,test/fuzz/fuzz_template.c src/stdlib/template.c,)
# http.c's TLS paths are compiled out without TK_HAVE_OPENSSL; it still calls
# the capability checks, hence capabilities.c (not in 120.22's link line).
fuzz-multipart:
	$(call fuzz_build_rule,multipart,test/fuzz/fuzz_multipart.c src/stdlib/http.c src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c src/stdlib/capabilities.c,-lz)
fuzz-ws-frame:
	$(call fuzz_build_rule,ws_frame,test/fuzz/fuzz_ws_frame.c src/stdlib/ws.c,)
fuzz-image:
	$(call fuzz_build_rule,image,test/fuzz/fuzz_image.c src/stdlib/image.c,-lm)
# 121.30: the compiler front end, compiled from source like the rest.
fuzz-lexer:
	$(call fuzz_build_rule,lexer,test/fuzz/fuzz_lexer.c src/lexer.c src/diag.c src/arena.c,)
fuzz-parser:
	$(call fuzz_build_rule,parser,test/fuzz/fuzz_parser.c src/lexer.c src/parser.c src/diag.c src/arena.c,)
# The real server loop (http_handle_fd) over a socketpair, not a copy of it.
fuzz-http-parse:
	$(call fuzz_build_rule,http_parse,test/fuzz/fuzz_http_parse.c src/stdlib/http.c src/stdlib/encoding.c src/stdlib/str.c src/stdlib/log.c src/stdlib/capabilities.c,-lz -lpthread)
fuzz-url-route:
	$(call fuzz_build_rule,url_route,test/fuzz/fuzz_url_route.c src/stdlib/router.c src/stdlib/str.c src/stdlib/ws.c src/stdlib/capabilities.c,-lz)

# Build all of them (the name security-nightly.yml calls; image joined in
# 122.13; lexer, parser, http_parse and url_route in 121.30).
fuzz-120-22: fuzz-json fuzz-yaml fuzz-toml fuzz-toon fuzz-xml fuzz-template \
	fuzz-multipart fuzz-ws-frame fuzz-image \
	fuzz-lexer fuzz-parser fuzz-http-parse fuzz-url-route

# Replay every seed and adversarial input through every harness, once.  Works
# under either engine (libFuzzer's -runs=0 executes the given corpus and
# exits), so it is the same check here and on CI.  Fails on any sanitizer
# report, on a harness that replays nothing, or on a target with no seeds.
fuzz-smoke: fuzz-120-22
	@echo "=== fuzz-smoke: replay seeds + adversarial inputs [$(FUZZ_ENGINE_R)] -- a replay, not fuzzing ==="
	@fail=0; for t in $(FUZZ_TARGETS); do \
	  b=$(FUZZ_OUT)/fuzz-$$(echo $$t | tr _ -); \
	  [ -d test/fuzz/seeds/$$t ] && [ -n "$$(ls test/fuzz/seeds/$$t)" ] \
	    || { echo "  FAIL $$t: no seeds in test/fuzz/seeds/$$t"; fail=1; continue; }; \
	  adv=$$(ls test/fuzz/corpus-adversarial/$${t}_* 2>/dev/null); \
	  if [ "$(FUZZ_ENGINE_R)" = libfuzzer ]; then \
	    out=$$($$b -runs=0 test/fuzz/seeds/$$t $$adv 2>&1); rc=$$?; \
	  else \
	    out=$$($$b test/fuzz/seeds/$$t $$adv 2>&1); rc=$$?; \
	  fi; \
	  n=$$(ls test/fuzz/seeds/$$t | wc -l | tr -d ' '); a=$$(printf '%s\n' $$adv | grep -c . ); \
	  if [ $$rc -eq 0 ]; then echo "  ok   $$t  ($$n seed(s), $$a adversarial)"; \
	  else echo "  FAIL $$t (exit $$rc)"; \
	    printf '%s\n' "$$out" | grep -a -E 'ERROR: |runtime error|SUMMARY|fuzz-replay' | head -6 | sed 's/^/      /'; fail=1; fi; \
	done; \
	[ $$fail -eq 0 ] && echo "fuzz-smoke: $(words $(FUZZ_TARGETS)) harnesses replayed clean" \
	  || { echo "fuzz-smoke: FAILED"; exit 1; }

# The nightly run.  libfuzzer: fuzz each target for FUZZ_SECONDS on a scratch
# copy of its seeds, crash inputs to $(FUZZ_OUT)/crash-<target>-*, then
# minimise the grown corpus into $(FUZZ_OUT)/corpus-min/<target>.
# standalone: there is no fuzzer to run -- it says so by name and replays
# (fuzz-smoke), rather than printing a green "fuzzed" it did not do.
fuzz-120-22-run: fuzz-120-22
	@if [ "$(FUZZ_ENGINE_R)" != libfuzzer ]; then \
	  echo "SKIP fuzz-120-22-run: FUZZ_ENGINE=$(FUZZ_ENGINE_R) -- $(CC) cannot link -fsanitize=fuzzer here,"; \
	  echo "     so NO FUZZING RAN.  Replaying the corpora instead (make fuzz-smoke)."; \
	  $(MAKE) --no-print-directory fuzz-smoke FUZZ_ENGINE=standalone; exit $$?; \
	fi; \
	fail=0; for t in $(FUZZ_TARGETS); do \
	  b=$(FUZZ_OUT)/fuzz-$$(echo $$t | tr _ -); \
	  rm -rf $(FUZZ_OUT)/corpus/$$t $(FUZZ_OUT)/corpus-min/$$t; \
	  mkdir -p $(FUZZ_OUT)/corpus/$$t $(FUZZ_OUT)/corpus-min/$$t; \
	  cp test/fuzz/seeds/$$t/* $(FUZZ_OUT)/corpus/$$t/; \
	  cp test/fuzz/corpus-adversarial/$${t}_* $(FUZZ_OUT)/corpus/$$t/ 2>/dev/null; \
	  echo "=== fuzz $$t for $(FUZZ_SECONDS)s ==="; \
	  $$b -max_total_time=$(FUZZ_SECONDS) -artifact_prefix=$(FUZZ_OUT)/crash-$$t- \
	      $(FUZZ_OUT)/corpus/$$t || fail=1; \
	  $$b -merge=1 $(FUZZ_OUT)/corpus-min/$$t $(FUZZ_OUT)/corpus/$$t >/dev/null 2>&1 || true; \
	done; \
	exit $$fail

# ── 123.10a: SBOM refresh + dependency CVE recheck ────────────────────────
# security-nightly.yml's weekly job called `make sbom` and `make cve-scan`
# and neither target existed, so the job was red by construction.  Text is
# Section 3 of docs/security/audit-120/ci-security.md, which specifies them
# completely; the same syft invocation as release.yml, so SBOMs taken
# between releases stay comparable.
#
# syft and grype are installed by the workflow, not vendored here.  --fail-on
# high honours the dependency-tracking.md SLA: Medium/Low are reported, not
# gated.
#
# 123.10: local runs make NO network call, and a missing tool is a named SKIP
# rather than `syft: command not found`.  In CI (GitHub sets CI=true) a
# missing tool FAILS -- there the step is real or it is red.
#   sbom      syft with its update check off (it phones home by default).
#   cve-scan  grype needs a vulnerability database, which it downloads.
#             Locally the download and the update check are off, so it uses
#             a database already in its cache or fails saying it has none;
#             CI downloads a fresh one, which is the point of the job.
# SBOM_CI=1 forces the CI behaviour locally; SBOM_CI=0 the local one.
SBOM_CI ?= $(if $(filter true 1,$(CI)),1,0)
sbom: $(BIN)
	@if ! command -v syft >/dev/null 2>&1; then \
	  if [ "$(SBOM_CI)" = 1 ]; then echo "sbom: FAIL -- syft is not installed (CI must install it)" >&2; exit 1; fi; \
	  echo "SKIP sbom: syft is not installed (local run; the CI job installs it, and there this step is real)"; \
	  exit 0; \
	fi; \
	SYFT_CHECK_FOR_APP_UPDATE=false syft packages file:./$(BIN) -o spdx-json > tkc-sbom.spdx.json && \
	{ command -v sha256sum >/dev/null 2>&1 && sha256sum tkc-sbom.spdx.json || shasum -a 256 tkc-sbom.spdx.json; } \
	  > tkc-sbom.spdx.json.sha256 && \
	echo "Wrote tkc-sbom.spdx.json (+ .sha256)"

cve-scan: sbom
	@if ! command -v grype >/dev/null 2>&1 || [ ! -s tkc-sbom.spdx.json ]; then \
	  if [ "$(SBOM_CI)" = 1 ]; then echo "cve-scan: FAIL -- grype is not installed or no SBOM was written" >&2; exit 1; fi; \
	  echo "SKIP cve-scan: $$(command -v grype >/dev/null 2>&1 && echo 'no SBOM (sbom skipped)' || echo 'grype is not installed') -- local run, nothing was scanned"; \
	  exit 0; \
	fi; \
	if [ "$(SBOM_CI)" = 1 ]; then \
	  grype sbom:tkc-sbom.spdx.json --fail-on high --output table; \
	else \
	  echo "cve-scan: local run -- DB auto-update and update check OFF (no network); using the cached DB"; \
	  GRYPE_DB_AUTO_UPDATE=false GRYPE_CHECK_FOR_APP_UPDATE=false \
	    grype sbom:tkc-sbom.spdx.json --fail-on high --output table; \
	fi

# ── 121.30: secret scan (Section 1 of docs/security/audit-120/ci-security.md) ─
# gitleaks over the working tree with the committed .gitleaks.toml (default
# rules + the reviewed allowlist).  It was specified in 120.24 and never
# landed: no secret scan ran in `make ci` or on any push.  Same convention as
# sbom/cve-scan: locally a missing gitleaks is a named SKIP; in CI (CI=true)
# it FAILS, so the step there is real or it is red.  No network either way.
# Only the rule id, file and line of a finding are printed (--redact).
secrets-scan:
	@if ! command -v gitleaks >/dev/null 2>&1; then \
	  if [ "$(SBOM_CI)" = 1 ]; then echo "secrets-scan: FAIL -- gitleaks is not installed (CI must install it)" >&2; exit 1; fi; \
	  echo "SKIP secrets-scan: gitleaks is not installed -- NO SECRET SCAN RAN (local run; CI installs it)"; \
	  exit 0; \
	fi; \
	r=$$(mktemp "$${TMPDIR:-/tmp}/tkc_gitleaks.XXXXXX"); \
	gitleaks dir . --config .gitleaks.toml --redact --no-banner --log-level error \
	    --exit-code 1 --report-format csv --report-path "$$r"; rc=$$?; \
	if [ $$rc -eq 0 ]; then echo "secrets-scan: gitleaks found nothing (.gitleaks.toml)"; \
	else echo "secrets-scan: FAIL -- gitleaks findings (rule, file, line):" >&2; \
	  awk -F, 'NR>1 {print "  " $$1 "  " $$3 ":" $$7}' "$$r" >&2; fi; \
	rm -f "$$r"; exit $$rc

.PHONY: secrets-scan

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

# ── 123.8: NEITHER OF THESE TWO COULD FAIL ─────────────────────────────────
#
# Both ran `$(BIN) --check` -- the bare word `toke`, with no `./`. make does
# not prepend `./`, so the shell resolved it through PATH, and the recipe
# checked whatever compiler PATH happened to find. Measured here on
# 2026-09-24: `which toke` in this worktree returned
# /Users/matthew.watt/tk/toke/toke -- ANOTHER CHECKOUT's binary, a different
# md5 from the one the same `make` had just built two lines earlier. The two
# gates disagreed with each other about the same file for that reason alone.
#
# On a CI runner there is no `toke` on PATH at all, and that is worse, because
# of the second defect: the verdict was "did the OUTPUT contain
# severity":"error"", so NO output passed. The compiler's exit status was
# discarded. Proven by substituting a command that does not exist:
#
#     $ for f in test/standalone/test_*_full.tk; do
#     >   if nonexistent-toke --check "$f" 2>&1 | grep -q '"severity":"error"'
#     >   then echo FAIL; else echo "  OK: $f"; fi; done
#     ... OK for every file ...
#     9 checked, 0 failed   <-- with a compiler that DOES NOT EXIST
#
# So these two targets would have reported "48 checked, 0 failed" from a CI
# job forever. This is the third gate-that-cannot-fail of this epic, after
# `make lint` (135.3a) and `make build-all` (136.62), and it was one `./`
# away from being wired into PR CI by this very story.
#
# Fixed three ways: `./$(BIN)`, so the binary is the one in THIS tree; an
# order-only-style prerequisite so it exists; and a verdict that fails on a
# non-zero exit status as well as on a diagnostic, so silence is no longer a
# pass.
define check_tk_files
	@fails=0; total=0; \
	for f in $(1); do \
		[ -e "$$f" ] || { echo "  ERROR: no such file: $$f"; fails=$$((fails+1)); continue; }; \
		total=$$((total + 1)); \
		out="$$(./$(BIN) --check "$$f" 2>&1)"; rc=$$?; \
		if [ $$rc -ne 0 ]; then \
			echo "  FAIL: $$f (tkc exited $$rc)"; \
			echo "$$out" | head -3 | sed 's/^/        /'; \
			fails=$$((fails + 1)); \
		elif printf '%s' "$$out" | grep -q '"severity":"error"'; then \
			echo "  FAIL: $$f"; \
			printf '%s' "$$out" | head -2 | sed 's/^/        /'; \
			fails=$$((fails + 1)); \
		else \
			echo "  OK: $$f"; \
		fi; \
	done; \
	[ $$total -gt 0 ] || { echo "  ERROR: the glob matched no files at all"; exit 1; }; \
	echo "$$total checked, $$fails failed"; \
	test $$fails -eq 0
endef

# Syntax-check all new e2e and standalone tests
test-e2e-check: $(BIN)
	@echo "=== E2E syntax check ==="
	$(call check_tk_files,test/e2e/e2e_*.tk)

test-standalone-check: $(BIN)
	@echo "=== Standalone syntax check ==="
	$(call check_tk_files,test/standalone/test_*_full.tk test/standalone/test_process_env.tk)

# Run all test suites
test-all: test-stdlib test-e2e-check test-standalone-check conform
	@echo ""
	@echo "=== All test suites complete ==="
