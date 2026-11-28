#!/usr/bin/env bash
# C2285_yaml_fromjson_terminates.sh -- yaml.fromjson terminates, in bounded
# time and memory, on every malformed flow collection (story 127.542).
#
# THE DEFECT (measured on eca5062f, standalone fuzz-yaml, ASan+UBSan): the
# 35-byte input the security nightly saved as crash-yaml-oom-bcd1502b
# (kept as test/fuzz/corpus-adversarial/yaml_flow_seq_no_progress_127542.json)
# never returned: libFuzzer died at a 2 GiB malloc, the standalone replay was
# killed by a 20 s alarm.  json_to_yaml_r's sequence loop ran
#     while (*p && *p != ']') { append "\n- "; convert one element; ... }
# and an element that consumes nothing -- a byte that is none of { [ " and no
# JSON literal (`[1,2@]`: '@'), or any element nested past the
# YAML_MAX_INDENT depth cap (`[` x 400) -- left p where it was, so the loop
# appended "\n- " forever.  Any program calling yaml.fromjson on untrusted
# text could be held at 100% CPU with unbounded memory by a few bytes.
#
# THE FIX: the sequence loop in json_to_yaml_r stops when an element made no
# progress (an empty element on a comma still consumes the comma, as before)
# and the stop unwinds every enclosing frame, so no parent re-parses the bytes
# a child left behind.  The object loop never spun (each iteration consumes a
# key's '"' or breaks), so it only honours a stop from below.  Every iteration consumes input: work is linear in the input
# and the output at most a few hundred bytes per input byte.  What converted
# before converts to exactly the same text.
#
# WHAT IS ASSERTED (toke programs, each under tk_run's hang guard,
# TK_CONFORM_RUN_SECS, 127.559):
#   1. yaml.fromjson / yaml.tojson / yaml.dec+str of the fuzz artifact, of a
#      sequence nested 400 deep (past the depth cap), of `{"b":[1,2@]}` and
#      `[1@]`, and of a 60 KiB sequence of 30,000 elements ending in '@':
#      each returns, with the expected output length, in < 5 s wall.
#   2. controls that must convert exactly as before (text taken from base
#      eca5062f): `{"a":1,"b":[1,2]}`, `[1,[2,3],{"k":"v"}]`, `{}`, `[]`, and
#      the elided elements `[1,,2]`, `{"a":,"b":1}`, `[[1,,2],3]`, and
#      empty values on a closer, `[{"a":},1]`, `{"x":{"a":},"y":1}`,
#      `[{"a": }, {"b":3}]`, `[[{"a":],2]` (text taken from v3.2.0).
#
# Negative control: C2285_STDLIB=<dir> builds against another tree's
# src/stdlib (its ../../stdlib must hold the .tki files).  The v3.2.0 tree
# (~/tk/tmpwork/wt_toke320) and base eca5062f fail every case in 1 (alarm)
# and pass every control.
#
# Story: 127.542, 127.559
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C2285_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
ART="${REPO_ROOT}/test/fuzz/corpus-adversarial/yaml_flow_seq_no_progress_127542.json"
DEEP="${REPO_ROOT}/test/fuzz/corpus-adversarial/yaml_deep_seq_past_cap_127542.json"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
for f in "${ART}" "${DEEP}"; do
    [ -f "${f}" ] || { echo "FAIL: missing ${f}"; exit 1; }
done
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2285_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2285: yaml.fromjson terminates on malformed flow collections (127.542)"
echo "----------------------------------------------------------------------"

# The program converts the file named by its first argument three ways and
# prints the length of each result.
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=yaml:std.yaml;
f=main():i64{
  let path=mt args.get(1) {$ok:v v;$err:e ""};
  let s=mt file.read(path) {$ok:v v;$err:e ""};
  let fj=yaml.fromjson(s);
  let tj=yaml.tojson(s);
  let k=mt yaml.dec(s) {$ok:y mt yaml.str(y;"k") {$ok:v v.len;$err:x 0-1};$err:x 0-2};
  io.println("in=\(s.len) fj=\(fj.len) tj=\(tj.len) k=\(k)");
  <0
};
TOKE
if ! "${TKC}" --out p p.tk > p.log 2>&1; then
    fail "compile"; head -8 p.log | sed 's/^/        /'
    echo "C2285: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# run <name> <file> <want>: the program must print <want> under the hang
