#!/usr/bin/env bash
# C1880_cap_scope_fs_paths.sh -- fs.read / fs.write grants scoped to paths
# (story 138.19, D1; docs/decisions/138.19-scoped-capability-grants.md §1, §3.1).
#
# THE DEFECT (loke T-59 point 1).  A grant was always the whole class:
# `--allow-read=/srv/www` and `fs_read = "/srv/www"` both granted fs.read for
# EVERY path; the value was accepted and ignored.  Measured on v3.2.0: a
# binary run with --allow-read=<dir> reads a file outside <dir> (exit 0).
#
# THE CHANGE.  A grant is the whole class or a list of path scopes.  A path
# matches a scope when, after canonicalisation (realpath of the longest
# existing prefix, then the components that do not exist yet), it is the
# scope or lies beneath it, compared component by component.  A sink the
# scope does not admit stops with CAP002 before the syscall.
#
# WHAT IS ASSERTED (run-time flags; the manifest form is C1885, D2 is C1883)
#   1. --allow-read=<dir> admits <dir>/in.txt and denies out.txt beside it
#   2. a `..` escape (<dir>/../out.txt) is denied
#   3. a symlink inside <dir> that points outside it is denied
#   4. prefix confusion: --allow-read=<base>/www denies <base>/wwwx/f
#   5. a trailing slash and a relative grant behave like the absolute one
#   6. `~` in a grant expands from HOME
#   7. --allow-write=<dir> admits creating a NEW file in <dir> (and a new
#      file in a new sub-path's existing parent), denies one outside
#   8. a directory reader (file.list) is checked on the directory
#   9. a dangling symlink inside <dir> (target outside, not yet created) is
#      denied for a write -- a symlink cannot pass by pointing at nothing
#  10. the whole class (--allow-read, no value) still reads anywhere
#  11. file.listglob (138.19 round 2): its pattern's literal directory is
#      gated (CAP002 outside the scope), and a match glob(3) found through a
#      directory outside the scope is left out -- through the '..' entry,
#      which '.?', '.*' and '.[.]' match, or through a symlink inside the
#      scope -- with the same empty answer whether or not the outside file
#      exists (no existence oracle).  Each pattern is shown, under the whole
#      class, to reach the outside file: the scope is what removes it.
#
# NEGATIVE CONTROL: run this script with TKC=<a v3.2.0 toke>.  On v3.2.0 the
# value of --allow-read=<dir> is ignored, so 1-4 and 7's denials FAIL (the
# outside file is read / written, exit 0), and so do 11's (every glob match
# is returned, and the outside directory is listed).
#
# Story: 138.19
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1880_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"   # canonical (macOS: /var -> /private/var)
echo "C1880: fs.read / fs.write scoped to paths (138.19 D1)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/fsprobe.tk" <<'EOF'
m=fsprobe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=str:std.str;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  let p=mt args.get(2) {$ok:v v;$err:e ""};
  if(op=="read"){
    let r=mt file.read(p) {$ok:v 1;$err:e 2};
    io.println("read-done \(r)");
  }el{};
  if(op=="write"){
    let w=mt file.write(p;"x") {$ok:b 1;$err:e 2};
    io.println("write-done \(w)");
  }el{};
  if(op=="list"){
    let l=mt file.list(p) {$ok:v 1;$err:e 2};
    io.println("list-done \(l)");
  }el{};
  if(op=="glob"){
    let g=file.listglob(p);
    io.println("glob-done \(g.len) [\(str.join(",";g))]");
  }el{};
  <0
};
EOF
if ! "${TKC}" --out "${WORK}/fsprobe" "${WORK}/fsprobe.tk" >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: probe did not build"; sed 's/^/      | /' "${WORK}/build.log" | head -5; exit 1
fi
P="${WORK}/fsprobe"
D="${WORK}/www"; mkdir -p "${D}/sub" "${WORK}/wwwx" "${WORK}/home/.cfg"
echo in >"${D}/in.txt"; echo out >"${WORK}/out.txt"; echo x >"${WORK}/wwwx/f"
echo h >"${WORK}/home/.cfg/c.txt"
ln -s "${WORK}/out.txt" "${D}/escape"
ln -s "${WORK}/newfile-outside" "${D}/dangling"

# run LABEL WANT(admit|deny) [env...] -- ARGS: runs the probe, checks the
# outcome.  admit = exit 0 and a "-done" line; deny = exit 1 and CAP002.
run() {
    local label="$1" want="$2"; shift 2
    local rc=0
    ( cd "${WORK}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    if [ "${want}" = admit ] && [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: admitted ($(head -1 "${WORK}/o"))"
    elif [ "${want}" = deny ] && [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability" "${WORK}/e" \
         && ! /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: denied ($(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g" | cut -c1-110))"
    else
        fail "${label}: wanted ${want}, got rc=${rc}"
        sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -4
    fi
}

