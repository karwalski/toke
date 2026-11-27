#!/usr/bin/env bash
# C2290_toon_fromjson_terminates.sh — toon.fromjson terminates on every input
# (story 127.543).
#
# THE DEFECT (security nightly, libFuzzer timeout after 1201 s): a 50-byte
# input (test/fuzz/corpus-adversarial/toon_from_json_stray_close_127543.json)
# never returned from toon_from_json.  After each array element the converter
# skips whatever is not the next object with jskip_val, which returns the end
# of a scalar; a stray '}' at array level IS a scalar's end, so the skip made
# no progress and the loop spun forever.  Three bytes are enough:
# `[{"a":1}}`.  Any program converting untrusted JSON with toon.fromjson
# could be wedged by one request.
#
# THE FIX: every pass of that skip loop consumes at least one byte, so a
# stray byte is dropped like any other non-object element (the documented
# lenient reading) and the conversion is linear in the input.
#
# WHAT THIS DRIVES (a toke program, each run under tk_run's hang guard,
# TK_CONFORM_RUN_SECS; 127.559):
#   - the fuzz artifact through toon.fromjson, toon.tojson of the result,
#     and toon.tojson / toon.dec of the raw input: returns, exact output;
#   - `[{"a":1}}`: one row;  `[{"a":1}},{"a":2}]`: the stray '}' is skipped
#     and the second element is still read (two rows, as without it);
#   - 600,000 stray '}' between two elements: returns (linear, not stuck).
#
# Negative control: TKC=~/tk/tmpwork/wt_toke320/tkc (v3.2.0) times out on
# all four inputs; C2290_STDLIB=<dir> exports TKC_STDLIB_DIR=<dir> for a
# compiler built from another tree.
#
# Story: 127.543, 127.559
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"   # 127.559: hang guards, not load limits
TKC="${TKC:-${REPO_ROOT}/tkc}"
[ -n "${C2290_STDLIB:-}" ] && export TKC_STDLIB_DIR="${C2290_STDLIB}"
ART="${REPO_ROOT}/test/fuzz/corpus-adversarial/toon_from_json_stray_close_127543.json"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if [ ! -f "${ART}" ]; then echo "FAIL: fuzz artifact missing: ${ART}"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2290_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

echo "C2290: toon.fromjson terminates on a stray '}' (127.543)"
echo "--------------------------------------------------------"

cp "${ART}" in.json
printf '[{"a":1}}' > m1.json
printf '[{"a":1}},{"a":2}]' > m2.json
python3 -c "open('big.json','w').write('[{\"a\":1}'+'}'*600000+',{\"a\":2}]')"

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=os:std.os;
i=file:std.file;
i=toon:std.toon;
f=main():i64{
  let c=mt file.read(os.getenv("C2290_IN")){$ok:s s;$err:e ""};
  let t=toon.fromjson(c);
  let j=toon.tojson(t);
  let r=toon.tojson(c);
  let d=mt toon.dec(c){$ok:v 1;$err:e 0};
  io.print("in=\(c.len) raw=\(r.len) dec=\(d)\n\(t)json=\(j)\n");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    # $1 input, $2 label, $3 expected stdout
    run() {
        got="$(C2290_IN="$1" tk_run ./p 2>&1)"; rc=$?
        if [ "${rc}" -eq 142 ]; then
            fail "$2: did not return: killed by the ${TK_RUN_SECS} s hang guard (rc 142)"
        elif [ "${rc}" -ne 0 ]; then
            fail "$2: exit ${rc}"; printf '%s\n' "${got}" | head -6 | sed 's/^/        /'
        elif [ "${got}" != "$3" ]; then
            fail "$2: output differs"; printf '%s\n' "${got}" | head -6 | sed 's/^/        /'
        else
            pass "$2"
        fi
    }
    run in.json "fuzz artifact (50 bytes): fromjson / tojson / dec return" \
'in=50 raw=73 dec=1
data[1]{id,na\}e}:
1|a
json=[{"id":1,"na}e":"a"}]'
    run m1.json "[{\"a\":1}}: one row" \
'in=9 raw=17 dec=1
data[1]{a}:
1
json=[{"a":1}]'
    run m2.json "[{\"a\":1}},{\"a\":2}]: stray '}' skipped, second element read" \
'in=18 raw=28 dec=1
data[2]{a}:
1
2
json=[{"a":1},{"a":2}]'
    run big.json "600,000 stray '}' between two elements: returns" \
'in=600017 raw=600027 dec=1
data[2]{a}:
1
2
json=[{"a":1},{"a":2}]'
fi

echo "--------------------------------------------------------"
echo "C2290: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
