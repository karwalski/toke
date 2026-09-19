#!/bin/bash
# glue_contract.sh -- Epic 136.16-136.25: the _w glue honours the contract that
# the .tki interface, the canonical documentation and the C core already agree
# on.
#
# Every defect in that group had the same shape: the glue wrapper dropped or
# reinterpreted an argument, so the module compiled, linked and produced a
# plausible-looking answer that was not the documented one. `make check-docs`
# proves such an example COMPILES. Nothing there proves it BEHAVES. This script
# is the behavioural half: it compiles real .tk consumers with tkc and runs
# them, comparing stdout against the values the documentation promises.
#
# Where the defect was about process-global state (136.18, 136.19) or about
# persistence, the check runs the program MORE THAN ONCE, or exercises two
# distinct values in one process, because a single-value probe cannot tell a
# per-argument implementation apart from a hidden global that happens to hold
# the right thing.
#
# Exit 0 = pass, 1 = fail.

set -u

TKC="${TKC:-./toke}"
STDLIB="${TKC_STDLIB_DIR:-$PWD/src/stdlib}"
TMP="$(mktemp -d)"
export TKC_STDLIB_DIR="$STDLIB"

pass=0
fail=0

cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

# build <name> <src> -> $TMP/<name>; fails loudly, never silently skips.
build() {
    local name="$1" src="$2"
    if ! "$TKC" --out "$TMP/$name" "$src" >"$TMP/$name.build.log" 2>&1; then
        echo "FAIL [$name] compile:"
        sed 's/^/        /' "$TMP/$name.build.log"
        fail=$((fail + 1))
        return 1
    fi
    return 0
}

# expect <label> <actual> <expected>
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

# line <output> <key> -> the value after "<key>=" on its own line
line() { printf '%s\n' "$1" | grep "^$2=" | head -1 | cut -d= -f2-; }

echo "=== 136.20 / 127.84 -- math.min and math.max over an array ==="
if build math_minmax test/stdlib/math_minmax.tk; then
    out="$("$TMP/math_minmax")"
    # The negative cases are the ones the old sign-bit compare inverted.
    expect "math.min(@(-1.0;-5.0;-3.0)) == -5"  "$(line "$out" neg.min)"   "-5"
    expect "math.max(@(-1.0;-5.0;-3.0)) == -1"  "$(line "$out" neg.max)"   "-1"
    expect "math.min(@(3.0;-2.5;7.0;0.0)) == -2.5" "$(line "$out" mixed.min)" "-2.5"
    expect "math.max(@(3.0;-2.5;7.0;0.0)) == 7"    "$(line "$out" mixed.max)" "7"
    expect "math.min over positives == 1"       "$(line "$out" pos.min)"   "1"
    expect "math.max over positives == 5"       "$(line "$out" pos.max)"   "5"
    expect "math.min of a single element"       "$(line "$out" one.min)"   "-42"
    expect "math.max of a single element"       "$(line "$out" one.max)"   "-42"
fi

echo
echo "=== 136.16 -- std.toon typed accessors take (Toon; key) ==="
if build toon_accessors test/stdlib/toon_accessors.tk; then
    out="$("$TMP/toon_accessors")"
    expect "toon.str(t; \"name\") == Alice"  "$(line "$out" str.name)"       "Alice"
    expect "toon.i64(t; \"id\") == 1"        "$(line "$out" i64.id)"         "1"
    expect "toon.f64(t; \"score\") == 1.5"   "$(line "$out" f64.score)"      "1.5"
    expect "toon.bool(t; \"active\") is ok"  "$(line "$out" bool.active)"    "1"
    # A key that is absent must take the \$err arm, not return the document.
    expect "toon.str of an absent key errs"  "$(line "$out" str.missing)"    "ERR"
    # arr returned a freshly calloc'd EMPTY array for every input before this.
    expect "toon.arr(t; \"name\") has 3 rows" "$(line "$out" arr.name.len)"  "3"
    expect "toon.arr(t; \"id\") has 3 rows"   "$(line "$out" arr.id.len)"    "3"
    expect "toon.arr of an absent key is empty" "$(line "$out" arr.missing.len)" "0"
