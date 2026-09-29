#!/usr/bin/env bash
# C531_nil_sum_match_traps.sh — `mt` on a nil sum value traps RT005; it never
# reads the tag through a null pointer (story 121.51 item 2).
#
# THE DEFECT.  llvm.c's sum-tag switch (114.41) loaded slot 0 of the
# scrutinee's box without a null test.  A sum value can be nil: a map miss
# (`m.get("zz")` on an @(str:$shape) map) handed to a `$shape` parameter, or
# an `$err:e` payload the callee never boxed (db.open's glue stores no box)
# handed to a `DbErr` parameter.  At -O0 that is SIGSEGV; at -O2 the
# optimiser treats the undefined load as unreachable, and the db program
# printed "ok" for a failed open -- a silent wrong answer (R2, 121.51).
#
# NOW: the switch tests the box for null first and calls tk_nil_trap, the
# 124.2d field-read guard: "RT005: nil dereference", exit 1, at -O0 and -O2.
#
# Negative control: TKC=<main d3a083b8's toke> -- case 1 exits 139 at both
# levels; case 2 exits 139 at -O0 and prints "ok" rc=0 at -O2.  Case 3 (a
# non-nil sum) is green on both.
#
# Story: 121.51
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
# run SRC OPT: stdout|, then the stderr line, then rc.
run() {
    rm -f b
    "${TKC}" --allow-all "$2" --out b "$1" >/dev/null 2>&1 || { echo "build-failed"; return; }
    ./b > o 2> e; local rc=$?
    echo "$(tr '\n' '|' < o)$(head -1 e) rc=${rc}"
}

cat > m.tk <<'EOF2'
m=m;
i=io:std.io;
t=$shape{$a:i64;$b:i64};
f=nm(s:$shape):str{ <mt s {$a:x "a"; $b:y "b"} };
f=main():i64{
  let m=@("k":$shape{$b:2});
  io.println(nm(m.get("k")));
  io.println(nm(m.get("zz")));
  <0
};
EOF2
for o in -O0 -O2; do
    expect "1 a map miss matched as a sum ${o}" "$(run m.tk "${o}")" "b|RT005: nil dereference rc=1"
done

cat > d.tk <<'EOF2'
m=d;
i=io:std.io;
i=db:std.db;
f=why(e:DbErr):str{ <mt e {$connection:m "conn"; $query:m "q"; $notfound:m "nf"; $constraint:m "c"} };
f=main():i64{
  let r=mt db.open("/nonexistent-c531/dir/x.db") { $ok:h "ok"; $err:e why(e) };
  io.println(r);
  <0
};
EOF2
for o in -O0 -O2; do
    out="$(run d.tk "${o}")"
    # If db.open's glue ever boxes its error, `e` is no longer nil and the
    # case names a variant instead; either is correct, "ok" never is.
    case "${out}" in
        "RT005: nil dereference rc=1"|"conn| rc=0") out="trap-or-variant" ;;
    esac
    expect "2 an unboxed \$err payload matched as DbErr ${o}" "${out}" "trap-or-variant"
done

cat > c.tk <<'EOF2'
m=c;
i=io:std.io;
t=$shape{$a:i64;$b:i64;$c:str};
f=nm(s:$shape):str{ <mt s {$a:x "a\(x)"; $b:y "b\(y)"; $c:z "c\(z)"} };
f=main():i64{
  io.println("\(nm($shape{$a:1})) \(nm($shape{$b:2})) \(nm($shape{$c:"z"}))");
  <0
};
EOF2
for o in -O0 -O2; do
    expect "3 control: non-nil sums ${o}" "$(run c.tk "${o}")" "a1 b2 cz| rc=0"
done

echo "C531: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
