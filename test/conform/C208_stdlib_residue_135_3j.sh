#!/usr/bin/env bash
# C208_stdlib_residue_135_3j.sh — story 135.3j (8): metrics duration_sum,
# the parsetoolcalls_w leak, hpack_insert's unchecked strdup, and the dead
# tk_cache_*_w stubs.  (chart_histogram at n == 0 is C201's.)
#
# THE DEFECTS (measured on main before 135.3j (8)).
#   metrics.c: `volatile double duration_sum` was written by nothing and
#     exported by nothing: http_request_duration_seconds had buckets and a
#     count but no _sum, so no mean latency could be derived.
#   llm_tool_glue.c: tk_llmtool_parsetoolcalls_w (and chatwithtools_w)
#     returned a block for calls[0] and dropped the rest of the parse result:
#     calls[0].args_json, every string of calls[1..], and the calls array --
#     on every successful parse.
#   http2.c: hpack_insert strdup'd the name/value after shifting the table,
#     unchecked; a failed strdup left a counted entry with a NULL name that
#     the next lookup handed out and the encoder strcmp'd.  Its callers
#     ignored its result.
#   tk_web_glue.c: tk_cache_{new,set,get,del}_w were stubs no .tki declared
#     and nothing called.
#
# WHAT THIS DRIVES
#   (A) metrics_record_request 250 ms + 1500.5 ms, then metrics_prometheus:
#       expects http_request_duration_seconds_sum 1.750500 (ASan+UBSan).
#   (B) llm_tool_glue.c/llm_tool.c/llm.c built with a counting allocator
#       (-include): the live allocations a parse leaves behind must be exactly
#       what the returned block owns, for a one-call and a three-call response
#       alike.  127.295 (tranche 7): the block's name and id are sealed strings
#       from the reserved range, not malloc, so the malloc'd part it owns is
#       the block and the args array = 2 (it was 4 while name and id were
#       malloc'd); the name and id must be sealed (header == strlen).
#   (C) http2.c #included with strdup failing on demand: hpack_insert must
#       return -1 and leave the table unchanged; hpack_encode must report the
#       failure (ASan+UBSan).
#   (D) no src/stdlib/*.c defines tk_cache_*_w.
#
# Negative control: C208_STDLIB=<dir> runs against another stdlib tree;
# main's FAILs A, B (both), C and D.
#
# Story: 135.3j

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C208_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c208_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C208: metrics _sum, parsetoolcalls leak, hpack strdup, dead cache stubs"
echo "------------------------------------------------------------------------"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

run() {   # name, then the command; each driver prints PASS/FAIL lines itself
    local name="$1"; shift
    if "$@" >"${WORK}/${name}.out" 2>&1; then :; fi
    awk '/^  (PASS|FAIL|info)/' "${WORK}/${name}.out"
    PASS=$((PASS + $(awk '/^  PASS:/{n++} END{print n+0}' "${WORK}/${name}.out")))
    FAIL=$((FAIL + $(awk '/^  FAIL:/{n++} END{print n+0}' "${WORK}/${name}.out")))
    if ! awk '/^RESULT OK$/{f=1} END{exit !f}' "${WORK}/${name}.out"; then
        fail "${name}: driver did not finish: $(awk '/ERROR|SUMMARY|runtime error/' "${WORK}/${name}.out" | head -3)"
    fi
}

# ---- (A) metrics ----
cat > "${WORK}/a.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
void  metrics_record_request(uint32_t status, size_t bytes, double latency_ms);
char *metrics_prometheus(void);
int main(void) {
    metrics_record_request(200, 10, 250.0);
    metrics_record_request(500, 10, 1500.5);
    char *p = metrics_prometheus();
    const char *want = "http_request_duration_seconds_sum 1.750500\n";
    if (p && strstr(p, want)) printf("  PASS: prometheus output has %s", want);
    else printf("  FAIL: prometheus output has no http_request_duration_seconds_sum 1.750500\n");
    if (p && strstr(p, "http_request_duration_seconds_count 2\n")) printf("  PASS: _count is 2\n");
    else printf("  FAIL: _count is not 2\n");
    free(p);
    printf("RESULT OK\n");
    return 0;
}
C
if "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o "${WORK}/a" "${WORK}/a.c" "${S}/metrics.c" \
        -lpthread >"${WORK}/a.cc" 2>&1; then
    run metrics "${WORK}/a"
