#!/usr/bin/env bash
# C164_quarantined_export_at_check.sh — `--check` rejects a call to a stdlib
# export the interface declares and no C source implements (story 127.224
# item 4; the remainder of 127.218).
#
# THE DEFECT.  127.218 made the LINK failure for such a call a located E4027,
# but `--check` still accepted it: the program passed the checker the repair
# loop runs and failed only when built.  The checker now reads
# src/stdlib_quarantine_gen.h, generated from scripts/check_tki_skiplist.txt
# by scripts/gen_tki_quarantine_h.py, and reports E4027 at the call.
#
# THE TRAP (case 3).  The skiplist is not proof of absence:
# check_tki_coverage.py recognises only `<type> tk_x(...) {` definitions, so
# the three image exports instantiated through TK_IMG_XFORM(...) are still
# listed although they link and run.  The generator drops any symbol a C
# source names; image.fliph must stay accepted.
#
# Negative control: TKC built from main at a705c93 — cases 1 and 2 red
# (`--check` exit 0); and case 5 red after appending a bogus entry to the
# generated header.
#
# Story: 127.224, 127.218

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
diags() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){c=substr($0,RSTART+14,RLENGTH-15); match($0,/"line":[0-9]+,"col":[0-9]+/); printf "%s@%s ",c,substr($0,RSTART,RLENGTH)}'; }

# 1. the C130 program, now at --check
cat > "${WORK}/p1.tk" <<'EOF'
m=q;
i=io:std.io;
i=cv:std.canvas;
f=main():i64{
  let c=cv.new("x";10;10);
  cv.fill(c);
  <0
};
EOF
expect "1-canvas-fill-at-check" "$(diags "${WORK}/p1.tk")" 'E4027@"line":6,"col":3 '

# 2. every entry of the generated table is rejected at --check
bad=0; n=0
while IFS= read -r line; do
    sym=$(printf '%s' "$line" | awk -F'"' '{print $2}')
    mod=$(printf '%s' "$line" | awk -F'"' '{print $4}')
    mem=$(printf '%s' "$line" | awk -F'"' '{print $6}')
    printf 'm=t;i=q:%s;\nf=main():i64{\n  q.%s();\n  <0\n};\n' "$mod" "$mem" > "${WORK}/e.tk"
    n=$((n + 1))
    out=$("${TKC}" --check "${WORK}/e.tk" </dev/null 2>&1)
    printf '%s\n' "$out" | awk -v s="$sym" 'index($0,"\"error_code\":\"E4027\"") && index($0,s){f=1} END{exit !f}' || { bad=$((bad + 1)); echo "        not rejected: $mod.$mem ($sym)"; }
done < <(awk '/^    \{ "tk_/' "${REPO_ROOT}/src/stdlib_quarantine_gen.h")
expect "2-every-quarantined-symbol-rejected ($n)" "$bad" "0"

# 3. a skiplisted export whose glue exists (macro-defined) is NOT rejected
if awk '$1=="image.tki::image.fliph"{f=1} END{exit !f}' "${REPO_ROOT}/scripts/check_tki_skiplist.txt"; then
    cat > "${WORK}/p3.tk" <<'EOF'
m=t;i=im:std.image;
f=main():i64{
  im.fliph();
  <0
};
EOF
    expect "3-macro-defined-glue-accepted" "$(diags "${WORK}/p3.tk")" ""
else
    echo "PASS 3-macro-defined-glue-accepted (image.fliph no longer skiplisted; nothing to guard)"; PASS=$((PASS + 1))
fi

# 4. the checked-in header matches the skiplist
expect "4-header-in-sync" "$(python3 "${REPO_ROOT}/scripts/gen_tki_quarantine_h.py" --check >/dev/null 2>&1; echo $?)" "0"

# 5. the drift check can fail
cp -R "${REPO_ROOT}/scripts" "${WORK}/scripts"; mkdir -p "${WORK}/src"
cp -R "${REPO_ROOT}/src/stdlib" "${WORK}/src/stdlib"; cp "${REPO_ROOT}/src/llvm.c" "${REPO_ROOT}/src/stdlib_tables.c" "${WORK}/src/"   # 135.21
cp -R "${REPO_ROOT}/stdlib" "${WORK}/stdlib"
{ cat "${REPO_ROOT}/src/stdlib_quarantine_gen.h"; echo '    { "tk_bogus_w", "std.bogus", "bogus" },'; } > "${WORK}/src/stdlib_quarantine_gen.h"
expect "5-drift-detected" "$(python3 "${WORK}/scripts/gen_tki_quarantine_h.py" --check >/dev/null 2>&1; echo $?)" "1"

echo "C164: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
