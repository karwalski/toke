#!/usr/bin/env bash
# C570_file_errno_kinds_agree.sh — every std.file call names a directory
# $isdir and a refused final-component symlink $symlink (127.246 (5),
# 127.252 (4)).
#
# THE DEFECT.  file.c had two errno -> FileErr mappings.  file_open_kind()
# (the byte calls: readbytes, writebytes, readrange, size) sent EISDIR to
# $isdir and ELOOP to $symlink; make_err() (read, write, append, copy, list,
# mkdir, ...) sent EISDIR to $invalid and ELOOP to $io.  So a program that
# matched $symlink to explain AMB-07 caught it from file.readbytes and missed
# it from file.read on the same path.  make_err now agrees.
#
# Cases (a directory "adir", a symlink "link.txt" -> "real.txt"):
#   1 file.write  on adir      -> isdir
#   2 file.append on adir      -> isdir
#   3 file.read   on link.txt  -> symlink
#   4 file.write  on link.txt  -> symlink
#   5 file.append on link.txt  -> symlink
#   6 file.copy   from link.txt -> symlink
#   7 file.readbytes on link.txt -> symlink   (the byte path: green on both)
#   8 file.writebytes on adir    -> isdir     (the byte path: green on both)
#
# Negative control: C570_STDLIB=<main's src/stdlib at cde89021, stdlib/ beside it> — 1, 2 read
# "invalid", 3–6 read "io"; 7 and 8 pass (measured Darwin 25.6).
#
# Story: 127.246 (5), 127.252 (4)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C570_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/adir"; printf 'hi' > "${WORK}/real.txt"; ln -s real.txt "${WORK}/link.txt"

FK='mt e {$notfound:m "notfound";$permission:m "permission";$io:m "io";$invalid:m "invalid";$isdir:m "isdir";$notregular:m "notregular";$symlink:m "symlink";$toolarge:m "toolarge";$nomem:m "nomem";$badarg:m "badarg"}'
# one program, one line per case, so the build cost is paid once
cat > "${WORK}/p.tk" <<EOF
m=main;i=io:std.io;i=file:std.file;i=s:std.str;
f=main():i64{
  io.println(mt file.write("adir";"x") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.append("adir";"x") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.read("link.txt") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.write("link.txt";"x") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.append("link.txt";"x") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.copy("link.txt";"c.txt") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.readbytes("link.txt") {\$ok:t "ok";\$err:e ${FK}});
  io.println(mt file.writebytes("adir";s.bytes("x")) {\$ok:t "ok";\$err:e ${FK}});
  <0
};
EOF
if ! "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C570: 0 passed, 1 failed"; exit 1
fi
OUT="$(cd "${WORK}" && ./b --allow-all 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-write-dir"         "$(line 1)" "isdir"
expect "2-append-dir"        "$(line 2)" "isdir"
expect "3-read-symlink"      "$(line 3)" "symlink"
expect "4-write-symlink"     "$(line 4)" "symlink"
expect "5-append-symlink"    "$(line 5)" "symlink"
expect "6-copy-symlink"      "$(line 6)" "symlink"
expect "7-readbytes-symlink" "$(line 7)" "symlink"
expect "8-writebytes-dir"    "$(line 8)" "isdir"
# the symlink target was not written through (AMB-07 still holds)
expect "9-target-untouched"  "$(cat "${WORK}/real.txt")" "hi"
echo "C570: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
