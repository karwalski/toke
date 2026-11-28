#!/usr/bin/env bash
# C1887_cap_scope_fs_bypass.sh -- the ways round an fs scope that the round-3
# review of 138.19 found or looked for (story 138.19, D1;
# docs/decisions/138.19-scoped-capability-grants.md §3.1).
#
# THE DEFECTS (measured on 71677b6b, the round-2 head, under --allow-read=www):
#   A. file.listglob gated and filtered the pattern's TEXT, but glob(3) reads
#      '\' as an escape.  "www/\.\./secret.txt" returned [www/../secret.txt]
#      when the file existed and [] when it did not; "www/\/\.\./*" listed the
#      parent directory; "www/\link/s.txt" and "www/\/link/*" went through a
#      symlink leading out of the scope.
#   B. A path was checked where it ENDS, not on the way there.
#      "www/../outside/../www/in.txt" ends inside www, so file.read,
#      file.exists, file.list, file.listglob and file.write admitted it -- and
#      whether the call succeeded said whether outside/ existed.  A symlink
#      inside the scope that leads out and back ("www/link/../www/in.txt") and
#      a command path (process.spawn "bin/../outside/../bin/tool") did the same.
#   C. http.servepages scanned its pages directory with stat(2), which follows
#      symlinks, and re-read each page and its layout at request time with no
#      check: a page or a layout that is a symlink leading out of the scope
#      was served.
#   D. log.openaccess under a scope naming only the log file also created the
#      log's directory, a ".app.log.lock" sidecar and rotated files beside it,
#      outside the scope.
#
# THE CHANGE.
#   A. Under a scoped fs.read grant, a pattern holding '\' is refused (CAP002,
#      no fix, a hint: write a literal *, ? or [ as [*], [?] or [[]), whether
#      or not anything matches.  Defence in depth: every match must start with
#      the literal directory text the gate checked (a pattern with no
#      metacharacter must come back as itself), as well as pass round 2's
#      per-directory filter.
#   B. Every directory a path enters before its last component must be
#      admitted, an ancestor of a granted scope, or the working directory or
#      one of its ancestors; otherwise CAP002 names that directory, with no
#      fix (adding it would not be the only edit needed in every case).
#   C. The scan skips an entry the scope does not admit; each page and layout
#      read at request time is checked, and a refused one fails the render
#      (500), as a layout("../x") already did.
#   D. Under a scoped fs.write grant, a log at a regular-file path needs its
#      directory admitted; CAP002 names the directory.
#   E. (round 4, AGENTS §6) A CAP002 fix must work on its own.  A path that
#      ENDS outside the scope and passes through an outside directory now
#      names that directory with no fix (it used to get its target's fix, and
#      applying it stopped at the transit denial); a log's directory is
#      checked before the log, with a fix only when the log resolves inside
#      it.  Every fix offered in §E is applied and the call must then pass.
#   The whole class (a bare --allow-read / --allow-write / --allow-run) is
#   unchanged: every case in A-D is first shown to reach outside under it.
#
# NOT COVERED HERE (decision note §3.1, D5 = story 138.44): a symlink swapped
# in by ANOTHER process between the check and the syscall.  The gates run in
# the process; only OS-level enforcement closes that window.
#
# NEGATIVE CONTROL: run with TKC=<a toke built at 71677b6b>: A (1e-1i), B and
# C fail and D's deny fails.  With TKC=<a toke built at bb2208ea> (round 3)
# 7a-7e and 7h-7i fail: a fix is offered that does not work.  With
# TKC=<v3.2.0 toke> every scoped deny fails.
#
# Port: 127.0.0.1:48215 (batch Z215's range), stopped by PID.
#
# Story: 138.19, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PORT="${C1887_PORT:-48215}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1887_XXXXXX")"
SRVPID=""
cleanup() { [ -n "${SRVPID}" ] && kill "${SRVPID}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
WORK="$(cd "${WORK}" && pwd -P)"   # canonical (macOS: /var -> /private/var)
echo "C1887: the ways round an fs scope (138.19 round 3)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/probe.tk" <<'EOF'
m=probe;
i=io:std.io;
i=args:std.args;
i=file:std.file;
i=str:std.str;
i=process:std.process;
i=log:std.log;
f=main():i64{
  let op=mt args.get(1) {$ok:v v;$err:e ""};
  let p=mt args.get(2) {$ok:v v;$err:e ""};
  if(op=="read"){
    let r=mt file.read(p) {$ok:v 1;$err:e 2};
    io.println("read-done \(r)");
  }el{};
  if(op=="exists"){
    io.println("exists-done \(file.exists(p))");
  }el{};
  if(op=="list"){
    let l=mt file.list(p) {$ok:v 1;$err:e 2};
    io.println("list-done \(l)");
  }el{};
  if(op=="glob"){
    let g=file.listglob(p);
    io.println("glob-done \(g.len) [\(str.join(",";g))]");
  }el{};
  if(op=="write"){
    let w=mt file.write(p;"x") {$ok:b 1;$err:e 2};
    io.println("write-done \(w)");
  }el{};
  if(op=="spawn"){
    let s=mt process.spawn(@(p)) {$ok:h 1;$err:e 2};
    io.println("spawn-done \(s)");
  }el{};
  if(op=="log"){
    let lr=log.openaccess(p;0;0;0);
    io.println("log-done \(lr)");
  }el{};
  <0
};
EOF
cat >"${WORK}/pages.tk" <<EOF
m=pages;
i=io:std.io;
i=args:std.args;
i=http:std.http;
f=main():i64{
  let op=mt args.get(1) {\$ok:v v;\$err:e ""};
  let n=http.servepages("${WORK}/g/pages";"${WORK}/g/tmpl");
  io.println("pages-done \(n)");
  if(op=="serve"){
    let s=http.serveon("127.0.0.1";${PORT} as u64);
  }el{};
  <0
};
EOF
for t in probe pages; do
    if ! "${TKC}" --out "${WORK}/${t}" "${WORK}/${t}.tk" >"${WORK}/build.log" 2>&1; then
        echo "  FAIL: ${t} did not build"; sed 's/^/      | /' "${WORK}/build.log" | head -5; exit 1
    fi
done
P="${WORK}/probe"
G="${WORK}/g"
mkdir -p "${G}/www/sub" "${G}/outside" "${G}/other" "${G}/bin" "${G}/logs" "${G}/outdir"
echo i >"${G}/www/in.txt"; echo s >"${G}/outside/s.txt"; echo x >"${G}/secret.txt"
echo star >"${G}/www/x*y.txt"
ln -s "${G}/outside" "${G}/www/link"
printf '#!/bin/sh\nexit 0\n' >"${G}/bin/tool"; chmod +x "${G}/bin/tool"
SR="--allow-read=${G}/www"

# run LABEL WANT(admit|deny) DIR ARGS...: admit = exit 0 and a "-done"
# line; deny = exit 1, CAP002 and no "-done" line.  stderr stays in e.
run() {
    local label="$1" want="$2" dir="$3"; shift 3
    local rc=0
    ( cd "${dir}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    if [ "${want}" = admit ] && [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: admitted ($(head -1 "${WORK}/o" | sed "s#${WORK}#<work>#g" | cut -c1-90))"
    elif [ "${want}" = deny ] && [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability" "${WORK}/e" \
         && ! /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: denied ($(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g" | cut -c1-100))"
    else
        fail "${label}: wanted ${want}, got rc=${rc}"
        sed "s#${WORK}#<work>#g; s/^/      | /" "${WORK}/o" "${WORK}/e" | head -4
    fi
}
# globn LABEL WANT_COUNT DIR ARGS...: exit 0 and "glob-done WANT_COUNT"
globn() {
    local label="$1" want="$2" dir="$3"; shift 3
    local rc=0
    ( cd "${dir}" && "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    local got; got="$(/usr/bin/grep -a -o "^glob-done [0-9]*" "${WORK}/o" | cut -d' ' -f2)"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
        pass "${label}: ${want} match(es) ($(head -1 "${WORK}/o" | cut -c1-80))"
    else
        fail "${label}: wanted ${want} match(es), got rc=${rc} count='${got}'"
        sed "s#${WORK}#<work>#g; s/^/      | /" "${WORK}/o" "${WORK}/e" | head -4
    fi
}
# nofix LABEL: the last run's TK_DIAG_JSON record is CAP002 with no "fix"
nofix() {
    if /usr/bin/grep -a -q '^{"diagnostic_id":"CAP002"' "${WORK}/e" \
       && ! /usr/bin/grep -a '^{"diagnostic_id":"CAP002"' "${WORK}/e" | /usr/bin/grep -q '"fix"'; then
        pass "$1: the JSON record has no fix"
    else fail "$1: JSON record: $(/usr/bin/grep -a '^{' "${WORK}/e" | head -1)"; fi
}

# ── A. glob escapes ─────────────────────────────────────────────────────
globn "1a www/\\.\\./secret.txt, whole class: reaches G/secret.txt" 1 "${G}" "${P}" glob 'www/\.\./secret.txt' --allow-read
globn "1b www/\\/\\.\\./*, whole class: lists G" 7 "${G}" "${P}" glob 'www/\/\.\./*' --allow-read
globn "1c www/\\link/s.txt, whole class: through the symlink" 1 "${G}" "${P}" glob 'www/\link/s.txt' --allow-read
globn "1d www/\\/link/*, whole class: through the symlink" 1 "${G}" "${P}" glob 'www/\/link/*' --allow-read
for pat in 'www/\.\./secret.txt' 'www/\.\./nosuch.txt' 'www/\/\.\./*' 'www/\link/s.txt' 'www/\/link/*'; do
    run "1e ${pat} under --allow-read=<g>/www" deny "${G}" "${P}" glob "${pat}" "${SR}"
    if /usr/bin/grep -a -q -F "  glob(3) reads '\\' as an escape" "${WORK}/e"; then
        pass "1e ${pat}: the hint says why"
    else fail "1e ${pat}: hint: $(sed -n 2p "${WORK}/e")"; fi
done
run "1f the same, TK_DIAG_JSON=1" deny "${G}" env TK_DIAG_JSON=1 "${P}" glob 'www/\.\./secret.txt' "${SR}"
nofix "1f escape refusal"
globn "1g www/x[*]y.txt (the hint's spelling) under the scope" 1 "${G}" "${P}" glob 'www/x[*]y.txt' "${SR}"
globn "1h www/*.txt under the scope (no escape): unchanged" 2 "${G}" "${P}" glob 'www/*.txt' "${SR}"
globn "1i www/in.txt (no metacharacter) under the scope: itself" 1 "${G}" "${P}" glob 'www/in.txt' "${SR}"
globn "1j www//in.txt keeps its text" 1 "${G}" "${P}" glob 'www//in.txt' "${SR}"

# ── B. the way there ────────────────────────────────────────────────────
for op in read exists; do
    run "2a ${op} www/../outside/../www/in.txt, whole class" admit "${G}" "${P}" "${op}" www/../outside/../www/in.txt --allow-read
    run "2b ${op} www/../outside/../www/in.txt (outside/ exists), scoped" deny "${G}" "${P}" "${op}" www/../outside/../www/in.txt "${SR}"
    if /usr/bin/grep -a -q "^CAP002: capability 'fs.read' is granted, but not for '${G}/outside'$" "${WORK}/e" \
       && /usr/bin/grep -a -q "^  the path passes through this directory" "${WORK}/e"; then
        pass "2b ${op}: names the directory passed through, and says why"
    else fail "2b ${op}: $(head -2 "${WORK}/e" | tr '\n' '|')"; fi
    run "2c ${op} www/../nosuch/../www/in.txt (nosuch/ does not), scoped: denied the same way" deny "${G}" "${P}" "${op}" www/../nosuch/../www/in.txt "${SR}"
    run "2d ${op} www/link/../www/in.txt (out through a symlink and back), whole class" admit "${G}" "${P}" "${op}" www/link/../www/in.txt --allow-read
    run "2e ${op} www/link/../www/in.txt, scoped" deny "${G}" "${P}" "${op}" www/link/../www/in.txt "${SR}"
done
run "2f list www/../outside/../www, whole class" admit "${G}" "${P}" list www/../outside/../www --allow-read
run "2f list www/../outside/../www, scoped" deny "${G}" "${P}" list www/../outside/../www "${SR}"
globn "2g glob www/../outside/../www/*, whole class" 4 "${G}" "${P}" glob 'www/../outside/../www/*' --allow-read
run "2g glob www/../outside/../www/*, scoped" deny "${G}" "${P}" glob 'www/../outside/../www/*' "${SR}"
run "2h TK_DIAG_JSON=1" deny "${G}" env TK_DIAG_JSON=1 "${P}" read www/../outside/../www/in.txt "${SR}"
nofix "2h a path through a directory outside the scope"
# what stays admitted: a '..' inside the scope, the way down from an
# ancestor of the scope, and a relative path climbing from the working
# directory (its ancestors are where the path starts, not something learnt)
run "2i www/sub/../in.txt" admit "${G}" "${P}" read www/sub/../in.txt "${SR}"
run "2j www/../www/in.txt (through the scope's parent, an ancestor)" admit "${G}" "${P}" read www/../www/in.txt "${SR}"
run "2k ../www/in.txt from <g>/other (the working directory's parent)" admit "${G}/other" "${P}" read ../www/in.txt "${SR}"
run "2l ../outside/../www/in.txt from <g>/other, scoped" deny "${G}/other" "${P}" read ../outside/../www/in.txt "${SR}"
# fs.write: nothing created through the outside directory
run "3a write www/../outside/../www/new1.txt, whole class" admit "${G}" "${P}" write www/../outside/../www/new1.txt --allow-write
run "3b write www/../outside/../www/new2.txt, --allow-write=<g>/www" deny "${G}" "${P}" write www/../outside/../www/new2.txt "--allow-write=${G}/www"
if [ -e "${G}/www/new2.txt" ]; then fail "3b the denied write created the file"; else pass "3b nothing created"; fi
# process.spawn: a command path walks its directories too
run "4a spawn bin/../outside/../bin/tool, whole class" admit "${G}" "${P}" spawn bin/../outside/../bin/tool --allow-run
run "4b spawn bin/tool under --allow-run=<g>/bin/tool" admit "${G}" "${P}" spawn bin/tool "--allow-run=${G}/bin/tool"
run "4c spawn bin/../outside/../bin/tool under the same scope" deny "${G}" "${P}" spawn bin/../outside/../bin/tool "--allow-run=${G}/bin/tool"

# ── C. http.servepages ──────────────────────────────────────────────────
mkdir -p "${G}/pages" "${G}/tmpl" "${G}/outpages" "${G}/outtmpl"
printf '{! layout("base") !}{! block("b") !}HOME{! end !}' >"${G}/pages/index.tkt"
printf '{! layout("evil") !}{! block("b") !}LEAK{! end !}' >"${G}/pages/leak.tkt"
printf '{! layout("base") !}{! block("b") !}OUTPAGE-SECRET{! end !}' >"${G}/outpages/secret.tkt"
ln -s "${G}/outpages" "${G}/pages/ext"
printf '<b>{! yield("b") !}</b>' >"${G}/tmpl/base.tkt"
printf 'OUTLAYOUT-SECRET {! yield("b") !}' >"${G}/outtmpl/evil.tkt"
ln -s "${G}/outtmpl/evil.tkt" "${G}/tmpl/evil.tkt"
PG="${WORK}/pages"
SRP="--allow-read=${G}/pages,${G}/tmpl"
run "5a servepages, whole class: 3 routes (ext/secret through the symlink)" admit "${G}" "${PG}" count --allow-read
/usr/bin/grep -a -q "^pages-done 3$" "${WORK}/o" && pass "5a 3 routes" || fail "5a routes: $(head -1 "${WORK}/o")"
run "5b servepages under --allow-read=<g>/pages,<g>/tmpl" admit "${G}" "${PG}" count "${SRP}"
/usr/bin/grep -a -q "^pages-done 2$" "${WORK}/o" && pass "5b 2 routes: the symlinked directory is skipped" || fail "5b routes: $(head -1 "${WORK}/o")"
# serve LABEL FLAGS...: start the pages server, fetch /leak, stop it by PID
serve_leak() {
    ( cd "${G}" && exec "${PG}" serve "$@" --allow-net ) >"${WORK}/so" 2>"${WORK}/se" &
    SRVPID=$!
    local body="" code=""
    leak_answered() { code="$(curl -s --max-time 5 -o "${WORK}/body" -w '%{http_code}' "http://127.0.0.1:${PORT}/leak" 2>/dev/null)" && [ "${code}" != 000 ]; }
    tk_wait_until "${SRVPID}" leak_answered
    kill "${SRVPID}" 2>/dev/null; wait "${SRVPID}" 2>/dev/null; SRVPID=""
    echo "${code}"
}
if lsof -nP -iTCP:"${PORT}" -sTCP:LISTEN >/dev/null 2>&1; then
    fail "5c port ${PORT} is in use; set C1887_PORT"
else
    code="$(serve_leak --allow-read)"
    if [ "${code}" = 200 ] && /usr/bin/grep -a -q OUTLAYOUT-SECRET "${WORK}/body"; then
        pass "5c /leak, whole class: 200 with the layout outside the scope"
    else fail "5c whole class: code=${code} body=$(head -c 80 "${WORK}/body" 2>/dev/null)"; fi
    code="$(serve_leak "${SRP}")"
    if [ "${code}" = 500 ] && ! /usr/bin/grep -a -q OUTLAYOUT-SECRET "${WORK}/body"; then
        pass "5d /leak under the scope: 500, the outside layout not read"
    else fail "5d scoped: code=${code} body=$(head -c 80 "${WORK}/body" 2>/dev/null)"; fi
fi

# ── D. log.openaccess ───────────────────────────────────────────────────
run "6a log <g>/logs/app.log, --allow-write=<g>/logs/app.log (the file only)" deny "${G}" "${P}" log "${G}/logs/app.log" "--allow-write=${G}/logs/app.log"
if /usr/bin/grep -a -q "^CAP002: capability 'fs.write' is granted, but not for '${G}/logs'$" "${WORK}/e" \
   && /usr/bin/grep -a -q "^  grant this scope at run time with --allow-write=${G}/logs$" "${WORK}/e"; then
    pass "6a names the log's directory, and the flag that admits it"
else fail "6a: $(head -2 "${WORK}/e" | tr '\n' '|')"; fi
if ls -A "${G}/logs" | /usr/bin/grep -q .; then fail "6a files created: $(ls -A "${G}/logs")"; else pass "6a nothing created beside the log"; fi
run "6b the same, --allow-write=<g>/logs" admit "${G}" "${P}" log "${G}/logs/app.log" "--allow-write=${G}/logs"
if [ -e "${G}/logs/.app.log.lock" ]; then pass "6b its sidecar is inside the scope"; else fail "6b no sidecar: $(ls -A "${G}/logs")"; fi
run "6c <g>/outdir/app.log, whole class" admit "${G}" "${P}" log "${G}/outdir/app.log" --allow-write
run "6d the 6a run, TK_DIAG_JSON=1" deny "${G}" env TK_DIAG_JSON=1 "${P}" log "${G}/logs/app.log" "--allow-write=${G}/logs/app.log"
if /usr/bin/grep -a '^{"diagnostic_id":"CAP002"' "${WORK}/e" | /usr/bin/grep -q -F "\"fix\":\"--allow-write=${G}/logs\"}"; then
    pass "6d the record's fix is exactly --allow-write=<g>/logs"
else fail "6d JSON record: $(/usr/bin/grep -a '^{' "${WORK}/e" | head -1 | sed "s#${WORK}#<work>#g")"; fi

# ── E. a fix offered is one that works on its own (AGENTS §6) ───────────
# Round-4 review: the target was checked before the way there, so a path
# that ENDS outside the scope and also passes through an outside directory
# got the target's fix -- and applying it stopped at the transit denial.
# The way there is now checked first (its denial has no fix), and a log's
# directory before the log.  fixworks: run with TK_DIAG_JSON=1; if the
# CAP002 record carries a fix, apply it (a run-time flag: append the scope to
# the grant) and the call must then be admitted.
mkdir -p "${G}/bin2" "${G}/logs2"
echo o >"${G}/other/f.txt"
printf '#!/bin/sh\nexit 0\n' >"${G}/bin2/tool"; chmod +x "${G}/bin2/tool"
echo l >"${G}/outdir/app2.log"; ln -s "${G}/outdir/app2.log" "${G}/logs2/app.log"
# fixworks LABEL EXPECT(fix|nofix) DIR FLAG GRANT ARGS...
fixworks() {
    local label="$1" expect="$2" dir="$3" flag="$4" grant="$5"; shift 5
    local rc=0
    ( cd "${dir}" && TK_DIAG_JSON=1 "${P}" "$@" "${flag}=${grant}" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    local rec; rec="$(/usr/bin/grep -a '^{"diagnostic_id":"CAP002"' "${WORK}/e" | head -1)"
    if [ "${rc}" -ne 1 ] || [ -z "${rec}" ]; then
        fail "${label}: wanted a CAP002 record, got rc=${rc}"; return
    fi
    local fix; fix="$(printf '%s' "${rec}" | sed -n 's/.*"fix":"\([^"]*\)".*/\1/p')"
    if [ -z "${fix}" ]; then
        if [ "${expect}" = nofix ]; then pass "${label}: no fix ($(printf '%s' "${rec}" | sed "s#${WORK}#<work>#g" | cut -c40-150))"
        else fail "${label}: wanted a fix: $(printf '%s' "${rec}" | sed "s#${WORK}#<work>#g")"; fi
        return
    fi
    if [ "${expect}" = nofix ]; then fail "${label}: offered '${fix//${WORK}/<work>}', wanted none"; return; fi
    case "${fix}" in "${flag}="*) ;; *) fail "${label}: fix '${fix}' is not ${flag}"; return ;; esac
    rc=0
    ( cd "${dir}" && "${P}" "$@" "${flag}=${grant},${fix#"${flag}="}" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    if [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: its fix ${fix//${WORK}/<work>} admits the call"
    else fail "${label}: applying '${fix//${WORK}/<work>}' still fails: $(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g")"; fi
}
fixworks "7a read www/../outside/../other/f.txt (ends outside, passes outside)" nofix "${G}" --allow-read "${G}/www" read www/../outside/../other/f.txt
/usr/bin/grep -a -q "^CAP002: capability 'fs.read' is granted, but not for '${G}/outside'$" "${WORK}/e" \
    && pass "7a names the directory passed through" || fail "7a: $(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g")"
fixworks "7b read www/../nosuch/../other/f.txt (through a missing directory)" nofix "${G}" --allow-read "${G}/www" read www/../nosuch/../other/f.txt
fixworks "7c glob www/../outside/../other/* (its literal prefix)" nofix "${G}" --allow-read "${G}/www" glob 'www/../outside/../other/*'
fixworks "7d write www/../outside/../other/n.txt" nofix "${G}" --allow-write "${G}/www" write www/../outside/../other/n.txt
fixworks "7e spawn bin/../outside/../bin2/tool" nofix "${G}" --allow-run "${G}/bin/tool" spawn bin/../outside/../bin2/tool
/usr/bin/grep -a -q "^CAP002: capability 'process.spawn' is granted, but not for '${G}/outside'$" "${WORK}/e" \
    && pass "7e names the directory passed through" || fail "7e: $(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g")"
# where the way there is admitted, the target's fix still comes, and works
fixworks "7f read other/f.txt (outside, no transit)" fix "${G}" --allow-read "${G}/www" read other/f.txt
fixworks "7g spawn bin2/tool" fix "${G}" --allow-run "${G}/bin/tool" spawn bin2/tool
fixworks "7h log <g>/logs/app.log under --allow-write=<g>/www (file and directory outside)" fix "${G}" --allow-write "${G}/www" log "${G}/logs/app.log"
# a log that is a symlink leading out of its directory: the directory's
# grant would not admit the file, so the directory's denial has no fix ...
fixworks "7i log <g>/logs2/app.log -> <g>/outdir/app2.log, --allow-write=<g>/www" nofix "${G}" --allow-write "${G}/www" log "${G}/logs2/app.log"
/usr/bin/grep -a -q "^  the file does not resolve inside this directory" "${WORK}/e" \
    && pass "7i says why no fix is offered" || fail "7i: $(sed -n 2p "${WORK}/e" | sed "s#${WORK}#<work>#g")"
# ... and once the directory is granted, the file's fix completes it
fixworks "7j the same, --allow-write=<g>/logs2" fix "${G}" --allow-write "${G}/logs2" log "${G}/logs2/app.log"

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
