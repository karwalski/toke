#!/usr/bin/env bash
# C775_xml_soap_typed_str.sh -- std.xml / std.soap results are typed `str`,
# and a lookup that finds nothing is the empty string (story 131.48).
#
# THE DEFECT.  stdlib/xml.tki and stdlib/soap.tki existed only as gen_tki.py
# output: every export tagged "gen":"abi", every parameter and most returns
# `i64`.  So `xml.get`, `xml.attr`, `xml.cdata`, `xml.comment` and
# `soap.extractfault` typed as i64: interpolating one printed a pointer, and
# `let n:i64=xml.get(...)` was accepted.  Behind that, the glue returned 0
# (NULL) for a missing path, a missing attribute and an envelope with no
# fault, and the next `==` on that "string" read address 0.
#
# THE FIX.  Both interfaces carry the types of docs/spec/stdlib-signatures.md
# (every export `str` -> `str`, except the opaque xml.parse, which stays an
# ABI helper); xml_glue.c / soap_glue.c answer "" where the C core answers
# NULL, as json.getval does for a missing path.
#
# WHAT IS ASSERTED, from a compiled and run program:
#   1. an untyped `let v=xml.get(...)` etc. is a str: interpolation prints
#      its text, not a pointer (on main every one printed a number, since a
#      gen:abi i64 result binds as i64);
#   2. `let n:i64=xml.get(...)` is refused (E4031), so the type is str and
#      not merely an ABI placeholder the checker lets through;
#   3. xml.get on a missing path, xml.attr on a missing attribute and
#      soap.extractfault on a fault-free envelope are "" (len 0), compared
#      with `==` without crashing;
#   4. the found cases still return the right text.
#
# Negative control: C775_STDLIB=<dir> compiles against another tree's
# src/stdlib (the .tki is read from <dir>/../../stdlib).  Against a git
# archive of main's src/stdlib + stdlib (8bb9f63e) the program dies SIGSEGV
# (exit 139, on `.len` of the NULL a miss returned) and the i64 binding is
# accepted: 0 passed, 11 failed.  On this branch: 11 passed.
#
# Story: 131.48
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C775_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c775_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C775: std.xml / std.soap results are str; a miss is \"\" (131.48)"
echo "----------------------------------------------------------------------"

cat > x.tk <<'TOKE'
m=main;
i=io:std.io;
i=xml:std.xml;
i=soap:std.soap;
f=main():i64{
  let doc="<root><name>John</name><a href=\"/x\">t</a></root>";
  let g=xml.get(doc;"root.name");
  io.println("get=[\(g)]");
  let m=xml.get(doc;"root.nope");
  io.println("miss=[\(m)] len=\(m.len) eq=\(m=="")");
  let a=xml.attr("href=\"/x\" id='7'";"id");
  io.println("attr=[\(a)]");
  let am=xml.attr("href=\"/x\"";"id");
  io.println("attrmiss=[\(am)] eq=\(am=="")");
  let c=xml.cdata("x<y");
  io.println("cdata=[\(c)]");
  let k=xml.comment("n");
  io.println("comment=[\(k)]");
  let env=soap.envelope("<P>1</P>";"");
  let b=soap.extractbody(env);
  io.println("body=[\(b)]");
  let nf=soap.extractfault(env);
  io.println("nofault=[\(nf)] eq=\(nf=="")");
  let ft=soap.extractfault(soap.envelope(soap.fault("Server";"E";"d");""));
  io.println("fault=\(ft.len>0)");
  <0
};
TOKE

if ! "${TKC}" --allow-all --out x x.tk > x.log 2>&1; then
    fail "x.tk did not compile"
    sed 's/^/        /' x.log | head -4
else
    ./x > x.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] && pass "program ran to exit 0" || fail "program exited ${rc}"
    for want in "get=[John]" \
                "miss=[] len=0 eq=true" \
                "attr=[7]" \
                "attrmiss=[] eq=true" \
                "cdata=[<![CDATA[x<y]]>]" \
                "comment=[<!-- n -->]" \
                "body=[<P>1</P>]" \
                "nofault=[] eq=true" \
                "fault=true"; do
        if grep -a -q -x -F -- "${want}" x.out; then
            pass "${want}"
        else
            fail "expected '${want}', got '$(grep -a -F -- "${want%%=*}=" x.out | head -1)'"
        fi
    done
fi

cat > n.tk <<'TOKE'
m=main;
i=xml:std.xml;
f=main():i64{ let n:i64=xml.get("<a>J</a>";"a"); <n };
TOKE
if "${TKC}" --check n.tk > n.log 2>&1; then
    fail "let n:i64=xml.get(...) was accepted (result is not typed str)"
elif grep -q '"error_code":"E4031"' n.log; then
    pass "let n:i64=xml.get(...) refused with E4031"
else
    fail "let n:i64=xml.get(...) refused, but not with E4031: $(head -c 200 n.log)"
fi

echo "----------------------------------------------------------------------"
echo "C775: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
