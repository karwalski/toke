#!/usr/bin/env bash
# C420_map_get_error_union.sh — `m.get(k)` is a fallible lookup: a stored 0
# is $ok, a missing key is $err (story 127.30, owner decision 2026-09-22:
# "error unions, not a 0 sentinel").
#
# THE DEFECT.  tk_map_get returned 0 for a missing key and 0 for a stored 0,
# and the ok/err match decided on the value, so
#
#     let m=@("a":0);  mt m.get("a"){$ok:v "ok";$err:e "err"}   -> err (wrong)
#
# and so did a stored 0.0, a stored 0 in an int-keyed map, a map held in a
# struct field and a map returned by a call.  Measured on main 8efba9e: every
# "... stored" line below printed err.
#
# THE FIX.  tk_map_get keeps @tk_current_error truthful (0 hit, 1 miss), and
# llvm.c's classify_fallible_call() treats a map `.get` as a slot call, so the
# direct match, the let-bound match, a re-assigned binding and the default
# idiom all decide on the slot (runtime-abi.md §7.2).
#
# BOTH DIRECTIONS.  A fix that always took $ok would pass every "stored" line,
# so every "missing" line must still take $err.  The stale-slot case binds a
# hit, then a miss, then matches the hit: the binding must keep its own copy
# of the slot (135.16), not read the miss.  The value channel is unchanged:
# an unconsumed `m.get(k)` is still the value, or the 0 filler on a miss.
# Everything is asserted at -O0 and -O2.
#
# Story: 127.30

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c420_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C420: m.get: a stored 0 is ok, a missing key is err"
echo "--------------------------------------"

cat > p.tk <<'EOF'
m=p;
i=io:std.io;
i=str:std.str;
t=$bag{meta:@($str:i64)};
t=$miss{$none:i64};
f=look(m:@($str:i64);k:$str):i64!$miss{
  let v=mt m.get(k){$ok:x x;$err:e <$miss{$none:0}};
  <v+100
};
f=mk():@($str:i64){<@("z":0)};
f=main():i64{
  let m=@("a":0;"b":2);
  io.println("direct stored \(mt m.get("a"){$ok:v "ok";$err:e "err"})");
  io.println("direct missing \(mt m.get("q"){$ok:v "ok";$err:e "err"})");
  let a=m.get("a");
  let q=m.get("q");
  io.println("let stored \(mt a{$ok:v "ok";$err:e "err"})");
  io.println("let missing \(mt q{$ok:v "ok";$err:e "err"})");
  let r=mut.m.get("q");
  r=m.get("a");
  io.println("reassigned stored \(mt r{$ok:v "ok";$err:e "err"})");
  let h=m.get("a");
  let x=m.get("nope");
  io.println("stale stored \(mt h{$ok:v "ok";$err:e "err"})");
  let fl=@("z":0.0;"o":1.5);
  io.println("f64 stored \(mt fl.get("z"){$ok:v "ok";$err:e "err"})");
  io.println("f64 missing \(mt fl.get("n"){$ok:v "ok";$err:e "err"})");
  let n=@(1:0;2:5);
  io.println("intkey stored \(mt n.get(1){$ok:v "ok";$err:e "err"})");
  io.println("intkey missing \(mt n.get(3){$ok:v "ok";$err:e "err"})");
  let bg=$bag{meta:@("k":0)};
  io.println("field stored \(mt bg.meta.get("k"){$ok:v "ok";$err:e "err"})");
  io.println("field missing \(mt bg.meta.get("j"){$ok:v "ok";$err:e "err"})");
  io.println("call stored \(mt mk().get("z"){$ok:v "ok";$err:e "err"})");
  io.println("early stored \(mt look(m;"a"){$ok:v str.fromint(v);$err:e "err"})");
  io.println("early missing \(mt look(m;"q"){$ok:v str.fromint(v);$err:e "err"})");
  let d0=mt m.get("a"){$ok:v v;$err:e 7};
  let d1=mt m.get("q"){$ok:v v;$err:e 7};
  io.println("default stored \(d0)");
  io.println("default missing \(d1)");
  io.println("value hit \(m.get("b")+1)");
  io.println("value filler \(m.get("q")+1)");
  <0
};
EOF

EXPECTED='direct stored ok
direct missing err
let stored ok
let missing err
reassigned stored ok
stale stored ok
f64 stored ok
f64 missing err
intkey stored ok
intkey missing err
field stored ok
field missing err
call stored ok
early stored 100
early missing err
default stored 0
default missing 7
value hit 3
value filler 1'

for opt in -O0 -O2; do
    rm -f p_bin
    if ! "${TKC}" --allow-all "${opt}" --out p_bin p.tk >compile.log 2>&1; then
        echo "  FAIL: ${opt}: program did not compile"
        grep -o '"message":"[^"]*"' compile.log | head -2 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        continue
    fi
    out="$(./p_bin 2>&1)"
    while IFS= read -r want; do
        if printf '%s\n' "${out}" | grep -qxF "${want}"; then
            echo "  PASS: ${opt} ${want}"
            PASS=$((PASS + 1))
        else
            got="$(printf '%s\n' "${out}" | awk -v p="${want% *} " 'index($0,p)==1' | head -1)"
            echo "  FAIL: ${opt} expected [${want}], got [${got}]"
            FAIL=$((FAIL + 1))
        fi
    done <<< "${EXPECTED}"
done

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
