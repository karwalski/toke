#!/usr/bin/env bash
# C611_template_context_escaping.sh — tpl.render and tpl.renderfile escape a
# value for the HTML context its slot sits in (story 121.37, ADR-0011 (b)).
#
# THE DEFECT.  124.3 made tpl.render escape every value with the HTML-body
# escaper wherever the slot sat, and left tpl.renderfile not escaping at all.
# Measured on main f605aec9, u="javascript:alert(1)", x="');alert(1);//":
#   tpl.render      <a href="javascript:alert(1)"                  (runs)
#                   onclick="f('&#39;);alert(1);//')"  -- the browser
#                   decodes &#39; before the handler runs          (runs)
#                   <script>var s=&#39;);alert(1);//;
#   tpl.renderfile  every value verbatim, <b> included.
#
# THE CONTRACT (docs/stdlib/template.md "Escaping"): the escaper is chosen
# from the slot's position -- text, attribute, URL attribute, on* / <script>
# (JavaScript), style / <style> (CSS), tag or attribute name.  {{v|raw}} is
# the explicit opt-out.
#
# NEGATIVE CONTROL: a trusted value through {{v|raw}} is emitted verbatim in
# the same positions, so the harness would see an unescaped payload.
# Each case runs through tpl.render and through tpl.renderfile (same
# template text, written to a file), at -O0 and -O2.
#
# Story: 121.37

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c611_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C611: template values are escaped for their HTML context"
echo "--------------------------------------"

# The template, one line; read by tpl.renderfile from page.tkt and passed to
# tpl.compile as a toke string literal (quotes escaped for toke).
TEMPLATE='<a href="{{u}}" onclick="f('"'"'{{x}}'"'"')" title={{t}}>{{b}}</a><script>var s={{x}};</script><style>p{color:{{c}}}</style><a href="{{u|raw}}">'
printf '%s' "${TEMPLATE}" > page.tkt
TOKE_TEMPLATE="${TEMPLATE//\"/\\\"}"

cat > c.tk <<EOF
m=p;
i=io:std.io;
i=tpl:std.template;
f=main():i64{
  let vars=tpl.vars(@("u":"javascript:alert(1)";"x":"');alert(1);//";"t":"a onmouseover=alert(1)";"b":"<b>";"c":"red;background:url(javascript:x)"));
  let t=mt tpl.compile("${TOKE_TEMPLATE}"){\$ok:t t;\$err:e \$tmpl{id:0;src:""}};
  io.println(mt tpl.render(t;vars){\$ok:s s;\$err:e e.msg});
  io.println(mt tpl.renderfile("page.tkt";vars){\$ok:s s;\$err:e e.msg});
  <0
};
EOF

WANT='<a href="#ZtokeZ" onclick="f('"'"'\u0027\u0029\u003Balert\u00281\u0029\u003B\u002F\u002F'"'"')" title=a&#32;onmouseover&#61;alert&#40;1&#41;>&lt;b&gt;</a><script>var s="\u0027\u0029\u003Balert\u00281\u0029\u003B\u002F\u002F";</script><style>p{color:ZtokeZ}</style><a href="javascript:alert(1)">'

for opt in -O0 -O2; do
    rm -f c_bin
    if ! "${TKC}" --allow-all "${opt}" --out c_bin c.tk >compile.log 2>&1; then
        echo "  FAIL: ${opt}: did not compile: $(grep -o '"error_code":"[^"]*"' compile.log | head -1)"
        FAIL=$((FAIL + 2)); continue
    fi
    ./c_bin > out.txt 2>&1
    n=0
    for label in tpl.render tpl.renderfile; do
        n=$((n + 1))
        got="$(sed -n "${n}p" out.txt)"
        if [ "${got}" = "${WANT}" ]; then
            echo "  PASS: ${label} ${opt}: every slot escaped for its context"
            PASS=$((PASS + 1))
        else
            echo "  FAIL: ${label} ${opt}"
            echo "        want: ${WANT}"
            echo "        got:  ${got}"
            FAIL=$((FAIL + 1))
        fi
        # negative control: the |raw slot is the unescaped payload
        case "${got}" in
            *'<a href="javascript:alert(1)">') echo "  PASS: ${label} ${opt}: NEGATIVE CONTROL |raw emits the payload verbatim"; PASS=$((PASS + 1)) ;;
            *) echo "  FAIL: ${label} ${opt}: |raw did not emit the payload verbatim"; FAIL=$((FAIL + 1)) ;;
        esac
    done
done

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
