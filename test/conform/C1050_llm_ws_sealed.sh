#!/usr/bin/env bash
# C1050_llm_ws_sealed.sh -- std.llm returns sealed strings and std.ws reads
# with tk_strlen; llm_tool's callers stop freeing the sealed content
# (story 127.295, S1 step 5 tranche 4; runtime-abi §3.1).  Also 127.331:
# llm.c's JSON string decoder no longer reads past a value that ends in a
# lone backslash at the end of the input.
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1), against a local mock
#      OpenAI-compatible endpoint:
#        - llm_parse_sse_chunk: a delta's content, the empty results (no
#          delta, role-only delta, empty content) and an escaped value are
#          sealed with header == strlen;
#        - 127.331: a value ending in a lone backslash at the end of an
#          exactly-sized heap buffer decodes to the bytes before the NUL
#          (backslash kept) with no over-read;
#        - llm_client: provider, model, base_url and api_key are sealed, and
#          llm_client_free (which frees only the struct now) does not abort;
#        - llm_chat / llm_complete: content sealed, token counts read;
#        - llm_chatstream: every chunk sealed, chunks join to the reply;
#        - llm_agentic_loop: a reply with no tool calls comes back as a
#          malloc'd copy the caller frees (the loop used to free the content
#          it held, and to hand the content itself back);
#        - llm_countokens over a sealed string;
#        - std.ws: ws_accept_key on the RFC 6455 §1.3 example,
#          ws_build_upgrade_response with and without a protocol (exact
#          bytes), ws_build_close_frame and ws_parse_upgrade_headers over
#          sealed inputs.
#   2. toke, under TK_STR_VERIFY=1 (every trusted length read asserts
#      header == strlen and aborts otherwise): llm.client's .provider/.model,
#      llm.chat's content, llm.complete and every llm.streamnext chunk have
#      the expected text and `.len`, and the program exits 0.
#
# Negative control: C1050_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files and vendor/).
#   - main's src/stdlib + stdlib (0a3211f2): 8 passed, 6 failed -- the five
#     llm_parse_sse_chunk cases "not sealed", then the 127.331 case dies
#     under ASan (heap-buffer-overflow in find_json_string, reading past the
#     exact-sized input), so the harness stops there.  The toke cases pass:
#     an unsealed string is read with strlen -- correctness never depended
#     on the header.
#   - this branch's tree with main's llm_tool.c: 25 passed, 2 failed --
#     llm_agentic_loop hands back the sealed content itself and the
#     caller's free() of it is an ASan bad-free, which is why llm_tool.c
#     changed in the same commit as llm.c.
# Measured: this branch 33 passed (25 C + 8 toke).
#
# Story: 127.295, 127.331
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1050_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0; SRV=""
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v python3 >/dev/null 2>&1; then echo "FAIL: python3 is needed for the mock endpoint"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1050_XXXXXX")"
trap 'rm -rf "${WORK}"; [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1050: std.llm results are sealed, std.ws reads with tk_strlen (127.295, 127.331)"
echo "----------------------------------------------------------------------"

# A local OpenAI-compatible endpoint: "stream":true gets two SSE deltas and
# [DONE]; anything else one chat completion naming the request's model.
cat > mock.py <<'PY'
import http.server, json
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        if body.get("stream"):
            out = b"".join(b"data: " + json.dumps(
                {"choices": [{"delta": {"content": t}}]}).encode() + b"\n\n"
                for t in ("hello", " world")) + b"data: [DONE]\n\n"
            ctype = "text/event-stream"
        else:
            out = json.dumps({"choices": [{"message": {"role": "assistant",
                              "content": "hi from " + body.get("model", "?")}}],
                              "usage": {"prompt_tokens": 3, "completion_tokens": 5}}).encode()
            ctype = "application/json"
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)
    def log_message(self, *a): pass
s = http.server.HTTPServer(("127.0.0.1", 0), H)
print(s.server_address[1], flush=True)
s.serve_forever()
PY
python3 mock.py > port.txt 2>/dev/null & SRV=$!
PORT=""
for _ in $(seq 1 50); do PORT="$(head -1 port.txt 2>/dev/null)"; [ -n "${PORT}" ] && break; sleep 0.1; done
[ -n "${PORT}" ] || { fail "mock endpoint did not start"; echo "C1050: ${PASS} passed, ${FAIL} failed"; exit 1; }

