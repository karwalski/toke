#!/usr/bin/env bash
# C621_json_csv_alloc_failure.sh — json.c and csv.c survive every allocation
# failure on the paths 121.41 (1) named (story 121.41 item 1).
#
#   json_arr     collect_array copied each element into an unchecked malloc
#                and memcpy'd through it (json.c:310 on main): an allocation
#                failure was a write through NULL.
#   json writer  json_newwriter leaves cap 0 when its malloc fails, and
#                writer_ensure doubled 0 until it reached `needed` -- forever.
#   csv          rowbuf_push (csv.c:63) and csv_parse (csv.c:499) assigned
#                realloc's result back to the pointer they grew, so a failure
#                leaked the array and the next store wrote through NULL.  A
#                NULL field from a failed strbuf_dup was stored as a field.
#
# Each case runs the call once cleanly to count its allocations, then again
# with the Nth malloc/realloc inside json.c or csv.c failing, for every N.
# Every run must return -- not crash, not hang (alarm(10)) -- and either
# succeed in full or refuse: json_arr gives $err; csv_parse gives NULL, never
# a short document; csv_reader_next gives an empty row with kind "nomem",
# never a row with a field missing and never "eof" while rows remain.
#
# Binaries are built with -fsanitize=address,undefined and
# -fno-sanitize-recover, so a memory error is a hard failure.
#
# Negative control: C621_STDLIB=<main's src/stdlib at f605aec9> -- json_arr
# and csv_parse crash (SEGV in memcpy / the row store), the writer hangs
# (alarm), and csv_reader_next hands out short rows or "eof".
#
# Story: 121.41

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C621_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c621_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1"
CF="-std=c99 -D_GNU_SOURCE -g -O1 ${SAN}"

echo "C621: 121.41 (1) -- json.c / csv.c allocation failures"
echo "------------------------------------------------------"

# run BIN NAME [MODE] — report the program's OK/BAD lines; a crash or hang
# fails.  Each MODE is its own process, so one crash does not hide the rest.
run() {
    local bin="$1" name="$2" mode="${3:-}"
    "${bin}" ${mode} >"${WORK}/${name}.out" 2>&1
    local rc=$?
    while IFS= read -r line; do
        case "${line}" in
            "OK "*)  pass "${name}: ${line#OK }" ;;
            "BAD "*) fail "${name}: ${line#BAD }" ;;
        esac
    done < "${WORK}/${name}.out"
    if [ "${rc}" -ne 0 ]; then
        fail "${name}: exit ${rc} at $(grep -a '^AT ' "${WORK}/${name}.out" | tail -1) -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|SEGV|Alarm' "${WORK}/${name}.out" | head -c 300)"
    fi
}

# The fault hooks: every malloc/realloc in the file under test goes through
# them (the macros are force-included into that translation unit only).
cat > "${WORK}/fault.h" <<'C'
#include <stdlib.h>
void *c621_malloc(size_t n);
void *c621_realloc(void *p, size_t n);
#define malloc(n)     c621_malloc(n)
#define realloc(p, n) c621_realloc(p, n)
C
cat > "${WORK}/fault.c" <<'C'
#include <stdlib.h>
int c621_fail_at = 0, c621_calls = 0;
void *c621_malloc(size_t n)             { if (++c621_calls == c621_fail_at) return NULL; return malloc(n); }
void *c621_realloc(void *p, size_t n)   { if (++c621_calls == c621_fail_at) return NULL; return realloc(p, n); }
C

# ── json ────────────────────────────────────────────────────────────────────
cat > "${WORK}/t_json.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "json.h"
extern int c621_fail_at, c621_calls;
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);
    const char *mode = argc > 1 ? argv[1] : "";
    JsonResult d = json_dec("{\"k\":[\"alpha\",2,{\"x\":3},[4,5],null,true,\"g\"]}");
    if (d.is_err) { printf("BAD json_dec failed\n"); return 0; }
    if (!strcmp(mode, "writer")) goto writer;
    c621_fail_at = 0; c621_calls = 0;
    JsonArrayResult r = json_arr(d.ok, "k");
    int total = c621_calls;
    printf("%s json_arr reads seven elements (%d allocations)\n",
           (!r.is_err && r.ok.len == 7 && !strcmp(r.ok.data[6].raw, "\"g\"")) ? "OK" : "BAD", total);
    int ok = 1;
    for (int n = 1; n <= total; n++) {
        printf("AT json_arr fail_at=%d\n", n);
        c621_fail_at = n; c621_calls = 0;
        JsonArrayResult f = json_arr(d.ok, "k");      /* must return, not crash */
        if (!f.is_err) ok = 0;                        /* every allocation is needed */
    }
    printf("%s json_arr returns $err when any of its %d allocations fails\n", ok ? "OK" : "BAD", total);
    return 0;

writer:
    /* writer: its first malloc fails, then an emit must not spin */
    printf("AT writer\n");
    c621_fail_at = 1; c621_calls = 0;
    JsonWriter w = json_newwriter(16);
    c621_fail_at = 0;
    JsonStreamVoidResult e = json_streamemit(&w, d.ok);
    (void)e;
    printf("OK json_streamemit returns after json_newwriter's malloc failed\n");
    return 0;
}
C
# 127.295: json.c reads lengths with tk_strlen, whose string-heap state lives
# in str.c, so the rest of std.json's link set comes from stdlib_link_set.sh
# --objects (--except json.c: it is compiled above with the fault hooks, and
# only its own allocations are counted), never a hand list.
# shellcheck disable=SC2206
JCF=(${CF} -iquote "${S}" -I"${S}")
JLINK=()
if JLS="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except json.c --cc "${CC}" \
        --objects json -- "${JCF[@]}" 2>"${WORK}/jls.log")"; then
    while IFS= read -r l; do [ -n "$l" ] && JLINK+=("$l"); done <<< "${JLS}"
