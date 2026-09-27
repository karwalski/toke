#!/usr/bin/env bash
# C299_module_get_index_spelling_arity.sh — `alias.get(x)` on a std module is
# arity-checked at --check (story 138.3 / 138.13, GB's finding).
#
# A one-argument `.get(x)` parses as an index expression, so check_tki_call
# never saw `http.get(url)`: it passed --check and failed the build with
# E4026 (http.get is the 2-argument route registration).  The checker now
# asks the glue arity for `<module>.get` exactly as the emitter does and
# reports the same message, naming http.geturl for the client GET.
#
# Negative control: TKC=<main's toke> -- case 1 fails (rc=0).
#
# Story: 138.3, 138.13

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|got|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
msg() { printf '%s\n' "$1" > "${WORK}/m.tk"; "${TKC}" --check "${WORK}/m.tk" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    print(d.get("error_code"), "|", d.get("message")); break'; }

case "$(msg 'm=q;i=http:std.http;f=main():i64{let r:str=http.get("http://127.0.0.1:1/");<0};')" in
  E4026*"route"*"http.geturl"*) echo "PASS 1 http.get(url): E4026 names the route and http.geturl"; PASS=$((PASS+1));;
  *) echo "FAIL 1 http.get(url): E4026 names the route and http.geturl"; FAIL=$((FAIL+1));;
esac
expect "2 an array's .get(i) stays clean" \
    "$(diags 'm=q;i=http:std.http;f=main():i64{let a=@(1;2);<a.get(0)};')" "rc=0"
expect "3 a map's .get(k) stays clean" \
    "$(diags 'm=q;f=main():i64{let m=@("a":1);<m.get("a")};')" "rc=0"

echo "C299: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
