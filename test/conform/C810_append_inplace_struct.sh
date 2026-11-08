#!/usr/bin/env bash
# C810_append_inplace_struct.sh — a linear array accumulator keeps its
# in-place append when its only escape is a struct literal that is returned,
# or bound at the top level of the function body with the accumulator dead
# afterwards (127.307); every case where an alias of the old block would be
# observable keeps the copying append.
#
# Before 127.307 `<$s{d:o;n:n}` after `lp(..){o=o.append(v)}` lowered every
# append to the copying tk_array_append_w (O(N^2) time and, since discarded
# copies are never freed, O(N^2) memory: 1 GB peak RSS at N=20000), while the
# same loop ending `<o` stayed in place.  A struct-literal field is a move of
# the block, not an alias, when the accumulator is never used again.
#
# Checks:
#   1. IR (--emit-llvm): in-place (and no copying) appends for a returned
#      struct literal, a nested one, a return inside the loop, a top-level
#      `let r=$s{..}` and a top-level reassignment `r=$s{..}`.
#   2. IR: COPYING appends where the old block stays observable or the
#      analysis cannot prove the accumulator dead: mutated after the store,
#      stored inside the loop, stored in a nested block then mutated, passed
#      to a function that wraps its parameter in a struct, stored twice.
#   3. Correctness: every case prints the expected values (the stored struct
#      never sees a later set/append of the accumulator; aftermut does the
#      set FIRST, so an in-place lowering there would rewrite r.d[0] -- the
#      negative control that showed this printed aftermut=45555).
#
# The allocation bound lives in C811.  Story: 127.307

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc not found at ${TKC}"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c810.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

# count <ll> <fn-symbol>  ->  "<inplace> <copying>" appends in that function
count() {
    awk -v fn="@$2(" 'index($0, "define ") == 1 && index($0, fn) {f=1}
        f && /call i64 @tk_array_append_inplace_w/ {i++}
        f && /call i64 @tk_array_append_w/ {c++}
        f && /^}/ {f=0}
        END {printf "%d %d", i, c}' "$1"
}

cat > "${WORK}/p.tk" <<'EOF'
m=main;
i=io:std.io;
t=$s{d:@i64;n:i64};
t=$w{s:$s;k:i64};
f=retlit(n:i64):$s{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  <$s{d:o;n:o.len}
};
f=stored(n:i64):$s{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  let r=$s{d:o;n:n};
  <r
};
f=nested(n:i64):$w{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  <$w{s:$s{d:o;n:n};k:7}
};
f=early(n:i64):$s{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){
    o=o.append(i+10);
    if(i==2){<$s{d:o;n:i}}
  };
  <$s{d:o;n:n}
};
f=reassign(n:i64):$s{
  let r=mut.$s{d:@();n:0};
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  r=$s{d:o;n:n};
  <r
};
f=aftermut(n:i64):i64{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  let r=$s{d:o;n:0};
  o=o.set(0;55);
  o=o.append(99);
  <r.d.len*10000+r.d.get(0)*100+o.len*10+o.get(0)/11
};
f=inloop(n:i64):i64{
  let last=mut.$s{d:@();n:0};
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10);last=$s{d:o;n:i}};
  o=o.append(5);
  <last.d.len*10+o.len
};
f=inif(n:i64):i64{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  if(n>0){let r=$s{d:o;n:n};o=o.append(1);<r.d.len*10+o.len};
  <0
};
f=wrap(a:@i64):$s{<$s{d:a;n:0}};
f=viaparam(n:i64):i64{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  let w=wrap(o);
  o=o.append(5);
  <w.d.len*10+o.len
};
f=twice(n:i64):i64{
  let o:@i64=mut.@();
  lp(let i=0;i<n;i=i+1){o=o.append(i+10)};
  let a=$s{d:o;n:1};
  let b=$s{d:o;n:2};
  <a.d.len*10+b.d.len
};
f=main():i64{
  let a=retlit(4);
  let b=stored(4);
  let c=nested(4);
  let d=early(4);
  let e=reassign(4);
  io.println("retlit=\(a.n as $str),\(a.d.get(3) as $str)");
  io.println("stored=\(b.d.len as $str),\(b.d.get(3) as $str)");
  io.println("nested=\(c.s.d.len as $str),\(c.k as $str)");
  io.println("early=\(d.n as $str),\(d.d.len as $str)");
  io.println("reassign=\(e.d.len as $str),\(e.d.get(0) as $str)");
  io.println("aftermut=\(aftermut(4) as $str)");
  io.println("inloop=\(inloop(4) as $str)");
  io.println("inif=\(inif(4) as $str)");
  io.println("viaparam=\(viaparam(4) as $str)");
  io.println("twice=\(twice(4) as $str)");
  <0
};
EOF

if ! "${TKC}" --emit-llvm "${WORK}/p.tk" -o "${WORK}/p.ll" > "${WORK}/ll.log" 2>&1; then
    bad "--emit-llvm failed: $(head -c 300 "${WORK}/ll.log")"
else
    # 1. the accumulator is dead once the struct holds it: in place
    for fn in retlit stored nested early reassign; do
        got="$(count "${WORK}/p.ll" "main_${fn}")"
        if [ "$got" = "1 0" ]; then ok "IR ${fn}: appends inplace/copying = ${got}"
        else bad "IR ${fn}: appends inplace/copying = ${got}, want 1 0"; fi
    done
    # 2. an alias of the old block could be observed (or is not proven
    #    unobservable): copying.  aftermut/inloop/inif/viaparam have a second
    #    append after the struct is built.
    for spec in aftermut:"0 2" inloop:"0 2" inif:"0 2" viaparam:"0 2" twice:"0 1"; do
        fn="${spec%%:*}"; want="${spec#*:}"
        got="$(count "${WORK}/p.ll" "main_${fn}")"
        if [ "$got" = "$want" ]; then ok "IR ${fn}: appends inplace/copying = ${got} (alias kept safe)"
        else bad "IR ${fn}: appends inplace/copying = ${got}, want ${want}"; fi
    done
fi

# 3. correctness
if ! "${TKC}" "${WORK}/p.tk" -o "${WORK}/p" > "${WORK}/build.log" 2>&1; then
    bad "build failed: $(head -c 300 "${WORK}/build.log")"
else
    out="$("${WORK}/p" 2>&1)"
    # aftermut: r keeps len 4 and d[0]=10; o is len 5 with o[0]=55
    # inloop/inif/viaparam: the struct keeps len 4, o grows to 5
    want="retlit=4,13
stored=4,13
nested=4,7
early=2,3
reassign=4,10
aftermut=41055
inloop=45
inif=45
viaparam=45
twice=44"
    if [ "$out" = "$want" ]; then ok "correctness: 10 cases print the expected values"
    else bad "correctness: got
${out}
want
${want}"; fi
fi

echo "C810: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
