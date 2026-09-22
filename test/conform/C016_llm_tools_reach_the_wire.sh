#!/bin/bash
# C016_llm_tools_reach_the_wire.sh -- story 136.27.
#
# `llmtool.chatwithtools` built a request body with "tools":[...] injected,
# then free()d that body and sent a plain chat instead:
#
#     free(full); /* not yet usable via the current public API */
#     TkLlmResp resp = llm_chat(c, msgs, nmsgs, 0.7);
#
# So the tool declarations never reached the provider. And the second half was
# just as wrong: it then looked for tool_calls in `resp.content`, while
# tool_calls is content's SIBLING inside choices[0].message -- so the array
# could not be found even from a request that had carried the tools.
#
# Neither half is visible from inside the process. The only way to prove the
# tools are on the wire is to be the server, so this stands up
# test/stdlib/fake_llm_endpoint.py, which logs the name of every tool the
# request declared, and asserts that log. A test that only inspected the
# returned value would have passed on a request that carried nothing, because
# an absent tool_calls array is reported as "no tool call", not as an error.
#
# The same assertion is made for `llmtool.submitresult`, which carried the
# identical defect one call along (its body ended in `(void)tools;`).
#
# No credential is involved: the endpoint is 127.0.0.1 over plain HTTP and the
# key is the literal string "dummy-not-a-key", which the fake echoes back so
# the test can also confirm the Authorization header is the one the caller
# supplied and not something read out of the environment.
#
# Exit 0 = pass, 1 = fail.

set -u

TKC="${TKC:-./toke}"
STDLIB="${TKC_STDLIB_DIR:-$PWD/src/stdlib}"
FAKE="${FAKE:-$PWD/test/stdlib/fake_llm_endpoint.py}"
TMP="$(mktemp -d)"
export TKC_STDLIB_DIR="$STDLIB"

pass=0
fail=0
SRV_PID=""

cleanup() {
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null
    rm -rf "$TMP"
}
trap cleanup EXIT

expect() {
    local label="$1" actual="$2" expected="$3"
    if [ "$actual" = "$expected" ]; then
        echo "PASS $label"
        pass=$((pass + 1))
    else
        echo "FAIL $label"
        echo "        expected: $expected"
        echo "        got:      $actual"
        fail=$((fail + 1))
    fi
}

# contains <label> <haystack> <needle>
contains() {
    local label="$1" hay="$2" needle="$3"
    case "$hay" in
        *"$needle"*) echo "PASS $label"; pass=$((pass + 1)) ;;
        *) echo "FAIL $label"
           echo "        expected to contain: $needle"
           echo "        got:                 $hay"
           fail=$((fail + 1)) ;;
    esac
}

# ── the fake provider ────────────────────────────────────────────────────
PORT=$(( 21000 + (RANDOM % 4000) ))
LOG="$TMP/wire.log"
: > "$LOG"

if [ ! -f "$FAKE" ]; then
    echo "FAIL harness: fake endpoint not found at $FAKE"
    exit 1
fi

python3 "$FAKE" "$PORT" "SRV" "$LOG" >"$TMP/srv.out" 2>&1 &
SRV_PID=$!
# The fake serves forever and is killed by the trap; disown so bash does not
# print "Terminated: 15" onto the suite's stderr on the way out.
disown "$SRV_PID" 2>/dev/null || true

# wait for the port to answer rather than sleeping a guessed amount
ready=0
for _ in $(seq 1 100); do
    if python3 - "$PORT" <<'PYEOF' 2>/dev/null
import socket, sys
s = socket.socket()
s.settimeout(0.2)
try:
    s.connect(("127.0.0.1", int(sys.argv[1])))
except Exception:
    sys.exit(1)
finally:
    s.close()
PYEOF
    then ready=1; break; fi
    python3 -c 'import time; time.sleep(0.05)'
done
if [ "$ready" != 1 ]; then
    echo "FAIL harness: fake endpoint never came up on port $PORT"
    cat "$TMP/srv.out"
    exit 1
fi