fi

echo
echo "=== 136.23 -- file.append(path; content), across two processes ==="
if build file_append test/stdlib/file_append.tk; then
    APPEND_FILE="$TMP/append.txt"
    rm -f "$APPEND_FILE"
    out1="$(APPENDPATH="$APPEND_FILE" "$TMP/file_append" --allow-write --allow-read)"
    # A SECOND process, so a pass means the bytes reached the filesystem
    # rather than an in-process buffer, and that append did not truncate.
    out2="$(APPENDPATH="$APPEND_FILE" "$TMP/file_append" --allow-write --allow-read)"
    expect "file.append reports success"        "$(line "$out1" append.rc)" "1"
    expect "first process leaves 5 bytes"       "$(line "$out1" bytes)"     "5"
    expect "second process appends, not truncates" "$(line "$out2" bytes)"  "10"
    expect "the file holds both lines"          "$(wc -l < "$APPEND_FILE" | tr -d ' ')" "2"
fi

echo
echo "=== 136.18 -- llm.client / llm.chat / llm.complete honour the client ==="
LLM_PIDS=""
stop_llm() { for p in $LLM_PIDS; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done; LLM_PIDS=""; }
trap 'stop_llm; cleanup' EXIT
PORT_A=18811
PORT_B=18812
LOG_A="$TMP/llm_a.log"
LOG_B="$TMP/llm_b.log"
: > "$LOG_A"; : > "$LOG_B"
python3 test/stdlib/fake_llm_endpoint.py "$PORT_A" ALPHA "$LOG_A" >/dev/null 2>&1 &
LLM_PIDS="$LLM_PIDS $!"
python3 test/stdlib/fake_llm_endpoint.py "$PORT_B" BETA "$LOG_B" >/dev/null 2>&1 &
LLM_PIDS="$LLM_PIDS $!"
# Wait for both to accept connections rather than sleeping a guessed interval.
ready=0
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
    if curl -s -o /dev/null --max-time 1 -X POST -d '{}' "http://127.0.0.1:$PORT_A/v1/chat/completions" \
    && curl -s -o /dev/null --max-time 1 -X POST -d '{}' "http://127.0.0.1:$PORT_B/v1/chat/completions"; then
        ready=1; break
    fi
    perl -e 'select(undef,undef,undef,0.25)'
done
if [ "$ready" -ne 1 ]; then
    echo "FAIL [llm] the two local endpoints never came up -- NOT skipped, this is a failure"
    fail=$((fail + 1))
elif build llm_clients test/stdlib/llm_clients.tk; then
    : > "$LOG_A"; : > "$LOG_B"
    out="$(LLMA="http://127.0.0.1:$PORT_A" LLMB="http://127.0.0.1:$PORT_B" \
           "$TMP/llm_clients" --allow-net)"
    # Each answer names the server that produced it, the model that client was
    # built with, and that client's key. One process-wide client cannot do this.
    expect "llm.complete on client A reaches A" "$(line "$out" complete.a)" \
           "ALPHA|model=modelA|auth=Bearer keyA"
    expect "llm.complete on client B reaches B" "$(line "$out" complete.b)" \
           "BETA|model=modelB|auth=Bearer keyB"
    expect "llm.chat on client A succeeds"      "$(line "$out" chat.a)" "ok"
    expect "llm.chat on client B succeeds"      "$(line "$out" chat.b)" "ok"
    expect "endpoint A saw exactly its 2 calls" "$(grep -c '^ALPHA|model=modelA|auth=Bearer keyA$' "$LOG_A")" "2"
    expect "endpoint B saw exactly its 2 calls" "$(grep -c '^BETA|model=modelB|auth=Bearer keyB$' "$LOG_B")" "2"
    expect "no call leaked from A to B"         "$(grep -c 'modelA' "$LOG_B")" "0"
    expect "llm.countokens uses the client"     "$(line "$out" tokens)" "2"
