#!/usr/bin/env bash
# S003_toml_value_vs_error.sh — std.toml must not conflate a value with an
# error (story 127.67).
#
# The std.toml wrappers return a bare i64 and the default codegen for
# `mt toml.x(...) {$ok:…;$err:…}` takes the $ok arm when that i64 is
# non-zero.  Sound for a table handle or a string pointer; wrong for a value
# whose valid domain includes 0.  `minify = false` and `retries = 0` were
# reported as errors, callers read that as "key absent" and substituted their
# default, so a key that says false was obeyed as true.
#
# Two halves:
#
#   1. The stdlib half — the wrappers keep @tk_current_error truthful and
#      always return the real value, the same protocol str.toint uses
#      (114.53/114.54); and toml.section no longer returns the PARENT table
#      when the key is missing.  Pinned by test/stdlib/test_toml_glue.c and
#      by the toke-level section case below.
#
#   2. The codegen half — llvm.c's is_num_parse_wrapper() lists the wrappers
#      whose $ok/$err arm is chosen from @tk_current_error instead of the 0
#      sentinel.  tk_toml_bool_w and tk_toml_i64_w were added there once the
#      127.64 bisect released llvm.c, so `mt toml.bool(cfg; "minify")` now
#      takes the $ok arm for a false value.  Pinned by
#      test/conform/C005_toml_false_zero_vs_missing.sh.
#
# Story: 127.67

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

WORK="$(mktemp -d /tmp/tkc_tomlerr_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "S003: std.toml distinguishes a value of false/0 from an error"
echo "--------------------------------------"

# ── 1: the wrapper-level protocol ─────────────────────────────────────
# 127.268: the link set is the one a program importing std.toml is linked
# with, read from tkc's own dependency table (src/stdlib_deps.c via
# --emit-deps): the real runtime, toml's cores, glue, vendored parser and
# dependencies.  It used to be listed here by hand, and 121.74's toml.c ->
# file.c dependency broke it at merge.  -DTK_TEST_REAL_RUNTIME retires the
# test's own runtime stand-ins in favour of the real tk_runtime.c.
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -Wno-misleading-indentation -g -DTK_TEST_REAL_RUNTIME -I"${REPO_ROOT}/stdlib/vendor/tomlc99")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${REPO_ROOT}/src/stdlib" --cc "${CC}" --objects toml -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/test_toml_glue" \
        "${REPO_ROOT}/test/stdlib/test_toml_glue.c" \
        "${LINK[@]}" \
        -lm -lpthread >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: wrapper harness did not build"
    sed 's/^/      /' "${WORK}/build.log"
    echo "Results: 0 passed, 1 failed"
    exit 1
fi

if "${WORK}/test_toml_glue" >"${WORK}/glue.log" 2>&1; then
    echo "  PASS: wrapper protocol ($(grep -c '^pass:' "${WORK}/glue.log") assertions)"
    PASS=$((PASS + 1))
else
    echo "  FAIL: wrapper protocol"
    grep '^FAIL' "${WORK}/glue.log" | sed 's/^/      /'
    FAIL=$((FAIL + 1))
fi

# ── 2: the toke-level consequence that is fixed today ─────────────────
# A missing section used to come back as the parent table through the $ok
# arm, so every following lookup silently read top-level keys.
if [ ! -x "${TKC}" ]; then
    echo "  SKIP: tkc binary not found at ${TKC} (run 'make' first)"
else
    cd "${WORK}"
    cat > sect.tk <<'TOKE'
m=tomlsect;
i=toml:std.toml;
i=io:std.io;

f=main():i64{
  let src = "port = 8080\n[server]\nhost = \"localhost\"\n";
  let cfg = mt toml.load(src) {$ok:v v;$err:e < 1};
  let good = mt toml.section(cfg; "server") {$ok:v io.println("server -> OK") ;$err:e io.println("server -> ERROR")};
  let gone = mt toml.section(cfg; "nosuch") {$ok:v io.println("nosuch -> OK") ;$err:e io.println("nosuch -> ERROR")};
  < 0
};
TOKE
    if ! "${TKC}" --allow-all --out sect_bin sect.tk >compile.log 2>&1; then
        echo "  FAIL: toml.section case did not compile"
        sed 's/^/      /' compile.log
        FAIL=$((FAIL + 1))
    else
        got="$(./sect_bin 2>&1)"
        want=$'server -> OK\nnosuch -> ERROR'
        if [ "${got}" = "${want}" ]; then
            echo "  PASS: a missing toml.section takes the \$err arm, not \$ok-with-the-parent"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: toml.section of a missing key"
            echo "      expected: $(printf '%s' "${want}" | tr '\n' '|')"
            echo "      got:      $(printf '%s' "${got}"  | tr '\n' '|')"
            FAIL=$((FAIL + 1))
        fi
    fi
    cd "${REPO_ROOT}"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
