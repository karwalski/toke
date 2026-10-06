#!/usr/bin/env bash
# C718_help_text_split.sh — `tkc --help` is printed from a table of sections,
# and no section comes near C99's 4,095-byte string-literal minimum (127.275).
#
# THE DEFECT.  The whole --help text in src/main.c was one string literal of
# 4,092 bytes.  C99 only guarantees 4,095, and the build uses -Wpedantic
# -Werror, so the next help line added would have broken the build
# (-Woverlength-strings) rather than any test.
#
# NOW the text is HELP_SECTIONS[], printed in sequence by print_help().
#
# CASES.
#   1. `tkc --help` exits 0 and its stdout is byte-identical to
#      fixtures/help/tkc_help.txt (captured from the single-literal build
#      before the split).  An intentional help change updates the fixture.
#   2. `tkc` with no source file prints the same text and exits 3 (EUSAGE).
#   3. HELP_SECTIONS[] in src/main.c has at least two entries and every entry
#      (its adjacent literals concatenated, escapes decoded) is under 2,048
#      bytes -- so the next addition that grows one section too far fails
#      here, with room to spare before the compiler limit.
#   4. Negative control: the same measurement run on a synthetic table with
#      one 2,100-byte entry reports it as too long.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
FIXTURE="${SCRIPT_DIR}/fixtures/help/tkc_help.txt"
MAIN_C="${REPO_ROOT}/src/main.c"

PASS=0
FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "${WORK}"' EXIT

# ── 1. --help is byte-identical to the fixture ────────────────────────
"${TKC}" --help > "${WORK}/help.txt"
rc=$?
if [ "${rc}" -eq 0 ]; then ok "--help exits 0"; else bad "--help exits ${rc}, want 0"; fi
if cmp -s "${WORK}/help.txt" "${FIXTURE}"; then
    ok "--help output byte-identical to fixtures/help/tkc_help.txt"
else
    bad "--help output differs from fixtures/help/tkc_help.txt"
    diff "${FIXTURE}" "${WORK}/help.txt" | head -20
fi

# ── 2. no source file: same text, EUSAGE ──────────────────────────────
"${TKC}" > "${WORK}/noarg.txt" 2>/dev/null
rc=$?
if [ "${rc}" -eq 3 ]; then ok "no source file exits 3"; else bad "no source file exits ${rc}, want 3"; fi
if cmp -s "${WORK}/noarg.txt" "${FIXTURE}"; then
    ok "no-source usage text identical to --help"
else
    bad "no-source usage text differs from --help"
fi

# ── 3/4. section sizes ────────────────────────────────────────────────
# measure FILE: prints one line per HELP_SECTIONS entry "<index> <bytes>",
# then "LONG <index>" for each entry >= 2048 bytes.  Exit 2 if the table is
# not found.
cat > "${WORK}/measure.py" <<'PY'
import re, sys
src = open(sys.argv[1], encoding='utf-8').read()
m = re.search(r'static const char \*const HELP_SECTIONS\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
if not m:
    print('NOTABLE'); sys.exit(2)
body = m.group(1)
tok = re.compile(r'"((?:[^"\\\n]|\\.)*)"|(,)|(/\*.*?\*/)|(\s+)', re.S)
entries, cur, pos = [], None, 0
while pos < len(body):
    t = tok.match(body, pos)
    if not t:
        print('PARSE', body[pos:pos + 40]); sys.exit(2)
    pos = t.end()
    if t.group(1) is not None:
        b = t.group(1).encode('utf-8').decode('unicode_escape').encode('latin-1')
        cur = (cur or b'') + b
    elif t.group(2):
        entries.append(cur); cur = None
if cur is not None:
    entries.append(cur)
for i, e in enumerate(entries):
    print(i, len(e))
for i, e in enumerate(entries):
    if len(e) >= 2048:
        print('LONG', i)
PY

python3 "${WORK}/measure.py" "${MAIN_C}" > "${WORK}/sizes.txt"
mrc=$?
if [ "${mrc}" -ne 0 ]; then
    bad "HELP_SECTIONS table not found or not parsed in src/main.c"
    cat "${WORK}/sizes.txt"
else
    n="$(grep -cv '^LONG' "${WORK}/sizes.txt")"
    if [ "${n}" -ge 2 ]; then ok "HELP_SECTIONS has ${n} entries"; else bad "HELP_SECTIONS has ${n} entries, want >= 2"; fi
    if grep -q '^LONG' "${WORK}/sizes.txt"; then
        bad "a help section is >= 2048 bytes: $(tr '\n' ' ' < "${WORK}/sizes.txt")"
    else
        ok "every help section < 2048 bytes ($(grep -v '^LONG' "${WORK}/sizes.txt" | awk '{print $2}' | tr '\n' ' '))"
    fi
    # The sections, concatenated, are the whole help text minus puts' '\n'.
    total="$(grep -v '^LONG' "${WORK}/sizes.txt" | awk '{s += $2} END {print s}')"
    want=$(( $(wc -c < "${FIXTURE}") - 1 ))
    if [ "${total}" -eq "${want}" ]; then
        ok "sections measured cover the whole help text (${total} bytes)"
    else
        bad "sections measure ${total} bytes, help text is ${want}"
    fi
fi

# Negative control: an oversize entry is reported.
python3 - "${WORK}/fake.c" <<'PY'
import sys
big = 'x' * 2100
open(sys.argv[1], 'w').write(
    'static const char *const HELP_SECTIONS[] = {\n'
    '    "short\\n",\n'
    '    "' + big[:1000] + '"\n    "' + big[1000:] + '"\n};\n')
PY
python3 "${WORK}/measure.py" "${WORK}/fake.c" > "${WORK}/fake_sizes.txt"
if grep -q '^LONG 1$' "${WORK}/fake_sizes.txt"; then
    ok "negative control: a 2,100-byte section is flagged"
else
    bad "negative control: oversize section not flagged: $(tr '\n' ' ' < "${WORK}/fake_sizes.txt")"
fi

echo ""
echo "C718: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
