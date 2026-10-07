#!/usr/bin/env bash
# C732 -- the stdlib .tki type names are lower-case, and the old spellings
# get a structured E3011 whose `fix` is the new name (story 127.251).
#
# THE DEFECT, measured on main (2df8369d):
#   * 40 exported types across 19 .tki files were upper-case (FileErr, DbErr,
#     Json, Row, Vec, ...).  A `$` type name is lower-case -- `$FileErr` is
#     E2002 -- so they could be written only bare (`e:DbErr`), a spelling the
#     default profile is meant to retire, and `$fileerr` (what file.md
#     documents) was E3011.  check_tki_nameable.py exempted type names.
#   * Seven of them lower-case to a module or a sub-namespace (json, row,
#     toon, yaml, vec, vecstore) or a time.* function (duration).  The module
#     pages documented `$json`, `$row`, `$toon`, `$yaml`, and those compiled
#     -- not because they named the type, but because a `$name` that found
#     only the import alias resolved and nothing checked it: `f=g(b:$json)`
#     then accepted a str (a type hole, part 4).
#
# NOW: the 40 are lower-cased, and the seven take `<module><noun>` (owner
# decision 2026-10-01 §2.5, the i18nbundle/tlsconfig precedent): jsonval,
# dbrow, toonval, yamlval, vechandle, vecstorehandle, timeduration.  An old
# spelling in a type position is E3011 with `fix` "did you mean '$new'?"
# (no sigil in the fix when the source already wrote one) -- offered only
# when the renamed type is in scope, i.e. its module is imported.  A `$name`
# whose only declaration is a module alias is E3011.  `tkc --migrate`
# rewrites the old spellings.  check_tki_nameable.py rejects an upper-case
# type name.
#
# WHAT IS ASSERTED.
#   1. every new name is a type with its module imported (40 cases)
#   2. every old bare spelling is E3011 with the deterministic fix (40)
#   3. the old `$` spellings of the seven are E3011, with the fix except
#      for `$row` (std.ml declares its own row, so no fix is right always)
#   4. the type hole: `$json` no longer accepts a str
#   5. negative controls: a std type that shares its alias's name
#      (canvas.canvas), ml's own `$row` beside std.db, no fix without the
#      module imported
#   6. --migrate rewrites bare and `$` old spellings; the result checks clean
#   7. end to end: $jsonval / $dbrow / $fileerr parameters build and run
#   8. the gate: check_tki_nameable.py passes the tree and fails an
#      upper-case type name in a fixture
#
# Negative control: TKC=<main 2df8369d's toke> TKC_STDLIB_DIR=<main's
# src/stdlib> -- 95 of 101 fail: all of parts 1-4, 6 and 7 (the six that
# pass are part 5's three regression controls and part 8, a script check).
#
# Story: 127.251
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c732_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
# code|message|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("message"), d.get("fix") or "ABSENT"))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

echo "C732: stdlib .tki type names are lower-case (127.251)"
echo "-----------------------------------------------------"

# old  new  module
TABLE='ArgsErr argserr args
AuthErr autherr auth
BracketErr bracketerr str
DbErr dberr db
DecryptResult decryptresult encrypt
Duration timeduration time
EncodingErr encodingerr encoding
EnvErr enverr env
FileErr fileerr file
Handle handle process
I18nErr i18nerr i18n
Json jsonval json
JsonErr jsonerr json
JsonStream jsonstream json
JsonStreamErr jsonstreamerr json
JsonToken jsontoken json
JsonWriter jsonwriter json
JwtClaims jwtclaims auth
Keypair keypair encrypt
MdErr mderr md
MdnsErr mdnserr mdns
ParseErr parseerr str
ProcessErr processerr process
RegexErr regexerr str
Row dbrow db
SearchResult searchresult vecstore
SecureBuf securebuf securemem
SliceErr sliceerr str
StrBuf strbuf str
TimeErr timeerr time
TimeParts timeparts time
Toon toonval toon
ToonErr toonerr toon
Vec vechandle vec
VecCollection veccollection vecstore
VecEntry vecentry vecstore
VecErr vecerr vecstore
VecStore vecstorehandle vecstore
Yaml yamlval yaml
YamlErr yamlerr yaml'

