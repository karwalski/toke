#!/usr/bin/env bash
# C736_str_sealed_len_verify.sh — lengths read from sealed strings are right,
# and TK_STR_VERIFY catches a sealed string written after sealing (story
# 127.170, gate 1 of docs/runtime-abi.md §3.1).
#
# THE CHANGE.  Converted producers (slice, concat, interpolation, the
# builder's build) return strings that carry their byte length in a header;
# the core readers (.len, charat, indexfrom, endswith, trimsuffix, the
# builder's add, concat and interpolation parts) trust it when the side bitmap
# marks the string as sealed (src/stdlib/tk_strhdr.h).  The header must equal
# strlen, and a sealed string must never be written again.
#
# Asserted, all under TK_STR_VERIFY=1 (every trusted read also runs strlen
# and aborts on a mismatch):
#   1. `.len` of a slice result (the design noted no conform case took one),
#      of a concat, of an interpolation, of a builder result, and of a builder
#      result holding a 0 byte (sealed with the C-string length, 2 not 5);
#   2. charat at 0, at len-1 and at len (0: out of range), indexfrom inside
#      and past the end, endswith, trimsuffix — the same values main prints;
#   3. threads: eight `spawn`ed tasks (std.task's worker pool) and main each
#      build 20,000 strings by interpolation, concat, the builder and slice
#      and fold their .len / indexfrom into a sum -- every task's sum equals
#      main's, so no allocation raced another;
#   4. the switch itself, in C against the shipped str.c: a byte planted into
#      a sealed string gives the silent wrong answer with the switch off
#      (len=6 text=abc) and aborts with it on ("header 6 != strlen 3").
#
# Negative control: case 4 with the planted write removed prints len=6
# text=abcdef both ways and does not abort (so the abort is the plant's).

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
[ -x "${TKC}" ] || { echo "FAIL: tkc not found at ${TKC}"; exit 1; }
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c736_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "      expected: $3"; echo "      got:      $2"; FAIL=$((FAIL + 1)); fi
}

echo "C736: sealed-string lengths under TK_STR_VERIFY=1 (127.170 gate 1)"
echo "--------------------------------------------------"

cat > "${WORK}/a.tk" <<'EOF'
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let a="abcdefgh";
  let sl=mt a.slice(1;5){$ok:v v;$err:e ""};
  let cat=s.concat(sl;"XY");
  let ip="<\(sl)|\(cat)>";
  let b=s.builder();
  s.add(b;ip);
  s.addbyte(b;33);
  let bt=s.build(b);
  let z=s.builder();
  s.add(z;"ab");
  s.addbyte(z;0);
  s.add(z;"cd");
  let zt=s.build(z);
  let n=bt.len;
  let f1=s.indexfrom(bt;"|";2);
  let f2=s.indexfrom(bt;"|";n+1);
  let e=s.endswith(bt;"Y>!");
  let tr=s.trimsuffix(bt;">!");
  io.println("\(sl.len) \(cat.len) \(ip.len) \(n) \(zt.len)");
  io.println("\(bt.charat(0)) \(bt.charat(n-1)) \(bt.charat(n)) \(f1) \(f2)");
  io.println(bt);
  io.println("\(e) \(tr) \(tr.len)");
  <0
};
EOF
if "${TKC}" --allow-all "${WORK}/a.tk" --out "${WORK}/a" >"${WORK}/c.log" 2>&1; then
    out="$(TK_STR_VERIFY=1 "${WORK}/a" 2>&1)"; rc=$?
    expect "runs to completion with TK_STR_VERIFY=1 (no abort)" "${rc}" "0"
    expect ".len of slice / concat / interpolation / build / build-with-a-0-byte" \
        "$(echo "${out}" | sed -n 1p)" "4 6 13 14 2"
    expect "charat 0, len-1, len; indexfrom inside, past the end" \
        "$(echo "${out}" | sed -n 2p)" "60 33 0 5 -1"
    expect "the built string's bytes" "$(echo "${out}" | sed -n 3p)" "<bcde|bcdeXY>!"
    expect "endswith, trimsuffix and its .len" "$(echo "${out}" | sed -n 4p)" "true <bcde|bcdeXY 12"
