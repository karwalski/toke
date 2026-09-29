#!/usr/bin/env bash
# C276_llmtool_args_escapes.sh — the `args` of a parsed tool call are decoded
# JSON strings, not their escape text (story 135.24 item 3).
#
# THE DEFECT.  args_pairs() in src/stdlib/llm_tool_glue.c walks the model's
# arguments object and hands each key/value to the program as @($str).  It
# copied the bytes between the quotes verbatim: an argument "a\"b" arrived
# as the four bytes a\"b, "l1\nl2" kept its backslash-n, "\u00e9" stayed six
# characters, and a key with an escape kept it too.  122.14 fixed the same
# thing one level up (llm_tool.c's extract_string) with
# strbuf_append_json_unescape; args_pairs now uses that decoder for keys and
# string values.  Non-string values (numbers, true) stay verbatim.
#
# It also allocated its zero-length result array before looking at the
# object and dropped it whenever the object had keys — one leaked block per
# parse.  Now the empty array is allocated only when it is the result.
#
# WHAT IS ASSERTED (a C driver calling tk_llmtool_parsetoolcalls_w, under
# ASan+UBSan, with a counting allocator):
#   1. OpenAI form (arguments is a JSON string holding the object): every
#      key and value decoded, numbers and literals verbatim, order kept
#   2. object form (arguments is the object itself): the same
#   3. an arguments object with 7 keys leaves exactly what the block owns:
#      block + name + id + outer array + 7 x (pair + key + value) = 25
#
# Negative control: C276_STDLIB=<dir> runs against another stdlib tree;
# main's llm_tool_glue.c fails 1 and 2 (a\"b, C:\\x, l1\nl2, \u00e9 kept as
# escape text; the key k\"ey split at its escaped quote into `k\` with value
# `ey": "v"`) and 3 (26: the dropped empty array).
#
# Story: 135.24 (item 3)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C276_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c276_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0
echo "C276: llmtool tool-call args are decoded JSON strings (135.24 item 3)"
echo "---------------------------------------------------------------------"

# The two response bodies, built by a JSON encoder so the escapes are exact.
python3 - "${WORK}" <<'PY'
import json, sys, os
args = {"q": 'a"b', "path": "C:\\x", "nl": "l1\nl2", "e": "\u00e9", 'k"ey': "v", "n": 42, "t": True}
def resp(a):
    return json.dumps({"choices": [{"message": {"role": "assistant", "content": "",
        "tool_calls": [{"id": "call_1", "type": "function",
                        "function": {"name": "look", "arguments": a}}]}}]})
open(os.path.join(sys.argv[1], "string_form.json"), "w").write(resp(json.dumps(args)))
open(os.path.join(sys.argv[1], "object_form.json"), "w").write(resp(args))
# expected: one line per pair, "key=value" with \n shown as <LF>
with open(os.path.join(sys.argv[1], "want.txt"), "w", encoding="utf-8") as f:
    for k, v in args.items():
        v = {True: "true"}.get(v, v) if isinstance(v, bool) else v
        f.write(f"{k}={str(v).replace(chr(10), '<LF>')}\n")
PY

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
cat > "${WORK}/d.c" <<'C'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
_Thread_local int64_t tk_current_error;
extern long cnt_live;
int64_t tk_llmtool_parsetoolcalls_w(int64_t raw);
/* print the args of a $toolcall block, one "key=value" per line */
static long dump(const char *path, const char *out) {
    FILE *f = fopen(path, "rb"); char buf[8192]; size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0; fclose(f);
    long before = cnt_live;
    int64_t h = tk_llmtool_parsetoolcalls_w((int64_t)(intptr_t)buf);
    long owned = cnt_live - before;
    FILE *o = fopen(out, "w");
    if (!h) { fprintf(o, "(no block)\n"); fclose(o); return owned; }
    int64_t args = ((int64_t *)(intptr_t)h)[1];
    int64_t len = ((int64_t *)(intptr_t)args)[-1];   /* tk_array length word */
    for (int64_t i = 0; i < len; i++) {
        int64_t *pair = (int64_t *)(intptr_t)((int64_t *)(intptr_t)args)[i];
        fprintf(o, "%s=", (const char *)(intptr_t)pair[0]);
        for (const char *v = (const char *)(intptr_t)pair[1]; *v; v++)
            if (*v == '\n') fputs("<LF>", o); else fputc(*v, o);
        fputc('\n', o);
    }
    fclose(o);
    return owned;
}
int main(int argc, char **argv) {
    (void)argc;
    long a = dump(argv[1], argv[2]);
    long b = dump(argv[3], argv[4]);
    printf("owned %ld %ld\n", a, b);
    return 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -c -o "${WORK}/cnt.o" "${WORK}/cnt.c" >"${WORK}/cc.log" 2>&1 \
   || ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -include "${WORK}/cnt.h" -o "${WORK}/d" "${WORK}/d.c" \
        "${S}/llm_tool_glue.c" "${S}/llm_tool.c" "${S}/llm.c" "${S}/capabilities.c" "${WORK}/cnt.o" >>"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: the driver does not build"; sed 's/^/      /' "${WORK}/cc.log" | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi
"${WORK}/d" "${WORK}/string_form.json" "${WORK}/got_string.txt" \
            "${WORK}/object_form.json" "${WORK}/got_object.txt" > "${WORK}/run.out" 2>&1
rc=$?

check() {   # label got-file
    if [ "${rc}" -eq 0 ] && cmp -s "${WORK}/want.txt" "$2"; then
        echo "  PASS: $1: every key and value decoded, literals verbatim"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (driver exit ${rc})"
        diff "${WORK}/want.txt" "$2" 2>&1 | sed 's/^/        /' | head -12
        sed 's/^/        | /' "${WORK}/run.out" | head -4
        FAIL=$((FAIL + 1))
    fi
}
check "OpenAI string-form arguments" "${WORK}/got_string.txt"
check "object-form arguments" "${WORK}/got_object.txt"

got="$(awk '/^owned /{print $2, $3}' "${WORK}/run.out")"
if [ "${got}" = "25 25" ]; then
    echo "  PASS: a 7-key parse leaves exactly what the block owns (25), both forms"; PASS=$((PASS + 1))
else
    echo "  FAIL: a 7-key parse leaves [${got}] allocations; the block owns 25 (26 = the dropped empty array)"
    FAIL=$((FAIL + 1))
fi

echo "---------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
