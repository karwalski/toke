#!/usr/bin/env bash
# C1461_map_getor_fn_values.sh -- `m.getor(k;d)` on a map of FUNCTION values
# is a function value (story 127.379).
#
# THE DEFECT: map_getor_type adopted the map's value type V only for the
# representation-bearing kinds (repr_value_type: str, bool, f64, maps,
# arrays of those), so with `let t=@("i":inc;"d":dbl)` the binding
# `let f=t.getor("d";inc)` was untyped and `f(4)` lowered as its argument:
# it printed 4, not 8.  `mt t.get("d"){$ok:v v;$err:e inc}` printed 8.
#
# NOW: a function-signature V is adopted too, so the binding is a function
# value (fnval_bind_sig) and calls through it are indirect calls, checked as
# a same-module function value is (E4031 / E4026).  The call yields a
# reference its consumer owns (the result retained, the default released --
# 127.383, C1470): case 5 calls closures read out of a map 400000 times.
#
# Negative control: TKC built from fix/z166-imported-fn-types (4ef15404) --
# cases 1-6, 8-10 and 13 red; 7, 11, 12 green on both.
#
# Story: 127.379

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
RUN="${REPO_ROOT}/test/run_test.sh"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

HEAD='i=io:std.io;
f=inc(x:i64):i64{<x+1};
f=dbl(x:i64):i64{<x*2};
f=ap(h:(i64):i64;x:i64):i64{<h(x)};
f=mkadd(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};'
# run NAME BODY -- build and run BODY as main; print its output and rc
run() {
    printf 'm=c1461;\n%s\nf=main():i64{\nlet t=@("i":inc;"d":dbl);\n%s\n<0};\n' "${HEAD}" "$2" > "${WORK}/$1.tk"
    rm -f "${WORK}/$1.bin"
    ( cd "${WORK}" && "${RUN}" 60 "${TKC}" "$1.tk" -o "$1.bin" >/dev/null 2>&1 )
    if [ -x "${WORK}/$1.bin" ]; then "${RUN}" 20 "${WORK}/$1.bin" 2>&1 | tr '\n' ' '; echo "rc=${PIPESTATUS[0]}"
    else echo "no binary"; fi
}
# chk NAME BODY -- --check BODY as main; print the error codes and rc
chk() {
    printf 'm=c1461;\n%s\nf=main():i64{\nlet t=@("i":inc;"d":dbl);\n%s\n<0};\n' "${HEAD}" "$2" > "${WORK}/$1.tk"
    local out rc
    out="$(cd "${WORK}" && "${TKC}" --check "$1.tk" 2>&1)"; rc=$?
    echo "$(printf '%s\n' "${out}" | grep -o '"error_code":"E[0-9]*"' | sed 's/.*"\(E[0-9]*\)"/\1/' | sort -u | tr '\n' ' ')rc=${rc}"
}

expect "1 getor hit (the reported call)" \
    "$(run r1 'let f=t.getor("d";inc); io.println("\(f(4))");')" "8 rc=0"
expect "2 getor miss: the default" \
    "$(run r2 'let f=t.getor("z";inc); io.println("\(f(4))");')" "5 rc=0"
expect "3 closure default" \
    "$(run r3 'let f=t.getor("z";fn(x:i64):i64{<x+100}); io.println("\(f(4))");')" "104 rc=0"
expect "4 mut binding, reassigned from another map" \
    "$(run r4 'let k=10; let u=@("a":fn(x:i64):i64{<x+k}); let w=mut.t.getor("i";dbl); io.println("\(w(4))"); w=u.getor("a";inc); io.println("\(w(4))");')" "5 14 rc=0"
expect "5 capturing closures read in a loop" \
    "$(run r5 'let k=10; let u=@("a":fn(x:i64):i64{<x+k};"m":mkadd(7)); let s=mut.0; lp(let j=0;j<200000;j=j+1){ let g=u.getor("m";inc); let h=u.getor("a";dbl); s=s+g(1)+h(1) }; io.println("\(s)");')" "3800000 rc=0"
expect "6 copied binding" \
    "$(run r6 'let f=t.getor("d";inc); let g=f; io.println("\(g(3)) \(f(3))");')" "6 6 rc=0"
expect "7 passed straight to a function parameter (control)" \
    "$(run r7 'io.println("\(ap(t.getor("d";inc);5))");')" "10 rc=0"

expect "8 str argument through the binding" \
    "$(chk n8 'let f=t.getor("d";inc); let y=f("x");')" "E4031 rc=1"
expect "9 argument count through the binding" \
    "$(chk n9 'let f=t.getor("d";inc); let y=f(1;2);')" "E4026 rc=1"
expect "10 bound to an i64" \
    "$(chk n10 'let n:i64=t.getor("d";inc);')" "E4031 rc=1"
expect "11 the mt t.get form (control)" \
    "$(run r11 'let f=mt t.get("d"){$ok:v v;$err:e inc}; io.println("\(f(4))");')" "8 rc=0"
expect "12 a map of i64 is unchanged (control)" \
    "$(run r12 'let m=@("a":41); let v=m.getor("a";0); io.println("\(v+1)");')" "42 rc=0"

# 13: a map of functions returned by another module, `[str:(i64):i64]`
cat > "${WORK}/a_lib.tk" <<'EOF2'
m=c1461.lib;
f=inc(x:i64):i64{<x+1};
f=dbl(x:i64):i64{<x*2};
f=tbl():@(str:(i64):i64){<@("i":inc;"d":dbl)};
EOF2
cat > "${WORK}/r13.tk" <<'EOF2'
m=c1461.main;
i=io:std.io;
i=l:c1461.lib;
f=sq(x:i64):i64{<x*x};
f=main():i64{let t=l.tbl(); let f=t.getor("d";sq); let g=t.getor("z";sq); io.println("\(f(4)) \(g(4))");<0};
EOF2
rm -f "${WORK}/r13.bin"
( cd "${WORK}" && "${RUN}" 60 "${TKC}" a_lib.tk r13.tk -o r13.bin >/dev/null 2>&1 )
r13="$([ -x "${WORK}/r13.bin" ] && "${RUN}" 20 "${WORK}/r13.bin" 2>&1 | tr '\n' ' ')"
expect "13 imported map of functions" "${r13}" "8 16 "

echo "C1461: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
