#!/usr/bin/env bash
# C1886_cap_scope_db.sh -- std.db under scoped fs grants: the files a database
# name or an SQL statement opens (story 138.19, round 2 of review).
#
# THE DEFECT (found reviewing 138.19).  db.open is gated on its path, but
#   - an SQLite URI filename ("file:/abs/x.db?mode=ro") was checked as the
#     relative name <cwd>/file:/abs/x.db?mode=ro, which a scope over the
#     working directory admits, while sqlite3_open (the macOS system library
#     honours URIs) opened /abs/x.db: a read outside the scope, and with
#     mode=rwc a database created anywhere;
#   - ATTACH DATABASE '<path>' and VACUUM INTO '<path>' open or create files
#     at any path from SQL on an already-open handle, and the broker never saw
#     them: with fs_read/fs_write scoped to ./data, ATTACH created a file
#     outside ./data and VACUUM INTO wrote a 4096-byte database there.
#
# THE CHANGE.  Under a scoped fs grant a "file:" database name is refused
# (CAP002, no fix: name the database by its path), and each connection gets an
# SQLite authorizer: an ATTACH -- which is also how SQLite runs VACUUM INTO --
# goes through the same fs.read + fs.write gates as db.open, an ATTACH whose
# file name is computed (a bound parameter) is refused, and so is PRAGMA
# temp_store_directory pointed outside the scope.  An empty ATTACH name (a
# private temporary database; plain VACUUM attaches one) opens no named file
# and passes.  Under the whole classes nothing changes.
#
# WHAT IS ASSERTED (cwd = <work>; scope ./data; <work>/outside holds secret.db)
#   1. db.open("file:<outside>/secret.db?mode=ro") under --allow-read=.
#      --allow-write=. -> CAP002 with the URI as its scope and no fix; under
#      the whole classes it opens and db.one reads it (the negative control
#      inside this script: the name does reach the outside file)
#   2. db.open("file:data/app.db") under ./data -> CAP002 (refused, not
#      misread as <cwd>/file:data/...)
#   3. ATTACH '<outside>/att.db' -> CAP002 naming the canonical path; no file
#      created outside; ATTACH 'data/b.db' admitted and created
#   4. VACUUM INTO '<outside>/vi.db' -> CAP002; nothing written outside;
#      VACUUM INTO 'data/v.db' admitted and written; plain VACUUM admitted
#   5. ATTACH ? with the name bound as a parameter -> CAP002, no fix
#   6. ATTACH 'file:<outside>/secret.db?mode=ro' -> CAP002 (URI)
#   7. PRAGMA temp_store_directory = '<outside>' -> CAP002
#   8. the whole classes: ATTACH '<outside>/att.db' admitted and created
#
# NEGATIVE CONTROL: TKC=<a v3.2.0 toke> -- the scope is ignored there, so
# every denial (1-7) FAILS: the URI opens the outside file and ATTACH /
# VACUUM INTO create files outside ./data.
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
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1886_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"
echo "C1886: std.db under scoped fs grants (138.19)"
echo "------------------------------------------------------------------------"

cat >"${WORK}/dbprobe.tk" <<'EOF'
m=dbprobe;
i=io:std.io;
i=args:std.args;
i=db:std.db;
f=main():i64{
  let p=mt args.get(1) {$ok:v v;$err:e ""};
  let q=mt args.get(2) {$ok:v v;$err:e ""};
  let b=mt args.get(3) {$ok:v v;$err:e ""};
  let h=mt db.open(p) {$ok:v 1;$err:e 2};
  io.println("open \(h)");
  if(q=="read"){
    let r=mt db.one("SELECT x FROM s"; @()) {$ok:v 1;$err:e 2};
    io.println("read-done \(r)");
  }el{
    if(b!=""){
      let n=mt db.exec(q; @(b)) {$ok:v 1;$err:e 2};
      io.println("exec-done \(n)");
    }el{
      let n=mt db.exec(q; @()) {$ok:v 1;$err:e 2};
      io.println("exec-done \(n)");
    };
  };
  <0
};
EOF
if ! "${TKC}" --out "${WORK}/dbprobe" "${WORK}/dbprobe.tk" >"${WORK}/build.log" 2>&1; then
    echo "  FAIL: probe did not build"; sed 's/^/      | /' "${WORK}/build.log" | head -5; exit 1
fi
P="${WORK}/dbprobe"
O="${WORK}/outside"; mkdir -p "${WORK}/data" "${O}"
# A database outside the scope, made with the whole classes.
( cd "${WORK}" && "${P}" "${O}/secret.db" "CREATE TABLE s(x TEXT); INSERT INTO s VALUES('top')" \
    --allow-read --allow-write ) >/dev/null 2>&1
if [ ! -s "${O}/secret.db" ]; then echo "  FAIL: could not create the outside database"; exit 1; fi
SC=(--allow-read=./data --allow-write=./data)

