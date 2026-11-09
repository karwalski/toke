#!/usr/bin/env bash
# C1111_llmtool_sealed.sh -- std.llmtool hands toke sealed strings, and
# llm_tool.c reads with tk_strlen (story 127.295, S1 step 5 tranche 7;
# runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# llm_tool.c's own results stay malloc'd -- its C callers (llm_submitresult,
# llm_parallel_tool_calls, llm_agentic_loop, the C API's users) free them --
# so llm_tool_glue.c seals a copy at the boundary and frees the original.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen):
#        - tk_llmtool_parsetoolcalls_w over a two-call response: the $toolcall
#          block's name and id (an escaped id included) and every args key and
#          value are sealed with the expected text (a \u0000 escape decodes to
#          U+FFFD, so no sealed value holds a 0 byte);
#        - tk_llmtool_resultmsgs_w: every $llmmsg role and content is sealed
#          with the exact expected bytes;
#        - llm_parse_tool_calls through the C API still returns malloc'd
#          strings its caller frees (no ASan bad-free), and
#          llm_tool_result_msgs likewise.
#   2. toke, under TK_STR_VERIFY=1: llmtool.parsetoolcalls' .name/.id/args
#      and llmtool.resultmsgs' .role/.content have the expected text and
#      `.len`, and the program exits 0.
#
# Negative control: C1111_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   main 983cd5c9's src/stdlib + stdlib: 7 passed, 5 failed -- the five
#   checks of a string the glue hands toke fail ("not sealed"); the message
#   count, the C-API case and the five toke cases pass, because an
#   unsealed string read with strlen was always correct (the conversion is
#   a speed change, gated by the ratchet).
# Measured: this branch 12 passed (7 C + 5 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1111_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1111_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1111: std.llmtool results are sealed, llm_tool.c reads with tk_strlen (127.295)"
echo "----------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include "llm_tool.h"
#include "tk_array.h"
#include "tk_strhdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_llmtool_parsetoolcalls_w(int64_t raw);
int64_t tk_llmtool_resultmsgs_w(int64_t results_arr);

#define CHECK(c, m) printf((c) ? "ok %s\n" : "FAIL %s\n", (m))
static int sealed(const char *s) { return s && tks_hdrlen(s) == (int64_t)strlen(s); }
static int sealed_eq(const char *s, const char *want)
{ return sealed(s) && strcmp(s, want) == 0; }
static const char *slot(int64_t b, int i)
{ return (const char *)(intptr_t)((int64_t *)(intptr_t)b)[i]; }

static const char RESP[] =
  "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\",\"tool_calls\":["
  "{\"id\":\"call_\\\"q\\\"\",\"type\":\"function\",\"function\":{\"name\":\"look\","
  "\"arguments\":\"{\\\"city\\\":\\\"Paris\\\",\\\"z\\\":\\\"ab\\\\u0000cd\\\",\\\"n\\\":42}\"}},"
  "{\"id\":\"call_2\",\"type\":\"function\",\"function\":{\"name\":\"other\",\"arguments\":\"{}\"}}"
  "]}}]}";