# ── 1 + 2: each new name resolves; each old bare spelling is E3011 + fix ──
n=0
while read -r old new mod; do
    n=$((n + 1))
    expect "1 \$${new} is a std.${mod} type" \
        "$(diags "m=q;i=${mod}:std.${mod};f=g(b:\$${new}):i64{<1};f=main():i64{<0};")" "rc=0"
    expect "2 bare ${old} is E3011, fix \$${new}" \
        "$(diags "m=q;i=${mod}:std.${mod};f=g(b:${old}):i64{<1};f=main():i64{<0};")" \
        "E3011|identifier '${old}' is not declared|did you mean '\$${new}'?"
done <<< "${TABLE}"
echo "  parts 1-2: ${n} names"

# ── 3: the `$` spellings the module pages documented ──
for row in "json jsonval json" "toon toonval toon" "yaml yamlval yaml" \
           "vec vechandle vec" "vecstore vecstorehandle vecstore"; do
    set -- ${row}
    expect "3 \$$1 (a module alias) is E3011, fix $2" \
        "$(diags "m=q;i=$3:std.$3;f=g(b:\$$1):i64{<1};f=main():i64{<0};")" \
        "E3011|no type '$1' is declared: '$1' is a module alias, not a type|did you mean '$2'?"
done
expect "3 \$row beside std.db is E3011 with NO fix: std.ml's row is the other reading" \
    "$(diags 'm=q;i=db:std.db;f=g(b:$row):i64{<1};f=main():i64{<0};')" \
    "E3011|no type 'row' is declared: 'row' is a module alias, not a type|ABSENT"
expect "3 \$duration (a time.* function, not a type) is E3011, fix timeduration" \
    "$(diags 'm=q;i=time:std.time;f=g(b:$duration):i64{<1};f=main():i64{<0};')" \
    "E3011|identifier 'duration' is not declared|did you mean 'timeduration'?"
expect "3 a \$json struct literal is E3011 too" \
    "$(diags 'm=q;i=json:std.json;f=main():i64{let j=$json{raw:"{}"};<0};')" \
    "E3011|no type 'json' is declared: 'json' is a module alias, not a type|did you mean 'jsonval'?"

# ── 4: the type hole ──
expect "4 \$jsonval rejects a str (the control)" \
    "$(diags 'm=q;i=json:std.json;f=g(b:$jsonval):i64{<1};f=main():i64{<g("x")};' | cut -d'|' -f1-2)" \
    "E4031|type mismatch: expected 'jsonval', got 'str'"
expect "4 \$json no longer types a parameter that takes a str" \
    "$(diags 'm=q;i=json:std.json;f=g(b:$json):i64{<1};f=main():i64{<g("x")};' | cut -d'|' -f1)" \
    "E3011"

# ── 5: negative controls ──
expect "5 a std type that shares its module's alias (canvas.canvas) still resolves" \
    "$(diags 'm=q;i=canvas:std.canvas;f=g(c:$canvas):i64{<1};f=main():i64{<0};')" "rc=0"
expect "5 std.ml's own \$row beside std.db's row.* namespace" \
    "$(diags 'm=q;i=db:std.db;i=ml:std.ml;f=g(r:$row):i64{<1};f=main():i64{<g($row{vals:@(1.0)})};')" "rc=0"
expect "5 no fix naming a type the program has not imported" \
    "$(diags 'm=q;f=g(b:DbErr):i64{<1};f=main():i64{<0};' | grep -c "did you mean '\$dberr'")" "0"
