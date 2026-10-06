#!/usr/bin/env bash
# C716_chained_get_map_of_arrays.sh — `.get` chained directly on a map-get
# (or array-get) result builds and yields the element, for str, i64, f64 and
# nested-array element types (story 127.274).
#
# THE BUG. `a.get("k").get(0)` on a map of arrays (`@($str:@($str))`, or a
# literal `@("z":@("q"))`) passed --check but failed at clang with E9003
# "'%tN' defined with type 'ptr' but expected 'i64'". The index lowering in
# emit_expr converts the loaded element on its resolved type (n->rtype) for
# any base, so a str element became an i8* and an f64 element a double; but
# expr_llvm_type answered "i64" for every NESTED subscript (base is itself an
# index — the 126.5 guard), so each consumer (a call argument, a `let`, an
# interpolation) declared the value i64. Binding the inner array to a `let`
# first made the subscript non-nested, which is why that workaround worked.
#
# WHAT IS ASSERTED
#   1. str element: printed directly, interpolated, compared with `==`, and
#      bound with `let`; on a literal map and on a typed-empty
#      `@($str:@($str))` that was `set`.
#   2. i64 element: arithmetic, interpolation, numeric comparison.
#   3. f64 element: arithmetic in an interpolation.
#   4. nested-array element (map of arrays of arrays): str and i64 leaves, and
#      `.len` of an inner array.
#   5. map receivers in the chain: array of maps, map of maps (str, f64,
#      i64 values). On main the f64 map-of-maps value printed its bit pattern
#      as an integer (4.61281e+18 for 3.5) — wrong output, not an error.
#   6. 126.5 still holds: a nested int subscript built from untyped `mut.@()`
#      rows compares numerically (the guard this story removes was its fix).
#
# Negative control: items 1, 3 and 4 are red on main (E9003 — item 4 through
# its str leaf), item 5 is red on main (wrong f64 output); items 2 and 6 are
# green on main by design — they pin that the
# fix keeps the i64-strided cases unchanged.
#
# Story: 127.274

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/c716.XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
# Build and run; on a build failure print the error code so the FAIL line
# names it (E9003 is the bug).
runs() {
    local src="$1"; shift
    if ! "${TKC}" "${src}" -o "${WORK}/b" >"${WORK}/err" 2>&1; then
        echo "BUILD-FAILED $(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/err" | head -1)"
        return
    fi
    "${WORK}/b" "$@" 2>/dev/null | tr '\n' ' '
}

# ── 1. str element ───────────────────────────────────────────────────────
cat > "${WORK}/s.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let a=@("z":@("q";"r"));
  io.println(a.get("z").get(1));
  io.println("s=\(a.get("z").get(0))");
  if(a.get("z").get(0)=="q"){io.println("eq")};
  let v=a.get("z").get(1);
  io.println(v);
  let b=mut.@($str:@($str));
  b=b.set("k";@("x";"y"));
  io.println(b.get("k").get(1));
  <0
};
EOF2
expect "1-str-element" "$(runs "${WORK}/s.tk")" "r s=q eq r y "

# ── 2. i64 element ───────────────────────────────────────────────────────
cat > "${WORK}/i.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let n=@("n":@(5;7));
  io.println("\(n.get("n").get(1)+1)");
  io.println("i=\(n.get("n").get(0))");
  if(n.get("n").get(1)>=7){io.println("ge")};
  <0
};
EOF2
expect "2-i64-element" "$(runs "${WORK}/i.tk")" "8 i=5 ge "

# ── 3. f64 element ───────────────────────────────────────────────────────
cat > "${WORK}/d.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let d=@("d":@(1.5;2.5));
  io.println("\(d.get("d").get(1)+1.0)");
  <0
};
EOF2
expect "3-f64-element" "$(runs "${WORK}/d.tk")" "3.5 "

# ── 4. nested-array element ──────────────────────────────────────────────
cat > "${WORK}/g.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let g=@("g":@(@("p";"w");@("o")));
  io.println(g.get("g").get(0).get(1));
  io.println("\(g.get("g").get(1).len)");
  let h=@("h":@(@(3;4)));
  io.println("\(h.get("h").get(0).get(1)*2)");
  <0
};
EOF2
expect "4-nested-array-element" "$(runs "${WORK}/g.tk")" "w 1 8 "

# ── 5. map receivers in the chain ────────────────────────────────────────
cat > "${WORK}/m.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let a=@(@("k":"v"));
  io.println(a.get(0).get("k"));
  let m=@("o":@("i":"w"));
  io.println(m.get("o").get("i"));
  let n=@("o":@("i":2.5));
  io.println("\(n.get("o").get("i")+1.0)");
  let p=@("o":@("i":4));
  io.println("\(p.get("o").get("i")+1)");
  <0
};
EOF2
expect "5-map-receivers" "$(runs "${WORK}/m.tk")" "v w 3.5 5 "

# ── 6. 126.5 still holds ─────────────────────────────────────────────────
cat > "${WORK}/r.tk" <<'EOF2'
m=cr;i=io:std.io;i=s:std.str;f=main():i64{
  let dist=mut.@();
  let rd=mut.@();
  rd=rd+@(s.toint("1"));
  rd=rd+@(s.toint("1000000"));
  dist=dist+@(rd);
  let v=dist.get(0).get(1);
  if(v>=999999){io.println("BIG")}el{io.println("ok")};
  if(dist.get(0).get(0)>=999999){io.println("BIG")}el{io.println("ok")};
  <0
};
EOF2
expect "6-nested-int-compare" "$(runs "${WORK}/r.tk")" "BIG ok "

echo "C716: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
