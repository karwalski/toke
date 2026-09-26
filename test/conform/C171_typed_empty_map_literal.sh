#!/usr/bin/env bash
# C171_typed_empty_map_literal.sh — `@($k:$v)` in value position is the
# typed EMPTY map, and a type anywhere else in a map literal is E4033
# (story 127.226, closing 127.98 (i)).
#
# THE DEFECT.  The typed-empty ARRAY `@($f64)` has long been an empty array of
# that element type (types.c NODE_ARRAY_LIT, llvm.c "skip type-annotation
# children").  Its map counterpart `@($str:$i64)` — ooke's canonical empty
# template cache, `@($str:@($token))` — was parsed as a ONE-entry map keyed by
# whatever a type node lowers to (an empty string), typed @(void:void).  So
# `.len` answered 1, `.keys()` held a phantom "" key, and 127.98 (i) found "no
# way to declare an empty typed map".  A type mixed with values
# (`@($str:1)`, `@("a":$str)`) checked clean and built that same junk entry.
#
# The form: a map literal with ONE entry whose key is a type and whose value is
# a type or itself a typed-empty literal (`@($t)`, `@($k:$v)`).
#
# Negative control: TKC built from main at 72d3d85 — sections 1-3 red
# (`1 1 1 ...`, a phantom key), 4 red (checks clean, exit 0).
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
# the error codes --check reports, in order, space-separated
codes() {
    "${TKC}" --check "$1" 2>&1 | python3 -c '
import json, sys
out = []
for line in sys.stdin:
    try: out.append(json.loads(line)["error_code"])
    except Exception: pass
print(" ".join(out))'
}

cat > "${WORK}/p1.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let a=mut.@($str:$i64);
  let n0=a.len;
  a=a.set("x";5);
  a=a.set("y";6);
  let ks=a.keys();
  io.println("\(n0) \(a.len) \(a.get("y")) \(ks.len)");
  let s=@($str:$str);
  let s2=s.set("k";"vee");
  io.println("\(s.len) \(s2.len) \(s2.get("k"))");
  <0
};
EOF
expect "1-str-keyed-O2" "$(runs "${WORK}/p1.tk")"     "0 2 6 2 0 1 vee "
expect "1-str-keyed-O0" "$(runs "${WORK}/p1.tk" -O0)" "0 2 6 2 0 1 vee "

cat > "${WORK}/p2.tk" <<'EOF'
m=t;i=io:std.io;
f=main():i64{
  let b=mut.@($i64:$str);
  let n0=b.len;
  b=b.set(3;"three");
  io.println("\(n0) \(b.len) \(b.get(3))");
  <0
};
EOF
expect "2-int-keyed" "$(runs "${WORK}/p2.tk")" "0 1 three "

cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=io:std.io;
t=$tok{v:i64};
t=$cache{entries:@(str:@$tok)};
f=add(m:@(str:i64);k:str):@(str:i64){ <m.set(k;7) };
f=main():i64{
  let c=$cache{entries:@($str:@($tok))};
  let n=c.entries.set("z";@($tok{v:4}));
  let t:@(str:i64)=@($str:$i64);
  let t2=add(t;"q");
  let mm=@($str:@($str:$i64));
  io.println("\(c.entries.len) \(n.len) \(n.get("z").get(0).v) \(t2.len) \(t2.get("q")) \(mm.len)");
  <0
};
EOF
expect "3-nested-typed-empty-and-flow" "$(runs "${WORK}/p3.tk")" "0 1 4 1 7 0 "

# 4: a type mixed with values is not a value — E4033, no fix field.
printf 'm=t;\nf=main():i64{\n  let m=@($str:1);\n  <0\n};\n' > "${WORK}/e1.tk"
printf 'm=t;\nf=main():i64{\n  let m=@("a":$str);\n  <0\n};\n' > "${WORK}/e2.tk"
printf 'm=t;\nf=main():i64{\n  let m=@($str:$str;"a":"b");\n  <0\n};\n' > "${WORK}/e3.tk"
expect "4a-type-as-key"          "$(codes "${WORK}/e1.tk")" "E4033"
expect "4b-type-as-value"        "$(codes "${WORK}/e2.tk")" "E4033"
expect "4c-type-entry-plus-more" "$(codes "${WORK}/e3.tk")" "E4033"
nofix="$("${TKC}" --check "${WORK}/e1.tk" 2>&1 | python3 -c '
import json, sys
ds = [json.loads(l) for l in sys.stdin if l.strip()]
e = [d for d in ds if d.get("error_code") == "E4033"]
print(bool(e) and all("fix" not in d for d in e))')"
expect "4d-E4033-has-no-fix-field" "${nofix}" "True"

echo "C171: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