# guard, and within 5 s wall.
run() {
    local t0 t1 got rc
    t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    got="$(tk_run ./p --allow-read "$2" 2>&1)"; rc=$?
    t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
    local dt; dt=$(perl -e "printf '%.2f', $t1 - $t0")
    if [ "${rc}" -ne 0 ]; then
        fail "$1: no result (exit $(tk_rc "${rc}")) after ${dt} s"
    elif [ "${got}" != "$3" ]; then
        fail "$1: output differs"; printf '        got:  %s\n        want: %s\n' "${got}" "$3"
    elif perl -e "exit !($t1 - $t0 < 5)"; then
        pass "$1 (${dt} s): ${got}"
    else
        fail "$1: took ${dt} s (bound 5 s)"
    fi
}

# ── 1. inputs that never returned ───────────────────────────────────────────
run "fuzz artifact crash-yaml-oom-bcd1502b (35 bytes)" "${ART}" "in=35 fj=25 tj=45 k=-1"
run "sequence nested 400 deep (past the depth cap)" "${DEEP}" "in=801 fj=66562 tj=2 k=-1"
printf '{"b":[1,2@]}' > obj.json
run "{\"b\":[1,2@]}" obj.json "in=12 fj=20 tj=18 k=-1"
printf '[1@]' > seq.json
run "[1@]" seq.json "in=4 fj=6 tj=2 k=-1"
python3 -c "import sys; sys.stdout.write('[' + '1,' * 30000 + '@]')" > big.json
run "60 KiB sequence ending in '@'" big.json "in=60003 fj=120002 tj=2 k=-1"

# ── 2. controls: well-formed JSON converts exactly as before ────────────────
cat > c.tk <<'TOKE'
m=c;
i=io:std.io;
i=yaml:std.yaml;
f=main():i64{
  io.println(yaml.fromjson("{\"a\":1,\"b\":[1,2]}"));
  io.println("--");
  io.println(yaml.fromjson("[1,[2,3],{\"k\":\"v\"}]"));
  io.println("--");
  io.println(yaml.fromjson("{}"));
  io.println(yaml.fromjson("[]"));
  io.println("--");
  io.println(yaml.fromjson("[1,,2]"));
  io.println("--");
  io.println(yaml.fromjson("{\"a\":,\"b\":1}"));
  io.println("--");
  io.println(yaml.fromjson("[[1,,2],3]"));
  io.println("--");
  io.println(yaml.fromjson("[{\"a\":},1]"));
  io.println("--");
  io.println(yaml.fromjson("{\"x\":{\"a\":},\"y\":1}"));
  io.println("--");
  io.println(yaml.fromjson("[{\"a\": }, {\"b\":3}]"));
  io.println("--");
  io.println(yaml.fromjson("[[{\"a\":],2]"));
  <0
};
TOKE
WANT='a: 1
b:
  - 1
  - 2
--
- 1
-
  - 2
  - 3
-
  k: v
--
{}
[]
--
- 1
-
- 2
--
a:
b: 1
--
-
  - 1
  -
  - 2
- 3
--
-
  a:
- 1
--
x:
  a:
y: 1
--
-
  a:
-
  b: 3
--
-
  -
    a:
- 2'
if ! "${TKC}" --out c c.tk > c.log 2>&1; then
    fail "controls: compile"; head -8 c.log | sed 's/^/        /'
else
    # (trailing blanks stripped: "b: " and "- " end in a space)
    got="$(tk_run ./c 2>&1 | sed 's/ *$//')"
    if [ "${got}" = "${WANT}" ]; then
        pass "controls: well-formed objects, nested sequences, {}, [], elided elements and empty values on a closer convert unchanged"
    else
        fail "controls: output differs"; printf '%s\n' "${got}" | sed 's/^/        /'
    fi
fi

echo "----------------------------------------------------------------------"
echo "C2285: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
