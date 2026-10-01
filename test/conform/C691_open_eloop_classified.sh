#!/usr/bin/env bash
# C691_open_eloop_classified.sh -- std.file says "final path component is a
# symlink" ($symlink) only when the final component IS a symlink; a directory
# is $isdir, and a symlink loop in a parent component is $io (story 121.77).
#
# THE DEFECT.  Every std.file open is O_NOFOLLOW, and every ELOOP from it was
# reported as the AMB-07 refusal of a final-component symlink.  ELOOP also
# comes from a symlink loop (or too long a chain) BEFORE the final
# component, so file.read("loop/x") -- x not a symlink, not even present --
# said "final path component is a symlink", and toml.loadfile, which reads
# through file_read, repeated it.  Measured on main 4d7417fb (Darwin).
# (121.77's report that a plain directory gets the symlink message on Darwin
# did not reproduce on this main: open(O_RDONLY|O_NOFOLLOW) of a directory
# succeeds and fstat gives $isdir.  Case 3 pins that it stays so.)
#
# THE FIX.  open_errno_refine (file.c) asks lstat(2) on ELOOP: a final
# symlink keeps $symlink, a directory is $isdir, anything else is $io "too
# many levels of symbolic links in the path ...".  Used by file.read/write/
# append/copy and the byte calls (file.readbytes/writebytes/readrange/size).
#
# WHAT IS ASSERTED
#   1. file.read  loop/x       -> $io      (main: $symlink)
#   2. file.read  link -> file -> $symlink
#   3. file.read  directory    -> $isdir
#   4. file.read  loop itself  -> $symlink (it is a symlink)
#   5. file.write loop/x       -> $io      (main: $symlink)
#   6. file.readbytes loop/x   -> $io      (main: $symlink)
#   7. toml.loadfile loop/x    -> a message that does not claim a symlink
#   8. toml.loadfile directory -> "is a directory"
#
# Negative control: C691_STDLIB=<a src/stdlib tree>.  With main's tree cases
# 1, 5, 6 and 7 fail.
#
# Story: 121.77
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C691_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c691_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C691: ELOOP from an O_NOFOLLOW open is classified by lstat (121.77)"
echo "-------------------------------------------------------------------"

ARMS='$isdir:m "isdir";$symlink:m "symlink";$io:m "io";$notfound:m "notfound";$notregular:m "notregular";$permission:m "permission";$invalid:m "invalid";$toolarge:m "toolarge";$nomem:m "nomem";$badarg:m "badarg"'
cat > rd.tk <<EOF
m=rd;
i=file:std.file;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let r=mt file.read(args.get(1)) {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.print(r);
  <0
};
EOF
cat > wr.tk <<EOF
m=wr;
i=file:std.file;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let r=mt file.write(args.get(1);"x") {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.print(r);
  <0
};
EOF
cat > rb.tk <<EOF
m=rb;
i=file:std.file;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let r=mt file.readbytes(args.get(1)) {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.print(r);
  <0
};
EOF
cat > tl.tk <<'EOF'
m=tl;
i=toml:std.toml;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let r=mt toml.loadfile(args.get(1)) {$ok:v "ok";$err:e e.msg};
  io.print(r);
  <0
};
EOF
for p in rd wr rb tl; do
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" "${p}.tk" --out "${p}" >"${p}.cl" 2>&1; then
        echo "  FAIL: compile ${p}.tk"; sed 's/^/      | /' "${p}.cl" | head -5; exit 1
    fi
done

ln -s loop loop
printf 'a = 1\n' > good.toml
ln -s good.toml link.toml
mkdir adir
chk() {   # chk N LABEL WANT PROG PATH
    local got; got="$(./"$4" --allow-read --allow-write "$5" 2>/dev/null)"
    if [ "${got}" = "$3" ]; then pass "$1 $2: $3"; else fail "$1 $2: got '${got}', want '$3'"; fi
}
chk 1 "file.read loop/x" io rd loop/x
chk 2 "file.read symlink" symlink rd link.toml
chk 3 "file.read directory" isdir rd adir
chk 4 "file.read the loop link itself" symlink rd loop
chk 5 "file.write loop/x" io wr loop/x
chk 6 "file.readbytes loop/x" io rb loop/x

m7="$(./tl --allow-read loop/x 2>/dev/null)"
case "${m7}" in
    *"final path component is a symlink"*|ok|"") fail "7 toml.loadfile loop/x: '${m7}'" ;;
    *) pass "7 toml.loadfile loop/x does not claim a final symlink: ${m7}" ;;
esac
m8="$(./tl --allow-read adir 2>/dev/null)"
case "${m8}" in
    *"is a directory"*) pass "8 toml.loadfile directory: ${m8}" ;;
    *) fail "8 toml.loadfile directory: '${m8}'" ;;
esac

echo "-------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
