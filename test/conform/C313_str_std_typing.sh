#!/usr/bin/env bash
# C313_str_std_typing.sh — std.str / std typing and glue: strings print as
# strings, compare orders, pads pad, a byte code or an integer where a str is
# declared no longer crashes, std.homedir links on its own (story 138.11;
# loke T-41, T-21, T-36, T-12, and T-28 of 138.13).
#
# THE DEFECTS.
#   T-41  str.frombool / str.replaceall were declared `-> i64` in the
#         generated ("gen":"abi") block, so interpolating them printed the
#         string's address.  gen_stdlib_decls.scan_str_returns() (127.220)
#         could not see a string through `b ? "true" : "false"` nor through
#         `return s;` of a parameter the body reads as a C string.
#   T-21  std.homedir had no interface (no stdlib/std.tki) -- it printed an
#         address -- and `i=std:std;` linked nothing, so it failed to link
#         unless std.process was imported too.
#   T-36  str.compare returned `equal ? 0 : 1`; padleft/padright padded with
#         the low byte of a str pad's address; str.tof32(12345) dereferenced
#         address 12345 (rc 139).
#   T-12  json.getregulations was a stub that always returned an empty array.
#   T-28  str.isdigit(str.charat(s;i)) dereferenced a byte code (SIGSEGV).
#
# NOT ASSERTED HERE, because they are the checker's (src/types.c) and not
# this suite's: that `str.nowunix()/86400` (T-47b), `str.tof32(12345)`,
# `str.isdigit(<i64>)` and a str pad are REJECTED at --check.  The .tki now
# declares them (str.nowunix -> str, tof32 (str), isdigit (str), pad i64);
# the checker does not yet enforce a .tki param/return type on these calls.
#
# Negative control: C313_STDLIB=<dir>/src/stdlib builds against another tree
# (the compiler reads that tree's <dir>/stdlib/*.tki too).  Measured
# 2026-09-27 against a copy with str.compare's old body and str.frombool's
# `i64` return restored: 2 of 7 fail (t41 prints two addresses, cmp is
# "ab=1 ba=1").
#
# Story: 138.11
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C313_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c313_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C313: std.str / std typing and glue (138.11)"
echo "---------------------------------------------"

run_tk() {   # name expected-output
    local name=$1 want=$2 got
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" --allow-all --out "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        echo "  FAIL: ${name}.tk did not compile"; sed 's/^/        /' "${name}.log" | head -4; FAIL=$((FAIL + 1)); return
    fi
    got="$(HOME=/h/c313 ./"${name}" 2>&1)"; local rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
        echo "  PASS: ${name}: ${want}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name} (rc=${rc}): expected '${want}'"; echo "        got '${got}'"; FAIL=$((FAIL + 1))
    fi
}

cat > t41.tk <<'TOKE'
m=t41;i=io:std.io;i=str:std.str;
f=main():i64{
  io.println("frombool=[\(str.frombool(1))] [\(str.frombool(0))] replaceall=[\(str.replaceall("aXa";"X";"Y"))] sub=[\(str.sub("aXa";"X";"Z"))]");
  <0
};
TOKE
run_tk t41 "frombool=[true] [false] replaceall=[aYa] sub=[aZa]"

cat > cmp.tk <<'TOKE'
m=cmp;i=io:std.io;i=str:std.str;
f=main():i64{
  io.println("ab=\(str.compare("a";"b")) ba=\(str.compare("b";"a")) aa=\(str.compare("a";"a")) pre=\(str.compare("ab";"abc"))");
  <0
};
TOKE
run_tk cmp "ab=-1 ba=1 aa=0 pre=-1"

cat > pad.tk <<'TOKE'
m=pad;i=io:std.io;i=str:std.str;
f=main():i64{
  let a:str=str.padleft("7";3;48);
  let b:str=str.padleft("7";3;"0");
  let c:str=str.padright("ab";5;"*");
  let d:str=str.padright("ab";5;42);
  let e:str=str.padleft("x";3;0);
  io.println("[\(a)] [\(b)] [\(c)] [\(d)] [\(e)]");
  <0
};
TOKE
run_tk pad "[007] [007] [ab***] [ab***] [  x]"

cat > crash.tk <<'TOKE'
m=crash;i=io:std.io;i=str:std.str;
f=main():i64{
  let d7:bool=str.isdigit(str.charat("7x";0));
  let dx:bool=str.isdigit(str.charat("7x";1));
  let ds:bool=str.isdigit("123");
  let t=mt str.tof32(12345) { $ok:v "ok"; $err:e "err" };
  let u=mt str.tof32("1.5") { $ok:v "ok \(v)"; $err:e "err" };
  io.println("isdigit7=\(d7) isdigitx=\(dx) isdigit123=\(ds) tof32(int)=\(t) tof32(str)=\(u)");
  <0
};
TOKE
run_tk crash "isdigit7=true isdigitx=false isdigit123=true tof32(int)=err tof32(str)=ok 1.5"

cat > home.tk <<'TOKE'
m=home;i=io:std.io;i=std:std;i=str:std.str;
f=main():i64{
  io.println("[\(std.homedir())/x] len=\(str.len(std.homedir()))");
  <0
};
TOKE
run_tk home "[/h/c313/x] len=7"

cat > regs.tk <<'TOKE'
m=regs;i=io:std.io;i=json:std.json;
f=main():i64{
  let doc=json.decode("{\"regulations\":[\"gdpr\",\"ccpa\"]}");
  let regs=json.getregulations(doc);
  io.println("regs=\(regs.len)");
  <0
};
TOKE
run_tk regs "regs=2"

# The scanner that types generated returns sees both new shapes.
if python3 - "${REPO_ROOT}" <<'PY'
import sys; sys.path.insert(0, sys.argv[1] + "/scripts")
import gen_stdlib_decls as D
k = D.scan_str_returns()
missing = [s for s in ("tk_str_frombool_w", "tk_str_replaceall_w", "tk_str_replace_w") if s not in k]
wrong = [s for s in ("tk_str_len_w", "tk_str_compare_w", "tk_str_isdigit_w") if s in k]
print("missing", missing, "wrongly-str", wrong)
sys.exit(1 if missing or wrong else 0)
PY
then echo "  PASS: scan_str_returns: ternary-of-literals and returned str parameter are strings"; PASS=$((PASS + 1))
else echo "  FAIL: scan_str_returns classification"; FAIL=$((FAIL + 1)); fi

echo "---------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