# ── the consumer ─────────────────────────────────────────────────────────
# Every field is read through a function with a DECLARED $toolcall parameter
# rather than off the `mt` result directly: the compiler infers the struct
# type of an mt result from the wrong struct, so `call.id` on the binding
# itself lowers to slot 0. Same workaround as test/stdlib/llmtool_calls.tk.
cat > "$TMP/c016.tk" <<'TKEOF'
m=c016;
i=llm:std.llm;
i=tool:std.llmtool;
i=io:std.io;
i=str:std.str;
i=env:std.env;

f=cname(c:$toolcall):str{ <c.name };
f=cid(c:$toolcall):str{ <c.id };
f=rtext(r:$llmresp):str{ <r.content };

f=main():$i64{
  let empty=$toolcall{name:"";args:@();id:""};
  let base=llm.client(env.get("C016URL");"dummy-not-a-key";"modelZ");
  let p=$toolparam{name:"city";type:"string";desc:"City name";required:true};
  let decl=$tooldecl{name:"getweather";desc:"Return weather";params:@(p)};
  let armed=tool.withtools(base;@(decl));
  let msgs=@($llmmsg{role:"user";content:"weather in Sydney?"});

  let call=mt tool.chatwithtools(armed;msgs) {$ok:tc tc;$err:e empty};
  io.println(str.concat("callname=";cname(call)));
  io.println(str.concat("callid=";cid(call)));
  io.println("callargs=\(call.args.len())");

  let r1=$toolresult{id:"call-1";content:"{\"tempc\":19}";error:false};
  let fin=mt tool.submitresult(armed;msgs;r1) {$ok:r rtext(r);$err:e "ERR"};
  io.println(str.concat("submit=";fin));
  <0
};
TKEOF

if ! "$TKC" --out "$TMP/c016" "$TMP/c016.tk" >"$TMP/build.log" 2>&1; then
    echo "FAIL c016 compile:"
    sed 's/^/        /' "$TMP/build.log"
    echo "Results: $pass passed, $((fail + 1)) failed"
    exit 1
fi

C016URL="http://127.0.0.1:$PORT/v1" "$TMP/c016" >"$TMP/out.txt" 2>&1
run_status=$?
OUT="$(cat "$TMP/out.txt")"
WIRE="$(cat "$LOG")"

echo "---- program output ----"
sed 's/^/    /' "$TMP/out.txt"
echo "---- what the provider actually received ----"
sed 's/^/    /' "$LOG"
echo "----"

expect "c016 exits 0" "$run_status" "0"

# The decisive assertion: the provider saw the tool. This is the one that
# fails before the fix, and it fails because the request carried no "tools"
# member at all, not because the name was wrong.
contains "the request carried the tool declaration" "$WIRE" "tools=getweather"

# Two requests were made (chatwithtools, then submitresult) and BOTH must
# carry the tools. Counting is what distinguishes "one of them works".
n_with_tools=$(grep -c 'tools=getweather' "$LOG")
expect "both requests carried tools" "$n_with_tools" "2"

# The client argument is honoured, not an environment global (136.18 guard).
contains "the caller's key reached the provider" "$WIRE" "auth=Bearer dummy-not-a-key"
contains "the caller's model reached the provider" "$WIRE" "model=modelZ"

# The tool call came back and was parsed out of the raw body.
line_val() { printf '%s\n' "$OUT" | grep "^$1=" | head -1 | cut -d= -f2-; }
expect "toolcall name parsed"     "$(line_val callname)" "getweather"
expect "toolcall id parsed"       "$(line_val callid)"   "call-1"
expect "toolcall args decoded"    "$(line_val callargs)" "1"
# submitresult took the $ok arm. Its content is the empty string and that is
# correct, not a failure: the fake answers a tools-bearing request the way an
# OpenAI-compatible provider does, with a message carrying `tool_calls` and no
# `content`. "ERR" here would mean the $err arm, which is the thing being
# excluded.
sub="$(line_val submit)"
expect "submitresult took the ok arm" "${sub:-<empty>}" "<empty>"

