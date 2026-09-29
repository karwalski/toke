#!/usr/bin/env bash
# C537_did_you_mean_only_writable_names.sh — E3011's "did you mean" fix never
# suggests a name that cannot be written where the error is (story 127.246
# item 1, AGENTS §6).
#
# THE DEFECT.  An interface may declare an upper-case type name (std.file's
# `FileErr`, and a user module's hand-written .tki).  `$fileerr` is E3011,
# and its fix said "did you mean 'FileErr'?" -- but `$FileErr` does not lex
# (E2002: a `$` type name is lower-case), so applying the fix trades one
# error for another.  A fix must be correct in every case it is emitted.
#
# NOW: the suggestion search skips a candidate that is not spellable in the
# position: after a `$` sigil no upper-case letter, and nowhere a `_`.  With
# no spellable candidate the fix is absent.  The upper-case stdlib names
# themselves are lower-cased separately (fc8865f0 did i18n/tls).
#
# Negative control: TKC=<main d3a083b8's toke> -- case 1 carries
# "did you mean 'WidgetErr'?".  Case 2 (a spellable suggestion) is green on
# both.
#
# Story: 127.246
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
# diags FILE: code|fix-or-ABSENT of each diagnostic, joined by ' / '.
diags() {
    "${TKC}" --check -I . "$1" > o 2>&1
    python3 - o <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s" % (d.get("error_code"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "none")
PY
}
cat > wlib.tki <<'EOF2'
{
  "schema_version": "1.0",
  "module": "wlib",
  "exports": [
    {"kind": "type", "name": "WidgetErr", "fields": [{"name": "msg", "type": "str"}]},
    {"kind": "func", "name": "wlib.make", "params": ["i64"], "return": "i64"}
  ]
}
EOF2
printf 'm=main;\ni=w:wlib;\nf=g(e:$widgeterr):i64{<0};\nf=main():i64{<0};\n' > a.tk
expect "1 \$widgeterr is not offered 'WidgetErr'" "$(diags a.tk)" "E3011|ABSENT"
printf 'm=main;\nt=$shape{$a:i64};\nf=g(e:$shap):i64{<0};\nf=main():i64{<0};\n' > b.tk
expect "2 control: a spellable type is offered" "$(diags b.tk)" "E3011|did you mean 'shape'?"
echo "C537: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
