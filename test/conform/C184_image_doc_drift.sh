#!/usr/bin/env bash
# C184_image_doc_drift.sh — the std.image pages agree with stdlib/image.tki
# (story 135.19 item 7).
#
# THE DRIFT.  stdlib/image.md, the first std.image page (story 12.6.1), still
# described a module with no rotate, blur or TIFF, claimed JPEG/WebP support,
# and spelled every export with underscores, while stdlib/README.md says
# docs/stdlib/ is the only place a module is documented.  And
# docs/loke-libraries-reply.md told loke CCITT G3/G4 decode was missing after
# it shipped (135.5a, C156).
#
# Asserted: (1) docs/stdlib/image.md names every function image.tki exports;
# (2) stdlib/image.md is gone (135.14), or names them all too, or opens with
# the dated `**Archived YYYY-MM-DD` banner the claim guards treat as a record;
# (3) the loke reply's "CCITT ... missing" sentence carries a dated update.
#
# Negative control: the tree at main 7c16729 — (2) lists the 18 exports the
# old page never names; (3) finds no update.
#
# Story: 135.19

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SRC_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
missing() {   # exports of image.tki whose bare name the page never mentions
    python3 - "$ROOT/stdlib/image.tki" "$1" <<'PY'
import json, re, sys
d = json.load(open(sys.argv[1])); page = open(sys.argv[2]).read()
names = [e["name"].split(".", 1)[1] for e in d["exports"] if e.get("kind") == "func"]
print(" ".join(n for n in names if not re.search(r"\bimage\." + n + r"\b", page)) or "none")
PY
}
expect "1-docs-page-names-every-export" "$(missing "$ROOT/docs/stdlib/image.md")" "none"
if [ ! -e "$ROOT/stdlib/image.md" ]; then
    # 135.14 (2026-10-07) removed the old page outright (archived outside the repo).
    expect "2-old-page-archived-or-complete" "removed" "removed"
elif head -40 "$ROOT/stdlib/image.md" | grep -q '\*\*Archived 20[0-9][0-9]-[0-9][0-9]-[0-9][0-9]'; then
    expect "2-old-page-archived-or-complete" "archived" "archived"
else
    expect "2-old-page-archived-or-complete" "$(missing "$ROOT/stdlib/image.md")" "none"
fi
expect "3-loke-reply-ccitt-updated" \
       "$(grep -c 'Update 20[0-9-]* (story 135.19).*CCITT G3/G4 decode has since shipped' "$ROOT/docs/loke-libraries-reply.md")" "1"

echo "C184: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
