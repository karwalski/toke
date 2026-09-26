#!/usr/bin/env bash
# C170_map_struct_field_receiver.sh — a method call whose receiver is a
# struct FIELD of map type dispatches to the map glue (story 127.226).
#
# THE DEFECT.  The checker never inferred the receiver of a method call, so a
# field receiver (`c.entries.set(k;v)`) reached the emitter with no type, and
# recv_is_map — which reads that type — answered "not a map".  `.set` lowered
# to tk_array_set_w: a silent no-op until 25e55f5 (127.217) made an
# out-of-range array store trap RT003, so ooke's template cache never stored
# an entry, and then every ooke template test aborted.  `.keys()` on a field
# linked against a nonexistent `_keys`.  Across a module boundary the field of
# an imported struct was untyped for the same reason one level down: the
# checker read only scalar field spellings out of the .tki, so `[str:[tok]]`
# was TY_UNKNOWN and `.len` / `.contains` read the map as an array.
#
# Negative control: TKC built from main at 72d3d85 — sections 1, 2, 4 and 5
# red (RT003 on the first field `.set`; E9003 undefined `_keys`; cross-module
# `0 false`); section 3 (a `let` re-bind) was already green and stays as a
# guard.
#
# Story: 127.226

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
runs() { "${TKC}" ${2:-} "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
t=$tok{v:i64};
t=$cache{entries:@(str:@$tok);hits:i64};
f=main():i64{
  let c=$cache{entries:@("seed":@($tok{v:1}));hits:0};
  let e=c.entries.set("k";@($tok{v:7}));
  let c2=$cache{entries:e;hits:1};
  io.println("\(e.len) \(c.entries.len) \(c2.entries.get("k").get(0).v) \(c2.entries.len)");
  io.println("\(c2.entries.contains("k")) \(c2.entries.contains("zz"))");
  <0
};
EOF
expect "1-field-set-get-len-contains-O2" "$(runs "${WORK}/p1.tk")"     "2 1 7 2 true false "
expect "1-field-set-get-len-contains-O0" "$(runs "${WORK}/p1.tk" -O0)" "2 1 7 2 true false "

cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
t=$bag{idx:@(str:i64)};
f=main():i64{
  let b=$bag{idx:@("a":1;"b":2)};
  let ks=b.idx.keys();
  io.println("\(ks.len)");
  <0
};
EOF
expect "2-field-keys-call" "$(runs "${WORK}/p2.tk")" "2 "

cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
t=$bag{idx:@(str:i64)};
f=main():i64{
  let b=$bag{idx:@("a":1)};
  let m=b.idx;
  let m2=m.set("j";3);
  io.println("\(m2.len) \(m2.contains("j")) \(m2.get("j")) \(b.idx.len)");
  <0
};
EOF
expect "3-rebind-through-let" "$(runs "${WORK}/p3.tk")" "2 true 3 1 "

cat > "${WORK}/p4.tk" <<'EOF'
m=t;i=io:std.io;
t=$bag{idx:@(str:i64)};
t=$outer{name:str;b:$bag};
f=bump(o:$outer;k:str):$outer{
  <$outer{name:o.name;b:$bag{idx:o.b.idx.set(k;o.b.idx.len as i64)}}
};
f=main():i64{
  let o=$outer{name:"o";b:$bag{idx:@("a":1)}};
  let n=o.b.idx.set("n";9);
  let o2=bump(bump(o;"x");"y");
  io.println("\(n.len) \(n.get("n")) \(o.b.idx.len) \(o2.b.idx.len) \(o2.b.idx.get("y")) \(o2.b.idx.contains("x"))");
  <0
};
EOF
expect "4-nested-struct-and-param" "$(runs "${WORK}/p4.tk")" "2 9 1 3 2 true "

# 5: across a module boundary — the struct is declared in the library.
mkdir -p "${WORK}/mm"
cat > "${WORK}/mm/a_lib.tk" <<'EOF'
m=c170.lib;
t=$tok{v:i64};
t=$cache{entries:@(str:@$tok);hits:i64};
f=newcache():$cache{ <$cache{entries:@("seed":@($tok{v:0}));hits:0} };
f=put(c:$cache;k:str;v:i64):$cache{
  <$cache{entries:c.entries.set(k;@($tok{v:v}));hits:c.hits}
};
f=one(v:i64):@$tok{ <@($tok{v:v}) };
EOF
cat > "${WORK}/mm/b_main.tk" <<'EOF'
m=c170.main;
i=io:std.io;
i=cl:c170.lib;
f=main():i64{
  let c=cl.put(cl.newcache();"a";5);
  io.println("\(c.entries.len) \(c.entries.contains("a"))");
  let e=c.entries.set("b";cl.one(6));
  let ks=c.entries.keys();
  io.println("\(e.len) \(e.contains("b")) \(ks.len)");
  <0
};
EOF
got5="$( ("${TKC}" "${WORK}/mm/a_lib.tk" "${WORK}/mm/b_main.tk" -o "${WORK}/mb" >/dev/null 2>&1 && "${WORK}/mb" 2>/dev/null) | tr '\n' ' ')"
expect "5-across-a-module-boundary" "${got5}" "2 true 3 true 2 "

echo "C170: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