else
    fail "metrics driver does not build: $(head -c 400 "${WORK}/a.cc")"
fi

# ---- (B) parsetoolcalls_w ownership ----
cat > "${WORK}/cnt.h" <<'C'
#include <stdlib.h>
#include <string.h>
void *cnt_malloc(size_t); void *cnt_calloc(size_t, size_t); void *cnt_realloc(void *, size_t);
void cnt_free(void *); char *cnt_strdup(const char *); char *cnt_strndup(const char *, size_t);
#define malloc(n)     cnt_malloc(n)
#define calloc(a, b)  cnt_calloc(a, b)
#define realloc(p, n) cnt_realloc(p, n)
#define free(p)       cnt_free(p)
#define strdup(s)     cnt_strdup(s)
#define strndup(s, n) cnt_strndup(s, n)
C
cat > "${WORK}/cnt.c" <<'C'
#include <stdlib.h>
#include <string.h>
long cnt_live = 0;
void *cnt_malloc(size_t n) { void *p = malloc(n); if (p) cnt_live++; return p; }
void *cnt_calloc(size_t a, size_t b) { void *p = calloc(a, b); if (p) cnt_live++; return p; }
void *cnt_realloc(void *o, size_t n) { void *p = realloc(o, n); if (p && !o) cnt_live++; return p; }
void cnt_free(void *p) { if (p) cnt_live--; free(p); }
char *cnt_strdup(const char *s) { char *p = strdup(s); if (p) cnt_live++; return p; }
char *cnt_strndup(const char *s, size_t n) { char *p = strndup(s, n); if (p) cnt_live++; return p; }
C
cat > "${WORK}/b.c" <<'C'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "tk_strhdr.h"
/* tk_current_error comes from the real tk_runtime.c in the link set (127.268). */
extern long cnt_live;
int64_t tk_llmtool_parsetoolcalls_w(int64_t raw);
static const char ONE[] =
  "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
  "{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"a\",\"arguments\":\"{}\"}}"
  "]}}]}";
static const char THREE[] =
  "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
  "{\"id\":\"call_1\",\"type\":\"function\",\"function\":{\"name\":\"a\",\"arguments\":\"{}\"}},"
  "{\"id\":\"call_2\",\"type\":\"function\",\"function\":{\"name\":\"b\",\"arguments\":\"{}\"}},"
  "{\"id\":\"call_3\",\"type\":\"function\",\"function\":{\"name\":\"c\",\"arguments\":\"{}\"}}"
  "]}}]}";
int main(void) {
    long b0 = cnt_live;
    int64_t h1 = tk_llmtool_parsetoolcalls_w((int64_t)(intptr_t)ONE);
    long d1 = cnt_live - b0;
    long b1 = cnt_live;
    int64_t h3 = tk_llmtool_parsetoolcalls_w((int64_t)(intptr_t)THREE);
    long d3 = cnt_live - b1;
    printf("  info: live allocations left per parse: one call %ld, three calls %ld\n", d1, d3);
    if (h1 && strcmp((const char *)(intptr_t)((int64_t *)(intptr_t)h1)[0], "a") == 0
           && strcmp((const char *)(intptr_t)((int64_t *)(intptr_t)h1)[2], "call_1") == 0)
        printf("  PASS: the block carries calls[0]'s name and id\n");
    else printf("  FAIL: the block does not carry calls[0]'s name and id\n");
    const char *nm = h1 ? (const char *)(intptr_t)((int64_t *)(intptr_t)h1)[0] : NULL;
    const char *id = h1 ? (const char *)(intptr_t)((int64_t *)(intptr_t)h1)[2] : NULL;
    if (nm && id && tks_hdrlen(nm) == (int64_t)strlen(nm) && tks_hdrlen(id) == (int64_t)strlen(id))
        printf("  PASS: the block's name and id are sealed (127.295)\n");
    else printf("  FAIL: the block's name and id are not sealed (127.295)\n");
    if (d1 == 2) printf("  PASS: a one-call parse leaves exactly what the block owns (2 malloc'd)\n");
    else printf("  FAIL: a one-call parse leaves %ld allocations, the block owns 2\n", d1);
    if (d3 == 2 && h3) printf("  PASS: a three-call parse leaves exactly what the block owns (2 malloc'd)\n");
    else printf("  FAIL: a three-call parse leaves %ld allocations, the block owns 2\n", d3);
    printf("RESULT OK\n");
    return 0;
}
C
# 127.268: the stdlib link sets below come from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not hand lists that a stdlib
# dependency change silently breaks.
LINK_B=()
if LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" llmtool 2>"${WORK}/b.cc")"; then
    while IFS= read -r l; do LINK_B+=("$l"); done <<< "${LINKSET}"
