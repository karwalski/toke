#!/usr/bin/env bash
# C890_strlen_rejects_split_array.sh — a `[T]` a std interface declares
# (str.split / str.fields / str.bytes), passed to a std parameter declared
# `str`, is E4031 whether it arrives directly or through an un-annotated
# immutable `let` (story 127.314).
#
# THE DEFECT. `let a=s.split("abcd\ny\nz";"\n"); s.len(a)` passed --check,
# built, and printed 1 (also 1 for a five-element split) -- the glue read the
# array handle as a C string.  The same call on an array literal,
# `s.len(@("x";"y"))`, was already E4031: check_tki_arg_types judged only an
# argument whose rtype the checker had set, and stdlib returns are not typed
# as binding types (127.66), so the split-bound identifier was never looked
# at.  It now asks std_array_ret -- the rule 127.230 item 5 already applies
# to a user function's str parameter -- when the argument is untyped and the
# parameter is `str`.  Read by the check only, never stored on a node, so
# the emitter sees exactly the rtypes it saw before.
#
# Negative control: TKC built from integrate/z-wave-1108 at ed88ff66 --
# cases 1-6 red (--check rc 0), 7-10 green on both.
#
# Story: 127.314

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
H='m=mb;i=s:std.str;i=io:std.io;'
# chk <body> — `--check` a main whose body is <body>; prints
# "rc=<n> <first error_code> <expected> <got>".
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/mb.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/mb.tk" 2>&1)"; rc=$?
    local code exp got
    code="$(printf '%s' "$out" | grep -o '"error_code":"[^"]*"' | head -1 | cut -d'"' -f4)"
    exp="$(printf '%s' "$out" | grep -o '"expected":"[^"]*"' | head -1 | cut -d'"' -f4)"
    got="$(printf '%s' "$out" | grep -o '"got":"[^"]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${code} ${exp} ${got}"
}
# run <body> — build and run; output on one line.
run() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/mb.tk"
    "${TKC}" -o "${WORK}/b" "${WORK}/mb.tk" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '
}

# 1 — the filed repro: a let bound to s.split, into s.len
expect "1-let-split-into-strlen" \
  "$(chk 'let a=s.split("abcd\ny\nz";"\n"); io.println(s.fromint(s.len(a) as $i64));')" \
  "rc=1 E4031 str @(str)"
# 2 — the split call passed directly
expect "2-direct-split-into-strlen" \
  "$(chk 'io.println(s.fromint(s.len(s.split("a,b";",")) as $i64));')" \
  "rc=1 E4031 str @(str)"
# 3 — str.fields, the other [str]-returning str export
expect "3-let-fields-into-strlen" \
  "$(chk 'let w=s.fields("a b c"); io.println(s.fromint(s.len(w) as $i64));')" \
  "rc=1 E4031 str @(str)"
# 4 — str.bytes ([byte]) into a str parameter
expect "4-let-bytes-into-strlen" \
  "$(chk 'let b=s.bytes("abc"); io.println(s.fromint(s.len(b) as $i64));')" \
  "rc=1 E4031 str @(u8)"
# 5 — through a let chain
expect "5-let-chain" \
  "$(chk 'let a=s.split("a,b";","); let c=a; io.println(s.fromint(s.len(c) as $i64));')" \
  "rc=1 E4031 str @(str)"
# 6 — another str parameter (s.upper)
expect "6-split-into-upper" \
  "$(chk 'let a=s.split("a,b";","); io.println(s.upper(a));')" \
  "rc=1 E4031 str @(str)"
# 7 — control: the array literal was already rejected
expect "7-control-literal" \
  "$(chk 'let a=@("x";"y"); io.println(s.fromint(s.len(a) as $i64));')" \
  "rc=1 E4031 str @(str)"
# 8 — control: .len on the split result is the repair, and is clean
expect "8-control-dot-len-check" \
  "$(chk 'let a=s.split("abcd\ny\nz";"\n"); io.println(s.fromint(a.len as $i64));')" \
  "rc=0   "
expect "9-control-dot-len-runs" \
  "$(run 'let a=s.split("abcd\ny\nz";"\n"); io.println(s.fromint(a.len as $i64));')" \
  "3 "
# 10 — control: an element of the split (a str) into s.len is clean
expect "10-control-element" \
  "$(run 'let a=s.split("abcd\ny";"\n"); io.println(s.fromint(s.len(a.get(0)) as $i64));')" \
  "4 "

echo "C890: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