int main(void)
{
    /* parsetoolcalls: name, id, args keys and values */
    int64_t h = tk_llmtool_parsetoolcalls_w((int64_t)(intptr_t)RESP);
    CHECK(h && sealed_eq(slot(h, 0), "look"), "parsetoolcalls: name sealed");
    CHECK(h && sealed_eq(slot(h, 2), "call_\"q\""), "parsetoolcalls: escaped id decoded and sealed");
    int64_t args = h ? ((int64_t *)(intptr_t)h)[1] : 0;
    int ok = args && tk_arr_len(args) == 3;
    const char *want[3][2] = { {"city", "Paris"}, {"z", "ab\xEF\xBF\xBD" "cd"}, {"n", "42"} };
    for (int64_t i = 0; ok && i < 3; i++) {
        int64_t pr = ((int64_t *)(intptr_t)args)[i];
        ok = sealed_eq(slot(pr, 0), want[i][0]) && sealed_eq(slot(pr, 1), want[i][1]);
        if (!ok) printf("info: pair %lld = [%s] (%lld) [%s] (%lld)\n", (long long)i,
                        slot(pr, 0), (long long)tks_hdrlen(slot(pr, 0)),
                        slot(pr, 1), (long long)tks_hdrlen(slot(pr, 1)));
    }
    CHECK(ok, "parsetoolcalls: every args key and value sealed (\\u0000 decoded as U+FFFD)");

    /* resultmsgs: role and content */
    int64_t rs = tk_arr_alloc(2, 2);
    int64_t *r0 = malloc(3 * sizeof(int64_t)), *r1 = malloc(3 * sizeof(int64_t));
    r0[0] = (int64_t)(intptr_t)"call_1"; r0[1] = (int64_t)(intptr_t)"{\"t\":21}"; r0[2] = 0;
    r1[0] = (int64_t)(intptr_t)"id\"2";  r1[1] = (int64_t)(intptr_t)"\"done\"";   r1[2] = 0;
    ((int64_t *)(intptr_t)rs)[0] = (int64_t)(intptr_t)r0;
    ((int64_t *)(intptr_t)rs)[1] = (int64_t)(intptr_t)r1;
    int64_t ms = tk_llmtool_resultmsgs_w(rs);
    CHECK(ms && tk_arr_len(ms) == 2, "resultmsgs: two messages");
    if (ms && tk_arr_len(ms) == 2) {
        int64_t m0 = ((int64_t *)(intptr_t)ms)[0], m1 = ((int64_t *)(intptr_t)ms)[1];
        CHECK(sealed_eq(slot(m0, 0), "tool") && sealed_eq(slot(m1, 0), "tool"),
              "resultmsgs: roles sealed");
        CHECK(sealed_eq(slot(m0, 1), "{\"tool_call_id\":\"call_1\",\"content\":{\"t\":21}}")
              && sealed_eq(slot(m1, 1), "{\"tool_call_id\":\"id\\\"2\",\"content\":\"done\"}"),
              "resultmsgs: contents sealed, exact bytes");
    }

    /* the C API keeps returning malloc'd strings its callers free */
    ToolCallResult r = llm_parse_tool_calls(RESP);
    int c_ok = !r.is_err && r.ncalls == 2 && strcmp(r.calls[1].tool_name, "other") == 0;
    for (uint64_t i = 0; i < r.ncalls; i++) {
        free((char *)r.calls[i].call_id);
        free((char *)r.calls[i].tool_name);
        free((char *)r.calls[i].args_json);
    }
    free(r.calls);
    TkToolResult tr = { "c9", "1" };
    uint64_t n = 0;
    TkLlmMsg *m = llm_tool_result_msgs(&tr, 1, &n);
    c_ok = c_ok && m && n == 1 && strcmp(m[0].content, "{\"tool_call_id\":\"c9\",\"content\":1}") == 0;
    if (m) { free((char *)m[0].role); free((char *)m[0].content); free(m); }
    CHECK(c_ok, "C API: llm_parse_tool_calls / llm_tool_result_msgs results are freed by the caller");
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects llmtool -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "C1111: ${PASS} passed, ${FAIL} failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    [ "${rc}" -ne 0 ] && fail "seal: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|tk_str' seal.out | head -c 300)"
    while IFS= read -r line; do
        case "${line}" in
            ok\ *)   pass "seal: ${line#ok }" ;;
            FAIL\ *) fail "seal: ${line#FAIL }" ;;
            info:*)  echo "        ${line}" ;;
        esac
    done < seal.out
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=llm:std.llm;
i=lt:std.llmtool;
f=main():i64{
  let raw="{\"tool_calls\":[{\"id\":\"call_7\",\"function\":{\"name\":\"weather\",\"arguments\":{\"city\":\"Oslo\"}}}]}";
  let tc=mt lt.parsetoolcalls(raw) {$ok:v v;$err:e $toolcall{name:"err";args:@();id:"err"}};
  io.println("name=[\(tc.name)] \(tc.name.len)");
  io.println("id=[\(tc.id)] \(tc.id.len)");
  let p0=tc.args.get(0);
  let k:$str=p0.get(0);
  let v:$str=p0.get(1);
  io.println("arg=[\(k)=\(v)] \(k.len) \(v.len)");
  let ms=lt.resultmsgs(@($toolresult{id:"call_7";content:"{\"t\":3}";error:false}));
  let m0:$llmmsg=ms.get(0);
  io.println("role=[\(m0.role)] \(m0.role.len)");
  io.println("content=[\(m0.content)] \(m0.content.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    for want in "name=[weather] 7" \
                "id=[call_7] 6" \
                "arg=[city=Oslo] 4 4" \
                "role=[tool] 4" \
                "content=[{\"tool_call_id\":\"call_7\",\"content\":{\"t\":3}}] 43"; do
        if [ "${rc}" -eq 0 ] && grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}' (exit ${rc}), got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C1111: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