# run_c NAME ARGS...: run ./NAME under TK_STR_VERIFY=1; each "ok X" /
# "FAIL X" line is one case; a non-zero exit (an ASan report, a verify
# abort) is a failure.
run_c() {
    local name="$1"; shift
    TK_STR_VERIFY=1 "./${name}" "$@" > "${name}.out" 2>&1; local rc=$?
    if [ "${rc}" -ne 0 ]; then
        fail "${name}: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|tk_str' "${name}.out" | head -c 300)"
    fi
    while IFS= read -r line; do
        case "${line}" in
            ok\ *)   pass "${name}: ${line#ok }" ;;
            FAIL\ *) fail "${name}: ${line#FAIL }" ;;
        esac
    done < "${name}.out"
}

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include "llm.h"
#include "llm_tool.h"
#include "ws.h"
#include "capabilities.h"
#include "tk_strhdr.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c, m) printf((c) ? "ok %s\n" : "FAIL %s\n", (m))
static int sealed(const char *s) { return s && tks_hdrlen(s) == (int64_t)strlen(s); }
static int sealed_eq(const char *s, const char *want)
{ return sealed(s) && strcmp(s, want) == 0; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* keep every line if ASan aborts */
    char *av[] = { "seal", "--allow-net", NULL };
    tk_cap_init(2, av);           /* llm's connect is a net sink (121.42) */
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%s/v1", argc > 1 ? argv[1] : "1");

    /* llm_parse_sse_chunk */
    CHECK(sealed_eq(llm_parse_sse_chunk("{\"choices\":[{\"delta\":{\"content\":\"hello\"}}]}"), "hello"),
          "sse: delta content sealed");
    CHECK(sealed_eq(llm_parse_sse_chunk("{\"choices\":[{\"index\":0}]}"), ""), "sse: no delta -> sealed \"\"");
    CHECK(sealed_eq(llm_parse_sse_chunk("{\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}"), ""),
          "sse: role-only delta -> sealed \"\"");
    CHECK(sealed_eq(llm_parse_sse_chunk("{\"choices\":[{\"delta\":{\"content\":\"\"}}]}"), ""),
          "sse: empty content sealed");
    CHECK(sealed_eq(llm_parse_sse_chunk("{\"delta\":{\"content\":\"a\\\"b\\nc\"}}"), "a\"b\nc"),
          "sse: escaped content decoded and sealed");

    /* 127.331: a lone trailing backslash at the end of an exact heap buffer */
    {
        static const char src[] = "{\"delta\":{\"content\":\"a}\\";
        char *buf = malloc(sizeof src);
        memcpy(buf, src, sizeof src);
        const char *v = llm_parse_sse_chunk(buf);
        CHECK(sealed_eq(v, "a}\\"), "127.331: trailing backslash kept, no read past the NUL");
        free(buf);
    }

    /* llm_client */
    TkLlmClient *c = llm_client(url, "k-c1050", "m1050");
    CHECK(c && sealed_eq(c->provider, "127.0.0.1"), "client: provider sealed");
    CHECK(c && sealed_eq(c->model, "m1050"), "client: model sealed");
    CHECK(c && sealed_eq(c->base_url, url), "client: base_url sealed");
    CHECK(c && sealed_eq(c->api_key, "k-c1050"), "client: api_key sealed");
    TkLlmClient *o = llm_client("http://localhost:11434/v1", NULL, "llama3");
    CHECK(o && sealed_eq(o->provider, "ollama") && o->api_key == NULL, "client: ollama provider sealed, NULL key stays NULL");
    llm_client_free(o);
    printf("ok client: llm_client_free does not free the sealed fields\n");

    /* llm_chat / llm_complete */
    TkLlmMsg msg = { "user", "q" };
    TkLlmResp r = llm_chat(c, &msg, 1, 0.7);
    CHECK(!r.is_err && sealed_eq(r.content, "hi from m1050"), "chat: content sealed");
    CHECK(r.input_tokens == 3 && r.output_tokens == 5, "chat: token counts");
    TkLlmResp r2 = llm_complete(c, "q2", 0.7);
    CHECK(!r2.is_err && sealed_eq(r2.content, "hi from m1050"), "complete: content sealed");

    /* llm_chatstream */
    TkLlmStream st = llm_chatstream(c, &msg, 1, 0.7);
    int all = !st.is_err && st.nchunks == 2;
    char joined[64] = "";
    for (uint64_t i = 0; all && i < st.nchunks; i++) {
        all = sealed(st.chunks[i]);
        strncat(joined, st.chunks[i], sizeof joined - strlen(joined) - 1);
    }
    CHECK(all, "chatstream: 2 chunks, every one sealed");
    CHECK(strcmp(joined, "hello world") == 0, "chatstream: chunks join to the reply");

    /* llm_agentic_loop: no tool calls -> the reply, as a malloc'd copy */
    const char *ans = llm_agentic_loop(c, "sys", "user", NULL, 0, 2);
    CHECK(ans && strcmp(ans, "hi from m1050") == 0 && tks_hdrlen(ans) < 0,
          "agentic_loop: reply returned as a malloc'd copy");
    free((void *)ans);
    printf("ok agentic_loop: the caller frees the result\n");

    /* llm_countokens */
    CHECK(llm_countokens(c, tks_strdup("0123456789012345678901234567890123456789")) == 10,
          "countokens over a sealed string");
    llm_client_free(c);

    /* std.ws readers */
    const char *ak = ws_accept_key(tks_strdup("dGhlIHNhbXBsZSBub25jZQ=="));
    CHECK(ak && strcmp(ak, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "ws_accept_key: RFC 6455 example");
    const char *up = ws_build_upgrade_response(tks_strdup("abc="), NULL);
    CHECK(up && strcmp(up, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                           "Connection: Upgrade\r\nSec-WebSocket-Accept: abc=\r\n\r\n") == 0,
          "ws_build_upgrade_response: exact bytes");
    const char *up2 = ws_build_upgrade_response("abc=", tks_strdup("chat"));
    CHECK(up2 && strcmp(up2, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                            "Connection: Upgrade\r\nSec-WebSocket-Accept: abc=\r\n"
                            "Sec-WebSocket-Protocol: chat\r\n\r\n") == 0,
          "ws_build_upgrade_response: with a protocol");
    free((void *)ak); free((void *)up); free((void *)up2);
    ByteArray cf = ws_build_close_frame(1000, tks_strdup("bye"));
    CHECK(cf.len == 5 && cf.data && cf.data[0] == 0x03 && cf.data[1] == 0xE8 && memcmp(cf.data + 2, "bye", 3) == 0,
          "ws_build_close_frame: sealed reason");
    free(cf.data);
    const char *hn[2] = { tks_strdup("Sec-WebSocket-Key"), tks_strdup("SEC-WEBSOCKET-PROTOCOL") };
    const char *hv[2] = { "k1", "p1" };
    WsUpgradeHeaders h = ws_parse_upgrade_headers(hn, hv, 2);
    CHECK(h.sec_key && strcmp(h.sec_key, "k1") == 0 && h.protocol && strcmp(h.protocol, "p1") == 0,
          "ws_parse_upgrade_headers: sealed names");
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects llm llmtool ws -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    run_c seal "${PORT}"
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<TOKE
m=p;
i=io:std.io;
i=str:std.str;
i=llm:std.llm;
f=main():i64{
  let c=llm.client("http://127.0.0.1:${PORT}/v1";"k-c1050";"m1050");
  io.println("provider=[\(c.provider)] \(c.provider.len)");
  io.println("model=[\(c.model)] \(c.model.len)");
  let msgs=@(\$llmmsg{role:"user";content:"q"});
  let r=mt llm.chat(c;msgs) {\$ok:v v.content;\$err:e "err"};
  io.println("chat=[\(r)] \(r.len)");
  let k=mt llm.complete(c;"q2") {\$ok:v v;\$err:e "err"};
  io.println("complete=[\(k)] \(k.len)");
  let s=mt llm.chatstream(c;msgs) {\$ok:x x;\$err:e <1};
  let a=mt llm.streamnext(s) {\$ok:t t;\$err:e "err"};
  io.println("chunk1=[\(a)] \(a.len)");
  let b=mt llm.streamnext(s) {\$ok:t t;\$err:e "err"};
  io.println("chunk2=[\(b)] \(b.len)");
  let z=mt llm.streamnext(s) {\$ok:t t;\$err:e "err"};
  io.println("end=[\(z)] \(z.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] && pass "toke program ran to exit 0 under TK_STR_VERIFY=1" \
                      || fail "toke program exited ${rc}: $(head -c 300 p.out)"
    for want in "provider=[127.0.0.1] 9" \
                "model=[m1050] 5" \
                "chat=[hi from m1050] 13" \
                "complete=[hi from m1050] 13" \
                "chunk1=[hello] 5" \
                "chunk2=[ world] 6" \
                "end=[] 0"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C1050: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