else
    echo "  FAIL: compile"; sed 's/^/      /' "${WORK}/c.log" | head -5; FAIL=$((FAIL + 1))
fi

# 3. Threads: sealed strings produced concurrently by spawned tasks.
cat > "${WORK}/th.tk" <<'EOF'
m=main;
i=io:std.io;
i=s:std.str;
i=task:std.task;
f=work():i64{
  let tot=mut.0;
  lp(let i=0;i<20000;i=i+1){
    let a="w\(i)-\(i*7)";
    let b=s.concat(a;"|");
    let bl=s.builder();
    s.add(bl;b);
    s.add(bl;a);
    let c=s.build(bl);
    let d=c.slice(1;c.len-1);
    tot=tot+a.len+b.len+c.len+d.len+s.indexfrom(c;"|";0)
  };
  <tot
};
f=main():i64{
  let h=mut.@(0;0;0;0;0;0;0;0);
  sc{
    h=h.set(0;spawn &work);
    h=h.set(1;spawn &work);
    h=h.set(2;spawn &work);
    h=h.set(3;spawn &work);
    h=h.set(4;spawn &work);
    h=h.set(5;spawn &work);
    h=h.set(6;spawn &work);
    h=h.set(7;spawn &work)
  };
  let tot=mut.0;
  lp(let k=0;k<8;k=k+1){tot=tot+task.result(h.get(k))};
  io.println("\(work()) \(tot)");
  <0
};
EOF
if "${TKC}" --allow-all "${WORK}/th.tk" --out "${WORK}/th" >"${WORK}/c.log" 2>&1; then
    out="$(TK_STR_VERIFY=1 "${WORK}/th" 2>&1)"; rc=$?
    expect "8 spawned tasks + main, TK_STR_VERIFY=1: each sum 1651105, total 8x" "${rc} ${out}" "0 1651105 13208840"
else
    echo "  FAIL: compile (threads)"; sed 's/^/      /' "${WORK}/c.log" | head -5; FAIL=$((FAIL + 1))
fi

# 4. The switch, against the shipped str.c.
cat > "${WORK}/plant.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "tk_strhdr.h"
int main(int argc, char **argv) {
    char *s = tks_dup("abcdef", 6);
    if (argc > 1 && !strcmp(argv[1], "plant")) s[3] = '\0';   /* the forbidden write */
    printf("len=%zu text=%s\n", tk_strlen(s), s);
    return 0;
}
EOF
if ${CC:-cc} -std=c99 -D_GNU_SOURCE -O1 -I"${STDLIB_DIR}" -o "${WORK}/plant" \
        "${WORK}/plant.c" "${STDLIB_DIR}/str.c" >"${WORK}/cc.log" 2>&1; then
    o="$(env -u TK_STR_VERIFY "${WORK}/plant" plant 2>&1)"; rc=$?
    expect "switch off: the planted write gives the silent wrong answer" "${rc} ${o}" "0 len=6 text=abc"
    o="$(TK_STR_VERIFY=1 "${WORK}/plant" plant 2>&1)"; rc=$?
    case "${o}" in
        *"TK_STR_VERIFY: header 6 != strlen 3"*) [ "${rc}" -ne 0 ] && v=abort || v="no-abort rc=0" ;;
        *) v="no message: ${o}" ;;
    esac
    expect "switch on: the same write aborts (header 6 != strlen 3)" "${v}" "abort"
    o="$(TK_STR_VERIFY=1 "${WORK}/plant" 2>&1)"; rc=$?
    expect "control: no plant, switch on -> no abort, right answer" "${rc} ${o}" "0 len=6 text=abcdef"
else
    echo "  FAIL: could not build the switch probe against ${STDLIB_DIR}/str.c"
    sed 's/^/      /' "${WORK}/cc.log" | head -5; FAIL=$((FAIL + 1))
fi

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
