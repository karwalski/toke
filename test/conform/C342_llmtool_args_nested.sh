#!/usr/bin/env bash
# C342_llmtool_args_nested.sh — a nested object or array argument of a parsed
# tool call reaches the program whole (story 135.26 item 3).
#
# THE DEFECT.  args_pairs() in src/stdlib/llm_tool_glue.c scanned a
# non-string value up to the first ',' or '}' anywhere, so
# {"a":{"x":1,"y":2}} arrived as a = `{"x":1` plus a bogus pair made of the
# rest, and an array value was cut at its first comma.
#
# THE FIX.  json_value_end() finds the end of the value at nesting depth 0,
# skipping strings (a ',' '}' or ']' inside a string does not end it).  The
# value is handed over as its JSON text, as numbers and literals already are.
#
# WHAT IS ASSERTED (a C driver calling tk_llmtool_parsetoolcalls_w, under
# ASan+UBSan -- the C276 driver):
#   1. OpenAI form (arguments is a JSON string holding the object): nested
#      object, nested array holding an object with "}]," in a string, then a
#      string and a number -- four pairs, each value whole
#   2. object form (arguments is the object itself): the same
#
# Negative control: C342_STDLIB=<dir> runs against another stdlib tree;
# main's (18b9f73) llm_tool_glue.c fails both (the values are cut at the
# first comma: a = `{"x": 1`, then y=2 as a pair of its own, and b, c, d lost).
#
# Story: 135.26 (item 3)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C342_STDLIB:-${REPO_ROOT}/src/stdlib}"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" llmtool)" \
    || { echo "  FAIL: stdlib link set: see stderr above"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c342_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0
echo "C342: llmtool nested tool-call args arrive whole (135.26 item 3)"
echo "---------------------------------------------------------------------"

# The two response bodies, built by a JSON encoder so the escapes are exact.
python3 - "${WORK}" <<'PY'
import json, sys, os
args = {"a": {"x": 1, "y": 2}, "b": [1, 2, {"z": "}],"}], "c": "s,t", "d": 3}
def resp(a):
    return json.dumps({"choices": [{"message": {"role": "assistant", "content": "",
        "tool_calls": [{"id": "call_1", "type": "function",
                        "function": {"name": "look", "arguments": a}}]}}]})
open(os.path.join(sys.argv[1], "string_form.json"), "w").write(resp(json.dumps(args)))
open(os.path.join(sys.argv[1], "object_form.json"), "w").write(resp(args))
# expected: one line per pair, "key=value" with \n shown as <LF>
with open(os.path.join(sys.argv[1], "want.txt"), "w", encoding="utf-8") as f:
    for k, v in args.items():
        v = v if isinstance(v, str) else json.dumps(v)
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
        "${LINK[@]}" "${WORK}/cnt.o" >>"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: the driver does not build"; sed 's/^/      /' "${WORK}/cc.log" | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi
"${WORK}/d" "${WORK}/string_form.json" "${WORK}/got_string.txt" \
            "${WORK}/object_form.json" "${WORK}/got_object.txt" > "${WORK}/run.out" 2>&1
rc=$?

check() {   # label got-file
    if [ "${rc}" -eq 0 ] && cmp -s "${WORK}/want.txt" "$2"; then
        echo "  PASS: $1: four pairs, nested values whole"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1 (driver exit ${rc})"
        diff "${WORK}/want.txt" "$2" 2>&1 | sed 's/^/        /' | head -12
        sed 's/^/        | /' "${WORK}/run.out" | head -4
        FAIL=$((FAIL + 1))
    fi
}
check "OpenAI string-form arguments" "${WORK}/got_string.txt"
check "object-form arguments" "${WORK}/got_object.txt"

echo "---------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
