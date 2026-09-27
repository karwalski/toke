#!/usr/bin/env bash
# C183_no_macro_generated_wrappers.sh — every `tk_*_w` stdlib wrapper is
# written out where the generators can see it (story 135.19 item 6).
#
# THE DEFECT.  gen_stdlib_decls.py, gen_tki.py and check_tki_coverage.py find
# a wrapper by its `int64_t tk_..._w(` definition line.  image_glue.c stamped
# out image.tograyscale / fliph / flipv through a TK_IMG_XFORM macro, so all
# three were absent from src/stdlib_decls_gen.h, quarantined in
# check_tki_skiplist.txt as "no _w glue", and reached programs only through
# the auto-declare fallback.  They are now written out and the quarantine
# entries are gone.
#
# Section 1 is the class gate: it preprocesses every src/stdlib/*.c and fails
# on any `tk_*_w` definition that exists after the preprocessor but not in
# the source text — a macro-generated wrapper, wherever it appears.
#
# Negative control: src/stdlib from main at 7c16729 — section 1 names
# tk_image_fliph_w, tk_image_flipv_w and tk_image_tograyscale_w; section 2
# finds none of the three rows.
#
# Story: 135.19

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
SRC_ROOT="${SRC_ROOT:-${REPO_ROOT}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# 1. no wrapper is defined only after preprocessing
cat > "${WORK}/scan.py" <<'EOF'
import pathlib, re, subprocess, sys
root = pathlib.Path(sys.argv[1]); src = root / "src" / "stdlib"
DEF = re.compile(r'\b(tk_[A-Za-z0-9_]+_w)\s*\([^;{)]*\)\s*\{')
out = []
for c in sorted(src.glob("*.c")):
    raw = set(DEF.findall(c.read_text(errors="replace")))
    r = subprocess.run(["cc", "-std=c99", "-D_GNU_SOURCE", "-E", "-P",
                        "-I/opt/homebrew/include", "-I", str(src),
                        "-I", str(root / "stdlib/vendor/cmark/src"),
                        "-I", str(root / "stdlib/vendor/tomlc99"), str(c)],
                       capture_output=True, text=True, errors="replace")
    if r.returncode:
        out.append("UNPREPROCESSABLE:" + c.name); continue
    out += sorted(set(DEF.findall(r.stdout)) - raw)
print(" ".join(out) if out else "none")
EOF
expect "1-no-macro-generated-wrapper" "$(python3 "${WORK}/scan.py" "${SRC_ROOT}")" "none"

# 2. the generated declaration table carries the three
rows="$(grep -cE '"tk_image_(fliph|flipv|tograyscale)_w"' "${SRC_ROOT}/src/stdlib_decls_gen.h")"
expect "2-decl-rows-generated" "${rows}" "3"

# 3. and they still do what they say
cat > "${WORK}/fl.tk" <<'EOF'
m=t;i=image:std.image;i=io:std.io;
f=main():i64{
  let a:@byte=@(1;2;3;4;5;6);
  let img=image.fromraw(a;3;2;1);
  let h=image.fliph(img);
  let v=image.flipv(img);
  let d1=h.data; let d2=v.data;
  io.println("\(d1.get(0)) \(d1.get(2)) \(d1.get(3)) \(d2.get(0)) \(d2.get(3))");
  let rgb:@byte=@(255;0;0;0;0;255);
  let g=image.tograyscale(image.fromraw(rgb;2;1;3));
  io.println("\(g.channels) \(g.data.len)");
  <0
};
EOF
if "${TKC}" "${WORK}/fl.tk" -o "${WORK}/fl" >/dev/null 2>&1; then
    expect "3-flip-and-gray-behave" "$("${WORK}/fl" | tr '\n' ' ')" "3 1 6 4 1 1 2 "
else
    expect "3-compiles" "compile-fail" "compiles"
fi

echo "C183: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
