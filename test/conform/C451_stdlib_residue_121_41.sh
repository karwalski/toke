#!/usr/bin/env bash
# C451_stdlib_residue_121_41.sh — three residues of Batch H1's 2026-09-29
# pass (story 121.41, items 1-yaml, 2 and 4).
#
#   yaml    yaml_arr copied each sequence item into an unchecked malloc
#           (yaml.c:288 on main): an allocation failure was a memcpy
#           through NULL.  Driven here by failing the Nth malloc for every
#           N the call makes; each run must return, $ok or $err, not crash.
#   metrics metrics_log_access / metrics_log_error wrote the request path,
#           User-Agent and Referer into the access log raw (a CR/LF or a
#           quote forged a line or a JSON field), and log_error took a
#           caller format string.  Nothing reached them -- no caller, glue
#           wrapper or .tki entry -- so they are deleted, with their setters.
#           Asserted on the compiled object: neither symbol is defined and
#           metrics.o references no stdio writer.  C133 and C208 keep the
#           rest of metrics.c covered.
#   md      (a) the literal text TKMDTABLEPLACEHOLDER0 in a document was
#           replaced by a copy of table 0, and inside a link destination the
#           table's quotes closed the href, so its cell text became
#           attributes of the link; (b) a table whose separator row was 512
#           bytes or longer was never detected (the quick scan copied lines
#           into a char[512]), nor was any CRLF table (the scan kept the
#           '\r'); (c) md.c's file-top comment still said cmark passes raw
#           HTML through under CMARK_OPT_UNSAFE, which 124.3 removed.
#
# Every binary is built with -fsanitize=address,undefined and
# -fno-sanitize-recover, so a memory error is a hard failure.
#
# Negative control: C451_STDLIB=<dir> points the suite at another
# src/stdlib tree (its ../../stdlib/vendor/cmark must exist, as md.c
# includes cmark.h relative to itself).  A tree with main's yaml.c,
# metrics.c and md.c fails every case above.
#
# Story: 121.41

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C451_STDLIB:-${REPO_ROOT}/src/stdlib}"
CMARK_DIR="${REPO_ROOT}/stdlib/vendor/cmark/src"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c451_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
CF="-std=c99 -D_GNU_SOURCE -g -O1 ${SAN}"

echo "C451: 121.41 residue -- yaml OOM, dead metrics log writers, md tables"
echo "---------------------------------------------------------------------"

# run BIN NAME — report the program's OK/BAD lines; a crash is a failure.
run() {
    local bin="$1" name="$2"
    "${bin}" >"${WORK}/${name}.out" 2>&1
    local rc=$?
    while IFS= read -r line; do
        case "${line}" in
            "OK "*)  pass "${name}: ${line#OK }" ;;
            "BAD "*) fail "${name}: ${line#BAD }" ;;
        esac
    done < "${WORK}/${name}.out"
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/${name}.out"; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|SEGV' "${WORK}/${name}.out" | head -c 300)"
    fi
}

# ── yaml: fail the Nth malloc inside yaml.c ─────────────────────────────────
cat > "${WORK}/fault.h" <<'C'
#include <stdlib.h>
void *c451_malloc(size_t n);
#define malloc(n) c451_malloc(n)
C
cat > "${WORK}/t_yaml.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yaml.h"
static int fail_at = 0, calls = 0;
void *c451_malloc(size_t n) {
    if (++calls == fail_at) return NULL;
    return malloc(n);
}
int main(void) {
    const char *doc = "k:\n  - alpha\n  - beta\n  - gamma\n";
    YamlResult d = yaml_dec(doc);
    /* one clean run: count the allocations and check the result */
    fail_at = 0; calls = 0;
    YamlArrayResult r = yaml_arr(d.ok, "k");
    int total = calls;
    printf("%s yaml_arr reads three items (%d allocations)\n",
           (!r.is_err && r.ok.len == 3 && !strcmp(r.ok.data[2].raw, "gamma")) ? "OK" : "BAD", total);
    int ok = 1;
    for (int n = 1; n <= total; n++) {
        fail_at = n; calls = 0;
        YamlArrayResult f = yaml_arr(d.ok, "k");   /* must return, not crash */
        if (!f.is_err) ok = 0;                     /* every allocation is needed */
    }
    printf("%s yaml_arr returns $err when any of its %d allocations fails\n", ok ? "OK" : "BAD", total);
    return 0;
}
C
if "${CC}" ${CF} -iquote "${S}" -I"${S}" -include "${WORK}/fault.h" -c -o "${WORK}/yaml.o" "${S}/yaml.c" >"${WORK}/yb.cc" 2>&1 \
   && "${CC}" ${CF} -iquote "${S}" -I"${S}" -o "${WORK}/t_yaml" "${WORK}/t_yaml.c" "${WORK}/yaml.o" \
        "${S}/file.c" "${S}/str.c" "${S}/crypto.c" "${S}/tk_time.c" "${S}/encoding.c" >>"${WORK}/yb.cc" 2>&1; then
    run "${WORK}/t_yaml" yaml
else
    fail "yaml: build: $(grep -a -m3 -E 'error' "${WORK}/yb.cc" | head -c 400)"
fi

