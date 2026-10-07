#!/usr/bin/env bash
# C742 -- a match-arm label must name a type or a variant; one that names
# nothing is E3011 at --check, not a silent positional arm or an E9005 at
# codegen (story 127.292).
#
# THE DEFECT, measured on main (b442a5d7):
#   * names.c skips the arm label ("validated by the type checker") and the
#     checker only looks for MISSING variants, so any label passed --check.
#   * 127.251 lower-cased the std types, so the old upper-case labels
#     (`ProcessErr:e`, `Handle:h`, `ParseErr:e`, `FileErr:e`, `EnvErr:e`)
#     name nothing now.  `{Handle:h ..;ProcessErr:e ..}` died at codegen
#     with E9005 ("neither of them '$ok' or '$err'") on a program --check
#     passed; and --migrate (127.197) took the --check-clean program for
#     clean and declined to rewrite it (exit 4).
#   * Beside a recognised label the dead one lowered by POSITION:
#     `{$i64:v v;StrErr:e 9}` happened to work, but the same arms in the
#     other order, `{StrErr:e 9;$i64:v v}`, gave 9 for "5" and an
#     uninitialised value for "x" -- a silent wrong value.  So did a
#     misspelled std variant (`$exists` is no fileerr variant).
#
# NOW: names.c checks the label (arm_label_names_something).  A label names
# something when it is ok/err/Ok/Err/some/none, a primitive type, a type or
# variant in scope, or a field/variant of a type declared here or imported
# from a .tki.  Otherwise E3011.  The fix is the 127.251 one -- "did you mean
# '$processerr'?" -- and only when the old spelling's renamed type is in
# scope (its module is imported); no fix for a name that was never a type
# (`StrErr`) or a misspelling (AGENTS §6).  --migrate rewrites the labels.
#
# WHAT IS ASSERTED.
#   1. each old label kind is E3011 with the exact fix (module imported)
#   2. no fix without the module, for a never-was type, a misspelt variant
#   3. negative controls: every label shape in use still checks clean
#   4. the build stops at name resolution (E3011), not codegen (E9005)
#   5. the silent wrong-value order is rejected
#   6. --migrate rewrites the labels; the result checks, builds and runs
#
# Negative control: TKC=<main b442a5d7's toke> -- 14 of 24 fail: all of
# parts 1, 2, 4 and 5, and three of part 6 (main declines the migration, so
# the echoed source "checks clean"); part 3's nine controls pass on both.
#
# Story: 127.292
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c742_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
# code|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s" % (d.get("error_code"), d.get("fix") or "ABSENT"))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

echo "C742: a match-arm label must name a type or a variant (127.292)"
echo "---------------------------------------------------------------"

PROC='m=t;i=io:std.io;i=process:std.process;
f=main():i64{
  let out=mt process.spawn(@("/bin/echo";"c742")){
    Handle:h (mt process.stdout(h){$str:o o;ProcessErr:e ""});
    ProcessErr:e ""
  };
  io.print(out);
  <0
};'

# ── 1: the old labels, module imported ──
expect "1 Handle / ProcessErr (std.process)" "$(diags "${PROC}")" \
    "E3011|did you mean '\$handle'? / E3011|did you mean '\$processerr'? / E3011|did you mean '\$processerr'?"
expect "1 ParseErr (std.str)" \
    "$(diags 'm=t;i=s:std.str;f=main():i64{<mt s.toint("1"){$i64:v v;ParseErr:e 0}};')" \
    "E3011|did you mean '\$parseerr'?"
expect "1 FileErr (std.file)" \
    "$(diags 'm=t;i=file:std.file;f=main():i64{<mt file.read("/x"){$str:v 1;FileErr:e 0}};')" \
    "E3011|did you mean '\$fileerr'?"
expect "1 EnvErr (std.env)" \
    "$(diags 'm=t;i=env:std.env;f=main():i64{<mt env.get("HOME"){$str:v 1;EnvErr:e 0}};')" \
    "E3011|did you mean '\$enverr'?"
expect "1 \$json label (std.json): the alias is not a type" \
    "$(diags 'm=t;i=json:std.json;f=main():i64{<mt json.dec("{}"){$json:v 1;$err:e 0}};')" \
    "E3011|did you mean 'jsonval'?"

# ── 2: no fix ──
expect "2 FileErr without std.file: \$fileerr is not in scope" \
    "$(diags 'm=t;t=$myerr{$bad:str};f=g():i64!$myerr{<1};f=main():i64{<mt g(){$ok:v v;FileErr:e 0}};')" \
    "E3011|ABSENT"
expect "2 StrErr was never a type" \
    "$(diags 'm=t;i=s:std.str;f=main():i64{<mt s.toint("1"){$i64:v v;StrErr:e 0}};')" \
    "E3011|ABSENT"
