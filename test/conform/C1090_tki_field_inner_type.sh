#!/usr/bin/env bash
# C1090_tki_field_inner_type.sh -- a std record field typed as an array of
# str/bool/f64 (to any depth) in its .tki keeps that element type (127.333).
#
# THE DEFECT (10600e5b).  imported_struct_type typed a std record's fields
# from scalar spellings only (127.226 adopted compound spellings for
# generated user interfaces alone), so `httpresp.headers` "[[str]]" was
# TY_UNKNOWN and every read through it fell back to the emitter's i64
# default: inside `f=show(r:$httpresp)`, `let a=r.headers.get(1);
# \(a.get(0))` printed the header name's address, as did `let k=a.get(0)`
# and `\(r.headers.get(1).get(0))`; only `let k2:str=a.get(0)` was right.
# `.len` was right in every form.  An `ml.row.vals` "[f64]" element printed
# the double's bits; a `[bool]` element printed 1/0.
#
# THE FIX.  types.c imported_struct_type: a std field whose spelling is
# `[`^n (str|bool|f64) `]`^n takes repr_payload_type -- the rule the other
# interface edges already use (127.297 / 127.308).  Struct-element arrays,
# [byte] and maps from std stay unknown, as before.
#
# The bool cases need a std record with a [bool] field, and no shipped std
# interface has one, so they run against a mirror of the stdlib directory
# (symlinks) with one extra type-only interface, std.zrepr.
#
# Negative control: C1090_TKC=<other tkc>.  At 10600e5b the r*, f*, b* and
# n1 cases fail; the c* controls pass.
#
# Story: 127.333
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${C1090_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1090_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1090: a std record's [str]/[f64]/[bool] field keeps its element type (127.333)"
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

REQ='let r=http.mockreq("GET";"/";@(@("A";"1");@("Bee";"2"));"";@(@("p";"q")));'

# ── [[str]]: the four forms of the story, through a $req parameter ───────
runs r1 'httpreq [[str]] headers: bound pair, bound name, annotated, chained' \
     '1=[Bee] 2=[Bee] 3=[Bee] 4=[Bee] ' <<EOF
i=http:std.http;
f=show(r:\$req):i64{
  let a=r.headers.get(1);
  io.println("1=[\\(a.get(0))]");
  let k=a.get(0);
  io.println("2=[\\(k)]");
  let k2:str=a.get(0);
  io.println("3=[\\(k2)]");
  io.println("4=[\\(r.headers.get(1).get(0))]");
  <0
};
f=main():i64{${REQ}<show(r)};
EOF
runs r2 'the same field on a local binding, and the params field' 'Bee 2 q ' <<EOF
i=http:std.http;
f=main():i64{${REQ}
  let h=r.headers;let p=r.params.get(0);
  io.println("\\(h.get(1).get(0)) \\(r.headers.get(1).get(1)) \\(p.get(1))");<0};
EOF
runs r3 'another std [[str]] field (dbrow.cols literal) and str ops on it' 'name 4 NAME ' <<'EOF'
i=db:std.db;i=s:std.str;
f=main():i64{let w=$dbrow{cols:@(@("name";"ada"))};let c=w.cols.get(0).get(0);
io.println("\(c) \(c.len) \(s.upper(c))");<0};
EOF

# ── [f64]: ml.row.vals / linearmodel.coef ───────────────────────────────
runs f1 'std [f64] field: in place, bound, arithmetic' '2.5 1.5 2.5 3.5 ' <<'EOF'
i=ml:std.ml;
f=main():i64{let r=$row{vals:@(1.5;2.5)};let v=r.vals;let x=r.vals.get(1);
io.println("\(v.get(1)) \(r.vals.get(0)) \(x) \(x+1.0)");<0};
EOF
runs f2 'std [f64] field through a parameter' '0.5 ' <<'EOF'
i=ml:std.ml;
f=c0(m:$linearmodel):f64{<m.coef.get(0)};
f=main():i64{let m=$linearmodel{coef:@(0.5;1.5);intercept:2.0};io.println("\(c0(m))");<0};
EOF

# ── the field is judged as the type it now has ──────────────────────────
printf 'm=n1;i=io:std.io;i=http:std.http;\nf=main():i64{%slet x:i64=r.headers.get(0).get(0);<x};\n' "${REQ}" > n1.tk
out="$("${TKC}" --check n1.tk 2>&1)"; rc=$?
if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031".*"expected":"i64","got":"str"'; then
    pass 'let x:i64= a [[str]] field element is E4031 (expected i64, got str)'
else fail "n1: want exit 1 and E4031 i64 vs str, got exit ${rc}: ${out}"; fi

# ── controls: right before and stay right ───────────────────────────────
runs c1 '.len in every form' '2 2 2 1 ' <<EOF
i=http:std.http;
f=main():i64{${REQ}let a=r.headers.get(1);
io.println("\\(r.headers.len) \\(a.len) \\(r.headers.get(0).len) \\(r.params.len)");<0};
EOF
runs c2 'a local nested literal' 'cde y ' <<'EOF'
f=main():i64{let h=@(@("ab";"cde");@("x";"y"));let a=h.get(0);
io.println("\(a.get(1)) \(h.get(1).get(1))");<0};
EOF

# ── bool, and deeper nesting, via a mirrored stdlib with std.zrepr ──────
MIR="${WORK}/mir"; mkdir -p "${MIR}/src/stdlib" "${MIR}/stdlib"
for f in "${REPO_ROOT}"/src/stdlib/*; do ln -s "${f}" "${MIR}/src/stdlib/"; done
for f in "${REPO_ROOT}"/stdlib/*;     do ln -s "${f}" "${MIR}/stdlib/"; done
cat > "${MIR}/stdlib/zrepr.tki" <<'EOF'
{"schema_version":"1.0","module":"std.zrepr","exports":[
 {"kind":"type","name":"zflags","fields":[
  {"name":"bits","type":"[bool]"},{"name":"grid","type":"[[bool]]"},
  {"name":"m","type":"[[f64]]"},{"name":"s3","type":"[[[str]]]"}]}
]}
EOF
ZLIT='let r=$zflags{bits:@(true;false);grid:@(@(false;true));m:@(@(1.5;2.5));s3:@(@(@("p";"q")))};'
TKC_STDLIB_DIR="${MIR}/src/stdlib" runs b1 'std [bool] and [[bool]] fields' 'true false true ' <<EOF
i=z:std.zrepr;
f=main():i64{${ZLIT}let g=r.grid.get(0);
io.println("\\(r.bits.get(0)) \\(r.bits.get(1)) \\(g.get(1))");<0};
EOF
TKC_STDLIB_DIR="${MIR}/src/stdlib" runs b2 'std [[f64]] and [[[str]]] fields' '2.5 q q ' <<EOF
i=z:std.zrepr;
f=main():i64{${ZLIT}let t=r.s3.get(0);
io.println("\\(r.m.get(0).get(1)) \\(r.s3.get(0).get(0).get(1)) \\(t.get(0).get(1))");<0};
EOF

echo "---------------------------------------------------------------------------"
echo "C1090: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
