#!/usr/bin/env bash
# C713_typed_empty_forms_ratified.sh — every typed-empty literal form that
# semantics.md §2.1 and grammar.ebnf ratify compiles to an EMPTY collection of
# the named type, and the forms they exclude are E4033 (story 127.227 item 3).
#
# THE GAP. `@($t)` and `@($k:$v)` already compiled (C042, C171,
# cg_typed_empty) and ooke depends on them, but neither semantics.md §2.1 nor
# the grammar mentioned them, so the language accepted a literal its own
# specification did not define. Owner decision 2026-10-01 §2.4: ratify them.
# The spec now describes the shipped behaviour; this case pins each form the
# spec text names, so the two cannot drift apart again.
#
# WHAT IS ASSERTED (one line per spec bullet):
#   1. `@($t)` — primitive (`$i64`, `$f64`, `$bool`), `$str` and a user struct:
#      length 0, and the element type is the named one (an append of that type
#      type-checks and reads back).
#   2. `@($k:$v)` — length 0; a set of that key/value type reads back.
#   3. `@($k:L)` with L typed-empty — `@($str:@($str))` (map to arrays) and
#      `@($str:@($str:$i64))` (map to maps): length 0, a set of the nested type
#      reads back.
#   4. The spec's examples from §2.1 compile as written.
#   5. Excluded: an unsigilled `@(i64)` and a type in another map-entry
#      position `@($str:1)` are E4033.
#   6. The grammar names the typed-empty alternatives (the spec is the other
#      half of this story; a grammar without them is the gap again).
#
# Negative control: the language behaviour predates this story, so items 1-5
# are green on main by design (they pin, they do not fix); item 6 is red on
# main, where grammar.ebnf has no typed-empty alternative.
#
# Story: 127.227

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/c713.XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
codes() {
    "${TKC}" --check "$1" 2>&1 | python3 -c '
import json, sys
out = []
for line in sys.stdin:
    try: out.append(json.loads(line)["error_code"])
    except Exception: pass
print(" ".join(out))'
}

# ── 1. @($t) ─────────────────────────────────────────────────────────────
cat > "${WORK}/a.tk" <<'EOF2'
m=t;i=io:std.io;
t=$pt{x:i64};
f=main():i64{
  let a=mut.@($i64);
  let b=mut.@($f64);
  let c=mut.@($bool);
  let d=mut.@($str);
  let e=mut.@($pt);
  io.println("\(a.len) \(b.len) \(c.len) \(d.len) \(e.len)");
  a=a.append(7);
  b=b.append(1.5);
  c=c.append(true);
  d=d.append("s");
  e=e.append($pt{x:9});
  let p=e.get(0);
  io.println("\(a.get(0)) \(b.get(0)) \(c.len) \(d.get(0)) \(p.x)");
  <0
};
EOF2
expect "1-array-forms" "$(runs "${WORK}/a.tk")" "0 0 0 0 0 7 1.5 1 s 9 "

# ── 2. @($k:$v) ──────────────────────────────────────────────────────────
cat > "${WORK}/m.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let a=mut.@($str:$i64);
  let n0=a.len;
  a=a.set("k";5);
  io.println("\(n0) \(a.len) \(mt a.get("k"){$ok:v v;$err:e 0})");
  <0
};
EOF2
expect "2-map-form" "$(runs "${WORK}/m.tk")" "0 1 5 "

# ── 3. @($k:L), L typed-empty ────────────────────────────────────────────
cat > "${WORK}/n.tk" <<'EOF2'
m=t;i=io:std.io;
f=main():i64{
  let a=mut.@($str:@($str));
  let b=mut.@($str:@($str:$i64));
  io.println("\(a.len) \(b.len)");
  a=a.set("k";@("v"));
  b=b.set("k";@("w":3));
  let av=a.get("k");
  let bv=b.get("k");
  io.println("\(av.get(0)) \(mt bv.get("w"){$ok:v v;$err:e 0})");
  <0
};
EOF2
expect "3-nested-forms" "$(runs "${WORK}/n.tk")" "0 0 v 3 "

# ── 4. the spec's own examples ───────────────────────────────────────────
cat > "${WORK}/s.tk" <<'EOF2'
m=t;
f=main():i64{
  let names=mut.@($str);
  let counts=mut.@($str:$i64);
  let groups=@($str:@($str));
  <names.len as i64+counts.len as i64+groups.len as i64
};
EOF2
"${TKC}" --check "${WORK}/s.tk" >/dev/null 2>&1
expect "4-spec-examples-check-clean" "$?" 0

# ── 5. excluded forms ────────────────────────────────────────────────────
printf 'm=t;\nf=main():i64{\n  let a=@(i64);\n  <0\n};\n' > "${WORK}/e1.tk"
printf 'm=t;\nf=main():i64{\n  let a=@($str:1);\n  <0\n};\n' > "${WORK}/e2.tk"
expect "5a-unsigilled-is-E4033" "$(codes "${WORK}/e1.tk")" "E4033"
expect "5b-type-in-value-position-is-E4033" "$(codes "${WORK}/e2.tk")" "E4033"

# ── 6. the grammar names them ────────────────────────────────────────────
G="${REPO_ROOT}/docs/spec/grammar.ebnf"
expect "6a-grammar-typed-empty-array" \
    "$(grep -c "'@' '(' '\$' IDENT ')'" "$G")" 1
expect "6b-grammar-typed-empty-map" \
    "$(grep -c "'@' '(' '\$' IDENT ':' ( '\$' IDENT | ArrayLit | MapLit ) ')'" "$G")" 1
expect "6c-semantics-names-them" \
    "$(grep -c 'Typed-empty' "${REPO_ROOT}/docs/spec/semantics.md")" 4

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
