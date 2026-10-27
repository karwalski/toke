#!/usr/bin/env bash
# C751_md_renderfile_err_msg.sh — md.renderfile's $err arm binds an $mderr
# whose msg is readable (story 113.B.7).
#
# THE DEFECT.  md.renderfile was a dead export until 136.32 added its glue;
# that glue then stored the payload-less flag 1 in the error slot and returned
# md_render_file's message as the VALUE.  stdlib/md.tki declares the error as
# a record $mderr{msg:$str}, so the $err arm bound address 1 and `e.msg`
# stopped the program with RT005 (measured on main 8bb9f63e: case 2 prints
# "RT005: nil dereference", exit 1).  The glue now stores a {msg} box
# (runtime-abi.md §7.3), as toml_glue.c does.
#
# WHAT IS ASSERTED (probe prints "OK <html>" or "ERR <msg>")
#   1. an existing file renders: $ok, "<h1>hi</h1>";
#   2. a missing file: $err, e.msg = "file not found", exit 0;
#   3. two let-kept errors keep their own message after a later success;
#   4. without --allow-read: CAP001 (the fs.read gate, 121.42) still holds.
#
# Negative control: C751_STDLIB=<main's src/stdlib at 8bb9f63e> — case 2 is
# RT005 there.
#
# Story: 113.B.7
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C751_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c751_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
printf '# hi\n' > a.md
cat > p.tk <<'EOF'
m=main;i=io:std.io;i=md:std.md;i=s:std.str;i=args:std.args;
f=r(p:$str):$str{ <mt md.renderfile(p) {$ok:v s.concat("OK ";s.trim(v));$err:e s.concat("ERR ";e.msg)} };
f=main():i64{
  let mode = mt args.get(1) {$ok:v v;$err:e ""};
  if(s.eq(mode;"one")){ io.println(r("a.md")); io.println(r("nope.md")); <0 };
  let e1 = r("nope1.md");
  let ok = r("a.md");
  let e2 = r("nope2.md");
  io.println(s.concat(e1; s.concat("|"; s.concat(ok; s.concat("|"; e2)))));
  <0
};
EOF
if ! "${TKC}" p.tk --out p > build.log 2>&1; then
    echo "FAIL build"; head -c 600 build.log; echo; echo "C751: 0 passed, 1 failed"; exit 1
fi
OUT="$(./p --allow-read one 2>&1)"; RC=$?
expect "1-render-ok"   "$(printf '%s\n' "${OUT}" | sed -n 1p)" "OK <h1>hi</h1>"
expect "2-missing-msg" "$(printf '%s\n' "${OUT}" | sed -n 2p) rc=${RC}" "ERR file not found rc=0"
expect "3-kept-msgs"   "$(./p --allow-read 2>&1)" "ERR file not found|OK <h1>hi</h1>|ERR file not found"
expect "4-cap-gate"    "$(./p one 2>&1 | grep -o CAP001 | head -1)" "CAP001"
echo "C751: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
