#!/usr/bin/env bash
# C1131_e1003_underscore_v04_text.sh — E1003 for an underscore identifier
# speaks v0.4: its message no longer says "v0.2 syntax ... convert to v0.3",
# and its `expected` field offers the default alphabet's identifier
# characters (a-z, 0-9; toke-spec-v0.4.md section G), not A-Z, which the
# default profile rejects (E2002).  Error code and the absent `fix` are
# unchanged (story 127.338).
#
# THE DEFECT. src/lexer.c emitted "identifier 'max_val' contains underscore
# (v0.2 syntax); run `toke --migrate` to convert to v0.3" with expected
# "identifier without underscores (a-z, A-Z, 0-9)", and for `_x` expected
# "identifier starting with a letter (a-z, A-Z)", teaching a repair loop an
# uppercase name that is itself an error.
#
# Negative control: the 131.103 pin (toke 1f39c2998110) -- cases 1-4 red,
# 5-6 green on both.
#
# Story: 127.338

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
# field <src> <flags> <key> — the first diagnostic's <key> (code/message/expected/fix)
field() {
    printf '%s\n' "$1" > "${WORK}/u.tk"
    # shellcheck disable=SC2086
    "${TKC}" --check $2 "${WORK}/u.tk" 2>&1 | python3 -c '
import json, sys
key = sys.argv[1]
for line in sys.stdin:
    line = line.strip()
    if not line.startswith("{"): continue
    d = json.loads(line)
    if key == "fix": print("fix" in d); break
    print(d.get(key, "")); break
else:
    print("no-diagnostic")
' "$3"
}
IN='m=t;
f=main():i64{let max_val=3;<max_val};'
LEAD='m=t;
f=main():i64{let _x=3;<_x};'

# 1 — inner underscore: the message names v0.4's identifier alphabet
expect "1-inner-message" "$(field "$IN" "" message)" \
  "identifier 'max_val' contains underscore; toke identifiers are lowercase letters and digits only (a-z, 0-9; spec v0.4 section G); \`toke --migrate\` removes underscores"
# 2 — inner underscore: expected offers no uppercase
expect "2-inner-expected" "$(field "$IN" "" expected)" \
  "identifier of lowercase letters and digits only (a-z, 0-9)"
# 3 — leading underscore: message
expect "3-leading-message" "$(field "$LEAD" "" message)" \
  "identifier '_x' starts with underscore; toke identifiers start with a lowercase letter (a-z; spec v0.4 section G); \`toke --migrate\` removes underscores"
# 4 — leading underscore: expected
expect "4-leading-expected" "$(field "$LEAD" "" expected)" \
  "identifier starting with a lowercase letter (a-z)"

# Controls (green on both sides).
# 5 — the code is still E1003, with no fix field
expect "5-code-and-no-fix" "$(field "$IN" "" error_code) $(field "$IN" "" fix)" "E1003 False"
# 6 — `got` still carries the identifier as written
expect "6-got-unchanged" "$(field "$IN" "" got)" "max_val"

echo "C1131: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