expect "2 a misspelt std variant (\$exists is no fileerr variant)" \
    "$(diags 'm=t;i=file:std.file;f=why(e:$fileerr):i64{<mt e {$notfound:m 1;$exists:m 2}};f=main():i64{<0};')" \
    "E3011|ABSENT"
expect "2 a misspelt user variant" \
    "$(diags 'm=t;t=$shape{$circle:f64;$square:f64};f=area(s:$shape):f64{<mt s {$circle:r r;$sqare:w w}};f=main():i64{<0};')" \
    "E3011|ABSENT"

# ── 3: negative controls -- every label shape in use checks clean ──
expect "3 \$ok / \$err" "$(diags 'm=t;i=s:std.str;f=main():i64{<mt s.toint("1"){$ok:v v;$err:e 0}};')" "rc=0"
expect "3 legacy Ok / Err" "$(diags 'm=t;i=s:std.str;f=main():i64{<mt s.toint("1"){Ok:v v;Err:e 0}};')" "rc=0"
expect "3 primitive + std error type labels" \
    "$(diags 'm=t;i=s:std.str;f=main():i64{<mt s.toint("1"){$i64:v v;$parseerr:e 0}};')" "rc=0"
expect "3 \$handle / \$processerr" \
    "$(diags 'm=t;i=process:std.process;f=main():i64{<mt process.spawn(@("/bin/true")){$handle:h 0;$processerr:e 1}};')" "rc=0"
expect "3 every std fileerr variant" \
    "$(diags 'm=t;i=file:std.file;f=why(e:$fileerr):i64{<mt e {$notfound:m 1;$permission:m 2;$io:m 3;$invalid:m 4;$isdir:m 5;$notregular:m 6;$symlink:m 7;$toolarge:m 8;$nomem:m 9;$badarg:m 10}};f=main():i64{<0};')" "rc=0"
expect "3 user sum variants" \
    "$(diags 'm=t;t=$shape{$circle:f64;$square:f64};f=area(s:$shape):f64{<mt s {$circle:r r;$square:w w}};f=main():i64{<0};')" "rc=0"
expect "3 user error sum, flat variant spelling" \
    "$(diags 'm=t;t=$perr{$neg:i64;$big:i64};f=g(n:i64):i64!$perr{<n};f=main():i64{<mt g(1){$ok:v v;$neg:n 0;$big:n 1}};')" "rc=0"
expect "3 a user record type label" \
    "$(diags 'm=t;t=$point{x:i64};t=$myerr{$bad:str};f=g():$point!$myerr{<$point{x:1}};f=main():i64{<mt g(){$point:p p.x;$myerr:e 0}};')" "rc=0"
expect "3 str / i32 / u8 / f64 / bool type labels" \
    "$(diags 'm=t;f=g():i64!$str{<1};f=main():i64{let a=mt g(){$str:s 1;$i64:v v};let b=mt g(){$ok:v v;$i32:x 0};let c=mt g(){$ok:v v;$u8:x 0};let d=mt g(){$ok:v v;$f64:x 0};let e=mt g(){$ok:v v;$bool:x 0};<a+b+c+d+e};')" "rc=0"

# ── 4: the build stops at name resolution, not codegen ──
printf '%s\n' "${PROC}" > b.tk
"${TKC}" --allow-all --out b b.tk > b.log 2>&1
expect "4 build of the old-label program: E3011 at name_resolution, no E9005" \
    "$(python3 -c "
import json,sys
s=set()
for l in open('b.log'):
    try: d=json.loads(l)
    except Exception: continue
    s.add(d['error_code']+'@'+d['stage'])
print(' '.join(sorted(s)))")" "E3011@name_resolution"

# ── 5: the silent wrong-value order ──
expect "5 {StrErr:e 9;\$i64:v v} is rejected (main gave 9 for \"5\")" \
    "$(diags 'm=t;i=s:std.str;f=main():i64{<mt s.toint("5"){StrErr:e 9;$i64:v v}};')" \
    "E3011|ABSENT"

# ── 6: --migrate rewrites the labels (main declined: --check clean) ──
"${TKC}" --migrate b.tk > mig.tk 2> mig.err; mrc=$?
expect "6 --migrate exits 0" "${mrc}" "0"
expect "6 --migrate rewrote the labels" \
    "$(grep -o '\$handle:h\|\$processerr:e\|Handle:h\|ProcessErr:e' mig.tk | sort | uniq -c | tr -s ' \n' ' ')" \
    " 1 \$handle:h 2 \$processerr:e "
"${TKC}" --check mig.tk > migc.log 2>&1
expect "6 the migrated program checks clean" "$?" "0"
if "${TKC}" --allow-all --out m mig.tk > m.log 2>&1; then
    expect "6 ... and builds and runs" "$(./m 2>/dev/null)" "c742"
else
    expect "6 ... and builds and runs" "compile failed: $(head -c 300 m.log)" "c742"
fi

echo ""
echo "C742: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
