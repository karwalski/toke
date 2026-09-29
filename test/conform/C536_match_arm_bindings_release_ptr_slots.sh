#!/usr/bin/env bash
# C536_match_arm_bindings_release_ptr_slots.sh — a function with many inline
# many-armed matches does not run out of pointer-local slots (story 127.246
# item 4).
#
# THE DEFECT.  Every str-typed `mt` arm binding took an entry in llvm.c's
# pointer-local registry (mark_ptr_with_type) and kept it for the rest of the
# function, though the binding dies with its arm.  Ten inline 10-arm matches
# over a str-payload sum (E1's ten-variant FileErr, matched inline) is 100
# entries; the registry holds max_locals (64 by default), so the build failed
# with a stream of unlocated E9010 "too many pointer locals" -- on a program
# --check accepted.
#
# NOW: when an arm ends, the entries it made that die with it (its binding,
# and the raw-name entries 127.237 (3) retired) are released; entries the arm
# body made for outer names are kept.
#
# Negative control: TKC=<main d3a083b8's toke> -- cases 1-3 fail to build
# (E9010).  Case 4 (an outer str re-bound as an f64 in a shadowing arm keeps
# its own tag after the arm, 127.237 (3)) is green on both.
#
# Story: 127.246
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
run() {
    local o out prev=""
    for o in -O0 -O2; do
        rm -f b
        "${TKC}" --allow-all "${o}" --out b "$1" >/dev/null 2>&1 || { echo "build-failed${o}"; return; }
        out="$(./b 2>&1 | sort | uniq -c | tr -s ' ' | tr '\n' '|'; echo "rc=${PIPESTATUS[0]}")"
        if [ -n "${prev}" ] && [ "${prev}" != "${out}" ]; then echo "O-levels differ: ${prev} / ${out}"; return; fi
        prev="${out}"
    done
    echo "${prev}"
}
python3 - <<'PY'
V = ['$v%d' % i for i in range(10)]
sumt = 't=$big{' + ';'.join('%s:str' % v for v in V) + '};'
arms = '; '.join('%s:x x' % v for v in V)
# 1. a sum-tag switch, twelve times
L = ['m=a;', 'i=io:std.io;', sumt, 'f=main():i64{', '  let s=$big{$v3:"three"};']
L += ['  io.println(mt s { %s });' % arms] * 12
L += ['  <0', '};']
open('a.tk', 'w').write('\n'.join(L) + '\n')
# 2. a flat result match over a user T!$big (FileErr's shape), twelve times
L = ['m=b;', 'i=io:std.io;', sumt,
     'f=g(n:i64):str!$big{ if(n<0){ <$v7("seven") }; <"ok" };',
     'f=main():i64{']
L += ['  io.println(mt g(0-1) { $ok:o o; %s });' % arms] * 12
L += ['  <0', '};']
open('b.tk', 'w').write('\n'.join(L) + '\n')
# 3. the same, as `$err:e mt e {..}` nested
L = ['m=c;', 'i=io:std.io;', sumt,
     'f=g(n:i64):str!$big{ if(n<0){ <$v7("seven") }; <"ok" };',
     'f=main():i64{']
L += ['  io.println(mt g(0-1) { $ok:o o; $err:e mt e { %s } });' % arms] * 12
L += ['  <0', '};']
open('c.tk', 'w').write('\n'.join(L) + '\n')
PY
expect "1 twelve 10-arm sum switches" "$(run a.tk)" " 12 three|rc=0"
expect "2 twelve 10-arm flat result matches" "$(run b.tk)" " 12 seven|rc=0"
expect "3 twelve nested \$err:e matches" "$(run c.tk)" " 12 seven|rc=0"

cat > d.tk <<'EOF2'
m=d;
i=io:std.io;
t=$sh{$a:f64;$b:str};
f=main():i64{
  let v=2.5;
  let s=$sh{$b:"bee"};
  let w=mt s { $a:v "a\(v)"; $b:v "b:\(v)" };
  io.println("\(w) \(v<3.0)");
  <0
};
EOF2
expect "4 control: an outer f64 keeps its tag after a shadowing str arm" "$(run d.tk)" " 1 b:bee true|rc=0"

echo "C536: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