fi

echo
echo "=== 136.17 -- std.llmtool is importable and its runtime is reachable ==="
if [ "$ready" -ne 1 ]; then
    echo "FAIL [llmtool] the local endpoint never came up -- NOT skipped"
    fail=$((fail + 1))
elif build llmtool_calls test/stdlib/llmtool_calls.tk; then
    out="$(LLMA="http://127.0.0.1:$PORT_A" "$TMP/llmtool_calls" --allow-net)"
    # The import line is half the assertion: neither std.llm.tool nor
    # std.llm_tool could be written before this.
    expect "parsetoolcalls reads the tool name" "$(line "$out" call.name)" "getweather"
    expect "parsetoolcalls reads the call id"   "$(line "$out" call.id)"   "call-1"
    expect "the arguments object becomes pairs" "$(line "$out" call.args)" "1"
    expect "junk input takes the err arm"       "$(line "$out" bad)"       "ERR"
    expect "resultmsgs returns one message per result" "$(line "$out" msgs.len)" "1"
    expect "the message is tool-role"           "$(line "$out" msgs.role)" "tool"
    expect "the message carries id and content" "$(line "$out" msgs.body)" \
           '{"tool_call_id":"call-1","content":{"ok":true}}'
    expect "two results give two messages"      "$(line "$out" two.len)"   "2"
    # withtools returns a NEW client; the original must be untouched.
    expect "the original client still works"    "$(line "$out" base)"  "ALPHA|model=modelA|auth=Bearer keyA"
    expect "the tool-carrying client works too" "$(line "$out" armed)" "ALPHA|model=modelA|auth=Bearer keyA"
fi
stop_llm

echo
echo "=== 136.19 -- i18n.load honours the locale, accessors honour the bundle ==="
if build i18n_locales test/stdlib/i18n_locales.tk; then
    out="$(I18NBASE="$PWD/test/stdlib/i18n/ui" "$TMP/i18n_locales" --allow-read)"
    # EN and FR are live at the same time and interleaved. A file-static
    # bundle answers French to every line after the second load; a discarded
    # locale argument answers with the environment's locale to both.
    expect "en bundle answers English"     "$(line "$out" en.greeting)" "Hello"
    expect "fr bundle answers French"      "$(line "$out" fr.greeting)" "Bonjour"
    expect "en still English after fr load" "$(line "$out" en.farewell)" "Goodbye"
    expect "fr still French"               "$(line "$out" fr.farewell)" "Au revoir"
    expect "i18n.fmt uses the en bundle"   "$(line "$out" en.welcome)" "Welcome, Alice!"
    expect "i18n.fmt uses the fr bundle"   "$(line "$out" fr.welcome)" "Bienvenue, Alice!"
    expect "i18n.get takes the bundle too" "$(line "$out" en.get)"     "Hello"
    expect "an absent key returns the key" "$(line "$out" missing)"    "nosuchkey"
    expect "an absent locale errs"         "$(line "$out" absent)"     "NOTFOUND"
fi

echo
echo "=== 136.21 -- test.asserteq / assertne carry the message ==="
if build test_asserts test/stdlib/test_asserts.tk; then
    out="$("$TMP/test_asserts" 2>"$TMP/asserts.err")"
    err="$(cat "$TMP/asserts.err")"
    # str.upper("hello") and the literal "HELLO" are equal bytes at DIFFERENT
    # addresses. The old wrapper compared the two i64 pointers, so this was 0.
    expect "asserteq on equal strings at different addresses" "$(line "$out" eq.same)"  "1"
    expect "asserteq on two empty strings"                    "$(line "$out" eq.empty)" "1"
    expect "asserteq on different strings fails"              "$(line "$out" eq.diff)"  "0"
    expect "assertne on different strings"                    "$(line "$out" ne.diff)"  "1"
    expect "assertne on equal strings fails"                  "$(line "$out" ne.same)"  "0"
    # The message is the whole point of the story: it must reach stderr.
    expect "the asserteq message reaches stderr" \
           "$(printf '%s\n' "$err" | grep -c 'msg="foo is not bar"')" "1"
    expect "asserteq reports both values" \
           "$(printf '%s\n' "$err" | grep -cF 'assert_eq: \"foo\" != \"bar\"')" "1"
    expect "the assertne message reaches stderr" \
           "$(printf '%s\n' "$err" | grep -c 'msg="x equals x so this must fail"')" "1"
    expect "a passing assertion prints nothing" \
           "$(printf '%s\n' "$err" | grep -c 'upper should produce HELLO')" "0"
