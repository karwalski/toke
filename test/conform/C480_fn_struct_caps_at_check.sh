#!/usr/bin/env bash
# C480_fn_struct_caps_at_check.sh — the function-signature and struct-type
# caps are met at --check, located, and name the count, the limit, what
# counted and how to raise them (story 138.15, loke T-25 and T-54).
#
# T-25: max_funcs counts every imported .tki signature, but only codegen's
# register_fn knew it -- `--check` was rc 0, then the build failed with
# "compiler limit exceeded: too many functions" at line 0, once per function
# over, naming nothing.
# T-54: at the struct cap an imported .tki type was skipped SILENTLY
# (register_tki_struct_types, load_tki_structs), so the program failed later
# with an E4034 blaming a field of the dropped type.
#
# Both caps are now counted by one rule (src/tkc_caps.h) that resolve_imports
# and emit_llvm_ir both call.  The boundary cases pin that the rule and
# codegen's registries agree: with the limit raised to exactly the reported
# count the program builds, and one below it is still refused with the same
# count.
#
# Negative control: TKC=<main's toke> -- cases 1-3, 5-7 and 9-11 fail
# (main is rc 0 at --check; its build gives a bare E9010 or an E4034).
#
# 127.250: the defaults rose to 1024 function signatures and 1024 struct
# types.  Cases 1-12 pin the rule at the OLD defaults by passing them
# explicitly (--max-funcs=128 / --max-structs=64, or tkc.toml), so they test
# what they always tested; cases 14-18 pin the new defaults, with no flag.
# Negative control for 14-18: the compiler at this change's parent (old
# defaults 128/64) passes 1-13 and fails all of 14-18 (13 passed, 5 failed).
#
# Story: 138.15 (T-25, T-54); 127.250

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