else
    echo "error: stdlib link set: $(head -c 300 "${WORK}/jls.log")" > "${WORK}/jb.cc"
fi
if [ "${#JLINK[@]}" -gt 0 ] \
   && "${CC}" "${JCF[@]}" -include "${WORK}/fault.h" -c -o "${WORK}/json.o" "${S}/json.c" >>"${WORK}/jb.cc" 2>&1 \
   && "${CC}" "${JCF[@]}" -o "${WORK}/t_json" "${WORK}/t_json.c" "${WORK}/json.o" "${WORK}/fault.c" \
        "${JLINK[@]}" -lm -lpthread >>"${WORK}/jb.cc" 2>&1; then
    run "${WORK}/t_json" json_arr arr
    run "${WORK}/t_json" json_writer writer
else
    fail "json: build: $(grep -a -m3 -E 'error' "${WORK}/jb.cc" | head -c 400)"
fi

# ── csv ─────────────────────────────────────────────────────────────────────
cat > "${WORK}/t_csv.c" <<'C'
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "csv.h"
extern int c621_fail_at, c621_calls;
const char *csv_lasterrkind(void);
static char doc[8192];
#define ROWS 20      /* > 16: csv_parse's row array grows */
#define COLS 10      /* > 8:  each row's field array grows  */
static int row_ok(StrArray a, int r) {
    char want[32];
    if (a.len != COLS) return 0;
    for (int c = 0; c < COLS; c++) {
        snprintf(want, sizeof want, "r%dc%d", r, c);
        if (!a.data[c] || strcmp(a.data[c], want)) return 0;
    }
    return 1;
}
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(10);
    const char *mode = argc > 1 ? argv[1] : "";
    size_t o = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            o += (size_t)snprintf(doc + o, sizeof doc - o, "r%dc%d%s", r, c, c == COLS - 1 ? "\n" : ",");
    uint64_t len = (uint64_t)o;
    int total, ok;
    if (!strcmp(mode, "reader")) goto reader;

    /* csv_parse: all rows or NULL */
    uint64_t n = 0;
    c621_fail_at = 0; c621_calls = 0;
    StrArray *rows = csv_parse(doc, len, &n);
    total = c621_calls; ok = (rows && n == ROWS);
    for (int r = 0; ok && r < ROWS; r++) ok = row_ok(rows[r], r);
    printf("%s csv_parse reads %d rows of %d (%d allocations)\n", ok ? "OK" : "BAD", ROWS, COLS, total);
    ok = 1;
    for (int k = 1; k <= total; k++) {
        printf("AT csv_parse fail_at=%d\n", k);
        c621_fail_at = k; c621_calls = 0;
        StrArray *f = csv_parse(doc, len, &n);
        if (f) {                                   /* then it must be whole */
            if (n != ROWS) ok = 0;
            for (int r = 0; ok && r < ROWS; r++) if (!row_ok(f[r], r)) ok = 0;
            if (!ok) printf("    fail_at=%d: a partial document (%llu rows)\n", k, (unsigned long long)n);
        }
    }
    printf("%s csv_parse returns NULL or the whole document when any of its %d allocations fails\n",
           ok ? "OK" : "BAD", total);
    return 0;

reader:
    /* csv_reader_next (lax): a row is whole, or empty with kind nomem */
    c621_fail_at = 0; c621_calls = 0;
    TkCsvReader *rd = csv_reader_new(doc, len);
    int before = c621_calls;
    for (int r = 0; r < ROWS; r++) (void)csv_reader_next(rd);
    int per_reader = c621_calls - before;
    csv_reader_free(rd);
    ok = 1;
    for (int k = 1; k <= per_reader; k++) {
        printf("AT csv_reader_next fail_at=%d\n", k);
        c621_fail_at = 0;
        TkCsvReader *q = csv_reader_new(doc, len);
        c621_fail_at = k; c621_calls = 0;
        for (int r = 0; r < ROWS; r++) {
            StrArray a = csv_reader_next(q);
            if (a.len == 0) {
                const char *kind = csv_lasterrkind();
                if (!kind || strcmp(kind, "nomem")) {
                    ok = 0; printf("    fail_at=%d row %d: empty row, kind \"%s\" (want nomem)\n", k, r, kind ? kind : "(null)");
                }
                break;                                 /* one refusal per run is enough */
            }
            if (!row_ok(a, r)) { ok = 0; printf("    fail_at=%d row %d: a row with %llu fields handed out\n", k, r, (unsigned long long)a.len); break; }
        }
        c621_fail_at = 0;
        csv_reader_free(q);
    }
    printf("%s csv_reader_next gives a whole row or refuses with nomem, for each of %d allocations\n",
           ok ? "OK" : "BAD", per_reader);
    return 0;
}
C
if "${CC}" ${CF} -iquote "${S}" -I"${S}" -include "${WORK}/fault.h" -c -o "${WORK}/csv.o" "${S}/csv.c" >"${WORK}/cb.cc" 2>&1 \
   && "${CC}" ${CF} -iquote "${S}" -I"${S}" -o "${WORK}/t_csv" "${WORK}/t_csv.c" "${WORK}/csv.o" "${WORK}/fault.c" >>"${WORK}/cb.cc" 2>&1; then
    run "${WORK}/t_csv" csv_parse parse
    run "${WORK}/t_csv" csv_reader reader
else
    fail "csv: build: $(grep -a -m3 -E 'error' "${WORK}/cb.cc" | head -c 400)"
fi

echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ] && [ "${PASS}" -gt 0 ]