expect "5 a user's own upper-case-free type is unaffected" \
    "$(diags 'm=q;i=db:std.db;t=$dberr2{msg:$str};f=g(b:$dberr2;c:$dberr):i64{<1};f=main():i64{<0};')" "rc=0"

# ── 6: --migrate ──
cat > m.tk <<'TK'
m=q;
i=json:std.json;
i=db:std.db;
f=g(b:Json;e:DbErr):i64{ <1 };
f=h(b:$json):$row!$dberr{ <db.one("SELECT 1";@()) };
f=main():i64{ <0 };
TK
"${TKC}" --migrate m.tk > m.out 2>m.err
expect "6 --migrate: bare and \$ old spellings rewritten" "$(sed -n '4,5p' m.out)" \
'f=g(b:$jsonval;e:$dberr):i64{ <1 };
f=h(b:$jsonval):$dbrow!$dberr{ <db.one("SELECT 1";@()) };'
cp m.out m2.tk
expect "6 --migrate: the result checks clean" "$("${TKC}" --check m2.tk >/dev/null 2>&1; echo $?)" "0"
cat > u.tk <<'TK'
m=q;
i=json:std.json;
t=$json{raw:$str};
f=g(b:Json;c:$json):i64{ <1 };
f=main():i64{ <0 };
TK
"${TKC}" --migrate u.tk > u.out 2>/dev/null
expect "6 --migrate: a program's own json type is not renamed" "$(sed -n '4p' u.out)" \
    'f=g(b:$json;c:$json):i64{ <1 };'

# ── 7: end to end ──
cat > e.tk <<'TK'
m=e;
i=io:std.io;
i=str:std.str;
i=json:std.json;
i=file:std.file;
f=k(j:$jsonval):i64{ <mt json.i64(j;"a") {$ok:n n;$err:e 0-1} };
f=why(e:$fileerr):str{ <mt e {$notfound:m "notfound";$permission:m "perm";$io:m "io";$invalid:m "inv";$isdir:m "isdir";$notregular:m "notreg";$symlink:m "sym";$toolarge:m "big";$nomem:m "nomem";$badarg:m "badarg"} };
f=main():i64{
  let o=mt json.dec("{\"a\":7}") {$ok:v v;$err:e $jsonval{raw:"{}"}};
  io.println(str.fromint(k(o)));
  io.println(mt file.read("/nonexistent/c732") {$ok:s s;$err:e why(e)});
  <0
};
TK
if "${TKC}" --allow-all --out e e.tk > e.log 2>&1; then
    expect "7 \$jsonval and \$fileerr parameters build and run" "$(./e 2>/dev/null | tr '\n' ' ')" "7 notfound "
else
    expect "7 \$jsonval and \$fileerr parameters build and run" "compile failed: $(head -c 300 e.log)" "7 notfound "
fi

# ── 8: the gate ──
expect "8 check_tki_nameable.py passes the tree" \
    "$(python3 "${REPO_ROOT}/scripts/check_tki_nameable.py" >/dev/null 2>&1; echo $?)" "0"
mkdir -p g/scripts g/stdlib
cp "${REPO_ROOT}/scripts/check_tki_nameable.py" g/scripts/
printf '{"schema_version":"1.0","module":"std.x","exports":[{"kind":"type","name":"XErr","fields":[{"name":"msg","type":"str"}]},{"kind":"func","name":"x.f","params":["str"],"return":"str!XErr"}]}\n' > g/stdlib/x.tki
expect "8 the gate fails an upper-case type name (declared and referenced)" \
    "$(python3 g/scripts/check_tki_nameable.py 2>&1 | grep -c "'XErr'")" "2"
sed -i.bak 's/XErr/xerr/g' g/stdlib/x.tki
expect "8 ... and passes it lower-cased" \
    "$(python3 g/scripts/check_tki_nameable.py >/dev/null 2>&1; echo $?)" "0"

echo ""
echo "C732: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