# ── phase 2: the two C-only entry points with the same defect ────────────
# llm_parallel_tool_calls and llm_agentic_loop also called llm_chat and also
# read tool_calls out of `content`. Neither is reachable from toke today, so
# the only way to test them is a C driver. Built with -fsanitize=address,
# which is headless, so the ownership of the new raw-body buffer and of the
# per-iteration tools JSON is checked at the same time.
cat > "$TMP/probe.c" <<'CEOF'
#include "llm_tool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *weather(const char *args_json) {
    (void)args_json;
    return strdup("{\"tempc\":19}");
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    TkLlmClient c;
    memset(&c, 0, sizeof c);
    c.base_url = argv[1];
    c.api_key  = "dummy-not-a-key";
    c.model    = "modelP";

    TkTool t;
    memset(&t, 0, sizeof t);
    t.name            = "getweather";
    t.description     = "Return weather";
    t.parameters_json = "{\"type\":\"object\",\"properties\":"
                        "{\"city\":{\"type\":\"string\"}},"
                        "\"required\":[\"city\"]}";
    t.handler         = weather;

    TkLlmMsg m;
    m.role    = "user";
    m.content = "weather in Sydney?";

    ToolCallResultArray a = llm_parallel_tool_calls(&c, &m, 1, &t, 1);
    printf("parallellen=%llu\n", (unsigned long long)a.len);
    if (a.len > 0) {
        printf("parallelname=%s\n", a.data[0].tool_name ? a.data[0].tool_name : "");
        printf("parallelresult=%s\n", a.data[0].result_json ? a.data[0].result_json : "");
    }
    for (unsigned long long i = 0; i < a.len; i++) {
        free((void *)a.data[i].call_id);
        free((void *)a.data[i].tool_name);
        free((void *)a.data[i].result_json);
    }
    free(a.data);

    /* The fake answers every tools-bearing request with a tool call, so a
     * loop that is really sending the tools runs out of iterations. Before
     * the fix it saw no tool_calls on iteration 0 and returned the toolless
     * prose as its FINAL ANSWER — which is the story's exact symptom. */
    const char *fin = llm_agentic_loop(&c, "sys", "weather in Sydney?", &t, 1, 2);
    printf("agentic=%s\n", fin ? fin : "");
    free((void *)fin);
    return 0;
}
CEOF

CC_BIN="${CC:-cc}"
if "$CC_BIN" -std=c99 -D_GNU_SOURCE -g -fsanitize=address \
        -iquote "$STDLIB" -o "$TMP/probe" "$TMP/probe.c" \
        "$STDLIB/llm_tool.c" "$STDLIB/llm.c" >"$TMP/probe.build.log" 2>&1; then
    : > "$LOG"
    "$TMP/probe" "http://127.0.0.1:$PORT/v1" >"$TMP/probe.out" 2>&1
    probe_status=$?
    POUT="$(cat "$TMP/probe.out")"
    echo "---- C probe output ----"
    sed 's/^/    /' "$TMP/probe.out"
    echo "----"
    pline() { printf '%s\n' "$POUT" | grep "^$1=" | head -1 | cut -d= -f2-; }

    expect "probe exits 0 under ASan" "$probe_status" "0"
    expect "parallel_tool_calls saw the call" "$(pline parallellen)"  "1"
    expect "parallel_tool_calls dispatched"   "$(pline parallelname)" "getweather"
    expect "parallel_tool_calls got a result" "$(pline parallelresult)" '{"tempc":19}'
    # The decisive one: prose here means the loop never sent the tools.
    expect "agentic_loop did not take prose as final" "$(pline agentic)" \
           "[max iterations reached]"
    n_probe_tools=$(grep -c 'tools=getweather' "$LOG")
    if [ "$n_probe_tools" -ge 3 ]; then
        echo "PASS every C-side request carried tools ($n_probe_tools)"
        pass=$((pass + 1))
    else
        echo "FAIL every C-side request carried tools"
        echo "        expected: at least 3 (1 parallel + 2 loop iterations)"
        echo "        got:      $n_probe_tools"
        fail=$((fail + 1))
    fi
    contains "no ASan report" "$POUT" "agentic="
    case "$POUT" in
        *"AddressSanitizer"*)
            echo "FAIL ASan reported an error"
            sed 's/^/        /' "$TMP/probe.out"
            fail=$((fail + 1)) ;;
        *) echo "PASS ASan clean"; pass=$((pass + 1)) ;;
    esac
else
    echo "FAIL C probe build:"
    sed 's/^/        /' "$TMP/probe.build.log"
    fail=$((fail + 1))
fi

echo "Results: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
