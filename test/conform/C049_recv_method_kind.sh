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
# decided by the same rtype. The per-member kind column (g_recv_methods in
# llvm.c, since 127.177) is the emitter's dispatch chain written down per kind.
#
# WHAT IS ASSERTED:
#   1. Wrong-kind members are E4036, with an empty `fix`.
#   2. Every right-kind member still checks clean AND runs to the right
#      answer — the guard against a false diagnostic.
#   3. A receiver the checker cannot type is not judged.
#   4. ONE TABLE (127.177): the kind column lives in the emitter's
#      g_recv_methods (llvm.c) — every row carries one — and types.c holds no
#      copy of it and reads recv_method_on_kind(). (Until 127.177 this section
#      was a drift guard between two tables; with one table there is nothing
#      to drift, so what is guarded now is that a second table does not come
#      back.)
#   5. Negative control for 4: a types.c with a copied kind table re-injected,
#      and an llvm.c row with its kind column removed, must each be detected.
#
# Negative control for 1: TKC built from 444a55c gives no E4036 (all red).
#
# Story: 127.151; section 4 rewritten for 127.177 (one table)

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

# ── 4. one table (127.177) ───────────────────────────────────────────────
# rows_without_kinds FILE — members of g_recv_methods whose row does not carry
# a kind column ({"member", chain, KINDS, {syms...}}).
rows_without_kinds() {
    awk '/^static const RecvMethod g_recv_methods\[\]/{p=1;next} p&&/^};/{exit}
         p && /^    \{"[a-z]+"/ {
             if ($0 !~ /^    \{"[a-z]+", *[01], *(0|RK_[A-Z_]+(\|RK_[A-Z_]+)*), *\{/) {
                 match($0,/\{"[a-z]+"/); print substr($0,RSTART+2,RLENGTH-3) } }' "$1"
}
emitter_rows() {
    awk '/^static const RecvMethod g_recv_methods\[\]/{p=1;next} p&&/^};/{exit}
         p && /^    \{"[a-z]+"/ {n++} END{print n+0}' "$1"
}
# second_table FILE — a per-member kind table in the checker: the old
# s_recv_kinds shape, or any {"member", RK_…} row.
second_table() { grep -cE 's_recv_kinds\[\]|\{"[a-z]+", *RK_' "$1" || true; }
expect "4-emitter-table-read" "$([ "$(emitter_rows "${REPO_ROOT}/src/llvm.c")" -ge 30 ] && echo ok || echo "only $(emitter_rows "${REPO_ROOT}/src/llvm.c")")" "ok"
expect "4-every-emitter-row-carries-kinds" "$(rows_without_kinds "${REPO_ROOT}/src/llvm.c" | tr '\n' ' ')" ""
expect "4-checker-holds-no-copy" "$(second_table "${REPO_ROOT}/src/types.c")" "0"
expect "4-checker-reads-the-emitter-column" \
    "$(grep -c 'recv_method_on_kind(member' "${REPO_ROOT}/src/types.c" || true)" "1"

# ── 5. negative controls for 4 ───────────────────────────────────────────
{ cat "${REPO_ROOT}/src/types.c"; printf 'static const RecvKinds s_recv_kinds[] = {\n    {"getor", RK_M},\n};\n'; } > "${WORK}/types_plus.c"
expect "5a-copy-injected" "$(grep -c '{"getor", RK_M}' "${WORK}/types_plus.c" || true)" "1"
expect "5b-copy-detected" "$([ "$(second_table "${WORK}/types_plus.c")" -ge 1 ] && echo detected || echo missed)" "detected"
sed 's/{"getor",     1, RK_M, /{"getor",     1, /' "${REPO_ROOT}/src/llvm.c" > "${WORK}/llvm_minus.c"
expect "5c-kind-removal-landed" "$(grep -c '{"getor",     1, {' "${WORK}/llvm_minus.c" || true)" "1"
expect "5d-row-without-kinds-detected" "$(rows_without_kinds "${WORK}/llvm_minus.c" | tr '\n' ' ')" "getor "

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