# run LABEL admit|deny ARGS...: admit = exit 0 and "-done"; deny = exit 1,
# CAP002, and no "-done".  Runs with TK_DIAG_JSON=1 in <work>.
run() {
    local label="$1" want="$2"; shift 2
    local rc=0
    ( cd "${WORK}" && TK_DIAG_JSON=1 "$@" ) >"${WORK}/o" 2>"${WORK}/e" || rc=$?
    if [ "${want}" = admit ] && [ "${rc}" -eq 0 ] && /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: admitted ($(/usr/bin/grep -a -- "-done" "${WORK}/o" | head -1))"
    elif [ "${want}" = deny ] && [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "^CAP002: capability 'fs\." "${WORK}/e" \
         && ! /usr/bin/grep -a -q -- "-done" "${WORK}/o"; then
        pass "${label}: denied ($(head -1 "${WORK}/e" | sed "s#${WORK}#<work>#g" | cut -c1-110))"
    else
        fail "${label}: wanted ${want}, got rc=${rc}"
        sed 's/^/      | /' "${WORK}/o" "${WORK}/e" | head -5
    fi
}
# jfield FIELD: that field of the JSON line on stderr ("<absent>" if missing)
jfield() {
    python3 -I - "$1" "${WORK}/e" <<'PY'
import json, sys
field, path = sys.argv[1], sys.argv[2]
rec = None
for line in open(path, encoding="utf-8", errors="replace"):
    if line.startswith("{"):
        rec = json.loads(line)
print("<nojson>" if rec is None else rec.get(field, "<absent>"))
PY
}
nofile() {   # LABEL PATH
    if [ -e "$2" ]; then fail "$1: $(basename "$2") was created"; else pass "$1: nothing created"; fi
}

# 1 URI filename
URI="file:${O}/secret.db?mode=ro"
run "1a db.open(\"file:<outside>/secret.db?mode=ro\") under ./ scopes" deny "${P}" "${URI}" read --allow-read=. --allow-write=.
if [ "$(jfield scope)" = "${URI}" ] && [ "$(jfield fix)" = "<absent>" ]; then
    pass "1a CAP002 scope is the URI as given, and there is no fix"
else fail "1a scope '$(jfield scope)' fix '$(jfield fix)'"; fi
run "1b the same URI under the whole classes reads the outside database" admit "${P}" "${URI}" read --allow-read --allow-write
# 2 a URI inside the scope is refused too
run "2 db.open(\"file:data/app.db\") under ./data" deny "${P}" "file:data/app.db" "SELECT 1" "${SC[@]}"
nofile "2" "${WORK}/data/app.db"

# 3 ATTACH
run "3a ATTACH '<outside>/att.db'" deny "${P}" data/app.db "ATTACH DATABASE '${O}/att.db' AS a" "" "${SC[@]}"
if [ "$(jfield scope)" = "${O}/att.db" ]; then pass "3a CAP002 names the canonical path"; else fail "3a scope '$(jfield scope)'"; fi
nofile "3a" "${O}/att.db"
run "3b ATTACH 'data/b.db'" admit "${P}" data/app.db "ATTACH DATABASE 'data/b.db' AS b" "" "${SC[@]}"
if [ -e "${WORK}/data/b.db" ]; then pass "3b data/b.db created"; else fail "3b data/b.db missing"; fi

# 4 VACUUM INTO
run "4a VACUUM INTO '<outside>/vi.db'" deny "${P}" data/app.db "VACUUM INTO '${O}/vi.db'" "" "${SC[@]}"
nofile "4a" "${O}/vi.db"
run "4b VACUUM INTO 'data/v.db'" admit "${P}" data/app.db "VACUUM INTO 'data/v.db'" "" "${SC[@]}"
if [ -s "${WORK}/data/v.db" ]; then pass "4b data/v.db written"; else fail "4b data/v.db missing"; fi
run "4c plain VACUUM (attaches a private temporary database)" admit "${P}" data/app.db "VACUUM" "" "${SC[@]}"

# 5 a computed ATTACH name
run "5 ATTACH ? AS c (the name bound as a parameter)" deny "${P}" data/app.db "ATTACH DATABASE ? AS c" "${O}/att5.db" "${SC[@]}"
if [ "$(jfield fix)" = "<absent>" ]; then pass "5 no fix"; else fail "5 fix '$(jfield fix)'"; fi
nofile "5" "${O}/att5.db"

# 6 a URI in ATTACH
run "6 ATTACH 'file:<outside>/secret.db?mode=ro'" deny "${P}" data/app.db "ATTACH '${URI}' AS s" "" "${SC[@]}"

# 7 temp_store_directory
run "7 PRAGMA temp_store_directory = '<outside>'" deny "${P}" data/app.db "PRAGMA temp_store_directory = '${O}'" "" "${SC[@]}"

# 8 the whole classes
run "8 ATTACH '<outside>/att.db' under the whole classes" admit "${P}" data/app.db "ATTACH DATABASE '${O}/att.db' AS a" "" --allow-read --allow-write
if [ -e "${O}/att.db" ]; then pass "8 <outside>/att.db created"; else fail "8 <outside>/att.db missing"; fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
