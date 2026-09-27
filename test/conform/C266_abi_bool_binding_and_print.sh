#!/usr/bin/env bash
# C266_abi_bool_binding_and_print.sh — a bool that crosses the i64 ABI (a
# stdlib call declared `bool`, a predicate receiver method, a bool struct
# field) prints true/false when it is bound without an annotation and then
# used, and when it is passed straight to j.print / j.enc (stories 127.229
# (4), 127.230 (1)).
#
# THE DEFECT. The checker leaves a stdlib call's result TY_UNKNOWN, so the
# backend's only evidence of bool-ness is the interface/field declaration.
# Interpolating the call directly consulted it (127.94/127.194); an
# unannotated `let b=a.contains(3)` did not — the local was an i64 and
# `\(b)`, `j.print(b)`, `b as str` all printed 1/0 — and neither did the
# j.print / j.enc dispatch (`j.print(s.startswith(..))` printed 1).  llvm.c
# now asks one predicate (expr_abi_bool) at the binding and at both
# dispatches; the binding becomes an i1 local, as `let b:bool=…` is.
#
# Negative control: TKC built from main at d236e6f — cases 1-5 red (1/0),
# case 6 (numeric control) green on both.  Case 7 (a bool error union bound
# and matched) is green on main and was red ("build-failed", invalid IR) on
# this branch before call_is_err_union — found by the full conform run (C007).
# Case 8 (Batch LS's report) red on main and before the match rule ("1").
#
# Story: 127.229 (4), 127.230 (1)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() {
    printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" >/dev/null 2>&1 || { echo "check-failed"; return; }
    "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '
}
H='m=main;i=j:std.json;i=s:std.str;i=io:std.io;'

expect "1-jprint-stdlib-call" \
  "$(run "$H f=main():i64{j.print(s.startswith(\"abc\";\"a\"));j.print(s.startswith(\"abc\";\"z\"));<0};")" \
  "true false "
expect "2-let-receiver-contains-interp" \
  "$(run "$H f=main():i64{let a=@(1;2;3);let b=a.contains(3);let c=a.contains(9);io.println(\"\\(b) \\(c)\");<0};")" \
  "true false "
expect "3-let-stdlib-call-jprint-as-str" \
  "$(run "$H f=main():i64{let c=s.contains(\"abc\";\"b\");j.print(c);io.println(c as str);<0};")" \
  "true true "
expect "4-reassigned-and-field" \
  "$(run "$H t=R{on:bool;n:i64}; f=main():i64{let a=@(1);let b=mut.a.contains(1);b=a.contains(2);let r=R{on:true;n:1};let o=r.on;io.println(\"\\(b) \\(o)\");<0};")" \
  "false true "
expect "5-jenc" \
  "$(run "$H f=main():i64{let a=@(1);io.println(j.enc(a.contains(1)));<0};")" \
  "true "
expect "6-numeric-control" \
  "$(run "$H f=main():i64{let n=s.len(\"abc\");j.print(n);io.println(\"\\(n)\");<0};")" \
  "3 3 "

# 7. a bool ERROR UNION bound and matched keeps the ok/err convention
#    (C007's `let w=file.writebytes(..); mt w {..}` — an i1 binding there was
#    invalid IR; found by the full conform run)
printf '%s\n' "$H f=main():i64{let w=f.writebytes(\"/nonexistent-dir-c266/x\";@(1 as byte));mt w {\$ok:v io.println(\"ok\");\$err:e io.println(\"err\")};<0};" | sed 's/^m=main;/m=main;i=f:std.file;/' > "${WORK}/p7.tk"
rm -f "${WORK}/b7"
"${TKC}" "${WORK}/p7.tk" -o "${WORK}/b7" >/dev/null 2>&1 && got7="$("${WORK}/b7" --allow-write 2>/dev/null | tr '\n' ' ')" || got7="build-failed"
expect "7-bool-err-union-binding-matched" "${got7}" "err "

# 8. a match whose arms are all bools, bound by let, interpolates as a bool
#    (its result slot is i64)
expect "8-bool-match-let" \
  "$(run "$H f=main():i64{let c=mt s.toint(\"5\") {\$ok:v s.contains(\"a5\";\"5\"); \$err:e false};io.println(\"\\(c)\");<0};")" \
  "true "

echo "C266: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
