#!/usr/bin/env bash
# C752_args_all_and_err_msg.sh — args.all() links and returns every argument,
# and args.get's $err carries the core's own reason (story 113.B.8).
#
# HISTORY.  113.B.8 was filed when args.all had no wrapper (an unlinkable
# export) and tk_args_get_w dropped ArgsErr.msg.  136.32 added tk_args_all_w
# and 127.241 made the $err arm bind an {msg} box, but that box held a
# constant written in the glue, not the reason args_get() returned.  The glue
# now forwards the core message ("args.get: " + args.c's reason); no test
# covered args.all at all until this one.
#
# WHAT IS ASSERTED
#   1. args.all().len equals args.count() (argv[0] included);
#   2. args.all() keeps order and content: element 2 is the 2nd user arg,
#      including an empty-string argument and one with a space;
#   3. args.get(99): $err with e.msg "args.get: index out of bounds";
#   4. args.get(0) is element 0 of args.all().
#
# Negative control: C752_STDLIB=<a src/stdlib tree before 136.32> fails to
# link args.all; case 3's text is unchanged by design (same message, now from
# the core).
#
# Story: 113.B.8
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C752_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c752_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;i=args:std.args;i=s:std.str;
f=main():i64{
  let a = args.all();
  io.println(s.concat(s.fromint(a.len as i64); s.concat(" "; s.fromint(args.count() as i64))));
  io.println(s.concat("["; s.concat(a.get(2); s.concat("]["; s.concat(a.get(3); "]")))));
  io.println(mt args.get(99) {$ok:v v;$err:e e.msg});
  let z = mt args.get(0) {$ok:v v;$err:e "err"};
  if(s.eq(z; a.get(0))){io.println("same")};
  <0
};
EOF
if ! "${TKC}" "${WORK}/p.tk" --out "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C752: 0 passed, 1 failed"; exit 1
fi
OUT="$("${WORK}/b" one "" "two words" 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-all-len-is-count" "$(line 1)" "4 4"
expect "2-all-content"      "$(line 2)" "[][two words]"
expect "3-get-err-msg"      "$(line 3)" "args.get: index out of bounds"
expect "4-get0-is-all0"     "$(line 4)" "same"
echo "C752: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
