#!/usr/bin/env bash
# C048_str_indexfrom.sh — str.indexfrom(s; sub; start): indexof with a start.
#
# Story: 127.91.
#
# THE GAP.  `str.indexof(s; sub)` takes two arguments and cannot be told
# where to begin, so a scanner written in toke had to slice off the tail
# before every search — allocating and copying the remaining string once per
# token, never freed.  `s.indexof(t; ","; 5)` was E4026 ("the implementation
# takes 2 arguments, the call passes 3").
#
# THE ADDITION.  A new name, not a third parameter, so no existing call
# changes: `str.indexfrom(s; sub; start)` returns the byte offset — from the
# start of `s` — of the first `sub` at or after byte `start`, or -1.  A
# negative start searches from 0; a start past the end finds nothing; a start
# equal to the length finds only "".  The name contains no `_`, so it is
# writeable in the default profile (check-tki-names).
#
# Asserted: each boundary by VALUE, a scanner loop's count, that indexof's
# two-argument form is untouched, and that a wrong arity is still E4026.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c048_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C048: str.indexfrom(s; sub; start)"
echo "--------------------------------------------------"

# at NAME EXPR EXPECTED — print EXPR (an i64) with t="a,b,,cd," in scope.
#   byte:  a0 ,1 b2 ,3 ,4 c5 d6 ,7   (length 8)
at() {
    local name="$1" expr="$2" expected="$3"
    cat > a.tk <<EOF
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let t="a,b,,cd,";
  let r=${expr};
  io.println("\(r)");
  <0
};
EOF
    rm -f a_bin
    if ! "${TKC}" --allow-all --out a_bin a.tk >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | tail -2
        FAIL=$((FAIL + 1)); return
    fi
    local out; out="$(./a_bin 2>&1)"
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name} — ${expected}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected ${expected}, got '${out}'"
        FAIL=$((FAIL + 1))
    fi
}

at 'start 0 = indexof'                 's.indexfrom(t;",";0)'   1
at 'start on a match returns it'       's.indexfrom(t;",";3)'   3
at 'start between matches'             's.indexfrom(t;",";2)'   3
at 'result is from the string start'   's.indexfrom(t;"cd";1)'  5
at 'last match'                        's.indexfrom(t;",";5)'   7
at 'start == length, non-empty sub'    's.indexfrom(t;",";8)'  -1
at 'start past the end'                's.indexfrom(t;",";9)'  -1
at 'start far past the end'            's.indexfrom(t;",";100000)' -1
at 'negative start searches from 0'    's.indexfrom(t;",";0-5)' 1
at 'empty sub at start'                's.indexfrom(t;"";4)'    4
at 'empty sub at length'               's.indexfrom(t;"";8)'    8
at 'empty sub past the end'            's.indexfrom(t;"";9)'   -1
at 'absent sub'                        's.indexfrom(t;"zz";0)' -1
at 'absent after start'                's.indexfrom(t;"a";1)'  -1
at 'indexof two-argument form unchanged' 's.indexof(t;",")'     1

# A scanner: count the fields of a comma list without slicing.
cat > sc.tk <<'EOF'
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let t="x,yy,,zzz,w";
  let n=mut.0;
  let last=mut.0;
  let p=mut.s.indexfrom(t;",";0);
  lp(let k=0;p>=0;k=k+1){
    n=n+1;
    last=p;
    p=s.indexfrom(t;",";p+1)
  };
  io.println("\(n) \(last)");
  <0
};
EOF
# A wrong offset (e.g. relative to `start`) makes this loop never end, so
# the run is bounded: perl's alarm kills it after 10 s.
rm -f sc_bin
scout=""
if "${TKC}" --allow-all --out sc_bin sc.tk >compile.log 2>&1; then
    scout="$(perl -e 'alarm 10; exec @ARGV' ./sc_bin 2>&1)"
fi
if [ "${scout}" = "4 9" ]; then
    echo "  PASS: scanner loop — 4 separators, last at 9"
    PASS=$((PASS + 1))
else
    echo "  FAIL: scanner loop: got '${scout}' (empty = did not build or did not finish in 10 s)"
    sed 's/^/      /' compile.log | tail -2
    FAIL=$((FAIL + 1))
fi

# The arity is still checked in both directions.
for bad in 's.indexfrom(t;",")' 's.indexof(t;",";2)'; do
    cat > bad.tk <<EOF
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let t="a,b";
  let r=${bad};
  io.println("\(r)");
  <0
};
EOF
    out="$("${TKC}" --check bad.tk 2>&1)"
    if printf '%s' "${out}" | grep -q '"error_code":"E4026"'; then
        echo "  PASS: ${bad} — E4026"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${bad}: expected E4026"
        printf '%s\n' "${out}" | head -2 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
done

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
