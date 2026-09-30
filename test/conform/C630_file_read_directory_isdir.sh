#!/usr/bin/env bash
# C630_file_read_directory_isdir.sh — file.read on a directory is $err $isdir,
# as file.readbytes and file.size on the same path already are (121.65).
#
# THE DEFECT.  file_read opens with open(2) O_RDONLY, which SUCCEEDS on a
# directory; on Darwin the fseek/ftell/fread that follow do not fail either
# (fread returns 0), so file.read("adir") came back `$ok ""`.  ooke's
# tplrenderfile, handed a directory path, rendered an empty page with no
# error.  The byte calls share file_open_for_read, which fstat's the
# descriptor and refuses S_ISDIR with $isdir (127.252 (4)); file_read now asks
# the descriptor it opened the same question.
#
# Cases (a directory "adir", an empty file "empty.txt", a file "t.txt"):
#   1 file.read      on adir   -> isdir           (red on main: "ok")
#   2 file.readbytes on adir   -> isdir           (green on both)
#   3 file.size      on adir   -> isdir           (green on both)
#   4 file.read      on adir   message names the call, the path and "is a directory"
#   5 file.read      on empty.txt -> ok, length 0 (an empty file is NOT an error)
#   6 file.read      on t.txt  -> ok, "hello"     (the positive control)
#
# Negative control: C630_STDLIB=<main's src/stdlib at 2aa48c88, stdlib/ beside
# it> — case 1 reads "ok" and case 4 reads the empty-string ok; 2, 3, 5, 6
# pass (measured Darwin 25.6).
#
# Story: 121.65
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C630_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/adir"; : > "${WORK}/empty.txt"; printf 'hello' > "${WORK}/t.txt"

FK='mt e {$notfound:m "notfound";$permission:m "permission";$io:m "io";$invalid:m "invalid";$isdir:m "isdir";$notregular:m "notregular";$symlink:m "symlink";$toolarge:m "toolarge";$nomem:m "nomem";$badarg:m "badarg"}'
MSG='mt e {$notfound:m m;$permission:m m;$io:m m;$invalid:m m;$isdir:m m;$notregular:m m;$symlink:m m;$toolarge:m m;$nomem:m m;$badarg:m m}'
cat > "${WORK}/p.tk" <<EOF
m=main;i=io:std.io;i=file:std.file;i=s:std.str;
f=main():i64{
  io.println(mt file.read("adir") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.readbytes("adir") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.size("adir") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.read("adir") {\$ok:t s.concat("ok:";t);\$err:e ${MSG}});
  io.println(mt file.read("empty.txt") {\$ok:t s.concat("ok:";s.fromint(s.len(t)));\$err:e ${FK}});
  io.println(mt file.read("t.txt") {\$ok:t s.concat("ok:";t);\$err:e ${FK}});
  <0
};
EOF
if ! "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C630: 0 passed, 1 failed"; exit 1
fi
OUT="$(cd "${WORK}" && ./b --allow-all 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-read-dir"        "$(line 1)" "isdir"
expect "2-readbytes-dir"   "$(line 2)" "isdir"
expect "3-size-dir"        "$(line 3)" "isdir"
expect "4-read-dir-msg"    "$(line 4)" "file.read: is a directory: adir"
expect "5-read-empty-ok"   "$(line 5)" "ok:0"
expect "6-read-file-ok"    "$(line 6)" "ok:hello"
echo "C630: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
