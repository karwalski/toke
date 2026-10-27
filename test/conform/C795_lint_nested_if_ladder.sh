#!/usr/bin/env bash
# C795_lint_nested_if_ladder.sh — `tkc --lint` reports a nested `if` ladder
# (an `el` block whose only statement is another `if`) and `--fix` rewrites it
# to an `el if` chain without changing what the program does (story 131.64).
#
# THE GAP, on main at 8bb9f63e: no lint rule matched `el{if(...)...}`, the
# largest single rewrite class the 134.1 audit found (53 sites in ooke alone,
# none locatable by any rule). quality_rubric.md counts an el-if chain as one
# nesting level, so every `el{if}` rung is a level of depth the chain does not
# cost.
#
# NOW (src/lint.c, rule `nested-if-ladder`, severity hint): one diagnostic per
# ladder, at its top rung, naming the rung count. The fix splices the top
# rung's inner `if` up into `el if`; the --fix fixpoint takes the next rung,
# so a ladder of any length converges in one invocation. No fix when a comment
# sits in the block, or when the outer `if` is a value and the inner one has
# no `el` (an expression-`if` requires one, so the splice would not parse).
#
# Negative control: TKC=<main's toke> -- cases 1, 2, 3, 4b, 6 and 8 fail
# (no rule, so nothing reported and nothing rewritten).
#
# Story: 131.64

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
count_rule() { printf '%s\n' "$1" | grep -c "\"rule\":\"nested-if-ladder\"" || true; }

# ── A program with every shape, run before and after --fix ────────────────
cat > "${WORK}/p.tk" <<'EOF'
m=q;
i=io:std.io;
f=tier(x:i64):i64{
  <if(x>90){5}el{if(x>80){4}el{if(x>70){3}el{if(x>60){2}el{1}}}}
};
f=walk(x:i64):i64{
  let r=mut.0;
  if(x>6){r=1}el{if(x>4){r=2}el if(x>3){r=3}el{if(x>2){r=4}}};
  <r
};
f=early(x:i64):i64{
  if(x>1){<1}el{
    if(x>0){<2};
  };
  <3
};
f=tag(x:i64):str{
  <if(x>1){"el{if(x)}"}el{if(x>0){"\(x)}\("q")"}el{"c"}}
};
f=main():i64{
  lp(let i=0;i<100;i=i+7){
    io.println("\(tier(i)) \(walk(i%9)) \(early(i%3)) \(tag(i%3))")
  };
  <0
};
EOF
"${TKC}" --check "${WORK}/p.tk" >/dev/null 2>&1
expect "0 program checks clean" "$?" "0"

LINT="$("${TKC}" --lint "${WORK}/p.tk" 2>&1)"
# 1: four ladders, each reported once (tier's three rungs are one diagnostic)
expect "1 one hint per ladder" "$(count_rule "${LINT}")" "4"
# 2: the top rung names the whole ladder's depth; `el if` links are free
expect "2 tier reports 3 rungs" \
    "$(printf '%s\n' "${LINT}" | grep -c '(3 nested rungs)')" "1"
expect "2b walk reports 2 rungs across an el-if link" \
    "$(printf '%s\n' "${LINT}" | grep -c '(2 nested rungs)')" "1"
# 3: severity hint, and every one of these carries a fix
expect "3 severity hint" \
    "$(printf '%s\n' "${LINT}" | grep '"rule":"nested-if-ladder"' | grep -c '"severity":"hint"')" "4"
expect "3b all four fixable" \
    "$(printf '%s\n' "${LINT}" | grep '"rule":"nested-if-ladder"' | grep -c '"fix":')" "4"

# 4: --fix behaviour is preserved, converges in one run, and is idempotent
cp "${WORK}/p.tk" "${WORK}/f.tk"
"${TKC}" --lint --fix "${WORK}/f.tk" >/dev/null 2>&1
expect "4 no ladder left after one --fix" \
    "$(count_rule "$("${TKC}" --lint "${WORK}/f.tk" 2>&1)")" "0"
expect "4b no el{if left in the source (outside the string)" \
    "$(grep -v '"el{if(x)}"' "${WORK}/f.tk" | grep -c 'el{if\|el {$')" "0"
expect "4c the string holding el{if( is untouched" \
    "$(grep -c '"el{if(x)}"' "${WORK}/f.tk")" "1"
cp "${WORK}/f.tk" "${WORK}/g.tk"
"${TKC}" --lint --fix "${WORK}/g.tk" >/dev/null 2>&1
expect "4d a second --fix is a no-op" "$(cmp -s "${WORK}/f.tk" "${WORK}/g.tk"; echo $?)" "0"
"${TKC}" "${WORK}/p.tk" -o "${WORK}/p.bin" >/dev/null 2>&1
"${TKC}" "${WORK}/f.tk" -o "${WORK}/f.bin" >/dev/null 2>&1
OUT_P="$("${WORK}/p.bin")"; OUT_F="$("${WORK}/f.bin")"
expect "4e original program runs" "$(printf '%s\n' "${OUT_P}" | wc -l | tr -d ' ')" "15"
expect "4f fixed program prints the same" "${OUT_F}" "${OUT_P}"

# ── 5-7: shapes that are not a ladder, or must not be rewritten ───────────
cat > "${WORK}/n.tk" <<'EOF'
m=q;
f=main():i64{
  let a=1;
  let r=mut.0;
  if(a>1){r=1}el{let b=a+1;if(b>0){r=2}};
  if(a>1){r=r+1}el if(a>0){r=r+2}el{r=r+3};
  if(a>0){if(a>2){r=r+4}el{r=r+5}};
  <r
};
EOF
expect "5 two-statement el block, el-if chain, then-branch nesting: silent" \
    "$(count_rule "$("${TKC}" --lint "${WORK}/n.tk" 2>&1)")" "0"

cat > "${WORK}/v.tk" <<'EOF'
m=q;
f=main():i64{
  let a=1;
  let v=if(a>1){1}el{if(a>0){2}};
  <v
};
EOF
LV="$("${TKC}" --lint "${WORK}/v.tk" 2>&1)"
expect "6 value-position ladder with no inner el: hinted" "$(count_rule "${LV}")" "1"
expect "6b ...but carries no fix" \
    "$(printf '%s\n' "${LV}" | grep '"rule":"nested-if-ladder"' | grep -c '"fix":')" "0"

cat > "${WORK}/c.tk" <<'EOF'
m=q;
f=main():i64{
  let a=1;
  let r=mut.0;
  if(a>1){r=1}el{if(a>0){r=2} (* after *) };
  <r
};
EOF
LC="$("${TKC}" --lint "${WORK}/c.tk" 2>&1)"
expect "7 comment in the block: no fix" \
    "$(printf '%s\n' "${LC}" | grep '"rule":"nested-if-ladder"' | grep -c '"fix":')" "0"

# 8: the rule honours --lint-only style filtering via its id in the JSON
expect "8 rule id is stable" \
    "$(printf '%s\n' "${LC}" | grep -c '"code":"nested-if-ladder","rule":"nested-if-ladder"')" "1"

echo "C795: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
