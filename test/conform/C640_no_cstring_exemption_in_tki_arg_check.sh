#!/usr/bin/env bash
# C640 -- check_tki_arg_types has no "str where the glue reads a C string"
# exemption any more (127.255).
#
# 177cb91e let a str through wherever the glue body reads a C string ('S' in
# stdlib_paramkind_gen.h), whatever the .tki record declared, because
# http.stream was declared (httpclient) while its glue took a URL.  Y1 made
# the two agree (C295 case 11) and a sweep of every hand-written record found
# no other record declaring a non-str where the glue reads a C string, so the
# exemption was deleted.  Such a call is now judged against the record.
#
# No shipped .tki has the disagreement any more, so the case builds one: a
# copy of the stdlib interfaces in which http.geturl's str parameter is
# declared `httpclient`.  --check reads the .tki at
# $TKC_STDLIB_DIR/../../stdlib, so a scratch root with that layout (and the
# tk_runtime.c marker the stdlib-dir probe asks for) is enough.
#
# Negative control: TKC=<a build with the exemption, e.g. 2aa48c88> -- case 1
# fails (rc=0: the str is let through).
#
# Story: 127.255

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
mkdir -p "${WORK}/root/src/stdlib" "${WORK}/root/stdlib"
: > "${WORK}/root/src/stdlib/tk_runtime.c"
cp "${REPO_ROOT}"/stdlib/*.tki "${WORK}/root/stdlib/"
python3 - "${WORK}/root/stdlib/http.tki" <<'PY'
import json, sys
p = sys.argv[1]; d = json.load(open(p)); n = 0
for e in d["exports"]:
    if e.get("kind") == "func" and e.get("name") == "http.geturl":
        assert e.get("params") == ["str"], e
        e["params"] = ["httpclient"]; n += 1
assert n == 1
json.dump(d, open(p, "w"), indent=2)
PY
# code|got of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$2" > "${WORK}/p.tk"
    TKC_STDLIB_DIR="$1" "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s" % (d.get("error_code"), d.get("got")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
PROG='m=q;i=h:std.http;f=main():i64{let r=h.geturl("http://x");<0};'

# The glue reads argument 1 as a C string ('S'); the record says httpclient.
expect "1 a str where the record declares a record is E4031 though the glue reads a C string" \
    "$(diags "${WORK}/root/src/stdlib" "${PROG}")" "E4031|str"
# Control: the shipped interface declares str there, so the same call is clean.
expect "2 the same call against the shipped http.tki is clean" \
    "$(diags "${REPO_ROOT}/src/stdlib" "${PROG}")" "rc=0"

echo "C640: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
