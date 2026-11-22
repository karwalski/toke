#!/usr/bin/env bash
# C1121_std_field_interface_spelling.sh -- the std .tki fields that were
# spelled in source syntax are typed (127.336).
#
# THE DEFECT (fix/z126-tki-field-inner-type 7ce76983).
# vecstore.vecentry.embedding "@(f64)", mdns servicerecord / discovered .txt
# "@(str)" and template.tmplvars.entries "@($str:$str)" were spelled in
# source syntax, which the .tki field reader never parses -- it reads the
# interface notation `[T]` / `[K:V]` that ir.c's render_type_node writes.
# `e.embedding.get(1)` printed 2.5's bits, and `tv.entries.len` lowered as
# the ARRAY glue on a map (0 for a two-entry map; getor printed an address).
#
# THE FIX.  The three .tki fields are respelled `[f64]`, `[str]`,
# `[str:str]` (the same types; docs/stdlib keeps the source spelling, as it
# does for every field), check_tki_layout.py rejects a `@`-spelled field,
# and types.c imported_struct_type takes a std `[K:V]` of scalars through
# tki_compound_type.  Layouts checked: TkTmplVarsRec.entries is a toke map
# handle (template_glue.c unpack_map); no glue builds or reads a vecentry or
# an mdns record (vecstore takes the embedding as an argument; mdns_glue.c
# is stubs), so tkc's own literal layout is the only one.
#
# Negative control: C1121_TKC=<other tkc> C1121_ROOT=<its tree>.  At 7ce76983
# e1, p1 and n1 fail; the c* controls pass.
#
# Story: 127.336
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${C1121_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
TKC="${C1121_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1121_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1121: std .tki fields once spelled @(..) are typed (127.336)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# runs name description expected-output  (program after the header on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
    if [ "${got}" = "${want}" ]; then pass "${desc} (${want})"
    else fail "${desc}: expected '${want}', got '${got}'"; fi
}

# ── vecentry.embedding [f64] ────────────────────────────────────────────
runs e1 'vecentry.embedding: read, arithmetic, len' '2.5 2.5 2 ' <<'EOF'
i=vs:std.vecstore;
f=main():i64{let e=$vecentry{id:"a";embedding:@(1.5;2.5);payload:"p";createdat:0};
let x=e.embedding.get(0)+1.0;
io.println("\(e.embedding.get(1)) \(x) \(e.embedding.len)");<0};
EOF

# ── tmplvars.entries [str:str] ──────────────────────────────────────────
runs p1 'tmplvars.entries: len, getor, contains' '2 w true false ' <<'EOF'
i=tpl:std.template;
f=main():i64{let tv=$tmplvars{entries:@("k":"v";"k2":"w")};
io.println("\(tv.entries.len) \(tv.entries.getor("k2";"none")) \(tv.entries.contains("k")) \(tv.entries.contains("zz"))");<0};
EOF

# ── the field is judged as the type it now has ──────────────────────────
printf 'm=n1;i=io:std.io;i=vs:std.vecstore;\nf=main():i64{let e=$vecentry{id:"a";embedding:@(1.5);payload:"p";createdat:0};let x:str=e.embedding;<0};\n' > n1.tk
out="$("${TKC}" --check n1.tk 2>&1)"; rc=$?
if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031".*"expected":"str","got":"@(f64)"'; then
    pass 'let x:str= a vecentry embedding is E4031 (expected str, got @(f64))'
else fail "n1: want exit 1 and E4031 str vs @(f64), got exit ${rc}: ${out}"; fi

# ── controls: right before and stay right ───────────────────────────────
runs c1 'mdns servicerecord.txt [str]' 'b=2 2 ' <<'EOF'
i=md:std.mdns;
f=main():i64{let r=$servicerecord{name:"n";type:"_h._tcp";port:80;txt:@("a=1";"b=2")};
io.println("\(r.txt.get(1)) \(r.txt.len)");<0};
EOF
runs c2 'tpl.render with a tmplvars literal' 'x=v y=w ' <<'EOF'
i=tpl:std.template;
f=main():i64{let tv=$tmplvars{entries:@("k":"v";"k2":"w")};
let o=mt tpl.render(mt tpl.compile("x={{k}} y={{k2}}"){$ok:t t;$err:e <1};tv) {$ok:s s;$err:e "err"};
io.println("\(o)");<0};
EOF

echo "---------------------------------------------------------------------------"
echo "C1121: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