fi

echo
echo "=== 136.22 -- chart.bar carries the title ==="
if build chart_title test/stdlib/chart_title.tk; then
    out="$("$TMP/chart_title")"
    t1="$(line "$out" titled)"
    t2="$(line "$out" other)"
    # The title is only observable in the serialised chart. Two DIFFERENT
    # titles in one process also rule out a constant or a global.
    expect "chart.bar emits the title it was given" \
           "$(printf '%s' "$t1" | grep -c '"title":{"display":true,"text":"Daily Hits"}')" "1"
    expect "a second chart gets its own title" \
           "$(printf '%s' "$t2" | grep -c '"title":{"display":true,"text":"Revenue by Quarter"}')" "1"
    expect "labels survive to the json"  "$(printf '%s' "$t1" | grep -c '"labels":\["Mon","Tue","Wed"\]')" "1"
    expect "data survives to the json"   "$(printf '%s' "$t1" | grep -c '"data":\[10,20,15\]')" "1"
    expect "the second chart keeps its own data" "$(printf '%s' "$t2" | grep -c '"data":\[1.5,2.5\]')" "1"
fi

echo
echo "=== 136.24 -- html.table(headers; rows) keeps them apart ==="
if build html_table test/stdlib/html_table.tk; then
    out="$("$TMP/html_table")"
    # Rendering is the only place the distinction is visible: headers become
    # <th> inside <thead>, data becomes <td> inside <tbody>. The old wrapper
    # took ONE array and recovered the header row by position.
    expect "the headers become the thead row" \
           "$(printf '%s' "$out" | grep -c '<thead><tr><th>Quarter</th><th>Revenue</th></tr></thead>')" "1"
    expect "all three data rows become tbody rows" \
           "$(printf '%s' "$out" | grep -o '<tr><td>' | wc -l | tr -d ' ')" "3"
    expect "no data value was promoted to a header" \
           "$(printf '%s' "$out" | grep -c '<th>Q1</th>')" "0"
    expect "the first data row is still data" \
           "$(printf '%s' "$out" | grep -c '<tr><td>Q1</td><td>100k</td></tr>')" "1"
    expect "the last data row survives" \
           "$(printf '%s' "$out" | grep -c '<tr><td>Q3</td><td>300k</td></tr>')" "1"
    # 136.24 side-finding: html.doc() is declared with no parameters and was
    # DEFINED with one, so it read a garbage register and crashed on line 1.
    expect "html.doc/render produce a document" \
           "$(printf '%s' "$out" | grep -c '<title>Report</title>')" "1"
fi

echo
echo "=== 136.25 -- ml.accuracy is reachable and correct ==="
if build ml_accuracy test/stdlib/ml_accuracy.tk; then
    out="$("$TMP/ml_accuracy")"
    # 0.75 is neither 1.0 nor 0.0: a wrapper comparing the wrong things or
    # misreading the array lengths cannot land on it by accident.
    expect "3 of 4 correct is 0.75"  "$(line "$out" three.of.four)" "0.75"
    expect "all correct is 1"        "$(line "$out" all.right)"     "1"
    expect "none correct is 0"       "$(line "$out" none.right)"    "0"
    expect "1 of 2 correct is 0.5"   "$(line "$out" half)"          "0.5"
    expect "ragged lengths use the shorter" "$(line "$out" ragged)" "1"
fi

echo
echo "glue_contract: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0
