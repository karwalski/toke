#!/usr/bin/env bash
# M001_min_literal.sh — `tkc --min` with raw control characters in literals
# (131.34) and multi-file invocation (131.36).
#
# Verifies:
#   1. A program whose string literals contain raw newline/tab/CR (and a raw
#      newline inside a `\(…)` interpolation) minifies to exactly ONE line.
#   2. Round-trip: the original and the minified program compile and print
#      byte-identical output (raw newline == `\n` escape for the compiler).
#   3. --min is idempotent (min(min(p)) == min(p)).
#   4. `tkc --min a.tk b.tk` prints one line per file, in order, does not
#      fall through to the compile pipeline, and leaves no stray binary.
#
# Stories: 131.34, 131.36

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${REPO_ROOT}/tkc"
RUN="${REPO_ROOT}/test/run_test.sh"

PASS=0
FAIL=0

check() {
    local name="$1" expected="$2" actual="$3"
    if [ "${expected}" = "${actual}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "    expected:"; echo "${expected}" | sed 's/^/      /'
        echo "    actual:";   echo "${actual}"   | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

echo "M001: --min string-literal control chars + multi-file"
echo "--------------------------------------"

WORK="$(mktemp -d /tmp/tkc_min_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

# Raw newline, tab and CR inside literals; escaped backslash and escaped quote
# followed by a raw newline; a raw newline inside an interpolation (code).
printf 'm=minlit;\ni=io:std.io;\nf=main():i64{\n  let a="name,age\nalice,30\nbob,25";\n  let b="tab\there";\n  let c="cr\rx";\n  let d="bs\\\\\nq";\n  let e="q\\"\nz";\n  let g="\\(1+\n2) end";\n  io.println(a);io.println(b);io.println(c);io.println(d);io.println(e);io.println(g);\n  <0\n};\n' > orig.tk

# ── 1. one line ──────────────────────────────────────────────────────────────
"${TKC}" --min orig.tk > min.tk
check "minified output is one line" "1" "$(wc -l < min.tk | tr -d ' ')"
check "raw newline re-emitted as the \\n escape" "1" "$(grep -cF 'name,age\nalice,30\nbob,25' min.tk | tr -d ' ')"

# ── 2. round-trip: identical program output ──────────────────────────────────
"${TKC}" orig.tk --out orig_bin >/dev/null 2>&1
"${TKC}" min.tk  --out min_bin  >/dev/null 2>&1
ORIG_OUT="$("${RUN}" 20 ./orig_bin | od -c)"
MIN_OUT="$("${RUN}" 20 ./min_bin  | od -c)"
check "compiled output byte-identical (orig vs --min)" "${ORIG_OUT}" "${MIN_OUT}"
check "original prints 10 lines (raw newlines preserved)" "10" "$("${RUN}" 20 ./orig_bin | wc -l | tr -d ' ')"

# ── 3. idempotent ────────────────────────────────────────────────────────────
check "min(min(p)) == min(p)" "$(cat min.tk)" "$("${TKC}" --min min.tk)"

# ── 4. multi-file --min ──────────────────────────────────────────────────────
printf 'm=a;f=main():i64{<0};\n' > a.tk
printf 'm=b;\nf=main():i64{\n  <1\n};\n' > b.tk
MULTI="$("${RUN}" 20 "${TKC}" --min a.tk b.tk </dev/null)"
check "multi-file --min prints one line per file, in order" \
      $'m=a;f=main():i64{<0};\nm=b;f=main():i64{<1};' "${MULTI}"
check "multi-file --min leaves no stray binary" "absent" "$([ -e b ] && echo present || echo absent)"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ] || exit 1
