#!/usr/bin/env bash
# C535_prefixed_export_str_returns.sh — a str returned by a stdlib export
# whose prefix is not its module's name (`tpl.render`, `tpl.escape` in
# std.template) interpolates as text, not as its address (story 127.242
# item 1).
#
# THE DEFECT.  llvm.c's tki_entry_for() looked an export up by the imported
# module's name ("template"), but std.template's exports are prefixed "tpl".
# No entry was found, so the call's result carried no "$str" tag: the value
# of `tpl.escape(x)` and the `$ok:s` binding of `tpl.render(t;v)` were
# interpolated through the integer path -- "got 4305626416", exit 0.  A bare
# `$ok:s s` handed to io.println printed correctly, which hid it.
#
# NOW: when the module name finds nothing, the export prefix is matched
# against the alias, within the file the alias imports (138.6's rule for
# call_declares_bool).
#
# Negative control: TKC=<main d3a083b8's toke> -- cases 1-3 print decimal
# addresses.  Case 4 (the bare binding) is green on both.
#
# Story: 127.242
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
cat > t.tk <<'EOF2'
m=t;
i=io:std.io;
i=tpl:std.template;
f=main():i64{
  let t=mt tpl.compile("hi {{name}}") { $ok:x x; $err:e $tmpl{id:0;src:""} };
  let v=tpl.vars(@("name":"bob"));
  let a=mt tpl.render(t;v) { $ok:s "1 got \(s)"; $err:e "err" };
  io.println(a);
  let r=tpl.render(t;v);
  let b=mt r { $ok:s "2 got \(s)"; $err:e "err" };
  io.println(b);
  let e=tpl.escape("<a>");
  io.println("3 \(e) \(tpl.escape("&"))");
  io.println(mt tpl.render(t;v) { $ok:s s; $err:e "err" });
  <0
};
EOF2
for o in -O0 -O2; do
    rm -f b
    if "${TKC}" --allow-all "${o}" --out b t.tk >/dev/null 2>&1; then ./b > out 2>&1; else echo build-failed > out; fi
    expect "1 \$ok:s of tpl.render interpolated ${o}" "$(sed -n 1p out)" "1 got hi bob"
    expect "2 the same, let-bound ${o}" "$(sed -n 2p out)" "2 got hi bob"
    expect "3 tpl.escape interpolated ${o}" "$(sed -n 3p out)" "3 &lt;a&gt; &amp;"
    expect "4 control: bare binding ${o}" "$(sed -n 4p out)" "hi bob"
done
echo "C535: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