# code|line|expected|got|fix of each diagnostic, joined by ' / '; "clean" if none
diags() { "${TKC}" -I . "$@" 2>&1 | python3 -c '
import sys, json
out = []
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") != "error": continue
    out.append("%s|%s|%s|%s|%s" % (d.get("error_code"), d.get("pos", {}).get("line"),
               d.get("expected"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "clean")'; }
msg() { "${TKC}" -I . "$@" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    if d.get("error_code") == "E9010": print(d.get("message")); break'; }

# ── T-25: 10 functions here, 120 imported ────────────────────────────────
{ printf 'm=big;\n'; for i in $(seq 1 120); do printf 'f=g%d():i64{<%d};\n' "$i" "$i"; done; } > big.tk
"${TKC}" --emit-interface --check big.tk > /dev/null 2>&1
{ printf 'm=s25;\ni=big:big;\n'; for i in $(seq 1 9); do printf 'f=f%d():i64{<big.g%d()};\n' "$i" "$i"; done
  printf 'f=main():i64{<f1()};\n'; } > s25.tk
FFIX="raise the limit: pass --max-funcs=130, or set max_funcs = 130 in tkc.toml"
expect "1 T-25: imported signatures count, caught at --check, at the import" \
    "$(diags --check --max-funcs=128 s25.tk)" "E9010|2|128|130|${FFIX}"
expect "2 the message names count, limit, key and what counted" \
    "$(msg --check --max-funcs=128 s25.tk)" \
    "compiler limit exceeded: this module needs 130 function signatures and the limit is 128 (max_funcs); counted: 10 declared in this module, 120 imported from big"
expect "3 one under the count is still refused, same count" \
    "$(diags --check --max-funcs=129 s25.tk)" \
    "E9010|2|129|130|${FFIX}"
expect "4 raised to exactly the count: builds (the rule and codegen agree)" \
    "$(diags --max-funcs=130 --emit-llvm --out s25.ll s25.tk)" "clean"

# ── T-54: 58 struct types imported, then std.http's records ──────────────
{ printf 'm=t54lib;\n'; for i in $(seq -w 1 58); do printf 't=$l%s{body:str;k%s:i64};\n' "$i" "$i"; done
  printf 'f=one():i64{<1};\n'; } > t54lib.tk
"${TKC}" --emit-interface --max-structs=256 --check t54lib.tk > /dev/null 2>&1
cat > t54.tk <<'EOF'
m=t54i;
i=io:std.io;
i=http:std.http;
i=tl:t54lib;
f=blen(r:$httpresp):i64{ <r.body.len };
f=main():i64{
  io.println("one=\(tl.one())");
  <0
};
EOF
SFIX="raise the limit: pass --max-structs=68, or set max_structs = 68 in tkc.toml"
expect "5 T-54: never dropped silently -- caught at --check, at the std import" \
    "$(diags --check --max-structs=64 t54.tk)" "E9010|3|64|68|${SFIX}"
expect "6 the message names every contributor" \
    "$(msg --check --max-structs=64 t54.tk)" \
    "compiler limit exceeded: this module needs 68 struct types and the limit is 64 (max_structs); counted: 58 imported from t54lib, 2 built in (timeparts, none), 8 imported from std.http"
expect "7 one under the count is still refused, same count" \
    "$(diags --check --max-structs=67 t54.tk)" "E9010|3|67|68|${SFIX}"
expect "8 raised to exactly the count: builds, no E4034 on r.body" \
    "$(diags --max-structs=68 --emit-llvm --out t54.ll t54.tk)" "clean"

# ── declared in this module ──────────────────────────────────────────────
{ printf 'm=lf;\n'; for i in $(seq 1 128); do printf 'f=f%d():i64{<%d};\n' "$i" "$i"; done
  printf 'f=main():i64{<0};\n'; } > lf.tk
expect "9 129 functions declared here: located at the 129th (line 130)" \
    "$(diags --check --max-funcs=128 lf.tk)" \
    "E9010|130|128|129|raise the limit: pass --max-funcs=129, or set max_funcs = 129 in tkc.toml"
{ printf 'm=ls;\n'; for i in $(seq 1 63); do printf 't=$r%d{a:i64};\n' "$i"; done
  printf 'f=main():i64{<0};\n'; } > ls.tk
expect "10 63 types + the 2 built-ins: located at the last type declared" \
    "$(diags --check --max-structs=64 ls.tk)" \
    "E9010|64|64|65|raise the limit: pass --max-structs=65, or set max_structs = 65 in tkc.toml"
expect "11 a build is refused at the same place: no bare codegen E9010" \
    "$(diags --max-structs=64 --emit-llvm --out ls.ll ls.tk)" \
    "E9010|64|64|65|raise the limit: pass --max-structs=65, or set max_structs = 65 in tkc.toml"
expect "12 tkc.toml max_structs sets it: 64 refuses, 65 is clean" \
    "$(printf 'max_structs = 64\n' > tkc.toml; diags --check ls.tk; printf 'max_structs = 65\n' > tkc.toml; diags --check ls.tk; rm -f tkc.toml)" \
    "E9010|64|64|65|raise the limit: pass --max-structs=65, or set max_structs = 65 in tkc.toml
clean"

# ── under the caps ───────────────────────────────────────────────────────
printf 'm=ok;\ni=io:std.io;\ni=http:std.http;\nt=$p{a:i64};\nf=main():i64{io.println("x");<0};\n' > ok.tk
expect "13 a module under both caps is clean" "$(diags --check ok.tk)" "clean"

# ── 127.250: the defaults (1024 / 1024), no flag ─────────────────────────
# 14/15: the programs above that crossed the OLD defaults build unflagged.
expect "14 130 signatures (120 imported): clean at the default" "$(diags --check s25.tk)" "clean"
expect "15 68 struct types (std.http + t54lib): builds at the default" \
    "$(diags --emit-llvm --out t54d.ll t54.tk)" "clean"
# 16/17: 1023 functions + main = 1024 is at the cap; one more is refused at it.
{ printf 'm=df;\n'; for i in $(seq 1 1023); do printf 'f=f%d():i64{<%d};\n' "$i" "$i"; done
  printf 'f=main():i64{<0};\n'; } > df.tk
expect "16 1024 functions declared here: clean at the default" "$(diags --check df.tk)" "clean"
{ printf 'm=df2;\n'; for i in $(seq 1 1024); do printf 'f=f%d():i64{<%d};\n' "$i" "$i"; done
  printf 'f=main():i64{<0};\n'; } > df2.tk
expect "17 1025 functions: refused at the default, at the 1025th (line 1026)" \
    "$(diags --check df2.tk)" \
    "E9010|1026|1024|1025|raise the limit: pass --max-funcs=1025, or set max_funcs = 1025 in tkc.toml"
# 18: 1022 types + the 2 built-ins is at the cap; the 1023rd type crosses it.
{ printf 'm=ds;\n'; for i in $(seq 1 1023); do printf 't=$r%d{a:i64};\n' "$i"; done
  printf 'f=main():i64{<0};\n'; } > ds.tk
expect "18 1023 types + 2 built-ins: refused at the default, at the last type (line 1024)" \
    "$(diags --check ds.tk)" \
    "E9010|1024|1024|1025|raise the limit: pass --max-structs=1025, or set max_structs = 1025 in tkc.toml"

echo "C480: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