fi
if [ "${#LINK_B[@]}" -gt 0 ] \
   && "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 -iquote "${S}" -c -o "${WORK}/cnt.o" "${WORK}/cnt.c" >>"${WORK}/b.cc" 2>&1 \
   && "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 -iquote "${S}" -include "${WORK}/cnt.h" -o "${WORK}/b" "${WORK}/b.c" \
        "${LINK_B[@]}" "${WORK}/cnt.o" -lm -lpthread >>"${WORK}/b.cc" 2>&1; then
    run parsetoolcalls "${WORK}/b"
else
    fail "parsetoolcalls driver does not build: $(head -c 400 "${WORK}/b.cc")"
fi

# ---- (C) hpack_insert with strdup failing ----
cat > "${WORK}/c.c" <<C
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fail_strdup = 0;
static char *t_strdup(const char *s) {
    if (fail_strdup && --fail_strdup == 0) return NULL;
    return strdup(s);
}
#define strdup(s) t_strdup(s)
#include "${S}/http2.c"
#undef strdup
int main(void) {
    HpackTable t;
    hpack_table_init(&t, 4096);
    int r0 = hpack_insert(&t, "x-a", "1");
    size_t n0 = t.count, s0 = t.size;
    fail_strdup = 2;                   /* the second strdup of the next insert */
    int r1 = hpack_insert(&t, "x-b", "2");
    if (r0 == 0 && r1 == -1) printf("  PASS: hpack_insert reports a failed strdup (-1)\n");
    else printf("  FAIL: hpack_insert returned %d on a failed strdup\n", r1);
    int clean = t.count == n0 && t.size == s0;
    for (size_t i = 0; i < t.count; i++) if (!t.entries[i].name || !t.entries[i].value) clean = 0;
    if (clean) printf("  PASS: the table is unchanged, no NULL entry\n");
    else printf("  FAIL: the table holds %zu entries (was %zu), NULL name/value present\n", t.count, n0);
    const char *names[] = { "x-custom" }, *vals[] = { "v" };
    uint8_t out[256];
    fail_strdup = 1;
    int e = hpack_encode(&t, names, vals, 1, out, sizeof out);
    if (e < 0) printf("  PASS: hpack_encode reports the failed insert\n");
    else printf("  FAIL: hpack_encode returned %d with its insert failed\n", e);
    fail_strdup = 0;
    hpack_table_free(&t);
    printf("RESULT OK\n");
    return 0;
}
C
# http2.c is #included into c.c above, so it is left out of the set.
# 127.270: as cached objects built with the driver's flags.
CF_C=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -Wno-unused-function)
LINK_C=()
if LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except http2.c --cc "${CC}" \
        --objects http -- "${CF_C[@]}" 2>"${WORK}/c.cc")"; then
    while IFS= read -r l; do LINK_C+=("$l"); done <<< "${LINKSET}"
fi
if [ "${#LINK_C[@]}" -gt 0 ] && "${CC}" "${CF_C[@]}" -o "${WORK}/c" "${WORK}/c.c" \
        "${LINK_C[@]}" -lm -lz -lpthread >>"${WORK}/c.cc" 2>&1; then
    run hpack "${WORK}/c"
else
    fail "hpack driver does not build: $(head -c 400 "${WORK}/c.cc")"
fi

# ---- (D) the dead cache stubs ----
defs="$(awk '/^int64_t tk_cache_[a-z]+_w\(/{print FILENAME": "$0}' "${S}"/*.c)"
if [ -z "${defs}" ]; then pass "no src/stdlib/*.c defines tk_cache_*_w"
else fail "tk_cache_*_w still defined: $(echo "${defs}" | head -2 | tr '\n' ' ')"; fi

echo ""
echo "C208: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