# ── metrics: the raw log writers are gone ───────────────────────────────────
if "${CC}" -std=c11 -D_GNU_SOURCE -O1 -c -o "${WORK}/metrics.o" "${S}/metrics.c" >"${WORK}/mb.cc" 2>&1; then
    syms="$(nm "${WORK}/metrics.o" 2>/dev/null)"
    defined="$(printf '%s\n' "${syms}" | awk '$2 ~ /^[TD]$/ {print $3}')"
    if printf '%s\n' "${defined}" | grep -a -q -E '^_?metrics_log_(access|error)$'; then
        fail "metrics: metrics_log_access/metrics_log_error still defined"
    else
        pass "metrics: metrics_log_access and metrics_log_error are not defined"
    fi
    writers="$(printf '%s\n' "${syms}" | awk '$1 == "U" {print $2}' | grep -a -E '^_?(v?fprintf|fputs|fputc|fwrite|__stderrp|__stdoutp|stderr|stdout)$' | tr '\n' ' ')"
    if [ -n "${writers}" ]; then
        fail "metrics: metrics.o still references stdio writers: ${writers}"
    else
        pass "metrics: metrics.o references no stdio writer"
    fi
    if printf '%s\n' "${defined}" | grep -a -q -E '^_?metrics_reopen_logs$'; then
        pass "metrics: metrics_reopen_logs (server_ops.c's SIGHUP hook) still links"
    else
        fail "metrics: metrics_reopen_logs is gone, server_ops.c will not link"
    fi
else
    fail "metrics: build: $(grep -a -m3 -E 'error' "${WORK}/mb.cc" | head -c 400)"
fi

# ── md: placeholders, long and CRLF separators, the stale comment ──────────
cat > "${WORK}/t_md.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md.h"
static int count(const char *h, const char *n) {
    int c = 0; size_t l = strlen(n);
    for (const char *p = strstr(h, n); p; p = strstr(p + l, n)) c++;
    return c;
}
#define CHECK(c, m) printf("%s %s\n", (c) ? "OK" : "BAD", m)
int main(void) {
    const char *h;
    h = md_render("| h |\n|---|\n| cell |\n\nTKMDTABLEPLACEHOLDER0\n");
    CHECK(count(h, "<table>") == 1 && count(h, "<p>TKMDTABLEPLACEHOLDER0</p>") == 1,
          "a literal placeholder stays text; the table appears once");
    h = md_render("| h |\n|---|\n| [x](onmouseover=alert(1)//) |\n\n[y](TKMDTABLEPLACEHOLDER0)\n");
    CHECK(count(h, "<table>") == 1 && !strstr(h, "href=\"<table") && strstr(h, "href=\"TKMDTABLEPLACEHOLDER0\""),
          "a placeholder in a link destination is not replaced (no href breakout)");
    h = md_render("| h |\n|---|\n| cell |\n\nTKMDTABLEPLACEHOLDE&#82;0\n");
    CHECK(count(h, "<table>") == 1 && count(h, "<p>TKMDTABLEPLACEHOLDER0</p>") == 1,
          "a placeholder spelled with a character reference stays text");
    h = md_render("| a |\n|---|\n| 1 |\n\npara\n\n| b |\n|---|\n| 2 |\n");
    CHECK(count(h, "<table>") == 2 && strstr(h, "<td>1</td>") && strstr(h, "<td>2</td>") && strstr(h, "<p>para</p>")
          && strstr(h, "<td>1</td>") < strstr(h, "<p>para</p>") && strstr(h, "<p>para</p>") < strstr(h, "<td>2</td>"),
          "two tables still render, in order");
    size_t n = 600;
    char *big = malloc(n + 64);
    strcpy(big, "| h |\n|"); memset(big + 7, '-', n); strcpy(big + 7 + n, "|\n| v |\n");
    h = md_render(big);
    CHECK(count(h, "<table>") == 1 && strstr(h, "<td>v</td>"), "a 600-byte separator row is detected");
    h = md_render("| h |\r\n|---|\r\n| v |\r\n");
    CHECK(count(h, "<table>") == 1 && strstr(h, "<td>v</td>"), "a CRLF table is detected");
    h = md_render("<script>x</script>\n\n| h |\n|---|\n| <b>c</b> |\n");
    CHECK(!strstr(h, "<script>") && !strstr(h, "<b>") && strstr(h, "&lt;b&gt;c&lt;/b&gt;"),
          "raw HTML stays neutralised in and out of tables (124.3)");
    return 0;
}
C
if "${CC}" ${CF} -Wno-pedantic -I"${CMARK_DIR}" -iquote "${S}" -I"${S}" -o "${WORK}/t_md" "${WORK}/t_md.c" "${S}/md.c" \
        $(ls "${CMARK_DIR}"/*.c | grep -a -v '/main\.c$') >"${WORK}/md.cc" 2>&1; then
    run "${WORK}/t_md" md
else
    fail "md: build: $(grep -a -m3 -E 'error' "${WORK}/md.cc" | head -c 400)"
fi
if awk 'NR <= 30' "${S}/md.c" | grep -a -q 'CMARK_OPT_UNSAFE'; then
    fail "md: the file-top comment still describes CMARK_OPT_UNSAFE"
else
    pass "md: the file-top comment no longer describes CMARK_OPT_UNSAFE"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ] && [ "${PASS}" -gt 0 ]