# 1 inside / outside
run "1a read inside --allow-read=<dir>" admit "${P}" read "${D}/in.txt" "--allow-read=${D}"
run "1b read beside it (out.txt)" deny "${P}" read "${WORK}/out.txt" "--allow-read=${D}"
# 2 .. escape
run "2 read <dir>/../out.txt" deny "${P}" read "${D}/../out.txt" "--allow-read=${D}"
# 3 symlink inside pointing outside
run "3 read <dir>/escape -> ../out.txt" deny "${P}" read "${D}/escape" "--allow-read=${D}"
# 4 prefix confusion
run "4 read <base>/wwwx/f under --allow-read=<base>/www" deny "${P}" read "${WORK}/wwwx/f" "--allow-read=${D}"
# 5 trailing slash, relative grant (relative to the working directory at start)
run "5a trailing slash grant" admit "${P}" read "${D}/in.txt" "--allow-read=${D}/"
run "5b relative grant ./www" admit "${P}" read "${D}/in.txt" "--allow-read=./www"
run "5c relative grant denies outside" deny "${P}" read "${WORK}/out.txt" "--allow-read=./www"
run "5d relative request under a relative grant" admit "${P}" read "www/sub/../in.txt" "--allow-read=www"
# 6 ~ expands from HOME
run "6a ~/.cfg grant (HOME=<work>/home)" admit env HOME="${WORK}/home" "${P}" read "${WORK}/home/.cfg/c.txt" "--allow-read=~/.cfg"
run "6b ~/.cfg grant denies outside it" deny env HOME="${WORK}/home" "${P}" read "${WORK}/out.txt" "--allow-read=~/.cfg"
# 7 creating new files under an fs.write scope
run "7a write a new file inside --allow-write=<dir>" admit "${P}" write "${D}/new.txt" "--allow-write=${D}"
if [ -f "${D}/new.txt" ]; then pass "7a the new file exists"; else fail "7a the new file was not created"; fi
run "7b write a new file in an existing subdirectory" admit "${P}" write "${D}/sub/new2.txt" "--allow-write=${D}"
run "7c write outside the scope" deny "${P}" write "${WORK}/created-outside.txt" "--allow-write=${D}"
if [ -e "${WORK}/created-outside.txt" ]; then fail "7c the denied write created the file"; else pass "7c nothing created outside"; fi
run "7d write <dir>/nonexistent/../../escape.txt" deny "${P}" write "${D}/nonexistent/../../escape.txt" "--allow-write=${D}"
# 8 directory readers are checked on the directory
run "8a file.list <dir>" admit "${P}" list "${D}" "--allow-read=${D}"
run "8b file.list <dir>/.. (the parent)" deny "${P}" list "${D}/.." "--allow-read=${D}"
# 9 a dangling symlink inside the scope
run "9 write through <dir>/dangling -> outside" deny "${P}" write "${D}/dangling" "--allow-write=${D}"
if [ -e "${WORK}/newfile-outside" ]; then fail "9 the dangling symlink's target was created"; else pass "9 nothing created through the symlink"; fi
# 10 whole class unchanged
run "10 --allow-read (whole class) reads out.txt" admit "${P}" read "${WORK}/out.txt" --allow-read

# 11 file.listglob.  G/www is the scope; G/outside holds s.txt; G/www/link
# is a symlink to G/outside.
G="${WORK}/g"; mkdir -p "${G}/www" "${G}/outside"
echo i >"${G}/www/in.txt"; echo s >"${G}/outside/s.txt"; ln -s "${G}/outside" "${G}/www/link"
# globn LABEL WANT_COUNT ARGS...: exit 0 and "glob-done WANT_COUNT"
globn() {
    local label="$1" want="$2"; shift 2
    local rc=0
    ( cd "${WORK}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    local got; got="$(/usr/bin/grep -a -o "^glob-done [0-9]*" "${WORK}/o" | cut -d' ' -f2)"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
        pass "${label}: ${want} match(es) ($(head -1 "${WORK}/o" | sed "s#${WORK}#<work>#g" | cut -c1-90))"
    else
        fail "${label}: wanted ${want} match(es), got rc=${rc} count='${got}'"
        sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -4
    fi
}
globn "11a www/*.txt under --allow-read=<g>/www" 1 "${P}" glob "${G}/www/*.txt" "--allow-read=${G}/www"
for pat in '.?' '.*' '.[.]'; do
    globn "11b www/${pat}/outside/* (the .. entry) -- whole class reaches it" 1 "${P}" glob "${G}/www/${pat}/outside/*" --allow-read
    globn "11b www/${pat}/outside/* under --allow-read=<g>/www: left out" 0 "${P}" glob "${G}/www/${pat}/outside/*" "--allow-read=${G}/www"
done
globn "11c www/*/s.txt (through the symlink) -- whole class reaches it" 1 "${P}" glob "${G}/www/*/s.txt" --allow-read
globn "11c www/*/s.txt under the scope: left out" 0 "${P}" glob "${G}/www/*/s.txt" "--allow-read=${G}/www"
globn "11c www/l*/* under the scope: left out" 0 "${P}" glob "${G}/www/l*/*" "--allow-read=${G}/www"
globn "11d www/.?/outside/s.txt (exists) under the scope" 0 "${P}" glob "${G}/www/.?/outside/s.txt" "--allow-read=${G}/www"
globn "11d www/.?/outside/nosuch (does not exist): the same answer" 0 "${P}" glob "${G}/www/.?/outside/nosuch" "--allow-read=${G}/www"
globn "11e www/l* names the symlink itself (as file.list of www would)" 1 "${P}" glob "${G}/www/l*" "--allow-read=${G}/www"
run "11f outside/* (the pattern's directory is outside the scope)" deny "${P}" glob "${G}/outside/*" "--allow-read=${G}/www"
if /usr/bin/grep -a -q "^  grant this scope at run time with --allow-read=${G}/outside$" "${WORK}/e"; then
    pass "11f the hint names the pattern's directory, never '/'"
else fail "11f hint: $(sed -n 2p "${WORK}/e")"; fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
