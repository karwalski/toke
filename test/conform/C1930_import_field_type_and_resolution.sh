#!/usr/bin/env bash
# C1930_import_field_type_and_resolution.sh — loke's T-39, reopened at 3.2.0
# (stories 138.39, 138.40).
#
# THE DEFECTS.
#   138.39 `unused-import` reported an import that the program depends on:
#     (a) used only as a struct-field type (`t=$holder{items:@$item}`,
#         `it:$item`).  The 137.8 caution collected every TYPE_IDENT inside a
#         `t=` declaration as "declared here", so a field TYPE looked local;
#         after `--lint --fix` the next --check was E3011.
#     (b) needed only so a field read resolves: `let r=srv.handle(x); <r.text`
#         where `handle` returns `$res`, a record of `proto`.  The alias `p`
#         never appears again, so the rule called it unused.
#   138.40 with `proto` removed, --check passed `r.text` (the base was simply
#     untyped) while the compile refused it with E4034.  The spec brings a
#     type into module scope only through an import (semantics.md 4.1, 6.1),
#     so both now refuse with E4034, and no `fix` is offered: which module
#     provides the record is not decidable from the file.
#
# WHAT IS ASSERTED.
#   1. typeonly: neither field-type spelling is flagged; the param-type use
#      (never flagged) still is not; --lint --fix leaves fieldonly.tk
#      byte-identical and it still checks and compiles.
#   2. fieldres: `p` is not flagged; --lint --fix keeps it; the result builds.
#   3. --check and the compile agree on the file without `proto`: both E4034,
#      neither carries a `fix`, in both spellings (bound `let`, read in place).
#   4. Negative controls: a genuinely unused import is still flagged and still
#      fixed, alone and beside a load-bearing one; the import-present program
#      checks clean (the new E4034 is not a false positive).
#
# Story: 138.39, 138.40

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0
expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"; PASS=$((PASS + 1))
    else
        echo "FAIL $label"; echo "        expected: $want"; echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}
nflag() { "${TKC}" -I "${W}/ifc" --lint "$1" 2>&1 | grep -c '"rule":"unused-import"' || true; }
codes() { "$@" 2>&1 | grep -o '"error_code":"[^"]*"' | sort -u | tr '\n' ' ' | sed 's/ $//'; }

W="$(mktemp -d)"
trap 'rm -rf "${W}"' EXIT
mkdir -p "${W}/ifc" "${W}/out"

# ── Fixtures (loke repro-2026-11-15/T-39, reduced) ───────────────────────
cat > "${W}/defs.tk" <<'EOF'
m=defs;
t=$item{name:str;n:i64};
f=mk(n:i64):$item{<$item{name:"x";n:n}};
EOF
cat > "${W}/fieldonly.tk" <<'EOF'
m=fieldonly;
i=d:defs;
t=$holder{items:@$item;k:i64};
f=count(h:$holder):i64{<h.k};
EOF
cat > "${W}/scalarfield.tk" <<'EOF'
m=scalarfield;
i=d:defs;
t=$holder{it:$item;k:i64};
f=count(h:$holder):i64{<h.k};
EOF
cat > "${W}/paramonly.tk" <<'EOF'
m=paramonly;
i=d:defs;
f=num(x:$item):i64{<x.n};
EOF
cat > "${W}/proto.tk" <<'EOF'
m=proto;
t=$res{iserror:bool;text:str};
EOF
cat > "${W}/server.tk" <<'EOF'
m=server;
i=p:proto;
f=handle(x:str):$res{<$res{iserror:false;text:x}};
EOF
cat > "${W}/user.tk" <<'EOF'
m=user;
i=srv:server;
i=p:proto;
f=preview(x:str):str{
  let r=srv.handle(x);
  <r.text
};
EOF
for m in defs proto server; do
    "${TKC}" -I "${W}/ifc" --emit-interface --emit-llvm --out "${W}/ifc" "${W}/${m}.tk" >/dev/null 2>&1
done
expect "premise-interfaces-built" "$(ls "${W}/ifc"/*.tki 2>/dev/null | wc -l | tr -d ' ')" "3"

