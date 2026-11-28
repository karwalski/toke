#!/usr/bin/env bash
# C380_llm_record_fields.sh — every field of the records std.llm hands to
# toke reads what its name says, and none of them reads the API key
# (story 135.30).
#
# THE DEFECT.  tkc reads `c.f` as 8-byte slot <index of f in llm.tki>.
# llm.tki's `$llmclient` is {provider; model}; the glue handed back the raw
# C TkLlmClient {base_url; api_key; model; ...}, so `c.provider` read the
# base URL and `c.model` read the API key -- a credential printed by any
# program that logged the model it was talking to.  llmtool.withtools
# returns a copy of the same struct, so its `.model` did the same.
#
# THE FIX (src/stdlib/llm.h, llm.c, llm_glue.c).  TkLlmClient starts with
# the .tki's two fields, provider then model, and keeps base_url and
# api_key after them, past every slot the record has; it is marked
# `tki-record-prefix: llm.llmclient`.  $llmresp and $llmstream are built as
# marked structs (TkLlmRespRec, TkLlmStreamRec).  make check-tki-layout
# keeps all three in step with llm.tki (C389).
#
# WHAT IS ASSERTED (a dummy key; no real credential is used):
#   1. llm.client: .provider is the URL's host, .model the model
#   2. .provider is "ollama" / "openai" by the documented rule
#   3. llmtool.withtools' client: the same two fields
#   4. llm.chat against a local mock endpoint: every $llmresp field
#   5. NEGATIVE CONTROL: no line the program prints contains the key,
#      and the key did reach the endpoint (so the client does hold it)
#
# C380_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: case 1 prints the base URL and the key.
#
# Story: 135.30, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C380_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c380_XXXXXX")"
trap 'rm -rf "${WORK}"; [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null' EXIT
cd "${WORK}"
echo "C380: std.llm records read the right slots; none reads the API key (135.30)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
KEY="sk-DUMMY-C380-not-a-real-key"

# A one-request-at-a-time OpenAI-compatible endpoint that records the
# Authorization header it was sent.
cat > mock.py <<'PY'
import http.server, json
import socketserver as _ss
def _bind(self):  # 127.439 (Z205): HTTPServer.server_bind does a reverse-DNS getfqdn() that stalls for seconds on the macos-15-intel runner
    _ss.TCPServer.server_bind(self); self.server_name = "127.0.0.1"; self.server_port = self.server_address[1]
http.server.HTTPServer.server_bind = _bind
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        open("auth.txt", "w").write(self.headers.get("Authorization", ""))
        out = json.dumps({"choices": [{"message": {"role": "assistant",
                          "content": "hi from " + body.get("model", "?")}}],
                          "usage": {"prompt_tokens": 3, "completion_tokens": 5}}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
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
tk_wait_until "${SRV}" tk_nonempty port.txt; PORT="$(head -1 port.txt 2>/dev/null)"

cat > p.tk <<TK
m=p;
i=io:std.io;
i=str:std.str;
i=llm:std.llm;
i=llmtool:std.llmtool;
f=both(c:\$llmclient):str{ <str.concat(c.provider; str.concat(" "; c.model)) };
f=main():i64{
  let c:\$llmclient=llm.client("http://127.0.0.1:${PORT}/v1";"${KEY}";"mymodel");
  io.println(both(c));
  io.println(both(llm.client("http://localhost:11434/v1";"";"llama3")));
  io.println(both(llm.client("https://api.openai.com/v1";"${KEY}";"gpt-4o")));
  let t:\$llmclient=llmtool.withtools(c;@());
  io.println(both(t));
  let r=mt llm.chat(c;@(\$llmmsg{role:"user";content:"q"})) {
    \$ok:v str.concat(v.content; str.concat("|"; str.concat(str.fromint(v.tokensin as i64);
          str.concat("|"; str.concat(str.fromint(v.tokensout as i64); str.concat("|"; v.model))))));
    \$err:e "chat failed"};
  io.println(r);
  <0
};
TK
if ! "${TKC}" --diag-text --allow-all --out p p.tk > p.log 2>&1; then
    expect "program builds" "$(grep -m1 '^error' p.log)" ""
else
    out="$(./p 2>p.err)"; rc=$?
    line() { printf '%s\n' "${out}" | sed -n "$1p"; }
    expect "1  llm.client: .provider is the host, .model the model" "$(line 1)" "127.0.0.1 mymodel"
    expect "2a .provider for an Ollama URL" "$(line 2)" "ollama llama3"
    expect "2b .provider for api.openai.com" "$(line 3)" "openai gpt-4o"
    expect "3  llmtool.withtools' client reads the same fields" "$(line 4)" "127.0.0.1 mymodel"
    expect "4  llm.chat: content|tokensin|tokensout|model" "$(line 5)" "hi from mymodel|3|5|mymodel"
    expect "5a NEGATIVE CONTROL: no printed field holds the key" \
        "$(printf '%s\n' "${out}" | grep -c -- "${KEY}")" "0"
    expect "5b ... and the endpoint was sent it, so the client does hold it" \
        "$(cat auth.txt 2>/dev/null)" "Bearer ${KEY}"
    expect "   exit status" "${rc}" "0"
fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
