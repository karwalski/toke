#!/usr/bin/env bash
# C301_large_string_constants.sh — a module whose string constants exceed
# 64 KB, or one literal of 300,000 characters, is emitted whole (story
# 138.14, loke T-35).
#
# THE DEFECT. String constants were buffered in a fixed 64 KB array
# (TKC_STR_GLOBALS_SIZE) whose append "silently truncated": with 670
# constants of 38 characters the module was cut inside the last constant,
# and a 300,000-character literal was emitted as `[65506 x i8]` against a
# declared `[300001 x i8]`.  toke exited 0 and clang rejected the IR.  The
# buffer grows now (llvm.c str_buf_append); an allocation failure is E9010.
#
# Negative control: TKC built from main at c15dd11 — cases 1 and 2 red
# (clang "constant expression type mismatch", build fails); case 3 (669
# constants, under the old limit) green on both.
#
# Story: 138.14

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
run() { rm -f "${WORK}/b"; "${TKC}" "$1" --out "${WORK}/b" >/dev/null 2>&1 || { echo "build-failed"; return; }
        "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
# many N: N string constants of 38 characters, the program prints the count
# and the last one
many() {
    python3 - "$1" "$2" <<'PY'
import sys
n, out = int(sys.argv[1]), sys.argv[2]
items = ";".join('"value-%05d-padding-to-make-it-longer"' % i for i in range(n))
open(out, "w").write('m=g;\ni=io:std.io;\nf=main():i64{\n  let a=@(%s);\n'
                     '  io.println("\\(a.len) \\(a.get(%d))");\n  <0\n};\n' % (items, n - 1))
PY
}
many 670 "${WORK}/m670.tk"
expect "1-670-constants" "$(run "${WORK}/m670.tk")" "670 value-00669-padding-to-make-it-longer "

python3 - "${WORK}/big.tk" <<'PY'
import sys
s = "".join("abcdefghij"[i % 10] for i in range(300000))
open(sys.argv[1], "w").write('m=g;\ni=s:std.str;\ni=io:std.io;\nf=main():i64{\n'
                             '  let b="%s";\n  io.println("\\(s.len(b)) \\(s.slice(b;299995;300000))");\n  <0\n};\n' % s)
PY
expect "2-300k-literal" "$(run "${WORK}/big.tk")" "300000 fghij "

many 669 "${WORK}/m669.tk"
expect "3-669-control" "$(run "${WORK}/m669.tk")" "669 value-00668-padding-to-make-it-longer "

echo "C301: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
