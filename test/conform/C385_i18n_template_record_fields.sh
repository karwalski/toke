#!/usr/bin/env bash
# C385_i18n_template_record_fields.sh — std.i18n's $i18nbundle and
# std.template's $tmpl/$tmplvars read what their names say, and literals of
# them work with the functions that take them (story 135.30).
#
# THE DEFECT.  tkc reads `x.f` as slot <index of f in the .tki> of the
# block a record handle points at.
#   - i18n.load returned the bundle's `data` pointer itself, unboxed, so
#     `b.data` read the bundle text's first 8 bytes as a pointer (SIGSEGV).
#   - tpl.compile returned the compiled TkTmpl *, so `t.src` read its
#     internals -- and tpl.render compiled that pointer AS SOURCE TEXT, so
#     a compiled template never rendered.  tpl.vars returned the map itself,
#     so `v.entries` read the map's internals.
#
# THE FIX (i18n.h, i18n_glue.c, template.h, template_glue.c).  i18n.load
# returns a pointer to an I18nBundle {data}; tpl.compile a TkTmplRec {id;
# src}, tpl.render compiles its src; tpl.vars a TkTmplVarsRec {entries =
# the map}.  All marked tki-record; make check-tki-layout checks them
# (C389).
#
# WHAT IS ASSERTED:
#   1. i18n.load: .data is the file's text; i18n.get / i18n.fmt still work
#   2. an $i18nbundle literal through i18n.get
#   3. tpl.compile: .id (1, then 2), .src
#   4. tpl.vars: .entries is the map (.len and .get, through a typed let:
#      `v.entries.len` straight off the field reads 0 and `.get` crashes,
#      because codegen does not dispatch a map-typed .tki field as a map --
#      a compiler defect reported with 135.30, not a layout one)
#   5. tpl.render of a compiled template, and of a $tmpl literal
#   6. NEGATIVE CONTROL: another template and other vars render differently,
#      and a value is HTML-escaped (124.3), so nothing is canned
#
# C385_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: 0/9 -- both programs die with SIGSEGV.
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C385_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c385_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# run NAME: build NAME.tk and run it; prints its stdout lines joined by '|',
# then " rc=<exit status>" (a crash shows as rc=139, a build failure as
# "compile failed: <first error>").
run() {
    if ! "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1; then
        echo "compile failed: $(grep -m1 '^error' "$1.log")"; return
    fi
    local out rc
    out=$(./"$1" 2>"$1.err"); rc=$?
    echo "$(printf '%s' "${out}" | tr '\n' '|') rc=${rc}"
}
echo "C385: std.i18n and std.template record fields (135.30)"
echo "------------------------------------------------------------"
mkdir -p lang
printf '{"greeting":"Bonjour","welcome":"Salut {name}, {count}"}' > lang/ui.fr.json
cat > i.tk <<'TK'
m=i;
i=io:std.io;
i=i18n:std.i18n;
f=main():i64{
  let ui=mt i18n.load("lang/ui";"fr") {$ok:u u;$err:e $i18nbundle{data:""}};
  io.println(ui.data);
  io.println(i18n.get(ui;"greeting"));
  io.println(i18n.fmt(ui;"welcome";"name=Alice|count=5"));
  let lit=$i18nbundle{data:"{\"greeting\":\"Hallo\"}"};
  io.println(i18n.get(lit;"greeting"));
  <0
};
TK
got="$(run i)"
f() { printf '%s' "${got% rc=*}" | cut -d'|' -f"$1"; }
expect "1a i18n.load: .data is the bundle text" "$(f 1)" '{"greeting":"Bonjour","welcome":"Salut {name}, {count}"}'
expect "1b i18n.get / i18n.fmt on it" "$(f 2)|$(f 3)" "Bonjour|Salut Alice, 5"
expect "2  an \$i18nbundle literal through i18n.get" "$(f 4)" "Hallo"
expect "   exit status (i18n)" "${got##* }" "rc=0"

cat > t.tk <<'TK'
m=t;
i=io:std.io;
i=str:std.str;
i=tpl:std.template;
f=main():i64{
  let t=mt tpl.compile("Hi {{name}}!") {$ok:v v;$err:e $tmpl{id:0;src:""}};
  let u=mt tpl.compile("Bye {{name}}.") {$ok:v v;$err:e $tmpl{id:0;src:""}};
  io.println(str.concat(str.fromint(t.id as i64); str.concat(" "; str.concat(str.fromint(u.id as i64); str.concat(" "; t.src)))));
  let m=@("name":"Bob");
  let v=tpl.vars(m);
  let e:@($str:$str)=v.entries;
  io.println(str.concat(str.fromint(e.len as i64); str.concat(" "; mt e.get("name"){$ok:v v;$err:e ""})));
  io.println(mt tpl.render(t;v) {$ok:s s;$err:e "err"});
  io.println(mt tpl.render($tmpl{id:0;src:"Yo {{name}}"};v) {$ok:s s;$err:e "err"});
  io.println(mt tpl.render(u;tpl.vars(@("name":"<Eve>"))) {$ok:s s;$err:e "err"});
  <0
};
TK
got="$(run t)"
expect "3  tpl.compile: .id and .src" "$(f 1)" "1 2 Hi {{name}}!"
expect "4  tpl.vars: .entries is the map" "$(f 2)" "1 Bob"
expect "5  tpl.render: compiled, and a \$tmpl literal" "$(f 3)|$(f 4)" "Hi Bob!|Yo Bob"
expect "6  NEGATIVE CONTROL: other template, other vars, value escaped" "$(f 5)" "Bye &lt;Eve&gt;."
expect "   exit status (template)" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
