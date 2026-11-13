#!/usr/bin/env bash
# C1010_if_expr_tails_agree.sh -- the two tails of an expression-`if` must
# have the same type; disagreeing tails are E4011, as disagreeing `mt` arms
# are (story 138.22).
#
# THE DEFECT (c26cbc1b).  NODE_IF_STMT in types.c returned the then-tail's
# type and never compared the else-tail, so
# `let v=if(n>3){ n }el{ "s" }; io.println("v=\(v)")` checked clean, built,
# and printed the string's address as an i64 (v=4306658964), and
# `if(n>3){ n }el{ @() }` was clean too -- while the same disagreement
# between `mt` arms is refused.
#
# THE FIX.  types.c if_tails_agree: spec v0.4 section C, "the two tails must
# have the same type".  Checked only for an `if` whose value is used, only
# between tails that are expressions (or an `el if`, which answers its own
# check), and only when both types are known.  Tolerated as for `mt` (E4011):
# u64 beside i64, and an integer literal tail that fits the other tail's
# integer type (`if(c){ b }el{ 0 }` with b:u8).  No `fix` field.
#
# Negative control: C1010_TKC=<tkc before 138.22> (checked with the 131.103
# corpus pin, toke 1f39c299).  There n1-n9 fail (rc 0)
# and the c* controls pass.
#
# Story: 138.22
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1010_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1010_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1010: expression-if tails must agree (E4011, 138.22)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# rejects name description expected got line  (program body on stdin):
# --check exits 1 with E4011 "if branches ..." expected/got at that line,
# and no diagnostic carries a fix field.
rejects() {
    local name=$1 desc=$2 want_e=$3 want_g=$4 line=$5 out rc
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    out="$("${TKC}" --check "${name}.tk" 2>&1)"; rc=$?
    if [ "${rc}" -eq 1 ] \
       && printf '%s' "${out}" | grep -F '"error_code":"E4011"' \
          | grep -F '"message":"if branches have inconsistent types' \
          | grep -F "\"line\":${line}," \
          | grep -qF "\"expected\":\"${want_e}\",\"got\":\"${want_g}\"" \
       && ! printf '%s' "${out}" | grep -qF '"fix"'; then
        pass "${desc} is E4011 (${want_e} vs ${want_g}, line ${line})"
    else fail "${desc}: want exit 1 and E4011 ${want_e} vs ${want_g} at line ${line}, got exit ${rc}: ${out}"; fi
}

# runs name description expected-output  (program body on stdin): builds clean
# (no E4011) and prints the expected output.
runs() {
    local name=$1 desc=$2 want=$3 got
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
    if [ "${got}" = "${want}" ]; then pass "${desc} (${want})"
    else fail "${desc}: expected '${want}', got '${got}'"; fi
}

# ── disagreeing tails are refused ───────────────────────────────────────
rejects n1 'let-bound i64 vs str (the 138.22 repro)' i64 str 4 <<'EOF'
f=main():i64{
  let n:i64=1;
  let v=if(n>3){ n }el{ "s" };
  io.println("v=\(v)");
  <0
};
EOF

rejects n2 'i64 vs an empty array' i64 '@(?)' 4 <<'EOF'
f=main():i64{
  let n:i64=7;
  let v=if(n>3){ n }el{ @() };
  <0
};
EOF

rejects n3 'f64 vs str' f64 str 4 <<'EOF'
f=main():i64{
  let c:bool=true;
  let v=if(c){ 1.5 }el{ "x" };
  <0
};
EOF

rejects n4 'bool vs an integer literal (no literal widening into bool)' bool i64 4 <<'EOF'
f=main():i64{
  let c:bool=true;
  let w=if(c){ true }el{ 1 };
  <0
};
EOF

rejects n5 'el-if chain, the last two tails disagree' i64 str 4 <<'EOF'
f=main():i64{
  let c:bool=true;
  let u=if(c){ 1 }el if(!c){ 2 }el{ "s" };
  <0
};
EOF

rejects n6 'el-if chain, the first tail disagrees with the agreeing rest' str i64 4 <<'EOF'
f=main():i64{
  let c:bool=true;
  let y=if(c){ "a" }el if(!c){ 2 }el{ 3 };
  <0
};
EOF

rejects n7 'returned if, struct vs struct' p q 4 <<'EOF'
t=$p{x:i64};
t=$q{y:i64};
f=pick(c:bool):$p{<if(c){ $p{x:1} }el{ $q{y:2} }};
f=main():i64{<0};
EOF

rejects n8 'if as a call argument' str i64 5 <<'EOF'
f=h(x:str):bool{ <x=="s" };
f=main():i64{
  let c:bool=false;
  let r=h(if(c){ "s" }el{ 4 });
  <0
};
EOF

rejects n9 'if as the value of an mt arm' i64 str 5 <<'EOF'
t=$s{$a:i64;$b:i64};
f=main():i64{
  let c:bool=false;
  let m=mt $s{$a:1} {$a:n if(c){ 1 }el{ "z" };$b:n 0};
  <m
};
EOF

# ── controls: agreeing, widened, diverging, and discarded tails ─────────
runs c1 'agreeing tails, el-if chain, negative literal' '-1 two ' <<'EOF'
f=main():i64{
  let n:i64=2;
  let q=if(n<0){ 1 }el if(n>1){ -1 }el{ 0 };
  let s=if(n==2){ "two" }el{ "other" };
  io.println("\(q) \(s)");
  <0
};
EOF

runs c2 'u8 beside a fitting literal, u64 beside i64' '7 3 ' <<'EOF'
f=main():i64{
  let c:bool=true;
  let b:u8=7;
  let v=if(c){ b }el{ 0 };
  let s:str="abc";
  let n:i64=2;
  let k=if(c){ s.len }el{ n };
  io.println("\(v) \(k)");
  <0
};
EOF

runs c3 'a diverging branch yields nothing to compare' '1 ' <<'EOF'
f=main():i64{
  let c:bool=true;
  let z=if(c){ 1 }el{ <2 };
  io.println("\(z)");
  <0
};
EOF

runs c4 'statement ifs (discarded value) may end in anything' 'x ' <<'EOF'
t=$s{$a:i64;$b:i64};
f=main():i64{
  let c:bool=false;
  if(c){ io.println("a") }el{ 5 };
  lp(let i=0;i<1;i=i+1){ if(c){ 1 }el{ "s" } };
  mt $s{$a:1} {$a:n if(c){ io.println("y") }el{ 9 };$b:n 0};
  io.println("x");
  <0
};
EOF

echo "---------------------------------------------------------------------------"
echo "C1010: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
