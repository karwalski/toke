#!/usr/bin/env bash
# C005_toml_false_zero_vs_missing.sh — a toml value of `false` or `0` takes the
# $ok arm; only a genuinely absent key takes $err (story 127.67).
#
# The std.toml accessors return a bare i64, and the default codegen for
#
#     mt toml.bool(cfg; "minify") {$ok:v …;$err:e …}
#
# takes the $ok arm when that i64 is non-zero.  The 0/null sentinel is exact
# for a table handle or a string pointer, which are never 0 on success, and it
# is wrong for a value type whose valid domain includes 0:
#
#     minify = false   -> 0 -> read as an error
#     retries = 0      -> 0 -> read as an error
#
# Callers read "error" as "key absent" and substitute their default, so a key
# that says false was obeyed as true — configuration read as the opposite of
# what it says, which is worse than configuration that fails loudly.  ooke read
# `minify = false` and `inlinecss = false` as true this way.
#
# The cure is the protocol str.toint/str.tofloat already use (114.53/114.54):
# the wrapper keeps @tk_current_error truthful and always returns the real
# value, and the match site discriminates ok/err on that global instead of
# comparing the value to zero.  Two halves, both required:
#
#   1. stdlib — src/stdlib/toml_glue.c sets @tk_current_error on every path
#      (landed in 3771868, pinned by S003).
#   2. codegen — llvm.c's is_num_parse_wrapper() lists tk_toml_bool_w and
#      tk_toml_i64_w, so both the `mt` match site and the `!$err` propagate
#      site read the global rather than the sentinel.  Without this half the
#      wrappers tell the truth and nothing listens.
#
# Only bool and i64 join that list.  toml.load / toml.section / toml.str return
# a handle or a pointer, so their sentinel is already exact, and the str.toint
# family must keep behaving exactly as it did — a regression there would be
# silent, showing up only as an error that stops being reported.  Cases 4 and 5
# below are that guard.
#
# Story: 127.67

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_tomlarm_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C005: toml false/0 reach \$ok; only a missing key reaches \$err"
echo "--------------------------------------"

# run_case NAME EXPECTED_STDOUT SOURCE
run_case() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > case.tk
    local out rc=0
    rm -f case_bin
    if ! "${TKC}" --allow-all --out case_bin case.tk >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | head -6
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(perl -e 'alarm 20; exec @ARGV' ./case_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "      expected: $(printf '%s' "${expected}" | tr '\n' '|')"
        echo "      got:      $(printf '%s' "${out}"      | tr '\n' '|') (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

# ── 1: the three cases that must be told apart, through `mt`. ──
#   false / 0 take $ok and carry the real value; a misspelt key takes $err.
run_case "false, zero and missing are three distinct outcomes" \
    $'minify=ok:0\nretries=ok:0\npretty=ok:1\nlimit=ok:7\nnosuch=err\ngone=err' '
m=t;
i=toml:std.toml;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let src="minify = false\nretries = 0\npretty = true\nlimit = 7\n";
  let cfg=mt toml.load(src){$ok:v v;$err:e <1};
  let a=mt toml.bool(cfg;"minify"){$ok:v io.println(str.concat("minify=ok:";str.fromi64(v)));$err:e io.println("minify=err")};
  let b=mt toml.i64(cfg;"retries"){$ok:v io.println(str.concat("retries=ok:";str.fromi64(v)));$err:e io.println("retries=err")};
  let c=mt toml.bool(cfg;"pretty"){$ok:v io.println(str.concat("pretty=ok:";str.fromi64(v)));$err:e io.println("pretty=err")};
  let d=mt toml.i64(cfg;"limit"){$ok:v io.println(str.concat("limit=ok:";str.fromi64(v)));$err:e io.println("limit=err")};
  let e=mt toml.bool(cfg;"nosuch"){$ok:v io.println("nosuch=ok");$err:e io.println("nosuch=err")};
  let f=mt toml.i64(cfg;"gone"){$ok:v io.println("gone=ok");$err:e io.println("gone=err")};
  <0
};'

