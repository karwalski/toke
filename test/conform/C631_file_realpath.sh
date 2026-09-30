#!/usr/bin/env bash
# C631_file_realpath.sh — file.realpath(path):$str!$fileerr resolves a path
# through every symlink, `.` and `..`, or names why not (121.60).
#
# THE GAP.  No toke-callable realpath existed: std.path is lexical only
# (join/normalize/dir/base), and file.c's file_absolute() (realpath(3)) had
# no .tki entry and no glue.  ooke could not keep template reads inside
# templates/: a symlinked directory under it points anywhere, and a lexical
# check cannot see that (121.59 (3)).  file.realpath is the resolver; its
# failure is a $fileerr, and it is fs.read (it lstat's every component).
#
# Layout under a fresh temp dir W (itself resolved first: macOS /var is a
# symlink to /private/var, which is exactly the case under test):
#   W/root/real/a.txt      a file
#   W/root/link -> real    a symlinked DIRECTORY
#   W/root/loop -> loop    a self-referencing symlink
#   W/root/f.txt           a regular file (not a directory)
# Cases:
#   1 realpath("root/link/a.txt") from W  -> W/root/real/a.txt (relative path in,
#     symlinked directory resolved, absolute path out)
#   2 realpath("root/real/../link")       -> W/root/real  (`..` and a final symlink)
#   3 realpath("root/nothere")            -> $notfound, message names call + path
#   4 realpath("root/loop")               -> $symlink ("too many levels"), not a hang
#   5 realpath("root/f.txt/x")            -> $invalid (ENOTDIR, as make_err maps it)
#   6 realpath("")                        -> $notfound (realpath(3) ENOENT)
#   7 --cap-enforce, no grant             -> exit 1, CAP001 naming fs.read
#   8 the same binary with --allow-read   -> exit 0, resolved
#
# Negative control: C631_STDLIB=<main's src/stdlib at 2aa48c88, stdlib/ beside
# it> — file.realpath is not an export there, and the program does not compile
# (E9005: the $err arm's sum type is not established) (measured Darwin 25.6).
#
# Story: 121.60
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C631_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
W="$(cd "${WORK}" && pwd -P)"
mkdir -p "${W}/root/real"; printf 'a' > "${W}/root/real/a.txt"; printf 'f' > "${W}/root/f.txt"
ln -s real "${W}/root/link"; ln -s loop "${W}/root/loop"

FK='mt e {$notfound:m "notfound";$permission:m "permission";$io:m "io";$invalid:m "invalid";$isdir:m "isdir";$notregular:m "notregular";$symlink:m "symlink";$toolarge:m "toolarge";$nomem:m "nomem";$badarg:m "badarg"}'
MSG='mt e {$notfound:m m;$permission:m m;$io:m m;$invalid:m m;$isdir:m m;$notregular:m m;$symlink:m m;$toolarge:m m;$nomem:m m;$badarg:m m}'
cat > "${W}/p.tk" <<EOF
m=main;i=io:std.io;i=file:std.file;
f=main():i64{
  io.println(mt file.realpath("root/link/a.txt") {\$ok:t t;\$err:e ${FK}});
  io.println(mt file.realpath("root/real/../link") {\$ok:t t;\$err:e ${FK}});
  io.println(mt file.realpath("root/nothere") {\$ok:t t;\$err:e ${FK}});
  io.println(mt file.realpath("root/nothere") {\$ok:t t;\$err:e ${MSG}});
  io.println(mt file.realpath("root/loop") {\$ok:t t;\$err:e ${FK}});
  io.println(mt file.realpath("root/f.txt/x") {\$ok:t t;\$err:e ${FK}});
  io.println(mt file.realpath("") {\$ok:t t;\$err:e ${FK}});
  <0
};
EOF
if ! "${TKC}" --allow-all "${W}/p.tk" -o "${W}/b" > "${W}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${W}/build.log"; echo; echo "C631: 0 passed, 1 failed"; exit 1
fi
OUT="$(cd "${W}" && ./b --allow-all 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-relative-through-symlinked-dir" "$(line 1)" "${W}/root/real/a.txt"
expect "2-dotdot-and-final-symlink"      "$(line 2)" "${W}/root/real"
expect "3-nonexistent"                   "$(line 3)" "notfound"
expect "3b-nonexistent-msg"              "$(line 4)" "file.realpath: file not found: root/nothere"
expect "4-symlink-loop"                  "$(line 5)" "symlink"
expect "5-component-not-dir"             "$(line 6)" "invalid"
expect "6-empty-path"                    "$(line 7)" "notfound"

# capability gate: fs.read
cat > "${W}/c.tk" <<'EOF'
m=main;i=io:std.io;i=file:std.file;
f=main():i64{
  io.println(mt file.realpath("root/link") {$ok:t t;$err:e "err"});
  <0
};
EOF
if "${TKC}" --cap-enforce "${W}/c.tk" --out "${W}/c" > "${W}/c.log" 2>&1; then
    rc=0; (cd "${W}" && ./c > c1.out 2> c1.err) || rc=$?
    if [ "${rc}" -ne 0 ] && grep -q 'CAP001' "${W}/c1.err" && grep -q 'fs.read' "${W}/c1.err"; then
        echo "PASS 7-no-grant-cap001"; PASS=$((PASS + 1))
    else
        echo "FAIL 7-no-grant-cap001 (rc=${rc})"; head -3 "${W}/c1.err"; FAIL=$((FAIL + 1))
    fi
    rc=0; (cd "${W}" && ./c --allow-read > c2.out 2> c2.err) || rc=$?
    expect "8-allow-read" "${rc}:$(cat "${W}/c2.out")" "0:${W}/root/real"
else
    echo "FAIL build --cap-enforce"; head -c 600 "${W}/c.log"; echo; FAIL=$((FAIL + 2))
fi
echo "C631: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
