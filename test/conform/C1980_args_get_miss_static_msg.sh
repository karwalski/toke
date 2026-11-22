#!/usr/bin/env bash
# C1980_args_get_miss_static_msg.sh — args.get's miss path stores a static
# message instead of formatting one on every miss (story 127.470).
#
# HISTORY.  113.B.8 made tk_args_get_w forward args.c's reason through
# tk_err_msg ("args.get: %s"), a vsnprintf into the per-thread message ring on
# EVERY miss.  The canonical cli-argv form `mt args.get(1){$ok:v v;$err:e d}`
# then ran ~11x slower than the count check (1016 ms vs 89 ms, 2026-11-15
# bench).  args_get's one reason is a literal, so the glue now stores the
# literal "args.get: index out of bounds"; any other reason is still formatted.
#
# WHAT IS ASSERTED
#   1. a miss binds e.msg "args.get: index out of bounds" (text unchanged);
#   2. 100,000 misses in a loop each read that same text (count of matches);
#   3. a let-kept args.get error still reads its own message after 20 later
#      raises that DO use the ring (toml.load): the static literal outlives
#      the ring's 8 slots;
#   4. a hit still returns the argument and clears the slot.
#
# Negative control: C1980_STDLIB=<a src/stdlib tree before 127.470> (v3.2.0)
# fails case 3 (prints "line 1: syntax error", the ring slot that overwrote
# it); cases 1, 2 and 4 pass on both by design.
#
# Story: 127.470
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1980_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1980_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;i=args:std.args;i=s:std.str;i=toml:std.toml;
f=main():i64{
  io.println(mt args.get(99) {$ok:v v;$err:e e.msg});
  let n=mut.0;
  lp(let i=0;i<100000;i=i+1){
    let m = mt args.get(args.count() as i64) {$ok:v "hit";$err:e e.msg};
    if(s.eq(m;"args.get: index out of bounds")){n=n+1};
  };
  io.println("\(n)");
  let k = args.get(args.count() as i64);
  lp(let j=0;j<20;j=j+1){
    let r = mt toml.load("= = =") {$ok:v "ok";$err:e e.msg};
  };
  io.println(mt k {$ok:v v;$err:e e.msg});
  io.println(mt args.get(1) {$ok:v v;$err:e "err"});
  <0
};
EOF
if ! "${TKC}" "${WORK}/p.tk" --out "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C1980: 0 passed, 1 failed"; exit 1
fi
OUT="$("${WORK}/b" first 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-miss-msg"          "$(line 1)" "args.get: index out of bounds"
expect "2-loop-misses"       "$(line 2)" "100000"
expect "3-kept-msg-survives" "$(line 3)" "args.get: index out of bounds"
expect "4-hit"               "$(line 4)" "first"
echo "C1980: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