# ── 2: the user-visible defect — a default is substituted only when the key
#      is really absent, never when it is present and false. ──
run_case "a false key is obeyed as false, not defaulted to true" \
    $'minify=false\ninlinecss=false\nverbose=true' '
m=t;
i=toml:std.toml;
i=io:std.io;
i=str:std.str;
f=readflag(cfg:i64;key:$str):bool{
  <mt toml.bool(cfg;key){$ok:v v;$err:e true}
};
f=say(name:$str;v:bool):i64{
  if(v){io.println(str.concat(name;"=true"))}el{io.println(str.concat(name;"=false"))};
  <0
};
f=main():i64{
  let src="minify = false\ninlinecss = false\n";
  let cfg=mt toml.load(src){$ok:v v;$err:e <1};
  say("minify";readflag(cfg;"minify"));
  say("inlinecss";readflag(cfg;"inlinecss"));
  say("verbose";readflag(cfg;"verbose"));
  <0
};'

# ── 3: the `!$err` propagate site — is_num_parse_wrapper() is consulted there
#      too, so a 0 must NOT early-return as a propagated error. ──
run_case "a zero does not propagate as an error through !\$err" \
    $'zero=ok:0\nfive=ok:5\nmissing=err' '
m=t;
i=toml:std.toml;
i=io:std.io;
i=str:std.str;
f=retries(src:$str):i64!$err{
  let cfg=toml.load(src)!$err;
  let n=toml.i64(cfg;"retries")!$err;
  <n
};
f=main():i64{
  let a=mt retries("retries = 0\n"){$ok:v io.println(str.concat("zero=ok:";str.fromi64(v)));$err:e io.println("zero=err")};
  let b=mt retries("retries = 5\n"){$ok:v io.println(str.concat("five=ok:";str.fromi64(v)));$err:e io.println("five=err")};
  let c=mt retries("other = 5\n"){$ok:v io.println("missing=ok");$err:e io.println("missing=err")};
  <0
};'

# ── 4: guard — the str.toint/str.tofloat family that already used this
#      protocol (114.53/114.54) must behave identically. ──
run_case "str.toint / str.tofloat unchanged" \
    $'toint0=ok\ntoint42=ok\ntointabc=err\ntofloat0=ok\ntofloatzz=err' '
m=t;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let a=mt str.toint("0"){$ok:v io.println("toint0=ok");$err:e io.println("toint0=err")};
  let b=mt str.toint("42"){$ok:v io.println("toint42=ok");$err:e io.println("toint42=err")};
  let c=mt str.toint("abc"){$ok:v io.println("tointabc=ok");$err:e io.println("tointabc=err")};
  let d=mt str.tofloat("0.0"){$ok:v io.println("tofloat0=ok");$err:e io.println("tofloat0=err")};
  let e=mt str.tofloat("zz"){$ok:v io.println("tofloatzz=ok");$err:e io.println("tofloatzz=err")};
  <0
};'

# ── 5: guard — the toml accessors that were NOT added to the list keep the
#      0/null sentinel, and it is still exact for them. ──
run_case "toml.str / toml.section keep the sentinel" \
    $'server=ok\ntypo=err\nhost=ok\nhostmissing=err\nbadsrc=err' '
m=t;
i=toml:std.toml;
i=io:std.io;
f=main():i64{
  let src="port = 8080\n[server]\nhost = \"localhost\"\n";
  let cfg=mt toml.load(src){$ok:v v;$err:e <1};
  let sec=mt toml.section(cfg;"server"){$ok:v v;$err:e 0};
  let a=mt toml.section(cfg;"server"){$ok:v io.println("server=ok");$err:e io.println("server=err")};
  let b=mt toml.section(cfg;"typo"){$ok:v io.println("typo=ok");$err:e io.println("typo=err")};
  let c=mt toml.str(sec;"host"){$ok:v io.println("host=ok");$err:e io.println("host=err")};
  let d=mt toml.str(sec;"nosuch"){$ok:v io.println("hostmissing=ok");$err:e io.println("hostmissing=err")};
  let e=mt toml.load("this is not = = toml\n"){$ok:v io.println("badsrc=ok");$err:e io.println("badsrc=err")};
  <0
};'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
