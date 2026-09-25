#!/usr/bin/env bash
# C073_recv_dispatch_complete.sh — the receiver-dispatch chain has a branch
# for every member its table routes to it, and neither of the emitter's two
# silent "safety net" lowerings survives (stories 127.167, 127.172).
#
# 127.167. llvm.c's receiver dispatch chain ended `else fn_im =
# "tk_str_eq_w"`: a member present in g_recv_methods (chain = 1) with no
# branch of its own was lowered as str.eq with the wrong arity, silently.
# Observed by Batch F2: a trial `indexfrom` row made `t.indexfrom(",";2)`
# print 0. The chain's final `else` is now an internal-error diagnostic
# (E9002, no fix) and `eq` has its own branch.
#
# 127.172. The `+` lowering for an i8* operand fell back to tk_str_concat —
# `str + str`, the operator ADR-0004 removed. The checker rejects every shape
# of it (C043, C057), so the path is unreachable from checked code; it is now
# an internal error rather than a concatenation that would hide a checker
# hole.
#
# Both paths are unreachable from any program the checker accepts, so they
# cannot be exercised by running a program; they are asserted on the source.
# The injected-row red/green for 127.167 was measured once, by hand, with a
# rebuilt compiler (see the commit and the progress row): old chain printed
# 0, new chain E9002.
#
# WHAT IS ASSERTED:
#   1. Every chain member of g_recv_methods has a `!strcmp(method_im, "…")`
#      branch in the chain, and the chain names no member the table lacks.
#   2. The chain's final `else` emits E9002 and assigns no glue symbol.
#   3. The `+` lowering emits no call to @tk_str_concat and emits E9002.
#   4. A checked program cannot reach 3: `str + str` is E4031 at --check.
#   5. Negative controls: a copy of llvm.c with one branch deleted, with the
#      old `else fn_im = "tk_str_eq_w"` restored, and with the old concat
#      restored, is each detected.
#
# Negative control against main 2e932d6: 1 fails ("eq" has no branch), 2
# fails, 3 fails.
#
# Stories: 127.167, 127.172

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
LLVM_C="${LLVM_C:-${REPO_ROOT}/src/llvm.c}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

echo "C073: receiver dispatch is complete; no silent fallback lowerings"
echo "------------------------------------------------------------------"

table_chain_members() {
    awk '/^static const RecvMethod g_recv_methods\[\]/{p=1;next} p&&/^};/{exit}
         p && /^    \{"[a-z]+", *1,/ { match($0,/\{"[a-z]+"/); print substr($0,RSTART+2,RLENGTH-3) }' "$1" | sort -u
}
chain_region() {
    awk '/if \(!is_mod_im && recv_method_in_chain\(method_im\)\) \{/{p=1} p&&/Emit base object as first arg/{exit} p' "$1"
}
chain_branches() {
    chain_region "$1" | awk '{ s=$0; while (match(s,/strcmp\(method_im, "[a-z]+"\)/)) {
        t=substr(s,RSTART,RLENGTH); sub(/^strcmp\(method_im, "/,"",t); sub(/"\)$/,"",t); print t
        s=substr(s,RSTART+RLENGTH) } }' | sort -u
}
# final_else FILE — the text of the chain's last `else` branch (to the E9002
# or the next statement).
final_else() {
    chain_region "$1" | awk '/^                else[ {]/ && !/else if/ {p=1} p' | head -20 |
        grep -vE '^[[:space:]]*(/\*|\*)'   # code only: the comment quotes the old line
}
plus_block() {
    awk '/Concat dispatch for `\+` with at least one i8\* operand/{p=1} p&&/String equality: when TK_EQ/{exit} p' "$1"
}
check_all() {   # FILE PREFIX
    local f="$1" pfx="$2"
    table_chain_members "$f" > "${WORK}/tbl.txt"
    chain_branches "$f" > "${WORK}/br.txt"
    expect "${pfx}1-table-read" "$([ "$(wc -l < "${WORK}/tbl.txt" | tr -d ' ')" -ge 25 ] && echo ok || echo short)" "ok"
    expect "${pfx}1-every-chain-member-has-a-branch" "$(comm -23 "${WORK}/tbl.txt" "${WORK}/br.txt" | tr '\n' ' ')" ""
    expect "${pfx}1-no-branch-for-a-non-member" "$(comm -13 "${WORK}/tbl.txt" "${WORK}/br.txt" | tr '\n' ' ')" ""
    local fe; fe="$(final_else "$f")"
    expect "${pfx}2-final-else-is-E9002" "$(printf '%s' "$fe" | grep -c 'E9002' | tr -d ' ')" "1"
    expect "${pfx}2-final-else-assigns-no-glue" "$(printf '%s' "$fe" | grep -c 'fn_im *= *"' | tr -d ' ')" "0"
    expect "${pfx}3-plus-emits-no-str-concat" "$(plus_block "$f" | grep -c '@tk_str_concat(' | tr -d ' ')" "0"
    expect "${pfx}3-plus-str-path-is-E9002" "$(plus_block "$f" | grep -c 'E9002' | tr -d ' ')" "1"
}

# ── 1-3. the tree ───────────────────────────────────────────────────────
check_all "${LLVM_C}" ""

# ── 4. unreachable from checked code ────────────────────────────────────
printf 'm=t;\ni=io:std.io;\nf=main():i64{\n  let a="x";\n  let b="y";\n  io.println(a+b);\n  <0\n};\n' > "${WORK}/plus.tk"
expect "4-str-plus-str-is-E4031-at-check" \
    "$("${TKC}" --check "${WORK}/plus.tk" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}')" "E4031 "

# ── 5. negative controls ────────────────────────────────────────────────
sed 's/^\( *\)else if (!strcmp(method_im, "charat"))  fn_im = "tk_str_charat_w";/\1\/\* deleted \*\//' "${LLVM_C}" > "${WORK}/nobranch.c"
expect "5a-branch-deletion-landed" "$(grep -c 'strcmp(method_im, "charat")' "${WORK}/nobranch.c" | tr -d ' ')" "0"
table_chain_members "${WORK}/nobranch.c" > "${WORK}/t5.txt"; chain_branches "${WORK}/nobranch.c" > "${WORK}/b5.txt"
expect "5a-missing-branch-detected" "$(comm -23 "${WORK}/t5.txt" "${WORK}/b5.txt" | tr '\n' ' ')" "charat "

awk '/^                else if \(!strcmp\(method_im, "eq"\)\)/{print "                else                                    fn_im = \"tk_str_eq_w\";"; skip=1; next}
     skip && /^                \/\* Emit base object/{skip=0}
     skip{next} {print}' "${LLVM_C}" > "${WORK}/oldelse.c"
expect "5b-old-else-landed" "$(grep -c '^                else                                    fn_im = "tk_str_eq_w";' "${WORK}/oldelse.c" | tr -d ' ')" "1"
expect "5b-old-else-detected" "$(final_else "${WORK}/oldelse.c" | grep -c 'fn_im *= *"' | tr -d ' ')" "1"

awk '/127.172: this was `tk_str_concat`/{print "                fprintf(c->out, \"  %%t%d = call i8* @tk_str_concat(i8* %%t%d, i8* %%t%d)\\n\", t, lhs, rhs);"} {print}' "${LLVM_C}" > "${WORK}/oldplus.c"
expect "5c-old-concat-landed-and-detected" "$(plus_block "${WORK}/oldplus.c" | grep -c '@tk_str_concat(' | tr -d ' ')" "1"

echo "------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
