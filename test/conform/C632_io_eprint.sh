#!/usr/bin/env bash
# C632_io_eprint.sh — io.eprint(s) writes s to stderr with no newline added
# (121.70 (1)).
#
# THE GAP.  std.io had print/println on stdout but only eprintln on stderr,
# so a program assembling one stderr line from parts (a prompt, a progress
# display, a diagnostic built piecewise) reached into std.os for
# os.write(os.stderrfd(); s).  io.eprint is the stderr twin of io.print.
#
# Cases (one program: eprint "a", eprint "", eprint "b", eprintln "c",
# print "out"):
#   1 stderr is exactly "abc\n" — no newline after either eprint, and the
#     empty eprint writes nothing
#   2 stdout is exactly "out" — eprint wrote nothing to stdout
#   3 io.eprint(42) is rejected at compile time (E4031, expected 'str'):
#     the export is typed, not an i64 ABI placeholder
#   4 exit status 0
#
# Negative control: C632_STDLIB=<main's src/stdlib at 2aa48c88, stdlib/ beside
# it> — io.eprint is not an export there: E4027 "'io.eprint' has no
# implementation" (measured Darwin 25.6).
#
# Story: 121.70 (1)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C632_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;
f=main():i64{
  io.eprint("a");
  io.eprint("");
  io.eprint("b");
  io.eprintln("c");
  io.print("out");
  <0
};
EOF
if ! "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C632: 0 passed, 1 failed"; exit 1
fi
rc=0; "${WORK}/b" > "${WORK}/out" 2> "${WORK}/err" || rc=$?
printf 'abc\n' > "${WORK}/err.want"; printf 'out' > "${WORK}/out.want"
if cmp -s "${WORK}/err" "${WORK}/err.want"; then expect "1-stderr-exact" ok ok
else expect "1-stderr-exact" "$(od -c "${WORK}/err" | head -2)" "$(od -c "${WORK}/err.want" | head -2)"; fi
if cmp -s "${WORK}/out" "${WORK}/out.want"; then expect "2-stdout-exact" ok ok
else expect "2-stdout-exact" "$(od -c "${WORK}/out" | head -2)" "$(od -c "${WORK}/out.want" | head -2)"; fi
printf 'm=main;i=io:std.io;\nf=main():i64{\n  io.eprint(42);\n  <0\n};\n' > "${WORK}/q.tk"
"${TKC}" --check "${WORK}/q.tk" > "${WORK}/q.log" 2>&1
expect "3-typed-str-param" "$(grep -o '"error_code":"E4031"' "${WORK}/q.log" | head -1)|$(grep -c "expected 'str'" "${WORK}/q.log")" '"error_code":"E4031"|1'
expect "4-exit-0" "${rc}" "0"
echo "C632: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
