#!/usr/bin/env bash
# C079_wide_structs.sh — a struct with more than 16 fields builds and runs,
# locally and across a module boundary.
#
# Story: 127.190 (reached by ooke's `$ookecfg` at 15 fields).
#
# THE DEFECT.  Code generation kept each struct's field names and types in
# arrays TKC_MAX_PARAMS (16) wide (StructInfo, src/llvm.c).  `--check`
# accepted a 17-field struct -- the checker's own cap is 64 -- and the build
# then failed `E4025 struct 'big' has no field 'f16'`, tagged stage
# type_check, naming a field that exists.  The arrays are now sized per
# struct, so check and build agree.
#
# WHY VALUES.  The high fields are read back and printed, including a str and
# an f64 past slot 16, so a fix that registered the names but lost the field
# types (str printed as a pointer, f64 as bits) would fail.  The cross-module
# case goes through the .tki record loader, the second place the arrays were
# fixed-width.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_widestruct_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C079: structs wider than 16 fields build and run"
echo "-----------------------------------------------"

# wide N — `t=$wide{f0:i64;...;f<N-3>:i64;name:str;ratio:f64}` and a
# constructor mk() that fills field k with k.
fields() {
    local n="$1" i out=""
    for ((i = 0; i < n - 2; i++)); do out="${out}f${i}:i64;"; done
    printf '%sname:str;ratio:f64' "${out}"
}
inits() {
    local n="$1" i out=""
    for ((i = 0; i < n - 2; i++)); do out="${out}f${i}:${i};"; done
    printf '%sname:"n%s";ratio:2.5' "${out}" "${n}"
}

runs() {
    local name="$1" expected="$2"; shift 2
    rm -f bin
    if ! "${TKC}" "$@" -o bin > build.log 2>&1 || [ ! -x bin ]; then
        echo "  FAIL: ${name}: build failed"
        grep -o '"message":"[^"]*"' build.log | head -1 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return
    fi
    local out
    out="$(./bin 2>&1)"
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}'"
        FAIL=$((FAIL + 1))
    fi
}

for n in 17 40; do
    last=$((n - 3))
    cat > "one${n}.tk" <<EOF
m=one;
i=io:std.io;
t=\$wide{$(fields "${n}")};
f=main():i64{let w=\$wide{$(inits "${n}")};io.println("\\(w.f0) \\(w.f${last}) \\(w.name) \\(w.ratio)");<0};
EOF
    runs "${n} fields, one module" "0 ${last} n${n} 2.5" "one${n}.tk"
done

cat > rec.tk <<EOF
m=wide.rec;
t=\$wide{$(fields 40)};
f=mk():\$wide{<\$wide{$(inits 40)}};
EOF
cat > app.tk <<'EOF'
m=wide.app;
i=io:std.io;
i=rec:wide.rec;
f=main():i64{let w=rec.mk();io.println("\(w.f0) \(w.f37) \(w.name) \(w.ratio)");<0};
EOF
runs "40 fields, across a module boundary" "0 37 n40 2.5" rec.tk app.tk

echo ""
echo "C079: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
