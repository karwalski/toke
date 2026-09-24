#!/usr/bin/env bash
# C049_recv_method_kind.sh — a receiver method that exists on SOME receiver
# type but not on this one is E4036 at --check (story 127.151).
#
# THE DEFECT. 127.1's check asks "does this member exist on any receiver";
# it cannot ask "on this one". Measured at 444a55c, with the receiver proven
# by the checker:
#
#     let s:str="hello"; s.push(1)     --check 0, run rc=139 (SIGSEGV)
#     let s:str="hello"; s.keys()      --check 0, ld: undefined "_keys"
#     let s:str="hello"; s.pop()       --check 0, prints pointer garbage
#     let m=@("k":"v");  m.upper()     --check 0, prints raw bytes
#     let a=@(1;2);      a.getor(0;1)  --check 0, run rc=139
#
# 46 (member, kind) pairs in all passed --check and then crashed, failed to
# link, or produced garbage.
#
# WHY THIS CAN'T FIRE ON A CORRECT PROGRAM. The check judges only receivers
# the checker proves are a str, an array or a map, and for exactly those the
# emitter's own kind tests (len_recv_kind, recv_is_array, recv_is_map) are
# decided by the same rtype. The per-member kind table (s_recv_kinds in
# types.c) is the emitter's dispatch chain written down per kind.
#
# WHAT IS ASSERTED:
#   1. Wrong-kind members are E4036, with an empty `fix`.
#   2. Every right-kind member still checks clean AND runs to the right
#      answer — the guard against a false diagnostic.
#   3. A receiver the checker cannot type is not judged.
#   4. DRIFT GUARD: every member of the emitter's g_recv_methods (llvm.c) has
#      a row in s_recv_kinds (types.c), except `get`, which is deliberately
#      unjudged. A member added to the emitter without a kind row fails here.
#   5. Negative control for 4: a table with one row removed must be detected.
#
# Negative control for 1: TKC built from 444a55c gives no E4036 (all red).
#
# Story: 127.151

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
prog() { printf 'm=t;\ni=io:std.io;\ni=st:std.str;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"; }

# ── 1. wrong kind ─────────────────────────────────────────────────────────
n=0
while IFS='|' read -r init call; do
    [ -z "$init" ] && continue
    n=$((n + 1))
    prog "bad$n" "  let r=${init};
  let x=r.${call};"
    kind=str; case "$init" in @\(\"k*) kind=map;; @*) kind=array;; esac
    expect "1-${kind}-${call%%(*}-is-E4036" "$(codes "${WORK}/bad$n.tk")" "E4036 "
done <<'EOF'
"hello"|push(1)
"hello"|append(1)
"hello"|pop()
"hello"|set(0;1)
"hello"|getor("a";1)
"hello"|keys()
"hello"|toarray()
@(1;2)|upper()
@(1;2)|keys()
@(1;2)|getor(0;1)
@(1;2)|split(",")
@("k":"v")|push(1)
@("k":"v")|upper()
@("k":"v")|slice(0;1)
@("k":"v")|join(",")
EOF
prog typedparam '  let s:str="hello";
  s.push(1);'
expect "1-annotated-str-push-is-E4036" "$(codes "${WORK}/typedparam.tk")" "E4036 "
expect "1-fix-field-empty" \
    "$("${TKC}" --check "${WORK}/typedparam.tk" 2>&1 | grep -c '"fix":""' || true)" "1"

# ── 2. right kind: checks clean and runs ─────────────────────────────────
cat > "${WORK}/good.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let s="hello";
  let a=mut.@(3;1;2);
  let m=mut.@("k":1);
  let w=@("x";"y");
  a=a.push(4);
  a=a.set(0;5);
  m=m.set("j";2);
  if(s.contains("ell")&&a.contains(4)&&m.contains("j")){io.println("contains")};
  io.println("\(s.len()) \(s.upper()) \(s.slice(1;3)) \(s.find("l"))");
  io.println("\(a.len()) \(a.indexof(2)) \(a.get(0))");
  io.println("\(m.len()) \(m.getor("z";9)) \(m.keys().len)");
  io.println("\(w.join("-")) \(",".join(w))");
  <0
};
EOF
expect "2-right-kind-checks" "$(codes "${WORK}/good.tk")" ""
"${TKC}" "${WORK}/good.tk" -o "${WORK}/good" >/dev/null 2>&1
expect "2-right-kind-runs" "$("${WORK}/good" 2>/dev/null | tr '\n' '|')" \
    "contains|5 HELLO el 2|4 2 5|2 9 2|x-y x,y|"

# ── 3. an untyped receiver is not judged ─────────────────────────────────
prog untyped '  let t=st.trim(" ab ");
  let u=t.upper();
  io.println(u);'
expect "3-untyped-receiver-not-judged" "$(codes "${WORK}/untyped.tk")" ""

# ── 4. drift guard: emitter table vs checker table ───────────────────────
emitter_members() {
    awk '/^static const RecvMethod g_recv_methods\[\]/{p=1;next} p&&/^};/{exit}
         p{ if (match($0,/\{"[a-z]+"/)) print substr($0,RSTART+2,RLENGTH-3) }' "$1" | sort -u
}
checker_members() {
    awk '/^static const RecvKinds s_recv_kinds\[\]/{p=1;next} p&&/^};/{exit}
         p{ s=$0; while (match(s,/\{"[a-z]+"/)) { print substr(s,RSTART+2,RLENGTH-3); s=substr(s,RSTART+RLENGTH) } }' "$1" | sort -u
}
emitter_members "${REPO_ROOT}/src/llvm.c" > "${WORK}/em.txt"
checker_members "${REPO_ROOT}/src/types.c" | cat - <(echo get) | sort -u > "${WORK}/ck.txt"
expect "4-emitter-table-read" "$([ "$(wc -l < "${WORK}/em.txt")" -ge 30 ] && echo ok || echo "only $(wc -l < "${WORK}/em.txt")")" "ok"
expect "4-every-emitter-member-has-a-kind-row" "$(comm -23 "${WORK}/em.txt" "${WORK}/ck.txt" | tr '\n' ' ')" ""
expect "4-no-kind-row-for-a-member-the-emitter-lacks" "$(comm -13 "${WORK}/em.txt" "${WORK}/ck.txt" | tr '\n' ' ')" ""

# ── 5. negative control for 4 ────────────────────────────────────────────
sed 's/{"getor", RK_M}, *//' "${REPO_ROOT}/src/types.c" > "${WORK}/types_minus.c"
expect "5a-injection-landed" "$(grep -c '{"getor", RK_M}' "${WORK}/types_minus.c" || true)" "0"
checker_members "${WORK}/types_minus.c" | cat - <(echo get) | sort -u > "${WORK}/ck2.txt"
expect "5b-missing-row-detected" "$(comm -23 "${WORK}/em.txt" "${WORK}/ck2.txt" | tr '\n' ' ')" "getor "

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