# ── 1. struct-field type uses (138.39 a) ─────────────────────────────────
expect "fieldonly-not-flagged"   "$(nflag "${W}/fieldonly.tk")"   "0"
expect "scalarfield-not-flagged" "$(nflag "${W}/scalarfield.tk")" "0"
expect "paramonly-not-flagged"   "$(nflag "${W}/paramonly.tk")"   "0"
cp "${W}/fieldonly.tk" "${W}/out/fieldonly.tk"
"${TKC}" -I "${W}/ifc" --lint --fix "${W}/out/fieldonly.tk" >/dev/null 2>&1
cmp -s "${W}/fieldonly.tk" "${W}/out/fieldonly.tk" && same=yes || same=no
expect "fieldonly-fix-leaves-file" "${same}" "yes"
"${TKC}" -I "${W}/ifc" --check "${W}/out/fieldonly.tk" >/dev/null 2>&1
expect "fieldonly-checks-after-fix" "$?" "0"
"${TKC}" -I "${W}/ifc" --emit-llvm --out "${W}/out" "${W}/out/fieldonly.tk" >/dev/null 2>&1
expect "fieldonly-compiles-after-fix" "$?" "0"

# ── 2. field-resolution use (138.39 b) ───────────────────────────────────
expect "fieldres-not-flagged" "$(nflag "${W}/user.tk")" "0"
cp "${W}/user.tk" "${W}/out/user.tk"
"${TKC}" -I "${W}/ifc" --lint --fix "${W}/out/user.tk" >/dev/null 2>&1
expect "fieldres-fix-keeps-import" "$(grep -c '^i=p:proto;$' "${W}/out/user.tk")" "1"
"${TKC}" -I "${W}/ifc" --emit-llvm --out "${W}/out" "${W}/out/user.tk" >/dev/null 2>&1
expect "fieldres-compiles-after-fix" "$?" "0"
"${TKC}" -I "${W}/ifc" --check "${W}/user.tk" >/dev/null 2>&1
expect "fieldres-with-import-checks" "$?" "0"

# ── 3. --check and the compile agree without the import (138.40) ────────
grep -v '^i=p:proto;$' "${W}/user.tk" > "${W}/noproto.tk"
cat > "${W}/inplace.tk" <<'EOF'
m=inplace;
i=srv:server;
f=preview(x:str):str{<srv.handle(x).text};
EOF
for f in noproto inplace; do
    chk="$(codes "${TKC}" -I "${W}/ifc" --check --diag-json "${W}/${f}.tk")"
    cmp_="$(codes "${TKC}" -I "${W}/ifc" --emit-llvm --out "${W}/out" --diag-json "${W}/${f}.tk")"
    expect "${f}-check-E4034"   "${chk}"  '"error_code":"E4034"'
    expect "${f}-compile-E4034" "${cmp_}" '"error_code":"E4034"'
    nfix="$("${TKC}" -I "${W}/ifc" --check --diag-json "${W}/${f}.tk" 2>&1 | grep -c '"fix"' || true)"
    expect "${f}-E4034-has-no-fix" "${nfix}" "0"
done
# A local record of the same name is this file's own, not the import's: not judged.
cat > "${W}/localres.tk" <<'EOF'
m=localres;
i=srv:server;
t=$res{iserror:bool;text:str};
f=main():i64{<0};
EOF
"${TKC}" -I "${W}/ifc" --check "${W}/localres.tk" >/dev/null 2>&1
expect "local-record-no-new-error" "$?" "0"

# ── 4. negative controls: a truly unused import still warns and is fixed ─
cat > "${W}/genuine.tk" <<'EOF'
m=genuine;
i=d:defs;
i=p:proto;
f=main():i64{<0};
EOF
expect "genuine-both-flagged" "$(nflag "${W}/genuine.tk")" "2"
cat > "${W}/mixed.tk" <<'EOF'
m=mixed;
i=d:defs;
i=p:proto;
t=$h{it:$item};
f=one(x:$h):i64{<x.it.n};
EOF
expect "mixed-only-proto-flagged" "$("${TKC}" -I "${W}/ifc" --lint "${W}/mixed.tk" 2>&1 | grep -o "imported module '[a-z]*'" | tr '\n' ' ')" "imported module 'p' "
cp "${W}/mixed.tk" "${W}/out/mixed.tk"
"${TKC}" -I "${W}/ifc" --lint --fix "${W}/out/mixed.tk" >/dev/null 2>&1
expect "mixed-fix-drops-proto-only" "$(grep -c '^i=' "${W}/out/mixed.tk")" "1"
"${TKC}" -I "${W}/ifc" --check "${W}/out/mixed.tk" >/dev/null 2>&1
expect "mixed-checks-after-fix" "$?" "0"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
